/*
 * ArgonOS - a Mega Drive pad on a PCF8574, and a keyboard beside it (.SYS).
 *
 *   drv load c:\segapad.sys
 *
 * THE KEYBOARD is on the same two wires, at 0x5F, answered as M5Stack's CardKB
 * answers (apps/common/cardkb): the ESP-07 stand-in's page has a KEYS view for
 * a phone's keyboard, and a real CardKB works too.  One driver for both, at
 * his asking (7 Nov 2026): one task and one stack on a board where MD counts
 * every kilobyte.  The keyboard is read after each read of the pad, in the
 * quiet a 6-button pad needs between reads anyway.
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
 *   B and C also Backspace and Tab, once a press (the file manager's
 *   folder-up and other panel; no game reads them)
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
 * One read is six steps under repeated STARTs (read_pad_steps), about 1.4 ms
 * at 400 kHz; the 6-button pad forgets its count after a quiet - 1.5 ms by
 * Sega's book, ~400 us on the one on the CYD at 3.3 V - which the gap
 * between reads is far longer than and a step must not be.  400 kHz is past
 * the PCF8574's datasheet and what that pad needs ([i2c0] khz in the CYD's
 * BOARD.CFG).
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
/* ag_i2c_steps (0.78) is asked for and falls back without it: firmware
 * from before it loads this driver and reads the pad a transfer a step. */
#define AG_APP_ABI_MINOR 77u
#include <argon/argon.h>
#include <argon/keys.h>
#include <argon/libc.h>

#include "ag_cardkb.h"
#include "ag_segapad.h"

AG_DRV("SEGAPAD", "0.1", "argon");

#define POLL_MS   8u   /* a read every 8 ms: well inside a 60 Hz frame     */
#define REPEAT_MS 60u  /* inside the console's 150 ms                       */
#define BUS_MS    20u  /* the longest one transfer may take                 */
/*
 * No good read for this long and the pad is gone, and what it held is let go.
 * It was four reads, some 50 ms: the ESP-07 stand-in drops its answers while
 * its page's request comes in, which is when he is pressing, and a spin dash
 * - Down held, A pressed and pressed - let go of Down (6 Nov 2026).  A pad
 * pulled out is let go of a quarter of a second later.
 */
#define GONE_MS 250u
#define PAD_LOOK_MS 1000u /* no expander: look for one again this often     */
#define START_HOLD_MS 80u /* Start waits this long for Mode (the menu)      */
#define SP_MENU 0x1000u   /* Start and Mode together: not a line of the pad  */
/* A 6-button pad starts its count again after 1.5 ms with SELECT still:
 * a step that takes longer spoils the read (see read_pad). */
#define STEP_SLOW_US 1500u
#define TALLY_MS 10000u   /* how often the journal hears the slow reads   */
#define TALLIES  30u      /* and how many times, at most                  */
/*
 * Above the CYD's MD drawer (4), which never sleeps while a picture goes out,
 * and not above 5, where the kernel's budget watches a task and holds one
 * that overruns below applications for the rest of its window (proc/budget.c)
 * - at 6 it did, at 21 ms of every 100.  The task is asleep or waiting on the
 * bus nearly all the time; what it needs is to be woken the moment a transfer
 * ends.  At 4 it waited its turn behind the drawer after each of a read's nine
 * transfers, and a step longer than 1.5 ms loses a 6-button pad its count:
 * steps over that in 10 s of Sonic, on reads of the ESP-07 stand-in (6 Nov
 * 2026) - with the I/O lock held across I2C (the drawer queued behind the pad
 * and lent it its priority) 136-236 of 470-580; with the lock let go (io.c)
 * 273-300 of ~290, and he saw the pad press buttons by itself; at 5, 0-2 of
 * 630.
 */
#ifndef PAD_PRIO
#define PAD_PRIO 5
#endif

/*
 * once: sent at the press and not again while held.  B and C also give
 * Backspace and Tab, the file manager's folder-up and other-panel (FM.AXE
 * with /pad, which a board without a keyboard boots into): a game reads
 * neither, and a key repeated there would climb a folder every 60 ms.
 */
