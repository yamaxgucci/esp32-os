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

#if AG_PORT_HAS_USB_MSC
#include "esp_partition.h"
#include "tinyusb_msc.h"
#include "wear_levelling.h"
#endif

static const char *TAG = "usbdev";

/*
 * Instance numbers, in the order the configuration descriptor lists them.
 *
 * There are two HID interfaces and not three.  The relative boot mouse that
 * used to sit between them is gone, and it is worth writing down why, because
 * it is the constraint the whole shape of this device is cut to: the S3's OTG
 * core has five device IN endpoints and the control endpoint EP0 is one of
 * them, so only FOUR are left for functions (soc/usb_dwc_cfg.h, OTG_NUM_IN_EPS
 * 5).  The keyboard, the pointer, the screen pipe and mass storage are four.
 * A fifth function IN endpoint does not exist to be had.
 *
 * The pipe is a single vendor (WinUSB) interface, not CDC: CDC cost two IN
 * endpoints (a notification nobody reads plus the bulk) and that second one is
 * exactly what the flash drive needed.  A vendor interface is one bulk IN, so
 * keyboard + pointer + pipe + drive fits the four.  What was given up to get
 * there earlier - a mouse in a BIOS, few have one and fewer use it - is not
 * given up again: a relative step becomes a position one layer up
 * (apps/common/kvm), which is what a phone's trackpad wanted anyway.
 *
 * Windows binds its WinUSB driver to the vendor interface on its own, with no
 * COM port and no driver to install, because the device advertises a Microsoft
 * OS 2.0 descriptor set through the BOS (see desc_bos / desc_ms_os_20 below).
 */
#define ITF_KBD 0
#define ITF_ABS 1

#if AG_PORT_HAS_USB_CDC
/*
 * The screen pipe: one vendor (WinUSB) interface.  A single interface needs no
 * Interface Association Descriptor, which is why the device class below is zero
 * rather than MISC/COMMON/IAD - that was only ever for CDC's two interfaces.
 */
#define ITF_VENDOR 2
#define ITF_AFTER_PIPE 3
#else
#define ITF_AFTER_PIPE 2
#endif

#if AG_PORT_HAS_USB_MSC
#define ITF_MSC ITF_AFTER_PIPE
#define ITF_COUNT (ITF_AFTER_PIPE + 1)
#else
#define ITF_COUNT ITF_AFTER_PIPE
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
 * The absolute pointer.  Same shape, but X and Y are sixteen bits and absolute:
 * a position on the far machine's screen, 0..32767 across it, whatever that
 * screen turns out to be.  The wheel stays relative because a wheel has no
 * position.
 */
static const uint8_t k_desc_abs[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop)                        */
    0x09, 0x02,       /* Usage (Mouse)                                       */
    0xA1, 0x01,       /* Collection (Application)                            */
    0x85, 0x01,       /*   Report ID (1) - the pointer                      */
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

    /*
     * The pipe again, as HID: the one class every host since Windows 98 SE,
     * Linux and macOS drives with no driver and no administrator.  The agent
     * uses it where WinUSB is not there (Windows before 8.1, Linux, macOS).
     * Report 2 carries pipe bytes either way, [len][up to 62 bytes]; report 3,
     * in only, is flow control - how many bytes the board has taken so far -
     * because an interrupt OUT endpoint cannot be held off the way bulk is.
     * It shares this interface's endpoints because the S3 has no IN endpoint
     * left to give it one of its own.
     */
    0x06, 0x00, 0xFF, /* Usage Page (Vendor 0xFF00)                          */
    0x09, 0x01,       /* Usage (1)                                           */
    0xA1, 0x01,       /* Collection (Application)                            */
    0x15, 0x00,       /*   Logical Minimum (0)                              */
    0x26, 0xFF, 0x00, /*   Logical Maximum (255)                            */
    0x75, 0x08,       /*   Report Size (8)                                  */
    0x85, 0x02,       /*   Report ID (2) - pipe bytes                       */
    0x95, 0x3F,       /*   Report Count (63)                                */
    0x09, 0x02,       /*   Usage (2)                                        */
    0x81, 0x02,       /*   Input (Data,Var,Abs)                             */
    0x09, 0x03,       /*   Usage (3)                                        */
    0x91, 0x02,       /*   Output (Data,Var,Abs)                            */
    0x85, 0x03,       /*   Report ID (3) - credit                           */
    0x95, 0x3F,       /*   Report Count (63)                                */
    0x09, 0x04,       /*   Usage (4)                                        */
    0x81, 0x02,       /*   Input (Data,Var,Abs)                             */
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
/* 0x4021, bumped from 0x4020 when the pipe went from CDC to WinUSB: Windows
 * caches the driver it bound to a VID/PID, so a machine that once saw this pair
 * as a COM port would keep trying usbser on the new descriptors.  A fresh PID is
 * a fresh device to Windows, so it re-reads the BOS and binds WinUSB.  The agent
 * finds the board by its WinUSB interface GUID, not this pair, so it is
 * unaffected. */
