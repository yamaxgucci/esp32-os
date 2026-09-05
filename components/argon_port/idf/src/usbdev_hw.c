/*
 * ArgonOS port: ESP-IDF - USB device, a HID keyboard and mouse for someone else.
 *
 * The mirror of usb_hw.c.  There the OTG peripheral is a host and a keyboard is
 * plugged into this board; here the peripheral is a device, this board is
 * plugged into a PC, and that PC enumerates a keyboard and a mouse it has no
 * driver for and needs none for.  That is the whole point: the machine at the
 * far end runs nothing of ours, and cannot be asked to, because the moments
 * this exists for - a BIOS, a boot menu, a password prompt, a wedged desktop -
 * are exactly the moments when no program can be installed there.
 *
 * Three interfaces, published as one composite device:
 *
 *   0  boot keyboard   eight bytes: modifiers, reserved, six key slots
 *   1  boot mouse      four bytes: buttons, dx, dy, wheel
 *   2  absolute mouse  six bytes: buttons, x, y (0..32767), wheel
 *
 * The first two carry bInterfaceSubClass = BOOT and are the reason for the
 * hand-written report descriptors below rather than TinyUSB's macros: a BIOS in
 * boot protocol does not read a report descriptor at all - it reads the
 * interface's subclass and protocol bytes and then assumes the fixed layout -
 * and TinyUSB's mouse macro adds an AC Pan byte that makes the report five
 * bytes long, which is a report no boot-protocol reader expects.  Descriptors
 * that spell out exactly the bytes the kernel above promises are worth the
 * sixty lines.
 *
 * The third has no boot subclass and never will: absolute positioning is a
 * report-descriptor idea, so a BIOS ignores this interface entirely, and a full
 * operating system uses it to put the cursor where a finger touched a picture of
 * the screen - which no number of relative steps can reliably do.
 *
 * TinyUSB arrives through the component manager and only when this is enabled
 * (main/idf_component.yml gates it on the same Kconfig), so a default image
 * neither downloads nor carries any of it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdkconfig.h"

#if defined(CONFIG_ARGON_USB_DEVICE) && CONFIG_ARGON_USB_DEVICE

#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED

#include <string.h>

#include <argon/port/usbdev.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"

static const char *TAG = "usbdev";

/* Instance numbers, in the order the configuration descriptor lists them. */
#define ITF_KBD 0
#define ITF_MOUSE 1
#define ITF_ABS 2

#if AG_PORT_HAS_USB_CDC
/*
 * The serial pipe takes two interfaces - a control one and a data one - which
 * is why a composite device with CDC in it needs an Interface Association
 * Descriptor and a device class of MISC/COMMON/IAD below: without that, a host
 * has no way to know the two belong together.
 */
#define ITF_CDC 3
#define ITF_COUNT 5
#else
#define ITF_COUNT 3
#endif

/* ------------------------------------------------------------------------ */
/* Report descriptors                                                       */
/* ------------------------------------------------------------------------ */

/*
 * The boot keyboard, HID 1.11 Appendix B.1 with one deliberate change: the key
 * slots range over 0..255 rather than the appendix's 0..101.  The appendix
 * predates every key past the original hundred-and-one, and a keyboard that
 * cannot say F13 or a media key is a keyboard with holes in it.  Nothing is
 * lost by widening it: a host in boot protocol ignores this descriptor
 * altogether, and one in report protocol reads what is written here.
 */
