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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

static const char *TAG = "usb";

/* HID class requests and the boot protocol value (USB HID 1.11, §7.2). */
#define HID_REQ_SET_IDLE     0x0Au
#define HID_REQ_SET_PROTOCOL 0x0Bu
#define HID_PROTOCOL_BOOT    0x00u
#define HID_PROTOCOL_REPORT  0x01u
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
    /*
     * ARGON: what the endpoint last did, and whether it needs picking up.
     *
     * A transfer that ends in anything but COMPLETED leaves an interrupt IN
     * endpoint halted, and a halted endpoint refuses every further submit
     * until it is cleared.  The first version of this file resubmitted from
     * the callback whatever the status was and dropped the submit's own error
     * with a (void) - so one bus error ended the mouse for the rest of the
     * boot, in silence.  Measured on 7 September 2026: 249 reports and then
     * nothing, with `usb` still saying the device was open, the journal
     * holding no complaint, and unplugging the receiver not helping.
     */
    volatile int  last_status;  /* usb_transfer_status_t of the last report  */
    volatile bool recover;      /* the client task owes this pipe a clear    */
    uint32_t      errors;       /* how many times it has needed one          */
    bool          dead;         /* cleared and it still would not go         */
} usb_pipe_t;

static usb_pipe_t        s_pipe[USB_MAX_PIPES];
static uint32_t          s_pipes;
static uint16_t          s_vid;
static uint16_t          s_pid;
static volatile uint32_t s_reports;
/*
 * ARGON: how many times the client task has been round its loop.
 *
 * The one number that separates the two explanations for a mouse that has
 * stopped, and there was no way to choose between them: either the device has
 * gone quiet with the endpoint healthy, or this task is stuck and no event of
 * any kind is being handled - which also explains why unplugging the receiver
 * does not help, because the unplug is handled here too.  Measured 7 September
 * 2026: 249 reports and then nothing, twice, with `usb` reporting the endpoint
 * as "completed", no errors, and nothing in the journal.
 */
static volatile uint32_t s_cli_loops;

/*
 * ARGON: when the last report arrived, so `usb` can say how long a mouse has
 * been quiet.
 *
 * Reported and nothing more.  There was a watchdog here that acted on it; see
 * the note in the client task for why acting on silence was the fault rather
 * than the cure.
 */
static volatile uint32_t s_last_report_ms;

/*
 * ARGON: when the device attached, and which HID protocol it is being asked
 * to speak.
 *
 * 8 September 2026, and the fault that has cost Maxim most of a day.  The
 * descriptor is not the problem and neither is the pipe: this receiver offers
 * exactly one HID interface - boot subclass, protocol 2, one interrupt IN
 * endpoint of eight bytes every four milliseconds - it is claimed, SET_PROTOCOL
 * succeeds, the transfer is submitted for a whole packet and the library
 * accepts it, and then no transfer ever completes.  Zero reports in two
 * minutes, six hundred laps of a healthy task.  The same receiver and the same
 * mouse work on a PC.
 *
 * The one thing this host does that a PC does not is ask for the *boot*
 * protocol.  A PC's HID driver reads the report descriptor and leaves the
 * device in report protocol, which is what its firmware is built around; boot
 * protocol is the BIOS fallback, and a receiver is entitled to implement it
 * badly or to stop reporting when it is selected.
 *
 * Both were tried, alternately, and neither produced a single report - so the
 * protocol is not the answer and the switching has been taken out again.  What
 * was left in is the printing of the first few reports as raw bytes: the
 * layout differs between the two protocols, and reading it off a working mouse
 * is a great deal easier than guessing it.
 */
static uint32_t s_attach_ms;
static uint8_t  s_protocol = HID_PROTOCOL_BOOT;
static uint32_t s_shown;

/*
 * ARGON: and the state of the mouse, said out loud on a schedule.
 *
 * Maxim, 8 September 2026: "мышь стала очень часто отваливаться, с этим надо
 * разобраться раз и навсегда."  Every answer so far has come from typing `usb`
 * at the prompt, and that cannot be done while he plays: the console shares the
 * wire with the picture, and asking cost him the screen twice.  So the port
 * says it by itself, into the log the recorder already reads.
 *
 * Loud on any change, quiet otherwise: a line when a device arrives or goes,
 * and one every ten seconds while one is attached and something is wrong with
 * it.  A healthy attached mouse says nothing at all beyond one line a minute.
 */
