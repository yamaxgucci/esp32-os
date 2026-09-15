/*
 * ArgonOS DESKTOP - where things go, worked out from the size of the screen.
 *
 * Pure C, and in its own file so the host tests can check the arithmetic on a
 * 320x240 panel and a 640x400 window without a display anywhere.  The chrome
 * itself does not scale - a Windows 3.x caption is eighteen pixels tall on any
 * screen, because it holds one line of an 8x16 font - so what changes with the
 * screen is only how much is left over.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "dsk.h"

/* The layout is one line of the shell's font per bar; see dsk_ui_h. */
#include "dsk_paint.h"

void dsk_metrics_init(dsk_metrics_t *m, int16_t w, int16_t h)
{
    if (m == NULL) {
        return;
    }
    m->screen_w = w;
    m->screen_h = h;
    /* One line of the shell's font, and a pixel each side. */
    m->menubar_h = (int16_t)(dsk_ui_h() + 2);
    m->title_h = (int16_t)(dsk_ui_h() + 2);
    m->border = 4;                   /* thick enough to grab with a pointer */
    m->statusbar_h = DSK_SMALL_H + 4;

    m->screen = dsk_rect(0, 0, w, h);
    m->menubar = dsk_rect(0, 0, w, m->menubar_h);
    m->statusbar =
        dsk_rect(0, (int16_t)(h - m->statusbar_h), w, m->statusbar_h);

    /*
     * A screen too short for both strips keeps the menu and loses the status
     * line: one of them is how the shell is operated and the other is how it
     * is read.  Forty-eight pixels is two rows of a folder listing plus its
     * caption - below that the work area is not a place windows can go, and
     * the strips are what should give way rather than the windows.
     *
     * The 160x120 surface on the CYD clears this with ninety pixels to spare;
     * what this guards is the surfaces nobody has configured yet.
     */
    int16_t top = m->menubar_h;
    int16_t bottom = (int16_t)(h - m->statusbar_h);
    if (bottom - top < 48) {
        m->statusbar = dsk_rect_none();
        bottom = h;
    }
    if (bottom - top < 16) {
        m->menubar = dsk_rect_none();
        top = 0;
        bottom = h;
    }
    m->work = dsk_rect(0, top, w, (int16_t)(bottom - top));
}