#define ARGON_USB_PID 0x4021

static const tusb_desc_device_t k_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    /*
     * 2.10, not 2.00, so the host asks for the BOS descriptor - which is where
     * the Microsoft OS 2.0 set lives that makes Windows bind WinUSB to the
     * vendor pipe with no driver install.  A 2.00 device is never asked.
     */
    .bcdUSB = 0x0210,
    /*
     * Class in the interfaces, not here.  Every interface this device publishes
     * - two HID, one vendor, one MSC - is a single-interface function, so none
     * needs an Interface Association Descriptor and the device names no class:
     * zero means "read the interfaces", which is what they all want.  (CDC was
     * the one exception, two interfaces that needed MISC/COMMON/IAD to be tied
     * together; it is gone, and so is the IAD.)
     */
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
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
    "Pointer",                  /* 5: interface 1                             */
#if AG_PORT_HAS_USB_CDC
    "ArgonOS screen link",      /* 6: the vendor (WinUSB) pipe                 */
#endif
#if AG_PORT_HAS_USB_MSC
    "ArgonOS disk",             /* 7: the drive the agent lives on            */
#endif
};

#define STR_KBD 4
#define STR_ABS 5
#define STR_VENDOR 6
#define STR_MSC 7

/*
 * The endpoint budget, which is the tightest thing in this file and the reason
 * the composite device stops where it does.
 *
 * The S3's OTG core is built with five device IN endpoints (soc/usb_dwc_cfg.h:
 * OTG_NUM_IN_EPS 5) and one kilobyte of FIFO shared between the receive path
 * and every transmit endpoint (OTG_DFIFO_DEPTH 256 words).  The control
 * endpoint EP0 claims one of the five, so FOUR IN endpoints are left for
 * functions.  The keyboard, the pointer, the vendor pipe's bulk IN and mass
 * storage's bulk IN are exactly four - no fifth function IN exists.
 *
 * This fits where CDC did not: CDC wanted two IN endpoints, a notification it
 * never used plus the bulk, so keyboard + pointer + CDC alone took four and
 * the drive had no endpoint (it asserted dfifo_alloc the moment a host opened
 * it).  A vendor interface is one bulk IN, which hands the fourth back to the
 * drive - see docs/14-usb-kvm.md.
 *
 * The FIFO, in words: 52 receive, 16 for EP0, 16 for each of the two HID
 * endpoints, what the vendor bulk IN carries the picture through, and the MSC
 * bulk IN.  The vendor IN is the only one throughput depends on.
 */
#if AG_PORT_HAS_USB_CDC
#define VENDOR_DESC_BYTES TUD_VENDOR_DESC_LEN
#else
#define VENDOR_DESC_BYTES 0
#endif
#if AG_PORT_HAS_USB_MSC
#define MSC_DESC_BYTES TUD_MSC_DESC_LEN
#else
#define MSC_DESC_BYTES 0
#endif

#define CFG_TOTAL_LEN                                                        \
    (TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN +       \
     VENDOR_DESC_BYTES + MSC_DESC_BYTES)

/* One interrupt IN endpoint each, polled every millisecond - the fastest a
 * full-speed interrupt endpoint is allowed to be asked, and what every keyboard
 * on a desk asks for. */