#define USB_SAY_BUSY_MS  10000u
#define USB_SAY_IDLE_MS  60000u

static uint32_t s_said_at_ms;

/*
 * ARGON: and a way back when the receiver is not seen at all.
 *
 * A chip reset does not cut VBUS - there is no switch on this board's OTG port
 * - so the receiver stays powered through it.  From its own point of view it
 * was never unplugged, so it never signals a connection again, while the host
 * that has just restarted is waiting for exactly that signal.  Measured on
 * 8 September 2026: after an esptool reset the mouse enumerated at once; after
 * three RTS resets in a row, `usb: host up, waiting for a keyboard` and not one
 * `connect on address` line, with the receiver plugged in the whole time.
 *
 * A full uninstall and install of the host library powers the root port down
 * and up, which is the one thing in reach that makes an already-attached device
 * enumerate again.  Tried a few times and then left alone: if it is not that,
 * saying so three times is enough.
 */
#define USB_NO_DEVICE_MS 12000u
#define USB_RETRIES      3

static volatile bool s_restart_wanted;
static uint32_t      s_up_at_ms;
static uint32_t      s_retries;
static bool          s_ever_attached;

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
                    q->last_status = (int)USB_TRANSFER_STATUS_COMPLETED;
                    q->recover = false;
                    q->errors = 0;
                    q->dead = false;
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

/*
 * ARGON: read the HID report descriptor, and throw it away.
 *
 * Not because anything here parses it - the boot protocol exists so that
 * nothing has to - but because some devices will not start reporting until the
 * host has asked for it.  A PC always asks: its HID driver needs the
 * descriptor to know the layout, so every mouse in the world has been tested
 * with a host that reads it first, and a receiver that waits for the request
 * is a receiver nobody ever noticed was waiting.
 *
 * That is the last difference left between this host and a working one, for
 * the fault of 8 September 2026: one HID interface, correctly claimed, the
 * interrupt transfer armed for a whole packet and accepted by the library,
 * SET_PROTOCOL answered in both protocols, and not one report in two minutes
 * from a receiver that works on a PC with the same mouse.
 *
 * The length comes from the HID class descriptor if the walk found one, and
 * 256 otherwise: a short read is fine, the bytes are not used.
 */
#define HID_DESC_REPORT 0x22u

/*
 * ARGON: and since 8 Oct 2026 it is used.  A mouse's descriptor goes to the
 * kernel (ag_port_usb_on_descriptor), which says whether it can read the
 * device's own layout - and then the interface is left in report protocol, the
 * way a PC leaves it, because a receiver that ignores SET_PROTOCOL(boot) sends
 * its report layout whatever is asked.  The bytes go to the journal as well:
 * the layout of a device that misbehaves is the first thing anybody needs.
 */
static ag_port_usb_desc_fn s_on_desc;

static int get_report_descriptor(uint8_t itf, uint16_t len,
                                 ag_usb_usage_t usage, bool *report_proto)
{
    if (report_proto != NULL) {
        *report_proto = false;
    }
    if (len == 0u || len > 512u) {
        len = 256u;
    }

    usb_transfer_t *ct = NULL;
    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + len, 0, &ct)
        != ESP_OK) {
        return -1;
    }
    usb_setup_packet_t *sp = (usb_setup_packet_t *)ct->data_buffer;
    sp->bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN |
                        USB_BM_REQUEST_TYPE_TYPE_STANDARD |
                        USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    sp->bRequest = USB_B_REQUEST_GET_DESCRIPTOR;
    sp->wValue = (uint16_t)(HID_DESC_REPORT << 8);
    sp->wIndex = itf;
    sp->wLength = len;

    ct->device_handle = s_dev;
    ct->bEndpointAddress = 0;
    ct->num_bytes = (int)(sizeof(usb_setup_packet_t) + len);
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
    const int got = (int)ct->actual_num_bytes - (int)sizeof(usb_setup_packet_t);
    ESP_LOGI(TAG, "interface %u: report descriptor asked %u, got %d (%s)",
             itf, (unsigned)len, got, rc == 0 ? "ok" : "refused");
    const uint8_t *desc = ct->data_buffer + sizeof(usb_setup_packet_t);
    for (int at = 0; rc == 0 && at < got; at += 16) {
        char line[16 * 3 + 1];
        int  n = 0;
        for (int k = at; k < got && k < at + 16; k++) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, "%02x ", desc[k]);
        }
        ESP_LOGI(TAG, "  desc %3d: %s", at, line);
    }
    if (s_on_desc != NULL && report_proto != NULL) {
        /* a failed read is said too, so no layout outlives its mouse */
        const bool have = (rc == 0 && got > 0);
        *report_proto = s_on_desc(usage, itf, have ? desc : NULL,
                                  have ? (uint32_t)got : 0u);
    }
    usb_host_transfer_free(ct);
    return rc;
}

