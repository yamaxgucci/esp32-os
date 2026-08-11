/*
 * ArgonOS file manager - text console UI backend.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "fm_ui.h"

#include <argon/argon.h>

void fm_ui_begin(void)
{
    ag_cursor(false);
    ag_cls();
}

void fm_ui_end(void)
{
    ag_color(AG_LGRAY, AG_BLACK);
    ag_cls();
    ag_cursor(true);
}

void fm_ui_cls(void) { ag_cls(); }

void fm_ui_poke(int x, int y, char ch, uint8_t attr)
{
    ag_poke((uint16_t)x, (uint16_t)y, ch, attr);
}

void fm_ui_fill(int x, int y, int w, int h, char ch, uint8_t attr)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    ag_fill((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, ch, attr);
}

void fm_ui_frame(int x, int y, int w, int h, uint8_t attr)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    /* Characters a plain terminal can show; one byte a cell. */
    fm_ui_poke(x, y, '+', attr);
    fm_ui_poke(x + w - 1, y, '+', attr);
    fm_ui_poke(x, y + h - 1, '+', attr);
    fm_ui_poke(x + w - 1, y + h - 1, '+', attr);
    for (int i = 1; i < w - 1; i++) {
        fm_ui_poke(x + i, y, '-', attr);
        fm_ui_poke(x + i, y + h - 1, '-', attr);
    }
    for (int j = 1; j < h - 1; j++) {
        fm_ui_poke(x, y + j, '|', attr);
        fm_ui_poke(x + w - 1, y + j, '|', attr);
    }
}

void fm_ui_present(void) { /* console is immediate */ }

void fm_ui_set_cursor_visible(bool on) { ag_cursor(on); }