#define EP_KBD 0x81
#define EP_ABS 0x82
#define EP_SIZE 16
#define EP_ABS_SIZE 64  /* report ID + 63: the HID pipe rides this endpoint */
/* No interrupt OUT for the HID pipe: the S3 has six endpoints past EP0 and the
 * keyboard, pointer, vendor pipe and drive use all six (four IN, two OUT).  A
 * seventh is accepted by the descriptor and never answers - the host's first
 * write hangs.  Without one, every host sends output reports as SET_REPORT on
 * EP0, which tud_hid_set_report_cb already takes. */
#define EP_INTERVAL_MS 1

/* The vendor pipe: a single bulk pair, no notification endpoint (that was
 * CDC's, and the whole reason the drive now fits).  Sixty-four bytes is the
 * largest a full-speed bulk endpoint may be, and the whole picture goes
 * through the IN side of it. */
#define EP_VENDOR_OUT 0x03
#define EP_VENDOR_IN 0x83
#define EP_VENDOR_SIZE 64

/* The drive.  The fourth and last function IN endpoint there is. */
#define EP_MSC_OUT 0x04
#define EP_MSC_IN 0x84
#define EP_MSC_SIZE 64

static const uint8_t k_desc_cfg[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CFG_TOTAL_LEN, 0x00, 100),

    TUD_HID_DESCRIPTOR(ITF_KBD, STR_KBD, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(k_desc_kbd), EP_KBD, EP_SIZE, EP_INTERVAL_MS),
    TUD_HID_DESCRIPTOR(ITF_ABS, STR_ABS, HID_ITF_PROTOCOL_NONE,
                       sizeof(k_desc_abs), EP_ABS, EP_ABS_SIZE, EP_INTERVAL_MS),
#if AG_PORT_HAS_USB_CDC
    TUD_VENDOR_DESCRIPTOR(ITF_VENDOR, STR_VENDOR, EP_VENDOR_OUT, EP_VENDOR_IN,
                          EP_VENDOR_SIZE),
#endif
#if AG_PORT_HAS_USB_MSC
    TUD_MSC_DESCRIPTOR(ITF_MSC, STR_MSC, EP_MSC_OUT, EP_MSC_IN, EP_MSC_SIZE),
#endif
};

/* ------------------------------------------------------------------------ */
/* BOS and the Microsoft OS 2.0 descriptor set                              */
/* ------------------------------------------------------------------------ */

#if AG_PORT_HAS_USB_CDC

/*
 * This is what makes the vendor pipe bind WinUSB on Windows with nothing
 * installed on the far machine, and it is the whole reason the device claims
 * USB 2.10 up top.  A 2.1 host asks for the Binary Object Store (BOS); the BOS
 * here carries one platform capability, Microsoft OS 2.0, which names a vendor
 * request code and a descriptor-set length.  Windows then issues that vendor
 * request, reads the set below, sees the WINUSB compatible-ID on our vendor
 * interface and a DeviceInterfaceGUID to register it under, and loads winusb.sys
 * by itself.  No .inf, no COM port, no driver download.
 *
 * Adapted from managed_components/espressif__tinyusb/examples/device/
 * webusb_serial/src/usb_descriptors.c, with two differences: there is no WebUSB
 * capability (that example had two device caps and a landing-page URL; we have
 * one cap and no URL), and the one vendor request we answer is the Microsoft
 * one.  The GUID is our own.
 *
 * How the host actually reaches this: esp_tinyusb installs strong
 * tud_descriptor_device_cb and tud_descriptor_configuration_cb that read the
 * tinyusb_config_t we pass in ag_port_usbdev_start, but it supplies NO BOS of
 * its own - grep espressif__esp_tinyusb for "bos" finds nothing, and
 * tinyusb_config_t has no BOS field.  TinyUSB's own tud_descriptor_bos_cb is a
 * TU_ATTR_WEAK stub returning NULL (src/device/usbd.c).  So the strong
 * definition below is the one the linker takes, and it is reached the moment
 * the host requests the BOS.  Nothing else needs wiring.
 */
#define VENDOR_REQUEST_MICROSOFT 0x01

#define BOS_TOTAL_LEN (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