/* ---- report transfer --------------------------------------------------- */

static const char *usage_name(ag_usb_usage_t u);

static void in_cb(usb_transfer_t *t)
{
    /* Which interface this arrived on, carried in the transfer rather than
     * looked up: the callback has no other way to know, and announcing a
     * mouse's report as a keyboard's is what the single-pipe version did. */
    usb_pipe_t *q = (usb_pipe_t *)t->context;

    if (q != NULL) {
        q->last_status = (int)t->status;
    }

    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        s_reports++;
        s_last_report_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (s_shown < 4u && t->actual_num_bytes > 0) {
            s_shown++;
            /* Bytes, not meaning: the layout depends on which protocol the
             * device settled on, and that is exactly what is in doubt. */
            ESP_LOGI(TAG, "report %u on interface %u, %d bytes: "
                          "%02x %02x %02x %02x %02x %02x %02x %02x",
                     (unsigned)s_reports, (q != NULL) ? q->itf : 0xFFu,
                     t->actual_num_bytes,
                     t->data_buffer[0],
                     t->actual_num_bytes > 1 ? t->data_buffer[1] : 0,
                     t->actual_num_bytes > 2 ? t->data_buffer[2] : 0,
                     t->actual_num_bytes > 3 ? t->data_buffer[3] : 0,
                     t->actual_num_bytes > 4 ? t->data_buffer[4] : 0,
                     t->actual_num_bytes > 5 ? t->data_buffer[5] : 0,
                     t->actual_num_bytes > 6 ? t->data_buffer[6] : 0,
                     t->actual_num_bytes > 7 ? t->data_buffer[7] : 0);
        }
        if (s_on_report != NULL && t->actual_num_bytes > 0) {
            s_on_report((q != NULL) ? q->usage : AG_USB_USAGE_OTHER, 0,
                        t->data_buffer, (uint32_t)t->actual_num_bytes);
        }
        /*
         * Resubmit while the device is still here.  A NAK (nothing pressed) is
         * not an error the driver sees - the library holds the transfer until
         * the device has something to say - so a completed transfer that is
         * empty is rare, and the hot path is exactly this: ask again at once,
         * from the callback, with no task in between.
         */
        if (s_running && s_dev != NULL) {
            if (usb_host_transfer_submit(t) != ESP_OK && q != NULL) {
                q->recover = true;
            }
        }
        return;
    }

    /*
     * ARGON: anything else is an error, and an errored interrupt IN endpoint
     * is halted.  Resubmitting a halted endpoint fails for ever, so this does
     * not resubmit here - the client task clears the endpoint first, and says
     * out loud what happened.  Not from the callback, which runs inside
     * usb_host_client_handle_events and must not call back into the library's
     * blocking half.
     *
     * A device that has gone or a transfer that was cancelled are not faults:
     * detach() owns the transfer from then on.
     */
    if (q != NULL && t->status != USB_TRANSFER_STATUS_NO_DEVICE &&
        t->status != USB_TRANSFER_STATUS_CANCELED) {
        q->recover = true;
    }
}