static const uint8_t k_desc_kbd[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop)                        */
    0x09, 0x06,       /* Usage (Keyboard)                                    */
    0xA1, 0x01,       /* Collection (Application)                            */
    0x05, 0x07,       /*   Usage Page (Keyboard/Keypad)                      */
    0x19, 0xE0,       /*   Usage Minimum (Left Control)                      */
    0x29, 0xE7,       /*   Usage Maximum (Right GUI)                         */
    0x15, 0x00,       /*   Logical Minimum (0)                               */
    0x25, 0x01,       /*   Logical Maximum (1)                               */
    0x75, 0x01,       /*   Report Size (1)                                   */
    0x95, 0x08,       /*   Report Count (8)                                  */
    0x81, 0x02,       /*   Input (Data,Var,Abs)   - byte 0, the modifiers    */
    0x95, 0x01,       /*   Report Count (1)                                  */
    0x75, 0x08,       /*   Report Size (8)                                   */
    0x81, 0x01,       /*   Input (Cnst)           - byte 1, reserved         */
    0x95, 0x05,       /*   Report Count (5)                                  */
    0x75, 0x01,       /*   Report Size (1)                                   */
    0x05, 0x08,       /*   Usage Page (LEDs)                                 */
    0x19, 0x01,       /*   Usage Minimum (Num Lock)                          */
    0x29, 0x05,       /*   Usage Maximum (Kana)                              */
    0x91, 0x02,       /*   Output (Data,Var,Abs)  - the host's lock lamps    */
    0x95, 0x01,       /*   Report Count (1)                                  */
    0x75, 0x03,       /*   Report Size (3)                                   */
    0x91, 0x01,       /*   Output (Cnst)          - padding to one byte      */
    0x95, 0x06,       /*   Report Count (6)                                  */
    0x75, 0x08,       /*   Report Size (8)                                   */
    0x15, 0x00,       /*   Logical Minimum (0)                               */
    0x26, 0xFF, 0x00, /*   Logical Maximum (255)                             */
    0x05, 0x07,       /*   Usage Page (Keyboard/Keypad)                      */
    0x19, 0x00,       /*   Usage Minimum (0)                                 */
    0x2A, 0xFF, 0x00, /*   Usage Maximum (255)                               */
    0x81, 0x00,       /*   Input (Data,Ary)       - bytes 2..7, six slots    */
    0xC0,             /* End Collection                                      */
};

/*
 * The boot mouse: buttons, then three signed bytes.  A BIOS reads the first
 * three and stops, which is exactly what the boot protocol promises; a full
 * operating system reads this descriptor and gets the wheel as well.
 */
static const uint8_t k_desc_mouse[] = {
    0x05, 0x01, /* Usage Page (Generic Desktop)                              */
    0x09, 0x02, /* Usage (Mouse)                                             */
    0xA1, 0x01, /* Collection (Application)                                  */
    0x09, 0x01, /*   Usage (Pointer)                                        */
    0xA1, 0x00, /*   Collection (Physical)                                  */
    0x05, 0x09, /*     Usage Page (Button)                                  */
    0x19, 0x01, /*     Usage Minimum (Button 1)                             */
    0x29, 0x03, /*     Usage Maximum (Button 3)                             */
    0x15, 0x00, /*     Logical Minimum (0)                                  */
    0x25, 0x01, /*     Logical Maximum (1)                                  */
    0x95, 0x03, /*     Report Count (3)                                     */
    0x75, 0x01, /*     Report Size (1)                                      */
    0x81, 0x02, /*     Input (Data,Var,Abs) - three buttons                 */
    0x95, 0x01, /*     Report Count (1)                                     */
    0x75, 0x05, /*     Report Size (5)                                      */
    0x81, 0x03, /*     Input (Cnst,Var,Abs) - padding to one byte           */
    0x05, 0x01, /*     Usage Page (Generic Desktop)                         */
    0x09, 0x30, /*     Usage (X)                                            */
    0x09, 0x31, /*     Usage (Y)                                            */
    0x09, 0x38, /*     Usage (Wheel)                                        */
    0x15, 0x81, /*     Logical Minimum (-127)                               */
    0x25, 0x7F, /*     Logical Maximum (127)                                */
    0x75, 0x08, /*     Report Size (8)                                      */
    0x95, 0x03, /*     Report Count (3)                                     */
    0x81, 0x06, /*     Input (Data,Var,Rel) - dx, dy, wheel                 */
    0xC0,       /*   End Collection                                          */
    0xC0,       /* End Collection                                            */
};

