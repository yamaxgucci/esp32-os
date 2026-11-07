/*
 * ArgonOS - a keyboard on I2C that answers like M5Stack's CardKB (.SYS).
 *
 *   drv install a:\cardkb.sys
 *
 * CardKB answers at 0x5F with the last key pressed, or 0.  The same address on
 * the ESP-07 that stands in for the Sega pad (firmware/esp8266-segapad) is a
 * keyboard typed on a phone: the module's page has a keyboard beside the pad,
 * and the keys come down the same two wires the pad does, IO21 and IO22 on
 * the CYD's CN1.  What a code means and how a read is judged is in
 * apps/common/cardkb (host-tests/test_cardkb.c).
 *
 * WHAT IT GIVES.  Keys, press and release, with the character each types - the
 * same events a USB keyboard gives, so the shell, the desktop and any program
 * read it without knowing about it.  Letters, digits and punctuation in the US
 * layout, Enter, Backspace, Tab, Esc, Delete and the four arrows.
 *
 * A key is taken only when its check byte agrees, and the module keeps it
 * until the board writes back to say so: the stand-in answers the bus in
 * software and garbles a bit now and then, and a keyboard that types the
 * wrong letter is worse than one a read late.
 *
 * SYSTEM.CFG keys, all optional, under [kbd]:
 *   bus  = 0        the I2C bus, as in [i2c0]
 *   addr = 0x5F     where the keyboard answers
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/keys.h>
#include <argon/libc.h>

#include "ag_cardkb.h"

AG_DRV("CARDKB", "0.1", "argon");

#define POLL_MS 10u   /* a read every 10 ms: typing is slower than that      */
#define BUS_MS  20u   /* the longest one transfer may take                  */
#define UP_MS   30u   /* how long a key stays down: a press, not a hold     */
/* Below SEGAPAD's 5, which has a game's timing to keep: a letter typed 10 ms
 * later is not one anybody sees. */
#define KBD_PRIO 4

static struct {
    int          bus;
    uint8_t      addr;
    volatile int stop;
    uint16_t     down;     /* the key that went down and is not up yet       */
    int          last_mask; /* the stand-in's mask on the last key typed       */
    uint8_t      last_key;
    uint32_t     down_at;
    uint32_t     keys, garbled, errs, dupes;
} s;

static void send_key(uint16_t key, uint32_t unicode, uint16_t mods, int down)
{
    ag_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = down ? AG_EV_KEY_DOWN : AG_EV_KEY_UP;
    ev.key.keycode = key;
    ev.key.unicode = down ? unicode : 0u;
    ev.key.mods = mods;
    (void)ag_inject_event(&ev);
}

static void key_up(void)
{
    if (s.down != 0u) {
        send_key(s.down, 0, 0, 0);
        s.down = 0;
    }
}

static void kbd_task(void *arg)
{
    (void)arg;
    while (!s.stop) {
        const uint32_t now = ag_millis();
        if (s.down != 0u && now - s.down_at >= UP_MS) {
            key_up();
        }
        uint8_t rd[2] = {0, 0};
        if (ag_i2c_read(s.bus, s.addr, rd, sizeof(rd), BUS_MS) != AG_OK) {
            s.errs++;
            ag_delay(POLL_MS);
            continue;
        }
        int mask = -1;
        const int said = ag_cardkb_judge(rd[0], rd[1], &mask);
        if (said == AG_CARDKB_GARBLED) {
            /* Not taken: the module keeps it, and the next read asks again. */
            if (++s.garbled <= 10u) {
                ag_log(AG_LOG_WARN, "CARDKB", "a read said %02x %02x",
                       (unsigned)rd[0], (unsigned)rd[1]);
            }
        } else if (said != AG_CARDKB_NONE) {
            if (said == AG_CARDKB_KEY) {
                const uint8_t took = 0u;
                (void)ag_i2c_write(s.bus, s.addr, &took, 1, BUS_MS);
                /* The same key under the same mask is the one typed last,
                 * read again because "taken" did not arrive: taken again
                 * above, not typed twice ("WWorld", 7 Nov 2026). */
                if (mask == s.last_mask && rd[0] == s.last_key) {
                    s.dupes++;
                    continue;
                }
                s.last_mask = mask;
                s.last_key = rd[0];
            }
            uint32_t ch = 0;
            uint16_t mods = 0;
            const uint16_t key = ag_cardkb_key(rd[0], &ch, &mods);
            if (key != 0u) {
                key_up(); /* the one before, if a fast typist got here first */
                send_key(key, ch, mods, 1);
                s.down = key;
                s.down_at = now;
                s.keys++;
            }
            continue; /* another may be waiting: read again at once */
        }
        ag_delay(POLL_MS);
    }
    key_up();
    ag_log(AG_LOG_INFO, "CARDKB", "%u keys, %u read twice, %u garbled reads, "
           "%u bus errors", (unsigned)s.keys, (unsigned)s.dupes,
           (unsigned)s.garbled, (unsigned)s.errs);
}

static void kbd_fini(void)
{
    s.stop = 1;
}

ag_err_t ag_driver_init(void)
{
    const ag_cfg_api_t *cfg = ag_api()->cfg;
    const int32_t bus = (cfg != NULL) ? cfg->get_int("kbd.bus", 0) : 0;
    const int32_t addr = (cfg != NULL) ? cfg->get_int("kbd.addr", AG_CARDKB_ADDR)
                                       : (int32_t)AG_CARDKB_ADDR;
    memset(&s, 0, sizeof(s));
    s.last_mask = -1;
    if (bus < 0) {
        ag_printf("CARDKB: kbd.bus %d is not a bus\n", (int)bus);
        return -AG_ERANGE;
    }
    if (addr < 0x08 || addr > 0x77) {
        ag_printf("CARDKB: kbd.addr 0x%02x is not a 7-bit address\n",
                  (unsigned)addr);
        return -AG_EINVAL;
    }
    s.bus = (int)bus;
    s.addr = (uint8_t)addr;
    /* The first probe also brings the bus up, and says why it cannot. */
    const ag_err_t e = ag_i2c_probe(s.bus, s.addr);
    if (e == -AG_ENODEV) {
        ag_printf("CARDKB: i2c%d has no pins: set i2c%d.sda and i2c%d.scl in "
                  "BOARD.CFG (CYD: 21 and 22)\n", s.bus, s.bus, s.bus);
        return e;
    }
    if (e != AG_OK) {
        ag_printf("CARDKB: no keyboard on i2c%d at 0x%02x\n", s.bus,
                  (unsigned)s.addr);
        return -AG_ENOENT;
    }
    ag_module_on_unload(kbd_fini);
    if (!ag_module_task(kbd_task, NULL, "ag_cardkb", 2048u, KBD_PRIO,
                        AG_THREAD_SYS_CORE)) {
        return -AG_ENOMEM;
    }
    ag_printf("CARDKB: keyboard on i2c%d at 0x%02x\n", s.bus, (unsigned)s.addr);
    return AG_OK;
}