static const char *status_name(int st)
{
    switch (st) {
    case USB_TRANSFER_STATUS_COMPLETED: return "completed";
    case USB_TRANSFER_STATUS_ERROR:     return "bus error";
    case USB_TRANSFER_STATUS_TIMED_OUT: return "timed out";
    case USB_TRANSFER_STATUS_CANCELED:  return "cancelled";
    case USB_TRANSFER_STATUS_STALL:     return "stalled";
    case USB_TRANSFER_STATUS_OVERFLOW:  return "overflow";
    case USB_TRANSFER_STATUS_SKIPPED:   return "skipped";
    case USB_TRANSFER_STATUS_NO_DEVICE: return "gone";
    default:                            return "unknown";
    }
}

/*
 * ARGON: take a halted endpoint back, on the client task.
 *
 * The sequence the library asks for: halt (idempotent, and required before the
 * other two are legal), flush the queue of whatever was in flight, then clear.
 * Only then will a submit be accepted again.
 *
 * A pipe that will not come back is marked dead and said once, rather than
 * retried for ever at the speed of the event loop - a mouse that has stopped
 * is worth one line in the journal, not a thousand.
 */
/* Declared here: the client task asks for it and it is defined after, next
 * to the two calls it makes. */
static void restart_task(void *arg);

