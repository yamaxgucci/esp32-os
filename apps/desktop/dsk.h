/*
 * ArgonOS DESKTOP - the shell's own vocabulary: colours, metrics, screen state.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_DSK_H
#define ARGON_DSK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsk_rect.h"

/*
 * The sixteen VGA colours plus the two greys Windows 3.x is built out of.
 *
 * Written as 0x00RRGGBB because that is what the display takes, and chosen so
 * that the narrowing to RGB565 is exact: 0xC0 is 11000 in five bits and 110000
 * in six, and both come back as 0xC0.  A face colour that survives the round
 * trip is a face colour that matches the bevel drawn next to it, which is the
 * whole of why this palette and not a prettier one.
 */
#define DSK_BLACK   0x00000000u
#define DSK_NAVY    0x00000080u
#define DSK_GREEN   0x00008000u
#define DSK_TEAL    0x00008080u
#define DSK_MAROON  0x00800000u
#define DSK_PURPLE  0x00800080u
#define DSK_OLIVE   0x00808000u
#define DSK_LGRAY   0x00C0C0C0u /* the face of everything */
#define DSK_DGRAY   0x00808080u /* its shadow */
#define DSK_BLUE    0x000000FFu
#define DSK_LIME    0x0000FF00u
#define DSK_CYAN    0x0000FFFFu
#define DSK_RED     0x00FF0000u
#define DSK_MAGENTA 0x00FF00FFu
#define DSK_YELLOW  0x00FFFF00u
#define DSK_WHITE   0x00FFFFFFu

/* As a text background: leave the pixels the glyph does not cover alone. */
#define DSK_TRANS 0xFFFFFFFFu

/* The two fonts, and they are the only two (see docs/plans/desktop.md §3.5). */
#define DSK_FONT_W 8
#define DSK_FONT_H 16
#define DSK_SMALL_W 8
#define DSK_SMALL_H 8

/*
 * Everything whose size depends on the screen's, worked out once from what
 * acquire() reported.  Nothing below is a constant in the code: this shell has
 * to look right on a 320x240 panel and on a 640x400 window, and the way that
 * goes wrong is a number typed in twice.
 */
typedef struct {
    int16_t screen_w;
    int16_t screen_h;
    int16_t menubar_h;  /* the desktop's own menu strip, along the top */
    int16_t title_h;    /* a window's caption */
    int16_t border;     /* a window's frame, all four sides */
    int16_t statusbar_h;/* the strip along the bottom of the desktop */
    dsk_rect_t screen;  /* the whole surface */
    dsk_rect_t menubar;
    dsk_rect_t statusbar;
    dsk_rect_t work;    /* what is left for windows and icons */
} dsk_metrics_t;

void dsk_metrics_init(dsk_metrics_t *m, int16_t w, int16_t h);

#endif /* ARGON_DSK_H */