static const struct {
    uint16_t bit;
    uint16_t key;
    uint8_t  once;
} k_keys[] = {
    {AG_SP_UP, AG_KEY_UP, 0},       {AG_SP_DOWN, AG_KEY_DOWN, 0},
    {AG_SP_LEFT, AG_KEY_LEFT, 0},   {AG_SP_RIGHT, AG_KEY_RIGHT, 0},
    {AG_SP_A, AG_KEY_Z, 0},         {AG_SP_B, AG_KEY_X, 0},
    {AG_SP_C, AG_KEY_C, 0},         {AG_SP_START, AG_KEY_ENTER, 0},
    {AG_SP_X, AG_KEY_A, 0},         {AG_SP_Y, AG_KEY_S, 0},
    {AG_SP_Z, AG_KEY_D, 0},         {AG_SP_MODE, AG_KEY_M, 0},
    {SP_MENU, AG_KEY_ESC, 0},
    {AG_SP_B, AG_KEY_BACKSPACE, 1}, {AG_SP_C, AG_KEY_TAB, 1},
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
    uint32_t          sent_at[16]; /* when each key last went out down    */
    uint32_t          good_at;  /* the last read that found a pad          */
    uint32_t          start_at; /* Start went down here, not yet sent; 0 none */
    int               start_out; /* Start has gone out                       */
    int               menu;      /* Start and Mode were held together        */
    uint32_t          errs;
    uint32_t          reads; /* since the last tally in the journal     */
    uint32_t          slow;  /* of them, with a step over STEP_SLOW_US  */
    uint32_t          worst; /* the longest step among them, us         */
    uint32_t          no6;   /* of them, a 6-button pad answering as a 3 */
    int               no_steps; /* the kernel has no i2c_steps: a transfer a step */
    uint32_t          pad_look_at; /* when the expander was last looked for */
    uint32_t          retried;  /* reads asked again for the 6-button step */
    uint32_t          told_at, tallies;
    /* The keyboard on the same bus (apps/common/cardkb): the ESP-07's page
     * typed on a phone, or a real CardKB.  kb_addr 0 while none answers. */
    uint8_t           kb_addr;
    uint8_t           kb_want;     /* where to look for one                  */
    uint32_t          kb_look_at;  /* when it was last looked for            */
    uint16_t          kb_down;     /* the key that went down, not up yet      */
    uint32_t          kb_down_at;
    int               kb_mask;     /* the stand-in's mask on the last key    */
    uint8_t           kb_key;
    uint32_t          kb_keys, kb_twice, kb_garbled, kb_held_back;
    int               kb_checked;  /* it has sent a checked key: the stand-in */
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
            s.sent_at[i] = ag_millis();
        } else if (!(now & b) && (was & b)) {
            send_key(k_keys[i].key, 0, 0);
        }
    }
    s.held = pad;
    s.out = now;
}

/*
 * Each held key again, REPEAT_MS after it last went out, by its own clock and
 * whether or not this read found anything: the console lets go of a key that
 * has not come again for 150 ms.  The repeat was one clock for all of them,
 * put back by every change and run only on a read two in a row agreed with -
 * so Down, held while A was pressed again and again, went out less and less
 * often, and while the stand-in dropped reads not at all.
 */
static void repeat_held(void)
{
    const uint32_t t = ag_millis();
    for (unsigned i = 0; i < sizeof(k_keys) / sizeof(k_keys[0]); i++) {
        if ((s.out & k_keys[i].bit) && !k_keys[i].once &&
            t - s.sent_at[i] >= REPEAT_MS) {
            send_key(k_keys[i].key, 1, 1);
            s.sent_at[i] = t;
        }
    }
}

/* ---- the keyboard ------------------------------------------------------- */

#define KB_UP_MS   30u   /* a typed key is a press, not a hold              */
#define KB_LOOK_MS 2000u /* how often to look again for a keyboard not there */
#define KB_READS   4     /* keys taken in one round at most                  */

static bool kb_send(uint16_t key, uint32_t unicode, uint16_t mods, int down)
{
    ag_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = down ? AG_EV_KEY_DOWN : AG_EV_KEY_UP;
    ev.key.keycode = key;
    ev.key.unicode = down ? unicode : 0u;
    ev.key.mods = mods;
    return ag_inject_event(&ev);
}

/* The key that is down let go; false while the console has no room for it,
 * and it is let go on a later round. */
static bool kb_up(void)
{
    if (s.kb_down != 0u) {
        if (!kb_send(s.kb_down, 0, 0, 0)) {
            return false;
        }
        s.kb_down = 0;
    }
    return true;
}

/*
 * The keys typed since the last round, after the pad's read and before its
 * next: a read here is one transfer, and the 6-button pad needs its 1.5 ms of
 * quiet between reads anyway.  A read whose two bytes disagree is left for the
 * next round - the stand-in keeps the key until it is told it was taken - and
 * the same key under the same mask is one taken already whose "taken" was
 * lost, taken again and not typed twice ("WWorld", 7 Nov 2026).
 */
