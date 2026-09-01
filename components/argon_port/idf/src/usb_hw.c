/*
 * ArgonOS port: ESP-IDF - USB host, boot-protocol HID keyboard and mouse.
 *
 * The counterpart of bt_hw.c for a chip that has USB.  It brings up the OTG
 * peripheral as a host, waits for a keyboard to be plugged in, asks it to speak
 * the boot protocol (a fixed eight-byte report), and hands each report up the
 * same way the radio does - to a callback the kernel turns into console events
 * (src/dev/hidkbd.c).  Nothing here knows what a keystroke is.
 *
 * Minimal on purpose: boot protocol, so there is no report-descriptor parser,
 * which is most of a general HID stack and none of what a keyboard or a mouse
 * needs.  One device at a time - but *all* of that device's HID interfaces,
 * which is not the same thing and was the bug: the receiver that comes with a
 * wireless mouse is one device presenting two interfaces, a keyboard and a
 * mouse, and a host that claims only the first one claims the keyboard and then
 * wonders why the mouse is silent.
 *
 * Which of the two a report came from is the interface's protocol byte, and
 * that is the whole classification: 1 is a keyboard, 2 is a mouse.  It used to
 * be read to *prefer* a keyboard and then thrown away, so every report was
 * announced as a keyboard's - including a mouse's.
 *
 * The board powers the keyboard: host mode sources 5 V on the OTG port's VBUS,
 * and the internal PHY has no VBUS switch here, so that is a fact about the
 * wiring, not something this code turns on.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdkconfig.h"

#if defined(CONFIG_ARGON_USB_HOST) && CONFIG_ARGON_USB_HOST

#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED

#include <string.h>

#include <argon/port/usb.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

static const char *TAG = "usb";

/* HID class requests and the boot protocol value (USB HID 1.11, §7.2). */
#define HID_REQ_SET_IDLE     0x0Au
#define HID_REQ_SET_PROTOCOL 0x0Bu
#define HID_PROTOCOL_BOOT    0x00u
#define HID_SUBCLASS_BOOT    0x01u
#define HID_PROTOCOL_KEYBOARD 0x01u
#define HID_PROTOCOL_MOUSE    0x02u

static volatile ag_usb_state_t s_state = AG_USB_OFF;
static volatile bool           s_running;
static ag_port_usb_report_fn   s_on_report;

static usb_host_client_handle_t s_client;
static TaskHandle_t             s_lib_task;
static TaskHandle_t             s_cli_task;

/* The attached keyboard, touched only from the client task. */
static usb_device_handle_t s_dev;

/*
 * One pipe per HID interface the device offers, because a device can offer more
 * than one and they are not interchangeable.  Three is enough for anything that
 * turns up on a desk: a keyboard, a mouse, and the consumer-control interface
 * that combo receivers add for volume keys.
 */
#define USB_MAX_PIPES 3

typedef struct {
    uint8_t         itf;
    uint8_t         ep;
    uint16_t        mps;
    ag_usb_usage_t  usage;
    bool            claimed;
    usb_transfer_t *in;
} usb_pipe_t;

static usb_pipe_t        s_pipe[USB_MAX_PIPES];
static uint32_t          s_pipes;
static uint16_t          s_vid;
static uint16_t          s_pid;
static volatile uint32_t s_reports;

/* Set by the client callback, acted on by the client task - never open a device
 * from inside the callback, which runs under usb_host_client_handle_events. */
static volatile uint8_t             s_pending_new;   /* address, 0 = none      */
static volatile usb_device_handle_t s_pending_gone;

/* One control transfer, completed synchronously by pumping client events. */
static volatile bool s_ctrl_done;
static volatile int  s_ctrl_status;

/* ---- descriptor walk --------------------------------------------------- */

/*
 * Every HID interface with an interrupt IN endpoint, classified by its protocol
 * byte.  All of them rather than the best one: see the note at the top about
 * receivers that present a keyboard and a mouse and expect the host to take
 * both.
 *
 * A protocol byte of 0 - "none" - is still taken and reported as OTHER.  Some
 * keyboards do not claim the boot subclass and honour SET_PROTOCOL anyway, and
 * refusing them for a byte they did not fill in would be refusing a keyboard
 * that works.
 */