/*
 * The absolute pointer.  Same shape, but X and Y are sixteen bits and absolute:
 * a position on the far machine's screen, 0..32767 across it, whatever that
 * screen turns out to be.  The wheel stays relative because a wheel has no
 * position.
 */
static const uint8_t k_desc_abs[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop)                        */
    0x09, 0x02,       /* Usage (Mouse)                                       */
    0xA1, 0x01,       /* Collection (Application)                            */
    0x09, 0x01,       /*   Usage (Pointer)                                  */
    0xA1, 0x00,       /*   Collection (Physical)                            */
    0x05, 0x09,       /*     Usage Page (Button)                            */
    0x19, 0x01,       /*     Usage Minimum (Button 1)                       */
    0x29, 0x03,       /*     Usage Maximum (Button 3)                       */
    0x15, 0x00,       /*     Logical Minimum (0)                            */
    0x25, 0x01,       /*     Logical Maximum (1)                            */
    0x95, 0x03,       /*     Report Count (3)                               */
    0x75, 0x01,       /*     Report Size (1)                                */
    0x81, 0x02,       /*     Input (Data,Var,Abs) - three buttons           */
    0x95, 0x01,       /*     Report Count (1)                               */
    0x75, 0x05,       /*     Report Size (5)                                */
    0x81, 0x03,       /*     Input (Cnst,Var,Abs) - padding                 */
    0x05, 0x01,       /*     Usage Page (Generic Desktop)                   */
    0x09, 0x30,       /*     Usage (X)                                      */
    0x09, 0x31,       /*     Usage (Y)                                      */
    0x16, 0x00, 0x00, /*     Logical Minimum (0)                            */
    0x26, 0xFF, 0x7F, /*     Logical Maximum (32767)                        */
    0x75, 0x10,       /*     Report Size (16)                               */
    0x95, 0x02,       /*     Report Count (2)                               */
    0x81, 0x02,       /*     Input (Data,Var,Abs) - x, y                    */
    0x09, 0x38,       /*     Usage (Wheel)                                  */
    0x15, 0x81,       /*     Logical Minimum (-127)                         */
    0x25, 0x7F,       /*     Logical Maximum (127)                          */
    0x75, 0x08,       /*     Report Size (8)                                */
    0x95, 0x01,       /*     Report Count (1)                               */
    0x81, 0x06,       /*     Input (Data,Var,Rel) - wheel                   */
    0xC0,             /*   End Collection                                    */
    0xC0,             /* End Collection                                      */
};

/* ------------------------------------------------------------------------ */
/* Device and configuration descriptors                                     */
/* ------------------------------------------------------------------------ */

/*
 * Espressif's vendor ID with a product ID from the range they hand out for
 * TinyUSB devices on their silicon.  Correct for a board on a desk and wrong
 * for anything sold: a product needs its own pair, and the place to change it
 * is here.
 */
#define ARGON_USB_VID 0x303A
#define ARGON_USB_PID 0x4020

static const tusb_desc_device_t k_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
#if AG_PORT_HAS_USB_CDC
    /*
     * With CDC in the mix the device has to say MISC/COMMON/IAD, because the
     * serial port is two interfaces that belong together and the association
     * descriptor is the only thing that says so.  Anything else and Windows
     * binds a driver to the control interface and leaves the data one alone.
     */
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
#else
    /*
     * Class in the interfaces, not here.  A composite device that names a class
     * at the device level tells the host to look for one driver for the whole
     * thing; zero means "read the interfaces", which is what a keyboard and two
     * mice want.
     */
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
#endif
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = ARGON_USB_VID,
    .idProduct = ARGON_USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