static void kb_poll(void)
{
    const uint32_t now = ag_millis();
    if (s.kb_down != 0u && now - s.kb_down_at >= KB_UP_MS) {
        (void)kb_up();
    }
    if (s.kb_addr == 0u) {
        /* The stand-in boots with the board and may answer later than the
         * driver loads: look again now and then. */
        if (now - s.kb_look_at >= KB_LOOK_MS) {
            s.kb_look_at = now;
            if (ag_i2c_probe(s.bus, s.kb_want) == AG_OK) {
                s.kb_addr = s.kb_want;
                ag_log(AG_LOG_INFO, "SEGAPAD", "a keyboard at 0x%02x",
                       (unsigned)s.kb_addr);
            }
        }
        return;
    }
    for (int n = 0; n < KB_READS; n++) {
        uint8_t rd[2] = {0, 0};
        if (ag_i2c_read(s.bus, s.kb_addr, rd, sizeof(rd), BUS_MS) != AG_OK) {
            return;
        }
        int mask = -1;
        int said = ag_cardkb_judge(rd[0], rd[1], &mask);
        if (said == AG_CARDKB_NONE) {
            return;
        }
        /*
         * 0xFF after the key is a real CardKB, which sends one byte - or the
         * stand-in, which sends two, losing its second to the radio and
         * leaving the line high.  Taken as a CardKB's, that key went in
         * unchecked and untaken, came again, and went in twice: 627 keys for
         * 600 sent (7 Nov 2026).  So once this keyboard has sent one checked
         * key, it is the stand-in, and an unchecked read is a garbled one.
         */
        if (said == AG_CARDKB_PLAIN && s.kb_checked) {
            said = AG_CARDKB_GARBLED;
        }
        if (said == AG_CARDKB_KEY) {
            s.kb_checked = 1;
        }
        if (said == AG_CARDKB_GARBLED) {
            s.kb_garbled++;
            return;
        }
        const uint8_t took = 0u;
        if (said == AG_CARDKB_KEY && mask == s.kb_mask && rd[0] == s.kb_key) {
            /* typed already; its "taken" was lost */
            (void)ag_i2c_write(s.bus, s.kb_addr, &took, 1, BUS_MS);
            s.kb_twice++;
            continue;
        }
        uint32_t ch = 0;
        uint16_t mods = 0;
        const uint16_t key = ag_cardkb_key(rd[0], &ch, &mods);
        if (key != 0u) {
            if (!kb_up()) { /* the one before, typed faster than KB_UP_MS */
                s.kb_held_back++;
                return;
            }
            /* The console's queue full: the key stays with the stand-in, not
             * taken, and comes again next round - a paste is typed only as
             * fast as the shell reads it. */
            if (!kb_send(key, ch, mods, 1)) {
                s.kb_held_back++;
                return;
            }
            s.kb_down = key;
            s.kb_down_at = ag_millis();
            s.kb_keys++;
        }
        if (said == AG_CARDKB_KEY) {
            (void)ag_i2c_write(s.bus, s.kb_addr, &took, 1, BUS_MS);
            s.kb_mask = mask;
            s.kb_key = rd[0];
        }
    }
}

/* ---- the pad ------------------------------------------------------------ */

/*
 * One read as six steps under repeated STARTs (ag_i2c_steps, ABI 0.78): two
 * transactions on the ESP32 where eight transfers were, with the bus's own
 * pace between the edges instead of the program's.  The pad on the CYD (a
 * 6-button one at 3.3 V) forgets its count after ~400 us without an edge:
 * measured 9 Nov 2026 with a pause before step 5, kept at +50 us and lost at
 * +100 on 330 us steps.  A transfer each, at 400 kHz, kept it at rest, and
 * lost it in 4-12% of the reads while The Lost Vikings ran - and in a third
 * while MD also printed its stats.  Steps 2 and 4 are only edges (the decode
 * reads 0 1 3 5 6 7), so each goes out as a second byte of the step after
 * it; the PCF8574 sets its port at each byte's ACK.
 *
 * 1 when it read, 0 when the kernel has no i2c_steps (before 0.78), -1 when
 * the bus failed.
 */
