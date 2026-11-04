/*
 * ArgonOS - the Mega Drive pad read (apps/common/segapad), against the pad
 * model in the same file: every button alone and some together, on both kinds
 * of pad; a port with nothing on it; and a read that waited too little, so the
 * 6-button counter had not forgotten.
 *
 * What this proves is that the driver and the model agree.  Whether the model
 * agrees with a pad is a question for a pad on the bench.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../apps/common/segapad/ag_segapad.h"

#include "test.h"

/* One read as SEGAPAD.SYS does it: each step written, then read back.  Step
 * `slow` comes 2 ms after the one before it, as when the task that reads is
 * held off mid-read; -1 for none. */
static int read_pad_slow(ag_segapad_model_t *m, uint32_t *us, uint16_t *held,
                         int slow)
{
    uint8_t rd[AG_SEGAPAD_STEPS];
    for (int i = 0; i < AG_SEGAPAD_STEPS; i++) {
        if (i == slow) {
            *us += 2000u;
        }
        const uint8_t out = ag_segapad_out(i);
        ag_segapad_model_select(m, (out & AG_SEGAPAD_SELECT) != 0u, *us);
        rd[i] = ag_segapad_model_read(m);
        *us += 200u; /* a write and a read at 100 kHz, about */
    }
    ag_segapad_model_select(m, 1, *us);
    return ag_segapad_decode(rd, held);
}

static int read_pad(ag_segapad_model_t *m, uint32_t *us, uint16_t *held)
{
    return read_pad_slow(m, us, held, -1);
}

static void each_button(int six)
{
    static const uint16_t k_three[] = {
        AG_SP_UP, AG_SP_DOWN, AG_SP_LEFT, AG_SP_RIGHT,
        AG_SP_A,  AG_SP_B,    AG_SP_C,    AG_SP_START,
    };
    ag_segapad_model_t m;
    uint32_t           us = 1000u;
    uint16_t           held = 0;
    ag_segapad_model_init(&m, six);

    const unsigned n = six ? 12u : 8u;
    for (unsigned i = 0; i < n; i++) {
        m.held = (i < 8u) ? k_three[i] : (uint16_t)(AG_SP_X << (i - 8u));
        AG_CHECK_INT(read_pad(&m, &us, &held), six ? AG_SP_6BTN : AG_SP_3BTN);
        AG_CHECK_INT(held, m.held);
        us += 10000u;
    }

    /* Nothing held, then a fistful: d-pad diagonal and every face button. */
    m.held = 0;
    AG_CHECK_INT(read_pad(&m, &us, &held), six ? AG_SP_6BTN : AG_SP_3BTN);
    AG_CHECK_INT(held, 0);
    us += 10000u;
    m.held = AG_SP_UP | AG_SP_LEFT | AG_SP_A | AG_SP_B | AG_SP_C |
             AG_SP_START;
    if (six) {
        m.held |= AG_SP_X | AG_SP_Y | AG_SP_Z | AG_SP_MODE;
    }
    read_pad(&m, &us, &held);
    AG_CHECK_INT(held, m.held);
}

static void nothing_there(void)
{
    uint8_t  rd[AG_SEGAPAD_STEPS];
    uint16_t held = 1;
    for (int i = 0; i < AG_SEGAPAD_STEPS; i++) {
        rd[i] = 0xffu; /* pull-ups and no pad */
    }
    AG_CHECK_INT(ag_segapad_decode(rd, &held), AG_SP_NONE);
    AG_CHECK_INT(held, 0);
}

/*
 * Two reads 1 ms apart: the counter has not gone quiet, so the second read
 * starts at the fourth fall.  It must not come out as buttons nobody holds.
 */
static void too_soon(void)
{
    ag_segapad_model_t m;
    uint32_t           us = 1000u;
    uint16_t           held = 0;
    ag_segapad_model_init(&m, 1);
    m.held = AG_SP_START;
    read_pad(&m, &us, &held);
    us += 1000u;
    const int kind = read_pad(&m, &us, &held);
    AG_CHECK(kind == AG_SP_NONE || (held & ~AG_SP_START) == 0u ||
             held == 0u);
}

/*
 * A step late anywhere in the read: the 6-button count runs out there and the
 * rest of the read is a 3-button pad's.  Whatever the step, the d-pad and the
 * buttons must come out right and no X, Y, Z or Mode appear - late after the
 * mark, the step read for them is the d-pad, and right held was taken for
 * Mode on the CYD.
 */
static void late_step(void)
{
    static const uint16_t k_held[] = {AG_SP_RIGHT, AG_SP_UP | AG_SP_LEFT,
                                      AG_SP_DOWN | AG_SP_RIGHT | AG_SP_C};
    for (unsigned h = 0; h < sizeof(k_held) / sizeof(k_held[0]); h++) {
        for (int slow = 1; slow < AG_SEGAPAD_STEPS; slow++) {
            ag_segapad_model_t m;
            uint32_t           us = 1000u;
            uint16_t           held = 0;
            ag_segapad_model_init(&m, 1);
            m.held = k_held[h];
            const int kind = read_pad_slow(&m, &us, &held, slow);
            AG_CHECK(kind == AG_SP_3BTN || kind == AG_SP_6BTN);
            AG_CHECK_INT(held, k_held[h]);
        }
    }
}

/*
 * A read given up after the mark - the bus failed - leaves the pad at its
 * third fall with SELECT put back high.  The next read, 8 ms on, starts with
 * no edge at all; the pad's timer has reset the count all the same, so it
 * must read Mode as Mode and not as right as well.
 */
static void after_a_broken_read(void)
{
    ag_segapad_model_t m;
    uint32_t           us = 1000u;
    uint16_t           held = 0;
    ag_segapad_model_init(&m, 1);
    m.held = AG_SP_MODE;
    for (int i = 0; i <= 5; i++) {
        ag_segapad_model_select(&m, (ag_segapad_out(i) & AG_SEGAPAD_SELECT) != 0u,
                                us);
        us += 200u;
    }
    ag_segapad_model_select(&m, 1, us);
    us += 8000u;
    AG_CHECK_INT(read_pad(&m, &us, &held), AG_SP_6BTN);
    AG_CHECK_INT(held, AG_SP_MODE);
}

void run_segapad_tests(void)
{
    each_button(0);
    each_button(1);
    nothing_there();
    too_soon();
    late_step();
    after_a_broken_read();
}