/*
 * Index 0 is the language, as a two-byte blob rather than a string - that is
 * what the descriptor is, and esp_tinyusb copies those two bytes straight out.
 */
static const char *k_desc_str[] = {
    (const char[]){0x09, 0x04}, /* 0: English (United States)                */
    "ArgonOS",                  /* 1: manufacturer                            */
    "ArgonOS HID",              /* 2: product                                 */
    "0001",                     /* 3: serial                                  */
    "Keyboard",                 /* 4: interface 0                             */
    "Mouse",                    /* 5: interface 1                             */
    "Tablet",                   /* 6: interface 2                             */
#if AG_PORT_HAS_USB_CDC
    "ArgonOS screen link",      /* 7: the serial pipe                         */
#endif
};

/*
 * The endpoint budget, which is the tightest thing in this file and the reason
 * the composite device stops where it does.
 *
 * The S3's OTG core is built with five device IN endpoints besides EP0
 * (soc/usb_dwc_cfg.h: OTG_NUM_IN_EPS 5) and one kilobyte of FIFO shared
 * between the receive path and every transmit endpoint (OTG_DFIFO_DEPTH 256
 * words).  Three HID interrupt endpoints plus the serial pipe's notification
 * and bulk IN is exactly five.  There is no sixth, which is why the flash
 * drive that would carry the agent to the far machine is not here: adding mass
 * storage means giving up an interface, and the one to give up is the relative
 * mouse - see docs/14-usb-kvm.md.
 *
 * The FIFO, in words: 52 receive, 16 for EP0, 16 for each of the three HID
 * endpoints, 16 for the notification, and what is left for the bulk IN, which
 * is the one that carries the picture and the only one throughput depends on.
 */
#if AG_PORT_HAS_USB_CDC
#define CFG_TOTAL_LEN                                                            (TUD_CONFIG_DESC_LEN + 3 * TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN)
#else
#define CFG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + 3 * TUD_HID_DESC_LEN)
#endif

/* One interrupt IN endpoint each, polled every millisecond - the fastest a
 * full-speed interrupt endpoint is allowed to be asked, and what every keyboard
 * on a desk asks for. */
#define EP_KBD 0x81
#define EP_MOUSE 0x82
#define EP_ABS 0x83
#define EP_SIZE 16
#define EP_INTERVAL_MS 1

/* The serial pipe: a notification endpoint nobody uses (CDC insists on one)
 * and a bulk pair.  Sixty-four bytes is the largest a full-speed bulk endpoint
 * may be, and the whole picture goes through it. */
#define EP_CDC_NOTIF 0x84
#define EP_CDC_OUT 0x05
#define EP_CDC_IN 0x85
#define EP_CDC_NOTIF_SIZE 8
#define EP_CDC_SIZE 64

static const uint8_t k_desc_cfg[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CFG_TOTAL_LEN, 0x00, 100),

    TUD_HID_DESCRIPTOR(ITF_KBD, 4, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(k_desc_kbd), EP_KBD, EP_SIZE, EP_INTERVAL_MS),
    TUD_HID_DESCRIPTOR(ITF_MOUSE, 5, HID_ITF_PROTOCOL_MOUSE,
                       sizeof(k_desc_mouse), EP_MOUSE, EP_SIZE, EP_INTERVAL_MS),
    TUD_HID_DESCRIPTOR(ITF_ABS, 6, HID_ITF_PROTOCOL_NONE, sizeof(k_desc_abs),
                       EP_ABS, EP_SIZE, EP_INTERVAL_MS),
#if AG_PORT_HAS_USB_CDC
    TUD_CDC_DESCRIPTOR(ITF_CDC, 7, EP_CDC_NOTIF, EP_CDC_NOTIF_SIZE, EP_CDC_OUT,
                       EP_CDC_IN, EP_CDC_SIZE),
#endif
};

/* ------------------------------------------------------------------------ */
/* State                                                                    */
/* ------------------------------------------------------------------------ */

