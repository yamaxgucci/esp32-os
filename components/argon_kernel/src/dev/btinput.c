/*
 * ArgonOS - a Bluetooth keyboard, as far as everything else is concerned.
 *
 * The port hands over HID reports; a keyboard's become console events through
 * the shared boot-keyboard parser (src/dev/hidkbd.c), which is where a USB
 * keyboard's reports go too.  This file is only the Bluetooth glue: register
 * the handler and route a keyboard report to the parser.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/btinput.h>

#if AG_PORT_HAS_BT

#include <argon/hidkbd.h>

static void on_report(ag_bt_usage_t usage, uint8_t report_id,
                      const uint8_t *data, uint32_t len)
{
    (void)report_id;
    if (usage == AG_BT_USAGE_KEYBOARD) {
        ag_hidkbd_report(data, len);
    }
    /*
     * A mouse would go to the pointer events here.  Left out rather than
     * stubbed: there is nothing on this screen to point at yet, and a half
     * done pointer is worse than none.
     */
}

ag_err_t ag_btinput_init(void)
{
    ag_hidkbd_reset();
    ag_port_bt_on_report(on_report);
    return AG_OK;
}

#endif /* AG_PORT_HAS_BT */
