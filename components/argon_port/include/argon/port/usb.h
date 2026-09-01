/*
 * ArgonOS port contract - USB host, for the things a person types on.
 *
 * The mirror of bt.h for a chip that does have USB: where BLE is how a board
 * with no USB attaches a keyboard, this is how a board with USB does.  The
 * shape is deliberately the same - find nothing to find, a device arrives on
 * its own when plugged in, and what comes out is HID reports - so the kernel
 * turns a USB keyboard into console events with the same code it uses for a
 * Bluetooth one (src/dev/hidkbd.c).
 *
 * Scope is one thing on purpose: a HID keyboard in boot protocol.  A boot
 * keyboard sends a fixed eight-byte report - one modifier byte, one reserved,
 * six key slots - and the port asks for that protocol rather than reading a
 * report descriptor, which is most of what a general HID stack is and none of
 * what a keyboard needs.  A mouse is the same report path once there is
 * something on the screen to point at.
 *
 * What a port must supply, when AG_PORT_HAS_USB_HID is 1:
 *
 *   ag_err_t ag_port_usb_start(void)
 *   ag_err_t ag_port_usb_stop(void)
 *   ag_err_t ag_port_usb_status(ag_port_usb_status_t *out)
 *   void     ag_port_usb_on_report(ag_port_usb_report_fn fn)
 *
 * Contract, not advice:
 *
 * - There is no scan and no open: USB tells the host when a device is plugged
 *   in, and the port attaches to a HID keyboard by itself.  start() brings up
 *   the host controller and a task to run it; a keyboard connects and
 *   disconnects under it without anything above being asked.
 * - Reports arrive on the port's own task, one call per report, and the
 *   callback must be short: what the kernel does with it is put events in a
 *   queue.  A report is the raw boot payload - deciding that byte 2 is the
 *   first keycode is the business of whoever knows the report layout, which for
 *   a boot keyboard is a fixed fact the parser holds, not the port.
 * - The board powers the device.  Host mode sources 5 V on the OTG port's VBUS;
 *   a keyboard that draws more than the port can give will not enumerate, and
 *   that is a wiring fact, not a fault the port hides.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_USB_H
#define ARGON_PORT_USB_H

#include <stdbool.h>
#include <stdint.h>

#include <argon/abi.h>

#include <argon/port/impl/usb.h>

#define AG_USB_NAME_MAX 31

typedef enum {
    AG_USB_OFF = 0,
    AG_USB_IDLE,   /* host is up, nothing attached                           */
    AG_USB_OPEN,   /* a keyboard is attached and reports are arriving         */
} ag_usb_state_t;

/* The port's classification of the attached device, so the kernel can route a
 * report without parsing it. */
typedef enum {
    AG_USB_USAGE_OTHER = 0,
    AG_USB_USAGE_KEYBOARD,
    AG_USB_USAGE_MOUSE,
} ag_usb_usage_t;

typedef struct {
    ag_usb_state_t state;
    char           name[AG_USB_NAME_MAX + 1]; /* iProduct if the device gives one */
    uint16_t       vid;
    uint16_t       pid;
    uint32_t       reports; /* how many have arrived since it attached         */
} ag_port_usb_status_t;

/*
 * One HID input report.  `usage` is the port's classification (keyboard, mouse,
 * other); `report_id` is 0 for a boot device, kept for the callback's shape to
 * match the Bluetooth one so the kernel sink is identical.
 */
typedef void (*ag_port_usb_report_fn)(ag_usb_usage_t usage, uint8_t report_id,
                                      const uint8_t *data, uint32_t len);

#if AG_PORT_HAS_USB_HID

ag_err_t ag_port_usb_start(void);
ag_err_t ag_port_usb_stop(void);
ag_err_t ag_port_usb_status(ag_port_usb_status_t *out);
void     ag_port_usb_on_report(ag_port_usb_report_fn fn);

#endif /* AG_PORT_HAS_USB_HID */

#endif /* ARGON_PORT_USB_H */