static volatile bool     s_running;
static volatile uint32_t s_sent;
static volatile uint32_t s_dropped;
static volatile uint8_t  s_leds;

/*
 * One sender at a time.  TinyUSB's device stack is not re-entrant, and while
 * its own task runs tud_task() we are queueing into an endpoint from whichever
 * task holds /dev/usbhid0.  The mutex serialises our side of that; the residual
 * race with tud_task is the one every TinyUSB application has, and it is
 * narrowed here by only ever calling tud_hid_n_report after tud_hid_n_ready has
 * said the endpoint is idle.
 */
static SemaphoreHandle_t s_lock;

/* ------------------------------------------------------------------------ */
/* TinyUSB HID callbacks                                                    */
/* ------------------------------------------------------------------------ */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    switch (instance) {
    case ITF_KBD:
        return k_desc_kbd;
    case ITF_MOUSE:
        return k_desc_mouse;
    case ITF_ABS:
        return k_desc_abs;
    default:
        return NULL;
    }
}

/*
 * GET_REPORT on the control endpoint.  Nothing here has state a host can read
 * back - a keystroke is an event, not a value - so answer zero, which tells
 * TinyUSB to stall.  Windows asks once at enumeration and does not mind.
 */
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

/*
 * SET_REPORT: the keyboard's lock lamps, which is the only thing a host ever
 * sends this device.  Worth keeping rather than discarding - it is the one
 * signal that says the far machine is listening, and Caps Lock in particular
 * decides whether the letters we send arrive in the case we meant.
 */
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)report_id;
    if (instance == ITF_KBD && report_type == HID_REPORT_TYPE_OUTPUT &&
        bufsize >= 1) {
        s_leds = buffer[0];
    }
}

/* ------------------------------------------------------------------------ */
/* The port contract                                                        */
/* ------------------------------------------------------------------------ */

ag_err_t ag_port_usbdev_start(void)
{
    if (s_running) {
        return AG_OK;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return -AG_ENOMEM;
        }
    }

    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    cfg.descriptor.device = &k_desc_device;
    cfg.descriptor.string = k_desc_str;
    cfg.descriptor.string_count = (int)(sizeof(k_desc_str) / sizeof(k_desc_str[0]));
    cfg.descriptor.full_speed_config = k_desc_cfg;

    const esp_err_t err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install: %s", esp_err_to_name(err));
        return -AG_EIO;
    }

    s_sent = 0;
    s_dropped = 0;
    s_leds = 0;
    s_running = true;
    ESP_LOGI(TAG, "attached as a HID keyboard and mouse (%04x:%04x)",
             ARGON_USB_VID, ARGON_USB_PID);
    return AG_OK;
}

ag_err_t ag_port_usbdev_stop(void)
{
    if (!s_running) {
        return AG_OK;
    }
    s_running = false;
    const esp_err_t err = tinyusb_driver_uninstall();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tinyusb_driver_uninstall: %s", esp_err_to_name(err));
        return -AG_EIO;
    }
    ESP_LOGI(TAG, "detached");
    return AG_OK;
}