/*
 * The length of the whole MS OS 2.0 set below.  Unchanged from the reference's
 * 0xB2 because our DeviceInterfaceGUID is a standard 8-4-4-4-12 GUID in braces,
 * the same 38 UTF-16 characters the reference's GUID was, so the registry
 * property's length fields are identical.  The TU_VERIFY_STATIC under the array
 * is the guard that keeps this honest.
 */
#define MS_OS_20_DESC_LEN 0xB2

static const uint8_t desc_bos[] = {
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, VENDOR_REQUEST_MICROSOFT),
};

uint8_t const *tud_descriptor_bos_cb(void)
{
    return desc_bos;
}

/*
 * The set itself.  Header, then a configuration subset, then a function subset
 * whose "first interface" is the vendor interface (ITF_VENDOR), then the WINUSB
 * compatible ID and a registry-property descriptor that writes our
 * DeviceInterfaceGUID so the interface can be opened by name.
 *
 * The GUID is {A7B3F1C2-9D4E-4F8A-B6C1-2E3D4A5B6C7D}, stored UTF-16LE with a
 * trailing null, 80 bytes (0x50) like the reference's.
 */
static const uint8_t desc_ms_os_20[] = {
    /* Set header: length, type, windows version, total length */
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

    /* Configuration subset header: length, type, config index, reserved,
       config total length */
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
    0, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A),

    /* Function subset header: length, type, first interface, reserved,
       subset length - the vendor interface is the one WinUSB binds */
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
    ITF_VENDOR, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08),

    /* Compatible ID descriptor: length, type, compatible ID, sub compatible */
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* sub-compatible */

    /* Registry property descriptor: length, type */
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08 - 0x08 - 0x14),
    U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    /* wPropertyDataType (7 = REG_MULTI_SZ), wPropertyNameLength,
       PropertyName "DeviceInterfaceGUIDs\0" in UTF-16 */
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A),
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00,
    'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00, 'r', 0x00, 'f', 0x00,
    'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00,
    'D', 0x00, 's', 0x00, 0x00, 0x00,
    U16_TO_U8S_LE(0x0050), /* wPropertyDataLength */
    /* bPropertyData: "{A7B3F1C2-9D4E-4F8A-B6C1-2E3D4A5B6C7D}" */
    '{', 0x00, 'A', 0x00, '7', 0x00, 'B', 0x00, '3', 0x00, 'F', 0x00,
    '1', 0x00, 'C', 0x00, '2', 0x00, '-', 0x00,
    '9', 0x00, 'D', 0x00, '4', 0x00, 'E', 0x00, '-', 0x00,
    '4', 0x00, 'F', 0x00, '8', 0x00, 'A', 0x00, '-', 0x00,
    'B', 0x00, '6', 0x00, 'C', 0x00, '1', 0x00, '-', 0x00,
    '2', 0x00, 'E', 0x00, '3', 0x00, 'D', 0x00, '4', 0x00,
    'A', 0x00, '5', 0x00, 'B', 0x00, '6', 0x00, 'C', 0x00,
    '7', 0x00, 'D', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00,
};

TU_VERIFY_STATIC(sizeof(desc_ms_os_20) == MS_OS_20_DESC_LEN, "Incorrect size");

/*
 * The vendor control request.  Only the setup stage matters, and the only
 * request we answer is the Microsoft one with wIndex == 7, which returns the
 * set above (its own header's total length, read from offset 8).  Everything
 * else in a later stage is acknowledged; an unknown request is stalled by
 * returning false.  There is no WebUSB branch - we never advertised one.
 */
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request)
{
    /* Nothing to do on the DATA and ACK stages. */
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }

    switch (request->bmRequestType_bit.type) {
    case TUSB_REQ_TYPE_VENDOR:
        switch (request->bRequest) {
        case VENDOR_REQUEST_MICROSOFT:
            if (request->wIndex == 7) {
                /* The descriptor set's own total length lives at offset 8. */
                uint16_t total_len;
                memcpy(&total_len, desc_ms_os_20 + 8, 2);
                return tud_control_xfer(rhport, request,
                                        (void *)(uintptr_t)desc_ms_os_20,
                                        total_len);
            }
            return false;
        default:
            break;
        }
        break;

    default:
        break;
    }

    /* Stall anything unrecognised. */
    return false;
}

