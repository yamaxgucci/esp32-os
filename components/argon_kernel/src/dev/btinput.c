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
#include <argon/hidptr.h>

static void on_report(ag_bt_usage_t usage, uint8_t report_id,
                      const uint8_t *data, uint32_t len)
{
    (void)report_id;
    if (usage == AG_BT_USAGE_KEYBOARD) {
        ag_hidkbd_report(data, len);
    } else if (usage == AG_BT_USAGE_MOUSE) {
        /*
         * This used to say a mouse would go here when there was something on
         * the screen to point at.  There is now - the screen is the board next
         * to this one, over a wire (apps/remdisp) - so it does.
         */
        ag_hidptr_report(data, len);
    }
}

ag_err_t ag_btinput_init(void)
{
    ag_hidkbd_reset();
    ag_hidptr_reset();
    ag_port_bt_on_report(on_report);
    return AG_OK;
}

#endif /* AG_PORT_HAS_BT */
