/*
 * ArgonOS - the CardKB-style keyboard (apps/common/cardkb): which two-byte
 * reads are a key, and which key each code is.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../apps/common/cardkb/ag_cardkb.h"

#include <argon/abi.h>
#include <argon/keys.h>

#include <stddef.h>

#include "test.h"

void run_cardkb_tests(void)
{
    /* No key, a checked key under each mask, a real CardKB's key, and one
     * bit gone. */
    int mask = 7;
    AG_CHECK(ag_cardkb_judge(0, 0xA5, &mask) == AG_CARDKB_NONE && mask == -1);
    AG_CHECK(ag_cardkb_judge(0, 0xFF, &mask) == AG_CARDKB_NONE);
    AG_CHECK(ag_cardkb_judge('a', 'a' ^ AG_CARDKB_CHECK, &mask) == AG_CARDKB_KEY &&
             mask == 0);
    AG_CHECK(ag_cardkb_judge('a', 'a' ^ AG_CARDKB_CHECK1, &mask) == AG_CARDKB_KEY &&
             mask == 1);
    AG_CHECK(ag_cardkb_judge('a', 0xFF, &mask) == AG_CARDKB_PLAIN && mask == -1);
    AG_CHECK(ag_cardkb_judge('a', ('a' ^ AG_CARDKB_CHECK) ^ 0x04, &mask) ==
             AG_CARDKB_GARBLED);
    AG_CHECK(ag_cardkb_judge('a' ^ 0x01, 'a' ^ AG_CARDKB_CHECK, NULL) ==
             AG_CARDKB_GARBLED);
    /* Every code under either mask is a key, under the mask it was sent with;
     * no single flipped bit of either byte makes a key of it under any mask. */
    int bad = 0;
    for (int k = 1; k < 256; k++) {
        for (int m = 0; m < 2; m++) {
            const uint8_t chk = (uint8_t)(k ^ (m ? AG_CARDKB_CHECK1 : AG_CARDKB_CHECK));
            if (ag_cardkb_judge((uint8_t)k, chk, &mask) != AG_CARDKB_KEY || mask != m) {
                bad++;
            }
            for (int bit = 0; bit < 8; bit++) {
                const uint8_t key = (uint8_t)(k ^ (1 << bit));
                if (key != 0 && ag_cardkb_judge(key, chk, NULL) == AG_CARDKB_KEY) {
                    bad++;
                }
                if (ag_cardkb_judge((uint8_t)k, (uint8_t)(chk ^ (1 << bit)), NULL) ==
                    AG_CARDKB_KEY) {
                    bad++;
                }
            }
        }
    }
    AG_CHECK(bad == 0);

    uint32_t ch = 0;
    uint16_t mods = 0;
    AG_CHECK(ag_cardkb_key('a', &ch, &mods) == AG_KEY_A && ch == 'a' && mods == 0);
    AG_CHECK(ag_cardkb_key('Q', &ch, &mods) == AG_KEY_Q && ch == 'Q' &&
             mods == AG_MOD_SHIFT);
    AG_CHECK(ag_cardkb_key('0', &ch, &mods) == AG_KEY_0 && ch == '0');
    AG_CHECK(ag_cardkb_key('7', &ch, &mods) == AG_KEY_7 && ch == '7');
    AG_CHECK(ag_cardkb_key('?', &ch, &mods) == AG_KEY_SLASH && ch == '?' &&
             mods == AG_MOD_SHIFT);
    AG_CHECK(ag_cardkb_key('\\', &ch, &mods) == AG_KEY_BACKSLASH && ch == '\\');
    AG_CHECK(ag_cardkb_key(' ', &ch, &mods) == AG_KEY_SPACE && ch == ' ');
    AG_CHECK(ag_cardkb_key(0x0D, &ch, &mods) == AG_KEY_ENTER && ch == '\r');
    AG_CHECK(ag_cardkb_key(0x08, &ch, &mods) == AG_KEY_BACKSPACE && ch == 0x08);
    AG_CHECK(ag_cardkb_key(0x1B, &ch, &mods) == AG_KEY_ESC);
    AG_CHECK(ag_cardkb_key(AG_CARDKB_LEFT, &ch, &mods) == AG_KEY_LEFT && ch == 0);
    AG_CHECK(ag_cardkb_key(AG_CARDKB_UP, &ch, &mods) == AG_KEY_UP);
    AG_CHECK(ag_cardkb_key(AG_CARDKB_DOWN, &ch, &mods) == AG_KEY_DOWN);
    AG_CHECK(ag_cardkb_key(AG_CARDKB_RIGHT, &ch, &mods) == AG_KEY_RIGHT);
    /* A code no key makes: nothing at all. */
    AG_CHECK(ag_cardkb_key(0x01, &ch, &mods) == 0 && ch == 0 && mods == 0);
    AG_CHECK(ag_cardkb_key(0xC8, &ch, &mods) == 0);
    /* Every printable ASCII character is some key. */
    int none = 0;
    for (int c = 0x20; c < 0x7F; c++) {
        if (ag_cardkb_key((uint8_t)c, &ch, &mods) == 0 || ch != (uint32_t)c) {
            none++;
        }
    }
    AG_CHECK(none == 0);
}