#endif /* AG_PORT_HAS_USB_CDC */

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
#if AG_PORT_HAS_USB_CDC
static void hidpipe_rx(const uint8_t *payload, uint16_t n);
static void hidpipe_pump(void);
#endif

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    if (instance == ITF_KBD && report_type == HID_REPORT_TYPE_OUTPUT &&
        bufsize >= 1) {
        s_leds = buffer[0];
        return;
    }
#if AG_PORT_HAS_USB_CDC
    if (instance == ITF_ABS) {
        /* From the interrupt OUT endpoint TinyUSB hands the raw report, its ID
         * first; from a control SET_REPORT it has already taken the ID off. */
        if (report_type == HID_REPORT_TYPE_INVALID && bufsize >= 1) {
            report_id = buffer[0];
            buffer++;
            bufsize--;
        }
        if (report_id == 2 && bufsize >= 1) {
            hidpipe_rx(buffer, bufsize);
        }
    }
#endif
}

#if AG_PORT_HAS_USB_CDC
/* The endpoint is free again: the next pipe report, if any, can go. */
void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report,
                                uint16_t len)
{
    (void)report;
    (void)len;
    if (instance == ITF_ABS && s_lock != NULL &&
        xSemaphoreTake(s_lock, 0) == pdTRUE) {
        hidpipe_pump(); /* busy lock: whoever holds it pumps on their way out */
        xSemaphoreGive(s_lock);
    }
}
#endif

/* ------------------------------------------------------------------------ */
/* The port contract                                                        */
/* ------------------------------------------------------------------------ */

#if AG_PORT_HAS_USB_MSC

static wl_handle_t s_wl = WL_INVALID_HANDLE;

/*
 * The drive the far machine mounts.  It is a FAT volume in its own flash
 * partition (partitions_kvm.csv, `usbdisk`) and it is deliberately not any of
 * ArgonOS's own filesystems: it belongs to whoever plugs the cable in.
 *
 * What it is for is the one thing that cannot be solved from this end - getting
 * the agent onto a computer that has never seen this device.  Drag KVMAGENT.EXE
 * onto the drive once, from any machine, and from then on the program is on the
 * device rather than being something you have to already have.
 *
 * Ownership is exclusive, and that is mass storage rather than a decision here:
 * a block device with two writers is a corrupted filesystem.  While the far
 * machine has it, this board does not.
 */
static void msc_start(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "usbdisk");
    if (part == NULL) {
        ESP_LOGW(TAG, "no 'usbdisk' partition: the drive will have no media");
        return;
    }

    esp_err_t err = wl_mount(part, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wl_mount(usbdisk): %s", esp_err_to_name(err));
        s_wl = WL_INVALID_HANDLE;
        return;
    }

    const tinyusb_msc_driver_config_t drv = {
        .user_flags = {.val = 0},
        .callback = NULL,
        .callback_arg = NULL,
    };
    err = tinyusb_msc_install_driver(&drv);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_msc_install_driver: %s", esp_err_to_name(err));
        return;
    }

    tinyusb_msc_storage_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.medium.wl_handle = s_wl;
    cfg.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
    /*
     * A blank partition is formatted rather than refused.  The first time a
     * board is flashed the drive has never held a filesystem, and a computer
     * that is offered unformatted media asks whether to format it - which is a
     * question nobody should have to answer about a device that came this way.
     */
    cfg.fat_fs.do_not_format = false;

    err = tinyusb_msc_new_storage_spiflash(&cfg, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usbdisk storage: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "usbdisk: %u KB, the drive the far machine mounts",
             (unsigned)(part->size / 1024u));
}

#endif /* AG_PORT_HAS_USB_MSC */

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

#if AG_PORT_HAS_USB_MSC
    /* Before the device stack: the class driver has to be there when the far
     * machine asks what this is. */
    msc_start();