static int read_pad_steps(uint8_t rd[AG_SEGAPAD_STEPS])
{
    static const uint8_t hi = 0xffu;
    static const uint8_t lo = (uint8_t)(0xffu & ~AG_SEGAPAD_SELECT);
    static const uint8_t hilo[2] = {0xffu, (uint8_t)(0xffu & ~AG_SEGAPAD_SELECT)};
    const ag_i2c_step_t steps[] = {
        {.wbuf = &hi, .rbuf = &rd[0], .wlen = 1, .rlen = 1},
        {.wbuf = &lo, .rbuf = &rd[1], .wlen = 1, .rlen = 1},
        {.wbuf = hilo, .rbuf = &rd[3], .wlen = 2, .rlen = 1},
        {.wbuf = hilo, .rbuf = &rd[5], .wlen = 2, .rlen = 1},
        {.wbuf = &hi, .rbuf = &rd[6], .wlen = 1, .rlen = 1},
        {.wbuf = &lo, .rbuf = &rd[7], .wlen = 1, .rlen = 1},
    };
    const ag_err_t err = ag_i2c_steps(s.bus, s.addr, steps,
                                      sizeof(steps) / sizeof(steps[0]), BUS_MS);
    if (err == -AG_ENOSYS) {
        return 0;
    }
    if (err != AG_OK) {
        return -1;
    }
    rd[2] = rd[0];
    rd[4] = rd[0];
    return 1;
}

/* One read: eight steps, then SELECT back high.  -1 when the bus failed. */
static int read_pad(uint16_t *held)
{
    uint8_t rd[AG_SEGAPAD_STEPS];
    uint32_t worst = 0;
    s.reads++;
    if (!s.no_steps) {
        const ag_time_t t0 = ag_micros();
        const int r = read_pad_steps(rd);
        if (r != 0) {
            const uint8_t idle = ag_segapad_out(0);
            (void)ag_i2c_write(s.bus, s.addr, &idle, 1, BUS_MS);
            if (r < 0) {
                return -1;
            }
            worst = (uint32_t)(ag_micros() - t0);
            if (worst > s.worst) {
                s.worst = worst;
            }
            return ag_segapad_decode(rd, held);
        }
        s.no_steps = 1;
    }
    for (int i = 0; i < AG_SEGAPAD_STEPS; i++) {
        const uint8_t   out = ag_segapad_out(i);
        const ag_time_t t0 = ag_micros();
        if (ag_i2c_wrrd(s.bus, s.addr, &out, 1, &rd[i], 1, BUS_MS) != AG_OK) {
            const uint8_t idle = 0xffu;
            (void)ag_i2c_write(s.bus, s.addr, &idle, 1, BUS_MS);
            return -1;
        }
        const uint32_t took = (uint32_t)(ag_micros() - t0);
        worst = (took > worst) ? took : worst;
    }
    if (worst > STEP_SLOW_US) {
        s.slow++;
    }
    if (worst > s.worst) {
        s.worst = worst;
    }
    const uint8_t idle = ag_segapad_out(0);
    (void)ag_i2c_write(s.bus, s.addr, &idle, 1, BUS_MS);
    return ag_segapad_decode(rd, held);
}

static const char *kind_name(int k)
{
    return k == AG_SP_6BTN ? "a 6-button pad" : "a 3-button pad";
}

static uint8_t pad_look(void);

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
        if (s.addr == 0u) { /* no pad (yet): a keyboard, perhaps */
            if (t0 - s.pad_look_at >= PAD_LOOK_MS) {
                s.pad_look_at = t0;
                s.addr = pad_look();
                if (s.addr != 0u) {
                    const uint8_t idle = 0xffu;
                    (void)ag_i2c_write(s.bus, s.addr, &idle, 1, BUS_MS);
                    ag_log(AG_LOG_INFO, "SEGAPAD", "PCF8574 at 0x%02x",
                           (unsigned)s.addr);
                }
            }
            kb_poll();
            ag_delay(POLL_MS);
            continue;
        }
        uint16_t  held = 0;
        int       kind = read_pad(&held);
        if (kind == AG_SP_3BTN && s.kind == AG_SP_6BTN) {
            /* The count ran out somewhere in the read: once more, after the
             * quiet that starts a new one - two transactions on the ESP32
             * leave one gap a busy core can stretch past ~400 us, 1-2% of
             * reads while MD ran (9 Nov 2026), and X Y Z Mode would wait for
             * the next read. */
            s.retried++;
            ag_delay(2);
            kind = read_pad(&held);
        }
        if (kind < 0) {
            s.errs++;
        }
        if (kind <= 0) {
            /* Not let go on one bad read: a pad is not pulled out in 8 ms. */
            if (t0 - s.good_at >= GONE_MS && s.kind != AG_SP_NONE) {
                apply(0);
                ag_log(AG_LOG_WARN, "SEGAPAD", "the pad is gone (%s)",
                       kind < 0 ? "the bus does not answer"
                                : "nothing on the port");
                s.kind = AG_SP_NONE;
            }
        } else {
            s.good_at = t0;
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
                s.no6++;
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
        if (t0 - s.told_at >= TALLY_MS && s.tallies < TALLIES) {
            /* evidence while it runs: whether the reads keep the 6-button
             * pad's count (the CYD's MD drawer once held them up, 6 Nov) */
            s.tallies++;
            ag_log(AG_LOG_INFO, "SEGAPAD", "%u reads, %u with a step over %u us, "
                   "the longest %u us, %u asked again and %u without the "
                   "6-button step; %u bus errors so far", (unsigned)s.reads,
                   (unsigned)s.slow, (unsigned)STEP_SLOW_US, (unsigned)s.worst,
                   (unsigned)s.retried, (unsigned)s.no6, (unsigned)s.errs);
            s.told_at = t0;
            s.reads = 0;
            s.slow = 0;
            s.worst = 0;
            s.no6 = 0;
            s.retried = 0;
        }
        repeat_held();
        kb_poll();
        ag_delay(POLL_MS);
    }
    apply(0);
    (void)kb_up();
    if (s.kb_keys != 0u || s.kb_garbled != 0u) {
        ag_log(AG_LOG_INFO, "SEGAPAD", "keyboard: %u keys, %u read twice, %u "
               "garbled reads, %u held back for the console",
               (unsigned)s.kb_keys, (unsigned)s.kb_twice,
               (unsigned)s.kb_garbled, (unsigned)s.kb_held_back);
    }
}

