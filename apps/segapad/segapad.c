/*
 * ArgonOS - a Mega Drive pad on a PCF8574 (.SYS).
 *
 *   drv load c:\segapad.sys
 *
 * The pad is seven wires and the CYD has three free pins, so the pad goes
 * through an I2C port expander: two pins, IO21 and IO22 on the CN1 header,
 * which also carries the 3.3 V and the ground the pad and the expander run on.
 * The wiring and the read itself are in apps/common/segapad/ag_segapad.h.
 *
 * WHAT IT GIVES.  Keys, the same keys the phone's pad and a keyboard give, so
 * every program that is played from a keyboard is played from this without
 * knowing about it:
 *
 *   d-pad  arrows       A  Z       B  X       C  C       Start  Enter
 *   X  A                Y  S       Z  D       Mode  M
 *   Start and Mode together  Esc (MD's pause menu), and neither of them
 *
 * X, Y, Z and Mode are the keys the input layer's own pad table gives them
 * (components/argon_kernel/src/dev/input.c), so a program that reads buttons
 * rather than keys finds them where it looks: MD's 6-button pad does.  Mode
 * was Esc until 5 Nov 2026, and a game that plays with Mode (The Lost
 * Vikings) could not have it - so the menu is the two together, which no
 * game asks for.  A pad without Mode opens MD's menu by a touch on the glass.
 *
 * Start goes out START_HOLD_MS late, so that Start and Mode pressed together
 * by a thumb that is a little early with Start do not pause the game on the
 * way to the menu.  A tap of Start shorter than that still goes out, as a
 * press and a release.
 *
 * A key held is sent again every REPEAT_MS, marked as a repeat, because the
 * console forgets a key 150 ms after it last heard of it (AG_KEY_STICKY_MS):
 * the phone's pad does the same, and MD already tells a repeat from a press.
 *
 * WHEN.  A task of its own on the system core reads the pad POLL_MS apart.
 * One read is eight write-and-read pairs on the bus, about 4 ms at 100 kHz;
 * the 6-button pad forgets its count after 1.5 ms of quiet, which the gap
 * between reads is far longer than.  100 kHz because that is the PCF8574's
 * speed - [i2c0] khz in BOARD.CFG.
 *
 * A pad pulled out, or a bus that stops answering, lets every key go at once,
 * so nothing stays held on a pad that is not there; the journal says when it
 * went and when it came back.
 *
 * SYSTEM.CFG keys, all optional, under [pad]:
 *   bus  = 0        the I2C bus, as in [i2c0]
 *   addr = 0        the expander's address; 0 looks at 0x20..0x27 (PCF8574)
 *                   and 0x38..0x3f (PCF8574A)
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/keys.h>
#include <argon/libc.h>

#include "ag_segapad.h"

AG_DRV("SEGAPAD", "0.1", "argon");

#define POLL_MS   8u   /* a read every 8 ms: well inside a 60 Hz frame     */
#define REPEAT_MS 60u  /* inside the console's 150 ms                       */
#define BUS_MS    20u  /* the longest one transfer may take                 */
#define GONE_READS 4   /* this many empty reads in a row and the pad is gone */
#define START_HOLD_MS 80u /* Start waits this long for Mode (the menu)      */
#define SP_MENU 0x1000u   /* Start and Mode together: not a line of the pad  */

static const struct {
    uint16_t bit;
    uint16_t key;
} k_keys[] = {
    {AG_SP_UP, AG_KEY_UP},       {AG_SP_DOWN, AG_KEY_DOWN},
    {AG_SP_LEFT, AG_KEY_LEFT},   {AG_SP_RIGHT, AG_KEY_RIGHT},
    {AG_SP_A, AG_KEY_Z},         {AG_SP_B, AG_KEY_X},
    {AG_SP_C, AG_KEY_C},         {AG_SP_START, AG_KEY_ENTER},
    {AG_SP_X, AG_KEY_A},         {AG_SP_Y, AG_KEY_S},
    {AG_SP_Z, AG_KEY_D},         {AG_SP_MODE, AG_KEY_M},
    {SP_MENU, AG_KEY_ESC},
};

static struct {
    int               bus;
    uint8_t           addr;
    volatile int      stop;
    uint16_t          held;
    uint16_t          out;   /* the keys that went out: keys_of(held)     */
    uint16_t          raw;   /* the last read, before it is believed */
    uint16_t          six;   /* X Y Z Mode as the last 6-button read had them */
    uint32_t          blips; /* changes one read alone reported      */
    int               kind;
    int               empty;
    uint32_t          repeat_at;
    uint32_t          start_at; /* Start went down here, not yet sent; 0 none */
    int               start_out; /* Start has gone out                       */
    int               menu;      /* Start and Mode were held together        */
    uint32_t          errs;
} s;