#endif

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
    ESP_LOGI(TAG, "attached as a keyboard, a pointer%s%s (%04x:%04x)",
             AG_PORT_HAS_USB_CDC ? ", a WinUSB pipe" : "",
             AG_PORT_HAS_USB_MSC ? " and a drive" : "", ARGON_USB_VID,
             ARGON_USB_PID);
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
    case AG_USBHID_ABS:
        instance = ITF_ABS;
        want = AG_USBHID_ABS_LEN;
        break;
    default:
        /* Including 2, which was the relative mouse: a frame built by
         * something older is refused rather than pointed somewhere else. */
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
    } else if (!tud_hid_n_report(instance, instance == ITF_ABS ? 1 : 0, rep,
                                 (uint16_t)len)) {
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

/*
 * The pipe over HID (see the report descriptor).  Two byte rings: what the
 * agent sent (filled from the OUT endpoint, drained by cdc_read) and what the
 * board sends back (filled by cdc_write, drained one report at a time as the IN
 * endpoint frees up).  Whichever transport the agent last wrote on is the one
 * answers go back on.
 *
 * Flow control is credit: an interrupt OUT endpoint is re-armed whether or not
 * there is room, so the agent must not send more than the board has taken plus
 * HIDPIPE_RX_SIZE - margin.  The board reports what it has taken (a running
 * byte count) in report 3, on a change of a quarter of the ring or so.
 */
#define HIDPIPE_RX_SIZE 4096u /* the agent's window is 3/4 of this */
#define HIDPIPE_TX_SIZE 1024u
static uint8_t           s_hrx[HIDPIPE_RX_SIZE];
static uint8_t           s_htx[HIDPIPE_TX_SIZE];
static volatile uint32_t s_hrx_head, s_hrx_tail; /* running counts */
static volatile uint32_t s_htx_head, s_htx_tail;
static volatile uint32_t s_hrx_credit_sent;      /* tail last reported */
static volatile bool     s_hid_mode;              /* answers go over HID */
static volatile uint32_t s_hrx_overrun;
static volatile bool     s_credit_req; /* an agent asked where the counts are */

static void hidpipe_rx(const uint8_t *payload, uint16_t n)
{
    uint32_t len = payload[0];
    if (len > (uint32_t)(n - 1u)) {
        len = (uint32_t)(n - 1u);
    }
    s_hid_mode = true;
    if (len == 0) {
        /* An agent starting up: it needs both counts to know its window. */
        s_credit_req = true;
        if (xSemaphoreTake(s_lock, 0) == pdTRUE) {
            hidpipe_pump();
            xSemaphoreGive(s_lock);
        }
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        if (s_hrx_head - s_hrx_tail >= HIDPIPE_RX_SIZE) {
            s_hrx_overrun++; /* the agent ignored its credit */
            break;
        }
        s_hrx[s_hrx_head % HIDPIPE_RX_SIZE] = payload[1 + i];
        s_hrx_head++;
    }
}

/* Send what is due on the IN endpoint, if it is free: a credit report when the
 * board has taken a fair amount since the last one, else pipe bytes. */
static void hidpipe_pump(void)
{
    if (!s_running || !tud_mounted() || !tud_hid_n_ready(ITF_ABS)) {
        return;
    }
    uint8_t rep[63];
    const uint32_t tail = s_hrx_tail;
    if (s_credit_req || tail - s_hrx_credit_sent >= HIDPIPE_RX_SIZE / 4u ||
        (tail != s_hrx_credit_sent && s_hrx_head == tail)) {
        /* [taken u32][received u32]: the agent's window is received-taken
         * plus what it has sent since, and both counts run from boot. */
        const uint32_t head = s_hrx_head;
        memset(rep, 0, sizeof(rep));
        rep[0] = (uint8_t)tail;
        rep[1] = (uint8_t)(tail >> 8);
        rep[2] = (uint8_t)(tail >> 16);
        rep[3] = (uint8_t)(tail >> 24);
        rep[4] = (uint8_t)head;
        rep[5] = (uint8_t)(head >> 8);
        rep[6] = (uint8_t)(head >> 16);
        rep[7] = (uint8_t)(head >> 24);
        if (tud_hid_n_report(ITF_ABS, 3, rep, sizeof(rep))) {
            s_hrx_credit_sent = tail;
            s_credit_req = false;
        }
        return;
    }
    const uint32_t have = s_htx_head - s_htx_tail;
    if (have == 0) {
        return;
    }
    const uint32_t take = (have < 62u) ? have : 62u;
    memset(rep, 0, sizeof(rep));
    rep[0] = (uint8_t)take;
    for (uint32_t i = 0; i < take; i++) {
        rep[1 + i] = s_htx[(s_htx_tail + i) % HIDPIPE_TX_SIZE];
    }
    if (tud_hid_n_report(ITF_ABS, 2, rep, sizeof(rep))) {
        s_htx_tail += take;
    }
}

int32_t ag_port_usbdev_cdc_read(void *buf, uint32_t len)
{
    if (buf == NULL || len == 0) {
        return -AG_EINVAL;
    }
    if (!s_running || !tud_mounted()) {
        return -AG_ENODEV;
    }

    /* HID first: bytes there mean the agent is on HID right now. */
    const uint32_t hhave = s_hrx_head - s_hrx_tail;
    if (hhave > 0) {
        const uint32_t take = (hhave < len) ? hhave : len;
        uint8_t       *out = (uint8_t *)buf;
        for (uint32_t i = 0; i < take; i++) {
            out[i] = s_hrx[(s_hrx_tail + i) % HIDPIPE_RX_SIZE];
        }
        s_hrx_tail += take;
        s_cdc_rx += take;
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) == pdTRUE) {
            hidpipe_pump(); /* maybe a credit report now */
            xSemaphoreGive(s_lock);
        }
        return (int32_t)take;
    }

    const uint32_t have = tud_vendor_available();
    if (have == 0) {
        if (s_hrx_tail != s_hrx_credit_sent &&
            xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) == pdTRUE) {
            hidpipe_pump(); /* drained: tell the agent it may send again */
            xSemaphoreGive(s_lock);
        }
        return -AG_EAGAIN;
    }

    s_hid_mode = false; /* the agent is on WinUSB */
    const uint32_t take = (have < len) ? have : len;
    const uint32_t got = tud_vendor_read(buf, take);
    s_cdc_rx += got;
    return (int32_t)got;
}