static void pad_fini(void)
{
    s.stop = 1;
}

static int found(uint8_t a)
{
    return ag_i2c_probe(s.bus, a) == AG_OK;
}

/* The expander, at 0x20..0x27 (PCF8574) or 0x38..0x3f (PCF8574A); 0 if none */
static uint8_t pad_look(void)
{
    for (uint8_t a = 0x20; a <= 0x3f; a++) {
        if ((a <= 0x27 || a >= 0x38) && found(a)) {
            return a;
        }
    }
    return 0;
}

ag_err_t ag_driver_init(void)
{
    const ag_cfg_api_t *cfg = ag_api()->cfg;
    const int32_t bus = (cfg != NULL) ? cfg->get_int("pad.bus", 0) : 0;
    const int32_t addr = (cfg != NULL) ? cfg->get_int("pad.addr", 0) : 0;
    const int32_t kb = (cfg != NULL) ? cfg->get_int("kbd.addr", AG_CARDKB_ADDR)
                                     : (int32_t)AG_CARDKB_ADDR;
    memset(&s, 0, sizeof(s));
    s.kb_mask = -1;
    s.kb_want = (kb >= 0x08 && kb <= 0x77) ? (uint8_t)kb : (uint8_t)AG_CARDKB_ADDR;
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
        /* A few times: from cold, a board's own 3.3 V brings the expander
         * up with it, and on the CYD it had not answered yet when this
         * loaded (10 Nov 2026) - the driver failed and the pad was dead
         * until the next reset. */
        for (int tries = 0; tries < 6 && s.addr == 0u; tries++) {
            if (tries > 0) {
                ag_delay(50);
            }
            s.addr = pad_look();
        }
    }
    /* The keyboard: the ESP-07 stand-in answers there beside its pad, and a
     * real CardKB on its own. */
    if (found(s.kb_want)) {
        s.kb_addr = s.kb_want;
    }
    s.kb_look_at = ag_millis();
    if (s.addr == 0u && s.kb_addr == 0u) {
        /* Loaded all the same, and looked for again every PAD_LOOK_MS: a pad
         * plugged in later is a pad, and so is one that was slow to wake. */
        ag_printf("SEGAPAD: no PCF8574 on i2c%d at 0x20..0x27 or 0x38..0x3f "
                  "yet, and no keyboard at 0x%02x; looking\n", s.bus,
                  (unsigned)s.kb_want);
    }

    const uint8_t idle = 0xffu;
    if (s.addr != 0u) {
        (void)ag_i2c_write(s.bus, s.addr, &idle, 1, BUS_MS);
    }
    ag_module_on_unload(pad_fini);
    if (!ag_module_task(pad_task, NULL, "ag_segapad", 3072u, PAD_PRIO,
                        AG_THREAD_SYS_CORE)) {
        return -AG_ENOMEM;
    }
    if (s.addr != 0u) {
        ag_printf("SEGAPAD: PCF8574 on i2c%d at 0x%02x, read every %u ms\n",
                  s.bus, (unsigned)s.addr, (unsigned)POLL_MS);
    }
    ag_printf("SEGAPAD: keyboard %s 0x%02x\n",
              s.kb_addr != 0u ? "at" : "not there yet, looking at",
              (unsigned)s.kb_want);
    return AG_OK;
}
