/*
 * ArgonOS - MD's pad port (apps/md/core/io/gwenesis_io.c) read the ways games
 * read it: the 6-button routine, TH high and low four times back to back, must
 * find X Y Z and Mode at the fourth high step; a game that reads the pad once
 * a frame must only ever see a 3-button pad, which the count running out
 * between frames is for; and `pad3` must never show the 6-button steps.
 *
 * The file itself is compiled here, with its save state calls stubbed: they
 * are not what is tested.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../apps/md/core/io/gwenesis_io.c"
#include "../apps/md/core/bus/gwenesis_bus.h"

#include "test.h"

SaveState *saveGwenesisStateOpenForRead(const char *n) { (void)n; return NULL; }
SaveState *saveGwenesisStateOpenForWrite(const char *n) { (void)n; return NULL; }
void saveGwenesisStateGetBuffer(SaveState *s, const char *t, void *b, int l)
{
    (void)s; (void)t; (void)b; (void)l;
}
void saveGwenesisStateSetBuffer(SaveState *s, const char *t, void *b, int l)
{
    (void)s; (void)t; (void)b; (void)l;
}
void gwenesis_io_get_buttons(void) {}

/* Port 1 as the 68000 sees it: DATA at 0xA10003, CTRL at 0xA10009. */
static unsigned th(int high)
{
    gwenesis_io_write_ctrl(0x03, high ? 0x40u : 0x00u);
    return gwenesis_io_read_ctrl(0x03) & 0x3fu;
}

static void frame(void)
{
    for (int i = 0; i < 262; i++) {
        gwenesis_io_line();
    }
}

static void reset_pad(int six)
{
    for (int b = PAD_UP; b <= PAD_M; b++) {
        gwenesis_io_pad_release_button(0, b);
    }
    gwenesis_io_write_ctrl(0x09, 0x40u); /* TH an output */
    th(1);
    gwenesis_io_set_six(six);
    frame();
}

/* The 6-button routine; returns what the fourth high step shows. */
static unsigned six_read(unsigned *third_low)
{
    unsigned v = 0;
    th(1); th(0); th(1); th(0); th(1);
    *third_low = th(0);
    v = th(1);
    th(0);
    th(1);
    return v;
}

static void the_routine(void)
{
    static const struct { int pad; unsigned bit; } k[] = {
        {PAD_Z, 0x01u}, {PAD_Y, 0x02u}, {PAD_X, 0x04u}, {PAD_M, 0x08u},
    };
    unsigned low3 = 0;
    for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        reset_pad(1);
        gwenesis_io_pad_press_button(0, k[i].pad);
        AG_CHECK_INT(six_read(&low3), 0x3fu & ~k[i].bit);
        AG_CHECK_INT(low3 & 0x0fu, 0); /* the 6-button mark */
        frame();
    }
    /* Mode and right at once: Mode at its step, right at the first. */
    reset_pad(1);
    gwenesis_io_pad_press_button(0, PAD_M);
    gwenesis_io_pad_press_button(0, PAD_RIGHT);
    AG_CHECK_INT(th(1), 0x3fu & ~0x08u);
    frame();
    AG_CHECK_INT(six_read(&low3), 0x3fu & ~0x08u);
}

/* Once a frame, TH high then low then high: a rising edge a frame, which
 * without the timer would reach the fourth step on the fourth frame. */
static void once_a_frame(void)
{
    reset_pad(1);
    gwenesis_io_pad_press_button(0, PAD_RIGHT);
    for (int f = 0; f < 10; f++) {
        AG_CHECK_INT(th(1), 0x3fu & ~0x08u);
        AG_CHECK_INT(th(0) & 0x0cu, 0); /* left and right low: a pad */
        th(1);
        frame();
    }
}

static void three_button(void)
{
    unsigned low3 = 0;
    reset_pad(0);
    gwenesis_io_pad_press_button(0, PAD_X);
    gwenesis_io_pad_press_button(0, PAD_UP);
    AG_CHECK_INT(six_read(&low3), 0x3fu & ~0x01u); /* up, never X */
    AG_CHECK_INT(low3 & 0x0cu, 0);
    AG_CHECK_INT(low3 & 0x03u, 0x02u);              /* up low, down high */
    gwenesis_io_set_six(1);
}

void run_mdpad_tests(void)
{
    the_routine();
    once_a_frame();
    three_button();
}