static void send_key(uint16_t key, int down, int repeat)
{
    ag_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = down ? AG_EV_KEY_DOWN : AG_EV_KEY_UP;
    ev.key.keycode = key;
    ev.key.repeat = repeat != 0;
    (void)ag_inject_event(&ev);
}

/* What the pad holds to what goes out: Start and Mode together are the
 * menu until both are let go, and Start waits START_HOLD_MS to see whether
 * Mode is coming. */
static uint16_t keys_of(uint16_t now)
{
    const uint16_t sm = AG_SP_START | AG_SP_MODE;
    const uint32_t t = ag_millis() | 1u;
    if (s.menu || (now & sm) == sm) {
        s.menu = (now & sm) != 0u;
        s.start_at = 0;
        s.start_out = 0;
        return s.menu ? (uint16_t)((now & ~sm) | SP_MENU) : now;
    }
    if (!(now & AG_SP_START)) {
        if (s.start_at != 0u && !s.start_out) {
            s.start_at = 0;
            return (uint16_t)(now | AG_SP_START); /* the tap; let go next read */
        }
        s.start_at = 0;
        s.start_out = 0;
        return now;
    }
    if (s.start_at == 0u) {
        s.start_at = t;
    }
    if (!s.start_out && t - s.start_at < START_HOLD_MS) {
        return (uint16_t)(now & ~AG_SP_START);
    }
    s.start_out = 1;
    return now;
}

/* What changed since the last read, as presses and releases. */
static void apply(uint16_t pad)
{
    const uint16_t now = keys_of(pad);
    const uint16_t was = s.out;
    for (unsigned i = 0; i < sizeof(k_keys) / sizeof(k_keys[0]); i++) {
        const uint16_t b = k_keys[i].bit;
        if ((now & b) && !(was & b)) {
            send_key(k_keys[i].key, 1, 0);
        } else if (!(now & b) && (was & b)) {
            send_key(k_keys[i].key, 0, 0);
        }
    }
    s.held = pad;
    s.out = now;

    const uint32_t t = ag_millis();
    if (now != was) {
        s.repeat_at = t + REPEAT_MS;
    } else if (now != 0u && (int32_t)(t - s.repeat_at) >= 0) {
        for (unsigned i = 0; i < sizeof(k_keys) / sizeof(k_keys[0]); i++) {
            if (now & k_keys[i].bit) {
                send_key(k_keys[i].key, 1, 1);
            }
        }
        s.repeat_at = t + REPEAT_MS;
    }
}

/* One read: eight steps, then SELECT back high.  -1 when the bus failed. */
static int read_pad(uint16_t *held)
{
    uint8_t rd[AG_SEGAPAD_STEPS];
    for (int i = 0; i < AG_SEGAPAD_STEPS; i++) {
        const uint8_t out = ag_segapad_out(i);
        if (ag_i2c_wrrd(s.bus, s.addr, &out, 1, &rd[i], 1, BUS_MS) != AG_OK) {
            const uint8_t idle = 0xffu;
            (void)ag_i2c_write(s.bus, s.addr, &idle, 1, BUS_MS);
            return -1;
        }
    }
    const uint8_t idle = ag_segapad_out(0);
    (void)ag_i2c_write(s.bus, s.addr, &idle, 1, BUS_MS);
    return ag_segapad_decode(rd, held);
}

static const char *kind_name(int k)
{
    return k == AG_SP_6BTN ? "a 6-button pad" : "a 3-button pad";
}

