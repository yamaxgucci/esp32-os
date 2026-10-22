/*
 * ArgonOS port contract - USB device: the keyboard and mouse this board is.
 *
 * The mirror image of usb.h.  There, the OTG peripheral is a host and a
 * keyboard is plugged into the board; here the peripheral is a device and the
 * board is plugged into somebody else's computer, which enumerates it and from
 * then on believes that whatever arrives was typed by a person.
 *
 * The two cannot both be true at once, and that is a fact about the silicon
 * rather than a simplification: the S3 has one OTG controller behind one
 * internal PHY, so a build is a host or a device and the Kconfig makes the two
 * exclusive.  That same PHY is what the USB-Serial-JTAG console uses on a board
 * whose Type-C socket goes straight to the chip, so on such a board this takes
 * the console away - see the Kconfig help.
 *
 * What a port must supply, when AG_PORT_HAS_USB_DEVICE is 1:
 *
 *   ag_err_t ag_port_usbdev_start(void)
 *   ag_err_t ag_port_usbdev_stop(void)
 *   ag_err_t ag_port_usbdev_status(ag_port_usbdev_status_t *out)
 *   ag_err_t ag_port_usbdev_send(ag_usbhid_kind_t kind, const uint8_t *rep,
 *                                uint32_t len)
 *
 * Contract, not advice:
 *
 * - start() attaches: it powers up the peripheral and lets the far machine
 *   enumerate.  Nothing is sent before that machine has configured us, and
 *   send() before then answers -AG_ENODEV rather than pretending.
 * - send() takes one whole report for one interface and is allowed to refuse.
 *   A HID interrupt endpoint holds one report in flight per interval (1 ms
 *   here); a caller that offers a second before the first has gone gets
 *   -AG_EAGAIN, and that is the truth about the wire rather than a queue this
 *   layer invents.  Whoever wants a queue keeps it above, where it knows which
 *   reports may be coalesced - two mouse steps may, a key press and its release
 *   may not.
 * - The host powers us.  Device mode draws from the far machine's VBUS, so a
 *   board that also wants to run a screen and a radio off that same socket is a
 *   wiring question, not one this layer answers.
 * - Nothing here knows what a keystroke is.  A report is bytes whose meaning
 *   lives in the descriptors this port publishes and in the code that fills
 *   them (apps/common/kvm), which is deliberately somewhere a host test can
 *   reach.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_USBDEV_H
#define ARGON_PORT_USBDEV_H

#include <stdbool.h>
#include <stdint.h>

#include <argon/abi.h>

#include <argon/port/impl/usbdev.h>

typedef struct {
    ag_usbhid_state_t state;
    uint32_t          sent;
    uint32_t          dropped;
    uint8_t           leds;     /* what the host says its lock lamps are    */
    uint8_t           protocol; /* 0 boot, 1 report - what the host asked for */
} ag_port_usbdev_status_t;

/*
 * The serial pipe, when the build has it (AG_PORT_HAS_USB_CDC).  It is the
 * same gadget and the same cable: one more function on the composite device,
 * because a program on the far machine has to be able to send its screen back
 * and USB gives it no other way.
 */
typedef struct {
    ag_usbcdc_state_t state;
    uint32_t          rx;
    uint32_t          tx;
} ag_port_usbdev_cdc_status_t;

#if AG_PORT_HAS_USB_DEVICE

ag_err_t ag_port_usbdev_start(void);
ag_err_t ag_port_usbdev_stop(void);
ag_err_t ag_port_usbdev_status(ag_port_usbdev_status_t *out);
ag_err_t ag_port_usbdev_send(ag_usbhid_kind_t kind, const uint8_t *rep,
                             uint32_t len);

#endif /* AG_PORT_HAS_USB_DEVICE */

#if AG_PORT_HAS_USB_CDC

/*
 * The serial pipe.  Both directions are non-blocking and partial: read takes
 * what has arrived and no more, write takes what fits in the endpoint's buffer
 * and says how much, and -AG_EAGAIN means "nothing / no room, come back".  The
 * caller above owns the framing, because bytes on a serial port have none.
 *
 * -AG_ENODEV until the far machine has opened the port.  That is a real state
 * and not a failure: the cable can be in, the device enumerated and the port
 * sitting there with no program at the other end of it, which is exactly what
 * happens before somebody starts the agent.
 */
int32_t  ag_port_usbdev_cdc_read(void *buf, uint32_t len);
int32_t  ag_port_usbdev_cdc_write(const void *buf, uint32_t len);
ag_err_t ag_port_usbdev_cdc_status(ag_port_usbdev_cdc_status_t *out);

#endif /* AG_PORT_HAS_USB_CDC */

#endif /* ARGON_PORT_USBDEV_H */
