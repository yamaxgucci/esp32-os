/*
 * ArgonOS port: ESP-IDF - USB host, boot-protocol HID keyboard.
 *
 * The counterpart of bt_hw.c for a chip that has USB.  It brings up the OTG
 * peripheral as a host, waits for a keyboard to be plugged in, asks it to speak
 * the boot protocol (a fixed eight-byte report), and hands each report up the
 * same way the radio does - to a callback the kernel turns into console events
 * (src/dev/hidkbd.c).  Nothing here knows what a keystroke is.
 *
 * Minimal on purpose: boot protocol, so there is no report-descriptor parser,
 * which is most of a general HID stack and none of what a keyboard needs.  One
 * device at a time; a mouse is the same report path once there is something on
 * the screen to point at.
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

static volatile ag_usb_state_t s_state = AG_USB_OFF;
static volatile bool           s_running;
static ag_port_usb_report_fn   s_on_report;

static usb_host_client_handle_t s_client;
static TaskHandle_t             s_lib_task;
static TaskHandle_t             s_cli_task;

/* The attached keyboard, touched only from the client task. */
static usb_device_handle_t s_dev;
static uint8_t             s_itf;      /* claimed interface number            */
static uint8_t             s_ep;       /* its interrupt IN endpoint           */
static usb_transfer_t     *s_in;       /* the in-flight report transfer       */
static uint16_t            s_vid;
static uint16_t            s_pid;
static volatile uint32_t   s_reports;

/* Set by the client callback, acted on by the client task - never open a device
 * from inside the callback, which runs under usb_host_client_handle_events. */
static volatile uint8_t             s_pending_new;   /* address, 0 = none      */
static volatile usb_device_handle_t s_pending_gone;

/* One control transfer, completed synchronously by pumping client events. */
static volatile bool s_ctrl_done;
static volatile int  s_ctrl_status;

/* ---- descriptor walk --------------------------------------------------- */

/*
 * Find a HID interface's interrupt IN endpoint in the active configuration.
 * Prefers a boot keyboard (subclass 1, protocol 1) but takes any HID interrupt
 * IN, so a keyboard that does not advertise the boot subclass but honours
 * SET_PROTOCOL still works.  Returns true and fills the outputs on a match.
 */
static bool find_kbd(const usb_config_desc_t *cfg, uint8_t *itf_out,
                     uint8_t *ep_out, uint16_t *mps_out)
{
    const uint8_t *p = (const uint8_t *)cfg;
    const uint16_t total = cfg->wTotalLength;

    const usb_intf_desc_t *cur = NULL;
    bool     found = false;
    bool     found_boot = false;
    uint16_t off = 0;

    while (off + 2u <= total) {
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
                const bool boot =
                    cur->bInterfaceSubClass == HID_SUBCLASS_BOOT &&
                    cur->bInterfaceProtocol == HID_PROTOCOL_KEYBOARD;
                /* Keep the first match; upgrade to a boot keyboard if one
                 * appears later, and stop once we have it. */
                if (!found || (boot && !found_boot)) {
                    *itf_out = cur->bInterfaceNumber;
                    *ep_out = ep->bEndpointAddress;
                    *mps_out = ep->wMaxPacketSize;
                    found = true;
                    found_boot = boot;
                }
                if (found_boot) {
                    break;
                }
            }
        }
        off = (uint16_t)(off + d->bLength);
    }
    return found;
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
static int class_request(uint8_t bRequest, uint16_t wValue)
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
    s->wIndex = s_itf;
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
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        s_reports++;
        if (s_on_report != NULL && t->actual_num_bytes > 0) {
            s_on_report(AG_USB_USAGE_KEYBOARD, 0, t->data_buffer,
                        (uint32_t)t->actual_num_bytes);
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

static void attach(uint8_t addr)
{
    if (s_dev != NULL) {
        return; /* one at a time */
    }
    usb_device_handle_t dev = NULL;
    if (usb_host_device_open(s_client, addr, &dev) != ESP_OK) {
        return;
    }

    const usb_device_desc_t *dd = NULL;
    const usb_config_desc_t *cd = NULL;
    if (usb_host_get_device_descriptor(dev, &dd) != ESP_OK ||
        usb_host_get_active_config_descriptor(dev, &cd) != ESP_OK) {
        (void)usb_host_device_close(s_client, dev);
        return;
    }

    uint8_t  itf = 0;
    uint8_t  ep = 0;
    uint16_t mps = 0;
    if (!find_kbd(cd, &itf, &ep, &mps)) {
        ESP_LOGI(TAG, "device %04x:%04x has no HID keyboard, ignoring",
                 dd->idVendor, dd->idProduct);
        (void)usb_host_device_close(s_client, dev);
        return;
    }
    if (mps == 0u || mps > 64u) {
        mps = 8u; /* a boot keyboard report is eight bytes */
    }

    if (usb_host_interface_claim(s_client, dev, itf, 0) != ESP_OK) {
        (void)usb_host_device_close(s_client, dev);
        return;
    }

    s_dev = dev;
    s_itf = itf;
    s_ep = ep;
    s_vid = dd->idVendor;
    s_pid = dd->idProduct;
    s_reports = 0;

    /* Boot protocol, then idle-forever so the keyboard reports only on change.
     * SET_IDLE is advisory - some keyboards STALL it - so its failure is not
     * fatal, but SET_PROTOCOL is what makes the eight-byte report the truth. */
    if (class_request(HID_REQ_SET_PROTOCOL, HID_PROTOCOL_BOOT) != 0) {
        ESP_LOGW(TAG, "SET_PROTOCOL(boot) failed; report layout unknown");
    }
    (void)class_request(HID_REQ_SET_IDLE, 0);

    if (usb_host_transfer_alloc(mps, 0, &s_in) != ESP_OK) {
        (void)usb_host_interface_release(s_client, dev, itf);
        (void)usb_host_device_close(s_client, dev);
        s_dev = NULL;
        return;
    }
    s_in->device_handle = dev;
    s_in->bEndpointAddress = ep;
    s_in->callback = in_cb;
    s_in->context = NULL;
    s_in->num_bytes = mps; /* an IN transfer asks for a whole packet */

    s_state = AG_USB_OPEN;
    ESP_LOGI(TAG, "keyboard %04x:%04x on interface %u, ep 0x%02x",
             s_vid, s_pid, itf, ep);
    if (usb_host_transfer_submit(s_in) != ESP_OK) {
        ESP_LOGW(TAG, "first report submit failed");
    }
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

    /* Stop the report pipe before touching what it uses.  s_dev = NULL first,
     * so a report callback that fires during the flush does not resubmit. */
    s_dev = NULL;
    (void)usb_host_endpoint_halt(dev, s_ep);
    (void)usb_host_endpoint_flush(dev, s_ep);
    if (s_in != NULL) {
        (void)usb_host_transfer_free(s_in);
        s_in = NULL;
    }
    (void)usb_host_interface_release(s_client, dev, s_itf);
    (void)usb_host_device_close(s_client, dev);

    s_state = (s_state == AG_USB_OFF) ? AG_USB_OFF : AG_USB_IDLE;
    s_vid = 0;
    s_pid = 0;
    ESP_LOGI(TAG, "keyboard unplugged");
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