static void pipe_recover(usb_pipe_t *q)
{
    q->recover = false;
    q->errors++;

    if (!s_running || s_dev == NULL || q->in == NULL || !q->claimed) {
        return;
    }

    ESP_LOGW(TAG, "%s on interface %u: %s after %u reports, clearing",
             usage_name(q->usage), q->itf, status_name(q->last_status),
             (unsigned)s_reports);

    (void)usb_host_endpoint_halt(s_dev, q->ep);
    (void)usb_host_endpoint_flush(s_dev, q->ep);
    const esp_err_t cl = usb_host_endpoint_clear(s_dev, q->ep);
    if (cl != ESP_OK) {
        ESP_LOGW(TAG, "interface %u: clear failed (%s)", q->itf,
                 esp_err_to_name(cl));
    }

    const esp_err_t sub = usb_host_transfer_submit(q->in);
    if (sub == ESP_ERR_NOT_FINISHED) {
        /*
         * ARGON: not a fault at all - the transfer is still queued, which is
         * the normal state of an armed interrupt IN pipe waiting on a device
         * that has nothing to say.  The first cut of this treated it as a
         * failure and marked the pipe dead, so the watchdog meant to revive a
         * silent mouse was the thing that finished it off.  Measured on
         * 8 September 2026: "not answering (ESP_ERR_NOT_FINISHED)" four
         * seconds after a clean attach.
         */
        return;
    }
    if (sub != ESP_OK) {
        q->dead = true;
        ESP_LOGE(TAG, "%s on interface %u is not answering (%s); it needs the "
                      "receiver unplugged and put back",
                 usage_name(q->usage), q->itf, esp_err_to_name(sub));
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

    /*
     * ARGON: and what it actually says, descriptor by descriptor.
     *
     * The mouse works on a PC and delivers exactly one report here - one, at
     * attach, and then nothing for thirty-nine minutes through five hundred
     * and eighty-eight attempts to re-arm the endpoint.  Every explanation
     * left is about *which* interface and *which* endpoint, and this walk has
     * never once printed what it saw: `usb` says "mouse interface 0 ep 0x82",
     * and on a receiver of this kind interface 0 is usually the keyboard.
     * Twelve lines of log against another day of guessing.
     */
    {
        const uint8_t *p = (const uint8_t *)cd;
        uint16_t off = 0;
        while (off + 2u <= cd->wTotalLength) {
            const usb_standard_desc_t *d = (const usb_standard_desc_t *)(p + off);
            if (d->bLength == 0u) {
                break;
            }
            if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
                const usb_intf_desc_t *i = (const usb_intf_desc_t *)d;
                ESP_LOGI(TAG, "  interface %u alt %u: class %u sub %u proto %u,"
                              " %u endpoints",
                         i->bInterfaceNumber, i->bAlternateSetting,
                         i->bInterfaceClass, i->bInterfaceSubClass,
                         i->bInterfaceProtocol, i->bNumEndpoints);
            } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
                const usb_ep_desc_t *e = (const usb_ep_desc_t *)d;
                ESP_LOGI(TAG, "    endpoint 0x%02x attr 0x%02x mps %u "
                              "interval %u",
                         e->bEndpointAddress, e->bmAttributes,
                         e->wMaxPacketSize, e->bInterval);
            }
            off = (uint16_t)(off + d->bLength);
        }
    }

    usb_pipe_t found[USB_MAX_PIPES];
    const uint32_t n = find_hid(cd, found, USB_MAX_PIPES);
    for (uint32_t i = 0; i < n; i++) {
        ESP_LOGI(TAG, "  taking %s on interface %u, endpoint 0x%02x, mps %u",
                 usage_name(found[i].usage), found[i].itf, found[i].ep,
                 (unsigned)found[i].mps);
    }
    if (n == 0) {
        ESP_LOGI(TAG, "device %04x:%04x has no HID interrupt input, ignoring",
                 dd->idVendor, dd->idProduct);
        (void)usb_host_device_close(s_client, dev);
        return;
    }

    s_dev = dev;
    /* ARGON: so the retry above stops once anything has ever been seen -
     * a device that comes and goes is a different fault from one that was
     * never enumerated at all. */
    s_ever_attached = true;
    s_vid = dd->idVendor;
    s_pid = dd->idProduct;
    s_reports = 0;
    s_pipes = 0;
    s_attach_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_last_report_ms = 0u;
    s_protocol = HID_PROTOCOL_BOOT;
    s_shown = 0u;

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
        /* ARGON: a mouse's descriptor first, because it decides the
         * protocol; a keyboard's after, as before.  See get_report_descriptor. */
        bool report_proto = false;
        if (q->usage == AG_USB_USAGE_MOUSE) {
            (void)get_report_descriptor(q->itf, 0, q->usage, &report_proto);
        }
        const uint8_t proto = report_proto ? HID_PROTOCOL_REPORT
                                           : HID_PROTOCOL_BOOT;
        if (class_request(q->itf, HID_REQ_SET_PROTOCOL, proto) != 0) {
            ESP_LOGW(TAG, "interface %u: SET_PROTOCOL(%s) failed", q->itf,
                     report_proto ? "report" : "boot");
        }
        (void)class_request(q->itf, HID_REQ_SET_IDLE, 0);
        if (q->usage != AG_USB_USAGE_MOUSE) {
            (void)get_report_descriptor(q->itf, 0, q->usage, NULL);
        }
        if (report_proto) {
            ESP_LOGI(TAG, "interface %u: mouse read by its own layout", q->itf);
        }

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
        s_cli_loops++;
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

        /* ARGON: and any endpoint that stopped answering.  See pipe_recover. */
        for (uint32_t i = 0; i < s_pipes; i++) {
            if (s_pipe[i].recover && !s_pipe[i].dead) {
                pipe_recover(&s_pipe[i]);
            }
        }

        /*
         * ARGON: say where things stand, so nobody has to type `usb` while a
         * game is on the screen.  See the note on USB_SAY_BUSY_MS.
         */
        {
            const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            const uint32_t quiet = (s_last_report_ms == 0u)
                ? 0u : (uint32_t)(now - s_last_report_ms);
            const bool worrying = (s_dev != NULL && quiet >= 4000u);
            const uint32_t every = worrying ? USB_SAY_BUSY_MS : USB_SAY_IDLE_MS;

            if (s_said_at_ms == 0u ||
                (uint32_t)(now - s_said_at_ms) >= every) {
                s_said_at_ms = now;
                if (s_dev == NULL) {
                    ESP_LOGI(TAG, "no device attached (%u laps, %u retries)",
                             (unsigned)s_cli_loops, (unsigned)s_retries);
                } else {
                    ESP_LOGI(TAG, "%04x:%04x, %u reports, quiet %u ms, "
                                  "%u laps%s",
                             s_vid, s_pid, (unsigned)s_reports,
                             (unsigned)quiet, (unsigned)s_cli_loops,
                             (s_pipes > 0 && s_pipe[0].dead) ? ", DEAD" : "");
                }
            }
        }

        /*
         * ARGON: and if nothing has ever turned up, try the host again.  See
         * the note on USB_NO_DEVICE_MS - a receiver that stayed powered
         * through a chip reset never says hello a second time.
         */
        if (s_dev == NULL && !s_ever_attached && s_retries < USB_RETRIES) {
            const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            if (s_up_at_ms != 0u &&
                (uint32_t)(now - s_up_at_ms) >= USB_NO_DEVICE_MS) {
                s_up_at_ms = now;
                s_retries++;
                ESP_LOGW(TAG, "nothing has enumerated in %u ms; taking the "
                              "host down and up again (attempt %u of %u)",
                         (unsigned)USB_NO_DEVICE_MS, (unsigned)s_retries,
                         (unsigned)USB_RETRIES);
                s_restart_wanted = false;
                if (xTaskCreate(restart_task, "usb_restart", 3072, NULL, 5,
                                NULL) != pdPASS) {
                    ESP_LOGE(TAG, "no room for the restart task");
                } else {
                    /* stop() will end this task; nothing after here runs. */
                    break;
                }
            }
        }

        /*
         * ARGON: nothing here touches a pipe that is merely quiet.
         *
         * There used to be a watchdog: no report for four seconds while a
         * device was attached, and it would halt, flush, clear and re-arm the
         * endpoint.  It was written to revive a mouse that had gone silent,
         * and it was the reason the mouse went silent.
         *
         * Silence is the normal state of an interrupt IN pipe on a mouse
         * nobody is touching - the device NAKs and the library holds the
         * transfer - so the watchdog fired on every idle moment, and each
         * firing halted the endpoint.  Measured 8 September 2026, on a board
         * doing nothing else at all: two of those in the first eleven seconds
         * after a clean attach, and then zero reports for forty-five minutes,
         * with the receiver enumerated, the descriptor read, both HID
         * protocols tried and the same mouse working on a PC.  Maxim's own
         * summary was the clue: "работала стабильно, но в какой-то момент
         * стала отваливаться" - and what changed was this.
         *
         * A halted endpoint is also exactly the fault this port had before,
         * fixed in 58cfd95, and the fix for that one was to stop resubmitting
         * blindly.  Adding code that halts a healthy endpoint on purpose put
         * it straight back.
         *
         * So: recovery happens only where a transfer has actually failed - the
         * error path in in_cb sets q->recover, and the sweep above acts on it.
         * A quiet pipe is left alone, for ever if need be.
         */
    }
    s_cli_task = NULL;
    vTaskDelete(NULL);
}

