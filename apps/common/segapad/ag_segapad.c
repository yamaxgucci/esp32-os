/*
 * ArgonOS - a Mega Drive pad read through an 8-bit port.  See ag_segapad.h.
 *
 * Freestanding: no libc, so it links into a .SYS, the host tests and the
 * ESP8266 firmware alike.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "ag_segapad.h"

/* A line is low while its button is held: the bit is the button released. */
AG_SEGAPAD_FN static int low(uint8_t b, int bit)
{
    return ((b >> bit) & 1u) == 0u;
}

AG_SEGAPAD_FN int ag_segapad_decode(const uint8_t rd[AG_SEGAPAD_STEPS], uint16_t *held)
{
    uint16_t h = 0;
    *held = 0;

    /*
     * Left and right low with SELECT low is the pad saying it is there; with
     * nothing on the port every line floats high and this is what fails.  The
     * second low step is asked too, so that one read garbled on the wire is
     * not taken for a pad.
     */
    if (!low(rd[1], 2) || !low(rd[1], 3) || !low(rd[3], 2) ||
        !low(rd[3], 3)) {
        return AG_SP_NONE;
    }

    if (low(rd[0], 0)) h |= AG_SP_UP;
    if (low(rd[0], 1)) h |= AG_SP_DOWN;
    if (low(rd[0], 2)) h |= AG_SP_LEFT;
    if (low(rd[0], 3)) h |= AG_SP_RIGHT;
    if (low(rd[0], 4)) h |= AG_SP_B;
    if (low(rd[0], 5)) h |= AG_SP_C;
    if (low(rd[1], 4)) h |= AG_SP_A;
    if (low(rd[1], 5)) h |= AG_SP_START;

    /*
     * Up and down low together on the third fall is the 6-button pad's mark.
     * A 3-button pad shows the d-pad there, and a d-pad cannot hold up and down
     * at once - so the mark cannot be a thumb.
     */
    /*
     * And the fourth fall shows all four high, which no 3-button step does
     * (its low steps hold left and right low).  Without that the count may
     * have run out between the third fall and the next step - one step that
     * took over 1.5 ms, which a busy core does - and the step read as X, Y,
     * Z and Mode was the d-pad: right held came out as Mode.  Such a read
     * still has the d-pad and the buttons right, so it counts as a 3-button
     * one.
     */
    if (!(low(rd[5], 0) && low(rd[5], 1) && low(rd[5], 2) &&
          low(rd[5], 3)) ||
        low(rd[7], 0) || low(rd[7], 1) || low(rd[7], 2) || low(rd[7], 3)) {
        *held = h;
        return AG_SP_3BTN;
    }
    if (low(rd[6], 0)) h |= AG_SP_Z;
    if (low(rd[6], 1)) h |= AG_SP_Y;
    if (low(rd[6], 2)) h |= AG_SP_X;
    if (low(rd[6], 3)) h |= AG_SP_MODE;
    *held = h;
    return AG_SP_6BTN;
}

AG_SEGAPAD_FN void ag_segapad_model_init(ag_segapad_model_t *m, int six)
{
    m->held = 0;
    m->six = six ? 1u : 0u;
    m->sel = 1u;
    m->falls = 0u;
    m->moved = 0u;
}

/* The counter forgets after this long with SELECT still. */
#define QUIET_US 1500u

AG_SEGAPAD_FN void ag_segapad_model_select(ag_segapad_model_t *m, int high, uint32_t us)
{
    const uint8_t s = high ? 1u : 0u;
    /* The pad's own timer, whether the line moved or not: a read that starts
     * with SELECT already high, as every read does, finds the count gone. */
    if (us - m->moved > QUIET_US) {
        m->falls = 0u;
    }
    if (s == m->sel) {
        return;
    }
    if (s == 0u && m->falls < 255u) {
        m->falls++;
    }
    m->sel = s;
    m->moved = us;
}

/* Six lines as a byte, a line high unless its button is held. */
AG_SEGAPAD_FN static uint8_t lines(int l0, int l1, int l2, int l3, int l4, int l5)
{
    return (uint8_t)(0xc0u | (l0 ? 0u : 0x01u) | (l1 ? 0u : 0x02u) |
                     (l2 ? 0u : 0x04u) | (l3 ? 0u : 0x08u) |
                     (l4 ? 0u : 0x10u) | (l5 ? 0u : 0x20u));
}

AG_SEGAPAD_FN uint8_t ag_segapad_model_read(const ag_segapad_model_t *m)
{
    const uint16_t h = m->held;
    const int      six = m->six != 0u;
#define H(b) ((h & (b)) != 0u)
    if (m->sel) {
        if (six && m->falls == 3u) {
            return lines(H(AG_SP_Z), H(AG_SP_Y), H(AG_SP_X), H(AG_SP_MODE),
                         H(AG_SP_B), H(AG_SP_C));
        }
        return lines(H(AG_SP_UP), H(AG_SP_DOWN), H(AG_SP_LEFT),
                     H(AG_SP_RIGHT), H(AG_SP_B), H(AG_SP_C));
    }
    if (six && m->falls == 3u) {
        return lines(1, 1, 1, 1, H(AG_SP_A), H(AG_SP_START));
    }
    if (six && m->falls == 4u) {
        return lines(0, 0, 0, 0, H(AG_SP_A), H(AG_SP_START));
    }
    return lines(H(AG_SP_UP), H(AG_SP_DOWN), 1, 1, H(AG_SP_A),
                 H(AG_SP_START));
#undef H
}