static uint32_t find_hid(const usb_config_desc_t *cfg, usb_pipe_t *out,
                         uint32_t max)
{
    const uint8_t *p = (const uint8_t *)cfg;
    const uint16_t total = cfg->wTotalLength;

    const usb_intf_desc_t *cur = NULL;
    uint32_t               n = 0;
    uint16_t               off = 0;

    while (off + 2u <= total && n < max) {
        const usb_standard_desc_t *d = (const usb_standard_desc_t *)(p + off);
        if (d->bLength == 0u) {
            break;
        }
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            cur = (const usb_intf_desc_t *)d;
        } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT &&
                   cur != NULL && cur->bInterfaceClass == USB_CLASS_HID) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)d;
            const bool is_in =
                (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) != 0;
            const bool is_int =
                (ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) ==
                USB_BM_ATTRIBUTES_XFER_INT;
            if (is_in && is_int) {
                /* One pipe per interface: a second endpoint on an interface
                 * already taken is the same device saying the same things. */
                bool dup = false;
                for (uint32_t i = 0; i < n; i++) {
                    if (out[i].itf == cur->bInterfaceNumber) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) {
                    usb_pipe_t *q = &out[n++];
                    q->itf = cur->bInterfaceNumber;
                    q->ep = ep->bEndpointAddress;
                    q->mps = ep->wMaxPacketSize;
                    q->claimed = false;
                    q->in = NULL;
                    switch (cur->bInterfaceProtocol) {
                    case HID_PROTOCOL_KEYBOARD:
                        q->usage = AG_USB_USAGE_KEYBOARD;
                        break;
                    case HID_PROTOCOL_MOUSE:
                        q->usage = AG_USB_USAGE_MOUSE;
                        break;
                    default:
                        q->usage = AG_USB_USAGE_OTHER;
                        break;
                    }
                }
            }
        }
        off = (uint16_t)(off + d->bLength);
    }
    return n;
}

/* ---- control transfer (SET_PROTOCOL / SET_IDLE) ------------------------ */

static void ctrl_cb(usb_transfer_t *t)
{
    s_ctrl_status = (t->status == USB_TRANSFER_STATUS_COMPLETED) ? 0 : -1;
    s_ctrl_done = true;
}

/*
 * A no-data class request to the interface, completed by pumping client events
 * until its callback fires.  We are on the client task here (called from the
 * attach path), so this is a fresh top-level pump, not re-entrancy.
 */
static int class_request(uint8_t itf, uint8_t bRequest, uint16_t wValue)
{
    usb_transfer_t *ct = NULL;
    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &ct) != ESP_OK) {
        return -1;
    }
    usb_setup_packet_t *s = (usb_setup_packet_t *)ct->data_buffer;
    s->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT |
                       USB_BM_REQUEST_TYPE_TYPE_CLASS |
                       USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    s->bRequest = bRequest;
    s->wValue = wValue;
    s->wIndex = itf;
    s->wLength = 0;

    ct->device_handle = s_dev;
    ct->bEndpointAddress = 0; /* EP0, the default control pipe */
    ct->num_bytes = sizeof(usb_setup_packet_t);
    ct->callback = ctrl_cb;
    ct->context = NULL;

    s_ctrl_done = false;
    s_ctrl_status = -1;
    if (usb_host_transfer_submit_control(s_client, ct) != ESP_OK) {
        usb_host_transfer_free(ct);
        return -1;
    }
    for (int i = 0; i < 50 && !s_ctrl_done; i++) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(20));
    }
    const int rc = s_ctrl_done ? s_ctrl_status : -1;
    usb_host_transfer_free(ct);
    return rc;
}

/* ---- report transfer --------------------------------------------------- */

static void in_cb(usb_transfer_t *t)
{
    /* Which interface this arrived on, carried in the transfer rather than
     * looked up: the callback has no other way to know, and announcing a
     * mouse's report as a keyboard's is what the single-pipe version did. */
    const usb_pipe_t *q = (const usb_pipe_t *)t->context;

    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        s_reports++;
        if (s_on_report != NULL && t->actual_num_bytes > 0) {
            s_on_report((q != NULL) ? q->usage : AG_USB_USAGE_OTHER, 0,
                        t->data_buffer, (uint32_t)t->actual_num_bytes);
        }
    }
    /*
     * Resubmit while the device is still here.  A NAK (nothing pressed) is not
     * an error the driver sees - the library holds the transfer until the
     * keyboard has something to say - so a completed transfer that is empty is
     * rare, and any non-fatal status is handled by simply asking again.  When
     * the device is gone, s_dev is already NULL and the transfer is left for
     * detach() to free.
     */
    if (s_running && s_dev != NULL &&
        t->status != USB_TRANSFER_STATUS_NO_DEVICE &&
        t->status != USB_TRANSFER_STATUS_CANCELED) {
        (void)usb_host_transfer_submit(t);
    }
}

/* ---- attach / detach --------------------------------------------------- */

