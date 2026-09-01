/*
 * ArgonOS - a boot-protocol HID mouse, turned into pointer events.
 *
 * The counterpart of hidkbd.h, and the same arrangement for the same reason:
 * the report layout of a boot mouse is the same whether it arrives over
 * Bluetooth (src/dev/btinput.c) or USB (src/dev/usbinput.c), so one place turns
 * it into events and neither transport is visible above.
 *
 * A mouse says how far it moved, never where it is, so the position lives here.
 * What is not obvious is which units to report it in, and the answer is that
 * there are two conventions already in the tree and both are right:
 *
 *   A graphical application reads ev.ptr.x against the surface it drew into -
 *   pixels (apps/stomp compares them with s_info.width).
 *   A text application reads the same field as console cells, because that is
 *   what the terminal's own mouse reports and what the touchscreen driver
 *   produces (apps/fm).
 *
 * So the pointer speaks the units of whoever is listening, and the kernel knows
 * which that is: while the display is acquired a graphical application owns the
 * screen, and otherwise the console does.  Inventing a third convention here
 * would leave the two that exist wrong.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_HIDPTR_H
#define ARGON_HIDPTR_H

#include <stdint.h>

#include <argon/abi.h>
#include <argon/port/bt.h>
#include <argon/port/usb.h>

#ifdef __cplusplus
extern "C" {
#endif

#if AG_PORT_HAS_BT || AG_PORT_HAS_USB_HID

/*
 * Forget the buttons and put the pointer back in the middle, so the first
 * report after a mouse attaches is not compared against a stale button set.
 */
void ag_hidptr_reset(void);

/*
 * One mouse report: buttons, then relative x and y, then optionally a wheel.
 * Reports shorter than three bytes are ignored rather than guessed at.
 */
void ag_hidptr_report(const uint8_t *data, uint32_t len);

/* Where the pointer is, in surface pixels.  For a cursor somebody has to draw
 * and for tests; the events carry their own coordinates. */
void ag_hidptr_pos(int32_t *x, int32_t *y);

#endif

#ifdef __cplusplus
}
#endif

#endif /* ARGON_HIDPTR_H */
