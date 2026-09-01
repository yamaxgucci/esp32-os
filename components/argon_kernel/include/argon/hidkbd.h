/*
 * ArgonOS - a boot-protocol HID keyboard, turned into console events.
 *
 * The report layout of a boot keyboard is the same whether it arrives over
 * Bluetooth (src/dev/btinput.c) or USB (src/dev/usbinput.c): one modifier byte,
 * one reserved, then up to six key slots.  This is the one place that turns
 * that report into key-down and key-up events, so both transports share it and
 * neither the shell, the editor nor the file manager can tell which keyboard a
 * key came from.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_HIDKBD_H
#define ARGON_HIDKBD_H

#include <stdint.h>

#include <argon/abi.h>
#include <argon/port/bt.h>
#include <argon/port/usb.h>

#ifdef __cplusplus
extern "C" {
#endif

#if AG_PORT_HAS_BT || AG_PORT_HAS_USB_HID

/* Clear the remembered key set, so the first report after a keyboard attaches
 * is compared against "nothing held". */
void ag_hidkbd_reset(void);

/* One boot keyboard report (modifiers, reserved, key slots).  What changed
 * since the last report becomes key events. */
void ag_hidkbd_report(const uint8_t *data, uint32_t len);

#endif

#ifdef __cplusplus
}
#endif

#endif /* ARGON_HIDKBD_H */