static const char *usage_name(ag_usb_usage_t u)
{
    switch (u) {
    case AG_USB_USAGE_KEYBOARD: return "keyboard";
    case AG_USB_USAGE_MOUSE:    return "mouse";
    default:                    return "hid";
    }
}

static void attach(uint8_t addr)
{
    if (s_dev != NULL) {
        return; /* one device at a time; all of its interfaces, though */
    }
    usb_device_handle_t dev = NULL;
    if (usb_host_device_open(s_client, addr, &dev) != ESP_OK) {
        /*
         * Said out loud, because silence here is indistinguishable from a
         * device that never arrived - and those two have nothing in common.
         * This is where a device that answered the bus but not the host lands.
         */
        ESP_LOGW(TAG, "address %u: open failed", addr);
        return;
    }

    const usb_device_desc_t *dd = NULL;
    const usb_config_desc_t *cd = NULL;
    if (usb_host_get_device_descriptor(dev, &dd) != ESP_OK ||
        usb_host_get_active_config_descriptor(dev, &cd) != ESP_OK) {
        ESP_LOGW(TAG, "address %u: no descriptors", addr);
        (void)usb_host_device_close(s_client, dev);
        return;
    }

    /*
     * Say what turned up, always.  Before this the only line on this path was
     * the refusal, so a device that enumerated and offered nothing usable and
     * a device that never enumerated at all looked identical from the console -
     * which is the wrong two things to be unable to tell apart when somebody
     * has just plugged something in and is waiting.
     */
    ESP_LOGI(TAG, "device %04x:%04x, %u configuration interfaces",
             dd->idVendor, dd->idProduct, cd->bNumInterfaces);

    usb_pipe_t found[USB_MAX_PIPES];
    const uint32_t n = find_hid(cd, found, USB_MAX_PIPES);
    if (n == 0) {
        ESP_LOGI(TAG, "device %04x:%04x has no HID interrupt input, ignoring",
                 dd->idVendor, dd->idProduct);
        (void)usb_host_device_close(s_client, dev);
        return;
    }

    s_dev = dev;
    s_vid = dd->idVendor;
    s_pid = dd->idProduct;
    s_reports = 0;
    s_pipes = 0;

    for (uint32_t i = 0; i < n; i++) {
        usb_pipe_t *q = &s_pipe[s_pipes];
        *q = found[i];
        if (q->mps == 0u || q->mps > 64u) {
            q->mps = 8u; /* a boot report is eight bytes or fewer */
        }

        if (usb_host_interface_claim(s_client, dev, q->itf, 0) != ESP_OK) {
            ESP_LOGW(TAG, "interface %u (%s) refused", q->itf,
                     usage_name(q->usage));
            continue;
        }
        q->claimed = true;

        /*
         * Boot protocol, then idle-forever so the device reports only on
         * change.  SET_IDLE is advisory - some devices STALL it - so its
         * failure is not fatal, while SET_PROTOCOL is what makes the fixed
         * report layout the truth.  Per interface, because that is what the
         * request is addressed to.
         */
        if (class_request(q->itf, HID_REQ_SET_PROTOCOL, HID_PROTOCOL_BOOT) != 0) {
            ESP_LOGW(TAG, "interface %u: SET_PROTOCOL(boot) failed", q->itf);
        }
        (void)class_request(q->itf, HID_REQ_SET_IDLE, 0);

        if (usb_host_transfer_alloc(q->mps, 0, &q->in) != ESP_OK) {
            (void)usb_host_interface_release(s_client, dev, q->itf);
            q->claimed = false;
            continue;
        }
        q->in->device_handle = dev;
        q->in->bEndpointAddress = q->ep;
        q->in->callback = in_cb;
        q->in->context = q;
        q->in->num_bytes = q->mps; /* an IN transfer asks for a whole packet */

        ESP_LOGI(TAG, "%s on interface %u, ep 0x%02x, %u byte reports",
                 usage_name(q->usage), q->itf, q->ep, (unsigned)q->mps);
        s_pipes++;
        if (usb_host_transfer_submit(q->in) != ESP_OK) {
            ESP_LOGW(TAG, "interface %u: first report submit failed", q->itf);
        }
    }

    if (s_pipes == 0) {
        ESP_LOGW(TAG, "device %04x:%04x offered %u HID interfaces and none took",
                 s_vid, s_pid, (unsigned)n);
        s_dev = NULL;
        (void)usb_host_device_close(s_client, dev);
        return;
    }
    s_state = AG_USB_OPEN;
}

