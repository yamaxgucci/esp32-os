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
#include <argon/hidptr.h>
#include <argon/proc.h>
#include <argon/hidrep.h>

/*
 * The mouse's own layout, when its descriptor could be read (argon/hidrep.h).
 * One, because the port attaches one device; a new descriptor replaces it.
 * Written on the port's task before the interface's first report and read on
 * the same task, so nothing guards it.
 */
static ag_hidrep_mouse_t s_mouse;
static bool              s_mouse_known;

static bool on_descriptor(ag_usb_usage_t usage, uint8_t itf,
                          const uint8_t *desc, uint32_t len)
{
    (void)itf;
    if (usage != AG_USB_USAGE_MOUSE) {
        return false;
    }
    s_mouse_known = ag_hidrep_parse_mouse(desc, len, &s_mouse);
    return s_mouse_known;
}

static void on_report(ag_usb_usage_t usage, uint8_t report_id,
                      const uint8_t *data, uint32_t len)
{
    (void)report_id;
    if (usage == AG_USB_USAGE_KEYBOARD) {
        ag_hidkbd_report(data, len);
    } else if (usage == AG_USB_USAGE_MOUSE) {
        /* The same shared pointer path a Bluetooth mouse takes. */
        if (!s_mouse_known) {
            ag_hidptr_report(data, len);
            return;
        }
        uint8_t buttons = 0;
        int32_t dx = 0, dy = 0, wheel = 0;
        if (ag_hidrep_decode_mouse(&s_mouse, data, len, &buttons, &dx, &dy,
                                   &wheel)) {
            ag_hidptr_move(buttons, dx, dy, wheel);
        }
    }
}

/* names the claim on internal memory (ag_proc_keep_internal) */
static const char s_keep_tag = 0;

ag_err_t ag_usbinput_init(void)
{
    ag_hidkbd_reset();
    ag_hidptr_reset();
    ag_port_usb_on_report(on_report);
    /* a mouse may be plugged in at any moment, a game running or not */
    ag_proc_keep_internal(&s_keep_tag, ag_port_usb_arrival_bytes());
    ag_port_usb_on_descriptor(on_descriptor);
    return AG_OK;
}

void ag_usbinput_off(void)
{
    ag_proc_keep_internal(&s_keep_tag, 0u);
}

#endif /* AG_PORT_HAS_USB_HID */