ag_err_t ag_port_usbdev_status(ag_port_usbdev_status_t *out)
{
    if (out == NULL) {
        return -AG_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    if (!s_running) {
        out->state = AG_USBHID_OFF;
        return AG_OK;
    }
    out->state = tud_mounted() ? AG_USBHID_READY : AG_USBHID_ATTACHED;
    out->sent = s_sent;
    out->dropped = s_dropped;
    out->leds = s_leds;
    out->protocol = (uint8_t)(tud_hid_n_get_protocol(ITF_KBD) == HID_PROTOCOL_BOOT
                                  ? 0u
                                  : 1u);
    return AG_OK;
}

ag_err_t ag_port_usbdev_send(ag_usbhid_kind_t kind, const uint8_t *rep,
                             uint32_t len)
{
    uint8_t instance;
    uint32_t want;

    switch (kind) {
    case AG_USBHID_KBD:
        instance = ITF_KBD;
        want = AG_USBHID_KBD_LEN;
        break;
    case AG_USBHID_MOUSE:
        instance = ITF_MOUSE;
        want = AG_USBHID_MOUSE_LEN;
        break;
    case AG_USBHID_ABS:
        instance = ITF_ABS;
        want = AG_USBHID_ABS_LEN;
        break;
    default:
        return -AG_EINVAL;
    }
    if (rep == NULL || len != want) {
        return -AG_EINVAL;
    }
    if (!s_running || !tud_mounted()) {
        return -AG_ENODEV;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        s_dropped++;
        return -AG_EAGAIN;
    }

    ag_err_t rc;
    if (!tud_hid_n_ready(instance)) {
        /*
         * The previous report has not left yet.  Saying so is the honest
         * answer: this layer must not coalesce, because it cannot tell a
         * mouse step (which may be merged with the next) from a key release
         * (which may not).
         */
        s_dropped++;
        rc = -AG_EAGAIN;
    } else if (!tud_hid_n_report(instance, 0, rep, (uint16_t)len)) {
        s_dropped++;
        rc = -AG_EIO;
    } else {
        s_sent++;
        rc = AG_OK;
    }
    xSemaphoreGive(s_lock);
    return rc;
}

/* ------------------------------------------------------------------------ */
/* The serial pipe                                                          */
/* ------------------------------------------------------------------------ */

#if AG_PORT_HAS_USB_CDC

static volatile uint32_t s_cdc_rx;
static volatile uint32_t s_cdc_tx;

int32_t ag_port_usbdev_cdc_read(void *buf, uint32_t len)
{
    if (buf == NULL || len == 0) {
        return -AG_EINVAL;
    }
    if (!s_running || !tud_mounted()) {
        return -AG_ENODEV;
    }

    const uint32_t have = tud_cdc_available();
    if (have == 0) {
        return -AG_EAGAIN;
    }

    const uint32_t take = (have < len) ? have : len;
    const uint32_t got = tud_cdc_read(buf, take);
    s_cdc_rx += got;
    return (int32_t)got;
}

int32_t ag_port_usbdev_cdc_write(const void *buf, uint32_t len)
{
    if (buf == NULL || len == 0) {
        return -AG_EINVAL;
    }
    if (!s_running || !tud_mounted()) {
        return -AG_ENODEV;
    }
    if (!tud_cdc_connected()) {
        /*
         * Enumerated, but nobody over there has opened the port.  Writing into
         * a CDC buffer that nothing drains fills it and then blocks every
         * later write, so this refuses instead - the caller sees the state in
         * the status and can stop offering.
         */
        return -AG_ENODEV;
    }

    const uint32_t room = tud_cdc_write_available();
    if (room == 0) {
        return -AG_EAGAIN;
    }

    const uint32_t take = (room < len) ? room : len;
    const uint32_t put = tud_cdc_write(buf, take);
    (void)tud_cdc_write_flush();
    s_cdc_tx += put;
    return (int32_t)put;
}

ag_err_t ag_port_usbdev_cdc_status(ag_port_usbdev_cdc_status_t *out)
{
    if (out == NULL) {
        return -AG_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    if (!s_running) {
        out->state = AG_USBCDC_OFF;
        return AG_OK;
    }
    if (!tud_mounted()) {
        out->state = AG_USBCDC_OFF;
    } else {
        out->state = tud_cdc_connected() ? AG_USBCDC_OPEN : AG_USBCDC_IDLE;
    }
    out->rx = s_cdc_rx;
    out->tx = s_cdc_tx;
    return AG_OK;
}

#endif /* AG_PORT_HAS_USB_CDC */

#endif /* SOC_USB_OTG_SUPPORTED */
#endif /* CONFIG_ARGON_USB_DEVICE */