static void detach(usb_device_handle_t dev)
{
    if (dev == NULL || dev != s_dev) {
        /* Close a stray handle the library still wants closed. */
        if (dev != NULL) {
            (void)usb_host_device_close(s_client, dev);
        }
        return;
    }

    /* Stop the report pipes before touching what they use.  s_dev = NULL
     * first, so a callback that fires during the flush does not resubmit. */
    s_dev = NULL;
    for (uint32_t i = 0; i < s_pipes; i++) {
        usb_pipe_t *q = &s_pipe[i];
        if (!q->claimed) {
            continue;
        }
        (void)usb_host_endpoint_halt(dev, q->ep);
        (void)usb_host_endpoint_flush(dev, q->ep);
        if (q->in != NULL) {
            (void)usb_host_transfer_free(q->in);
            q->in = NULL;
        }
        (void)usb_host_interface_release(s_client, dev, q->itf);
        q->claimed = false;
    }
    s_pipes = 0;
    (void)usb_host_device_close(s_client, dev);

    s_state = (s_state == AG_USB_OFF) ? AG_USB_OFF : AG_USB_IDLE;
    ESP_LOGI(TAG, "%04x:%04x unplugged", s_vid, s_pid);
    s_vid = 0;
    s_pid = 0;
}

/* ---- the two tasks ----------------------------------------------------- */

static void client_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        s_pending_new = msg->new_dev.address;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        s_pending_gone = msg->dev_gone.dev_hdl;
        break;
    default:
        break;
    }
}

static void lib_task(void *arg)
{
    (void)arg;
    while (s_running) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            (void)usb_host_device_free_all();
        }
    }
    s_lib_task = NULL;
    vTaskDelete(NULL);
}

static void cli_task(void *arg)
{
    (void)arg;
    while (s_running) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(200));

        const uint8_t addr = s_pending_new;
        if (addr != 0u) {
            s_pending_new = 0u;
            /*
             * The one line that separates "nothing is plugged in, or it is and
             * the bus never noticed" from "it was noticed and something after
             * that went wrong".  Without it both look like an empty log, and an
             * empty log sends you looking at the wrong half.
             */
            ESP_LOGI(TAG, "connect on address %u", addr);
            attach(addr);
        }
        usb_device_handle_t gone = s_pending_gone;
        if (gone != NULL) {
            s_pending_gone = NULL;
            detach(gone);
        }
    }
    s_cli_task = NULL;
    vTaskDelete(NULL);
}

/* ---- the contract ------------------------------------------------------ */

void ag_port_usb_on_report(ag_port_usb_report_fn fn) { s_on_report = fn; }

ag_err_t ag_port_usb_start(void)
{
    if (s_state != AG_USB_OFF) {
        return AG_OK;
    }

    const usb_host_config_t hc = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    if (usb_host_install(&hc) != ESP_OK) {
        return -AG_EIO;
    }

    const usb_host_client_config_t cc = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_cb,
            .callback_arg = NULL,
        },
    };
    if (usb_host_client_register(&cc, &s_client) != ESP_OK) {
        (void)usb_host_uninstall();
        return -AG_EIO;
    }

    s_running = true;
    s_state = AG_USB_IDLE;

    /* The library daemon and the client, each on a small stack: this is opt-in
     * (CONFIG_ARGON_USB_HOST) and only on a board that has the OTG peripheral. */
    if (xTaskCreate(lib_task, "usb_lib", 4096, NULL, 5, &s_lib_task) != pdPASS ||
        xTaskCreate(cli_task, "usb_cli", 4096, NULL, 5, &s_cli_task) != pdPASS) {
        (void)ag_port_usb_stop();
        return -AG_ENOMEM;
    }
    return AG_OK;
}

ag_err_t ag_port_usb_stop(void)
{
    if (s_state == AG_USB_OFF) {
        return AG_OK;
    }
    s_running = false;

    if (s_dev != NULL) {
        detach(s_dev);
    }
    /* The tasks wake from their handle_events within a tick or two and delete
     * themselves; give them that before tearing the host down under them. */
    for (int i = 0; i < 25 && (s_lib_task != NULL || s_cli_task != NULL); i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_client != NULL) {
        (void)usb_host_client_deregister(s_client);
        s_client = NULL;
    }
    (void)usb_host_uninstall();

    s_state = AG_USB_OFF;
    s_reports = 0;
    return AG_OK;
}

ag_err_t ag_port_usb_status(ag_port_usb_status_t *out)
{
    if (out == NULL) {
        return -AG_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    out->state = s_state;
    out->vid = s_vid;
    out->pid = s_pid;
    out->reports = s_reports;
    return AG_OK;
}

#endif /* SOC_USB_OTG_SUPPORTED */

#endif /* CONFIG_ARGON_USB_HOST */
