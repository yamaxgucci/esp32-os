/*
 * ArgonOS - a Bluetooth keyboard, as far as everything else is concerned.
 *
 * The port hands over HID reports; this turns them into the console events the
 * terminal decoder already produces, and from there nothing can tell the
 * difference between a key pressed on a keyboard paired to the board and one
 * typed into a serial terminal on the other side of the world.  That is the
 * whole design: the shell, the editor and the file manager gain a keyboard
 * without one line of change.
 *
 * A key code in this system is a USB HID usage id (sdk/include/argon/keys.h),
 * so the codes in a report need no translation at all.  What does need a table
 * is the character: which letter a key produces depends on the layout, and the
 * one here is US, because that is what the report gives no way to know.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/btinput.h>

#if AG_PORT_HAS_BT

#include <string.h>

#include <argon/console.h>
#include <argon/keys.h>
#include <argon/log.h>

#include <argon/port/time.h>

/* The boot keyboard report: modifiers, a reserved byte, six key slots. */
#define KBD_KEYS 6

static uint8_t s_prev[KBD_KEYS];
static uint8_t s_prev_mods;

/*
 * US layout, unshifted and shifted, indexed by HID usage id from 0x04.
 * Only the printable part: everything above 0x39 is a named key and carries no
 * character, which is exactly what a zero here means.
 */
static const char k_plain[] =
    "abcdefghijklmnopqrstuvwxyz" /* 0x04..0x1d */
    "1234567890"                 /* 0x1e..0x27 */
    "\n\x1b\b\t "                /* enter esc bksp tab space 0x28..0x2c */
    "-=[]\\#;'`,./";             /* 0x2d..0x38 */

static const char k_shift[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "!@#$%^&*()"
    "\n\x1b\b\t "
    "_+{}|~:\"~<>?";

static uint32_t character_for(uint8_t usage, uint16_t mods)
{
    if (usage < 0x04u || usage > 0x38u) {
        return 0;
    }
    const size_t i = (size_t)(usage - 0x04u);
    bool upper = (mods & AG_MOD_SHIFT) != 0;

    /* Caps lock is a letter-only shift, which is why it is not simply another
     * shift bit: Caps and 1 is 1, and Shift and 1 is !. */
    if ((mods & AG_MOD_CAPS) != 0 && usage <= 0x1du) {
        upper = !upper;
    }
    const char c = upper ? k_shift[i] : k_plain[i];
    return (c == '\0') ? 0u : (uint32_t)(unsigned char)c;
}

static uint16_t mods_from(uint8_t hid_mods, uint16_t carried)
{
    uint16_t m = carried & AG_MOD_CAPS; /* caps is a latch, not a report bit */

    if (hid_mods & 0x22u) {
        m |= AG_MOD_SHIFT;
    }
    if (hid_mods & 0x11u) {
        m |= AG_MOD_CTRL;
    }
    if (hid_mods & 0x44u) {
        m |= AG_MOD_ALT;
    }
    if (hid_mods & 0x88u) {
        m |= AG_MOD_GUI;
    }
    return m;
}

static void emit(ag_event_type_t type, uint8_t usage, uint16_t mods)
{
    ag_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.ts = (ag_time_t)ag_port_us();
    ev.key.keycode = usage;
    ev.key.mods = mods;
    ev.key.unicode = (type == AG_EV_KEY_DOWN) ? character_for(usage, mods) : 0u;
    (void)ag_console_inject_event(&ev);
}

static bool in_report(const uint8_t *keys, uint8_t usage)
{
    for (int i = 0; i < KBD_KEYS; i++) {
        if (keys[i] == usage) {
            return true;
        }
    }
    return false;
}

/*
 * A keyboard report is a set, not an event: it says which keys are down right
 * now.  What changed since the last one is what happened, and comparing the
 * two is the only way to know - which is also why a lost report is a key that
 * appears stuck, and why the release of every key is derived rather than
 * announced.
 */
static void on_keyboard(const uint8_t *data, uint32_t len)
{
    static uint16_t s_caps;

    if (len < 3u) {
        return;
    }
    const uint8_t  hid_mods = data[0];
    const uint8_t *keys = data + 2;
    const uint32_t n = (len - 2u > KBD_KEYS) ? KBD_KEYS : len - 2u;

    uint8_t now[KBD_KEYS];
    memset(now, 0, sizeof(now));
    memcpy(now, keys, n);

    const uint16_t mods = mods_from(hid_mods, s_caps);

    for (int i = 0; i < KBD_KEYS; i++) {
        const uint8_t k = s_prev[i];
        /* 0x01..0x03 are the codes a keyboard sends when too many keys are
         * held at once; they are not keys and must not be reported as any. */
        if (k > 0x03u && !in_report(now, k)) {
            emit(AG_EV_KEY_UP, k, mods);
        }
    }
    for (int i = 0; i < KBD_KEYS; i++) {
        const uint8_t k = now[i];
        if (k > 0x03u && !in_report(s_prev, k)) {
            if (k == AG_KEY_CAPSLOCK) {
                s_caps ^= AG_MOD_CAPS;
            }
            emit(AG_EV_KEY_DOWN, k, mods | s_caps);
        }
    }

    memcpy(s_prev, now, sizeof(s_prev));
    s_prev_mods = hid_mods;
}

static void on_report(ag_bt_usage_t usage, uint8_t report_id,
                      const uint8_t *data, uint32_t len)
{
    (void)report_id;
    if (data == NULL) {
        return;
    }
    if (usage == AG_BT_USAGE_KEYBOARD) {
        on_keyboard(data, len);
    }
    /*
     * A mouse would go to the pointer events here.  Left out rather than
     * stubbed: there is nothing on this screen to point at yet, and a half
     * done pointer is worse than none.
     */
}

ag_err_t ag_btinput_init(void)
{
    memset(s_prev, 0, sizeof(s_prev));
    s_prev_mods = 0;
    ag_port_bt_on_report(on_report);
    return AG_OK;
}

#endif /* AG_PORT_HAS_BT */