int32_t ag_port_usbdev_cdc_write(const void *buf, uint32_t len)
{
    if (buf == NULL || len == 0) {
        return -AG_EINVAL;
    }
    if (!s_running || !tud_mounted()) {
        /*
         * A vendor interface has no "host has opened the port" signal the way
         * CDC did (CDC's DTR line, tud_cdc_connected); the host either has the
         * WinUSB handle open and drains the bulk IN or it does not, and either
         * way the bulk endpoint's own flow control - tud_vendor_write_available
         * below returning zero - is what keeps a stalled pipe from blocking.
         * So mounted is the only gate here.
         */
        return -AG_ENODEV;
    }

    if (s_hid_mode) {
        const uint32_t room = HIDPIPE_TX_SIZE - (s_htx_head - s_htx_tail);
        if (room == 0) {
            if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) == pdTRUE) {
                hidpipe_pump();
                xSemaphoreGive(s_lock);
            }
            return -AG_EAGAIN;
        }
        const uint8_t *in = (const uint8_t *)buf;
        const uint32_t take = (room < len) ? room : len;
        for (uint32_t i = 0; i < take; i++) {
            s_htx[(s_htx_head + i) % HIDPIPE_TX_SIZE] = in[i];
        }
        s_htx_head += take;
        s_cdc_tx += take;
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) == pdTRUE) {
            hidpipe_pump();
            xSemaphoreGive(s_lock);
        }
        return (int32_t)take;
    }

    const uint32_t room = tud_vendor_write_available();
    if (room == 0) {
        return -AG_EAGAIN;
    }

    const uint32_t take = (room < len) ? room : len;
    const uint32_t put = tud_vendor_write(buf, take);
    (void)tud_vendor_flush();
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
        /*
         * A vendor pipe gives no host-open signal, so being mounted is as much
         * as this layer can tell: report OPEN once enumerated.  (CDC used to
         * distinguish IDLE from OPEN by its DTR line; there is no equivalent to
         * read here, and the pipe works regardless of whether one existed.)
         */
        out->state = AG_USBCDC_OPEN;
    }
    out->rx = s_cdc_rx;
    out->tx = s_cdc_tx;
    return AG_OK;
}

#endif /* AG_PORT_HAS_USB_CDC */

#endif /* SOC_USB_OTG_SUPPORTED */
#endif /* CONFIG_ARGON_USB_DEVICE */
