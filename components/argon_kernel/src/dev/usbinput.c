/*
 * ArgonOS - a USB keyboard, as far as everything else is concerned.
 *
 * The mirror of btinput.c for the wired keyboard: the USB port hands over boot
 * HID reports, and a keyboard's go to the same shared parser (src/dev/hidkbd.c)
 * a Bluetooth keyboard's do.  This file is only the USB glue.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/usbinput.h>

#if AG_PORT_HAS_USB_HID

#include <argon/hidkbd.h>

static void on_report(ag_usb_usage_t usage, uint8_t report_id,
                      const uint8_t *data, uint32_t len)
{
    (void)report_id;
    if (usage == AG_USB_USAGE_KEYBOARD) {
        ag_hidkbd_report(data, len);
    }
    /* A mouse is the same pointer path btinput.c leaves for later. */
}

ag_err_t ag_usbinput_init(void)
{
    ag_hidkbd_reset();
    ag_port_usb_on_report(on_report);
    return AG_OK;
}

#endif /* AG_PORT_HAS_USB_HID */