static void pad_task(void *arg)
{
    (void)arg;
    uint32_t last = ag_millis();
    unsigned told = 0;
    while (!s.stop) {
        /* A read that comes late is a release that comes late, and a game
         * that sees a direction held longer than it was repeats it. */
        const uint32_t t0 = ag_millis();
        if (t0 - last > 100u && told < 20u) {
            told++;
            ag_log(AG_LOG_WARN, "SEGAPAD", "no read for %u ms (held %x)",
                   (unsigned)(t0 - last), (unsigned)s.held);
        }
        last = t0;
        uint16_t  held = 0;
        const int kind = read_pad(&held);
        if (kind < 0) {
            s.errs++;
        }
        if (kind <= 0) {
            /* Not let go on one bad read: a pad is not pulled out in 8 ms. */
            if (++s.empty >= GONE_READS && s.kind != AG_SP_NONE) {
                apply(0);
                ag_log(AG_LOG_WARN, "SEGAPAD", "the pad is gone (%s)",
                       kind < 0 ? "the bus does not answer"
                                : "nothing on the port");
                s.kind = AG_SP_NONE;
            }
        } else {
            s.empty = 0;
            /* A 3-button pad never answers as a 6-button one, but a 6-button
             * one answers as a 3-button one whenever one step of the read
             * outlasts its 1.5 ms count.  On the CYD a step is 0.6 ms at
             * 100 kHz, and in about one read in thirty one step takes 1.5 to
             * 2.6 ms (measured 4 Nov 2026, with nothing else running; where
             * the time goes is not known).  Once a 6-button pad has been seen
             * it is what is on the port until it goes, and a read that lost
             * the count says nothing about X, Y, Z and Mode: they are what
             * the last read that kept it said.  Not what was last believed -
             * with every other read like this, Mode held never read the same
             * twice running and never went down.  Logged once, too: logged at
             * each change it held the journal lock for 3 s the first day. */
            const uint16_t six = AG_SP_X | AG_SP_Y | AG_SP_Z | AG_SP_MODE;
            if (kind == AG_SP_6BTN || s.kind == AG_SP_NONE) {
                if (kind != s.kind) {
                    ag_log(AG_LOG_INFO, "SEGAPAD", "%s, %u bus errors so far",
                           kind_name(kind), (unsigned)s.errs);
                    s.kind = kind;
                }
                s.six = (uint16_t)(held & six);
            } else if (s.kind == AG_SP_6BTN) {
                held = (uint16_t)((held & ~six) | s.six);
            }
            /* A change counts when two reads in a row agree on it, 12 ms
             * apart - a frame, about, which is how often a Mega Drive game
             * reads its pad.  One read alone that says otherwise is a button
             * let go and pressed again between two reads, which a game takes
             * as a second press: the ESP-07 stand-in garbles one read in a
             * few hundred, and MD's menu went down three rows for one touch
             * (4 November 2026). */
            if (held == s.raw) {
                apply(held);
            } else if (s.raw != s.held && held == s.held) {
                if (++s.blips <= 10u) {
                    ag_log(AG_LOG_WARN, "SEGAPAD", "one read said %x, held %x",
                           (unsigned)s.raw, (unsigned)s.held);
                }
            }
            s.raw = held;
        }
        ag_delay(POLL_MS);
    }
    apply(0);
}

static void pad_fini(void)
{
    s.stop = 1;
}

static int found(uint8_t a)
{
    return ag_i2c_probe(s.bus, a) == AG_OK;
}

ag_err_t ag_driver_init(void)
{
    const ag_cfg_api_t *cfg = ag_api()->cfg;
    const int32_t bus = (cfg != NULL) ? cfg->get_int("pad.bus", 0) : 0;
    const int32_t addr = (cfg != NULL) ? cfg->get_int("pad.addr", 0) : 0;
    memset(&s, 0, sizeof(s));
    if (bus < 0) {
        ag_printf("SEGAPAD: pad.bus %d is not a bus\n", (int)bus);
        return -AG_ERANGE;
    }
    s.bus = (int)bus;

    if (addr != 0) {
        if (addr < 0x08 || addr > 0x77) {
            ag_printf("SEGAPAD: pad.addr 0x%02x is not a 7-bit address\n",
                      (unsigned)addr);
            return -AG_EINVAL;
        }
        const ag_err_t e = ag_i2c_probe(s.bus, (uint8_t)addr);
        if (e != AG_OK) {
            ag_printf("SEGAPAD: i2c%d 0x%02x: %s\n", s.bus, (unsigned)addr,
                      ag_strerror(e));
            return e;
        }
        s.addr = (uint8_t)addr;
    } else {
        /* The first probe also brings the bus up, and says why it cannot. */
        const ag_err_t e = ag_i2c_probe(s.bus, 0x20);
        if (e == -AG_ENODEV) {
            ag_printf("SEGAPAD: i2c%d has no pins: set i2c%d.sda and "
                      "i2c%d.scl in BOARD.CFG (CYD: 21 and 22)\n", s.bus,
                      s.bus, s.bus);
            return e;
        }
        for (uint8_t a = 0x20; a <= 0x3f && s.addr == 0u; a++) {
            if ((a <= 0x27 || a >= 0x38) && found(a)) {
                s.addr = a;
            }
        }
        if (s.addr == 0u) {
            ag_printf("SEGAPAD: no PCF8574 on i2c%d at 0x20..0x27 or "
                      "0x38..0x3f\n", s.bus);
            return -AG_ENOENT;
        }
    }

    const uint8_t idle = 0xffu;
    (void)ag_i2c_write(s.bus, s.addr, &idle, 1, BUS_MS);
    ag_module_on_unload(pad_fini);
    if (!ag_module_task(pad_task, NULL, "ag_segapad", 3072u, 4,
                        AG_THREAD_SYS_CORE)) {
        return -AG_ENOMEM;
    }
    ag_printf("SEGAPAD: PCF8574 on i2c%d at 0x%02x, read every %u ms\n",
              s.bus, (unsigned)s.addr, (unsigned)POLL_MS);
    return AG_OK;
}