/*
 * ARGON: take the host down and up again, from a task of its own.
 *
 * Not from the client task: ag_port_usb_stop waits for the client task to
 * finish before tearing the library down, so a client task that called it
 * would wait for itself.  This one is created on demand, does the two calls,
 * and deletes itself.
 */
static void restart_task(void *arg)
{
    (void)arg;
    (void)ag_port_usb_stop();
    const ag_err_t rc = ag_port_usb_start();
    if (rc != AG_OK) {
        ESP_LOGE(TAG, "the host would not come back up (%d)", (int)rc);
    } else {
        ESP_LOGI(TAG, "host restarted; waiting for a device again");
    }
    vTaskDelete(NULL);
}

/* ---- the contract ------------------------------------------------------ */

void ag_port_usb_on_report(ag_port_usb_report_fn fn) { s_on_report = fn; }
void ag_port_usb_on_descriptor(ag_port_usb_desc_fn fn) { s_on_desc = fn; }

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
    s_up_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_said_at_ms = 0u;

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
    out->loops = s_cli_loops;
    out->silent_ms = (s_last_report_ms == 0u)
        ? 0u
        : (uint32_t)((uint32_t)(esp_timer_get_time() / 1000) - s_last_report_ms);
    out->prods = 0u;   /* the watchdog that used them is gone; see the note in cli_task */

    const uint32_t room = (uint32_t)(sizeof(out->pipe) / sizeof(out->pipe[0]));
    for (uint32_t i = 0; i < s_pipes && i < room; i++) {
        out->pipe[i].itf = s_pipe[i].itf;
        out->pipe[i].ep = s_pipe[i].ep;
        out->pipe[i].usage = s_pipe[i].usage;
        out->pipe[i].last = status_name(s_pipe[i].last_status);
        out->pipe[i].errors = s_pipe[i].errors;
        out->pipe[i].dead = s_pipe[i].dead;
        out->pipes = i + 1u;
    }
    return AG_OK;
}

#endif /* SOC_USB_OTG_SUPPORTED */

#endif /* CONFIG_ARGON_USB_HOST */
