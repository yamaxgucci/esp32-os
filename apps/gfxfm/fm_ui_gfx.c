/*
 * ArgonOS GFXFM - soft-gfx cell UI backend (80×25 @ 8×16 → 640×400).
 *
 * Panels and dialogs use fill_rect / stroke_rect so the UI reads as graphics,
 * not a bitmap of the text-mode +/-/| chrome.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "fm_ui.h"

#include <argon/argon.h>

#include "fm.h"

#define CELL_W 8
#define CELL_H 16

/* CGA / BIOS text colours as 0x00RRGGBB (same order as console attrs). */
static const uint32_t k_cga_rgb[16] = {
    0x00000000u, 0x000000AAu, 0x0000AA00u, 0x0000AAAAu, 0x00AA0000u,
    0x00AA00AAu, 0x00AA5500u, 0x00AAAAAAu, 0x00555555u, 0x005555FFu,
    0x0055FF55u, 0x0055FFFFu, 0x00FF5555u, 0x00FF55FFu, 0x00FFFF55u,
    0x00FFFFFFu,
};

static bool     s_have_gfx;
static uint16_t s_fb_w;
static uint16_t s_fb_h;

static uint32_t fg_of(uint8_t attr)
{
    return k_cga_rgb[attr & 0x0Fu];
}

static uint32_t bg_of(uint8_t attr)
{
    return k_cga_rgb[(attr >> 4) & 0x0Fu];
}

static void cell_rect(int x, int y, int w, int h, int16_t *px, int16_t *py,
                      uint16_t *pw, uint16_t *ph)
{
    *px = (int16_t)(x * CELL_W);
    *py = (int16_t)(y * CELL_H);
    *pw = (uint16_t)(w * CELL_W);
    *ph = (uint16_t)(h * CELL_H);
}

void fm_ui_begin(void)
{
    s_have_gfx = false;
    if (ag_api()->gfx == NULL) {
        return;
    }
    ag_gfxinfo_t info;
    if (ag_gfx_acquire(&info) != AG_OK) {
        return;
    }
    s_have_gfx = true;
    s_fb_w = info.width;
    s_fb_h = info.height;
    ag_gfx_clip_reset();
    ag_gfx_clear(0x00000000u);
}

void fm_ui_end(void)
{
    if (s_have_gfx) {
        ag_gfx_release();
        s_have_gfx = false;
    }
    ag_color(AG_LGRAY, AG_BLACK);
    ag_cls();
    ag_cursor(true);
}

void fm_ui_cls(void)
{
    if (s_have_gfx) {
        ag_gfx_clip_reset();
        ag_gfx_clear(0x00000000u);
        return;
    }
    ag_cls();
}

void fm_ui_poke(int x, int y, char ch, uint8_t attr)
{
    if (!s_have_gfx) {
        ag_poke((uint16_t)x, (uint16_t)y, ch, attr);
        return;
    }
    if (x < 0 || y < 0 || x >= FM_COLS || y >= FM_ROWS) {
        return;
    }
    const int16_t px = (int16_t)(x * CELL_W);
    const int16_t py = (int16_t)(y * CELL_H);
    if ((uint16_t)(px + CELL_W) > s_fb_w || (uint16_t)(py + CELL_H) > s_fb_h) {
        return;
    }
    const uint32_t fg = fg_of(attr);
    const uint32_t bg = bg_of(attr);

    /* Spaces: solid cell fill (no glyph), so panel blue stays a real rect. */
    if (ch == ' ') {
        ag_gfx_fill_rect(px, py, CELL_W, CELL_H, bg);
        return;
    }

    /* Skip ASCII box-drawing leftovers if any caller still emits them. */
    if (ch == '+' || ch == '-' || ch == '|' || ch == '=') {
        ag_gfx_fill_rect(px, py, CELL_W, CELL_H, bg);
        return;
    }

    char one[2] = {ch, '\0'};
    (void)ag_gfx_text(px, py, one, fg, bg);
}

void fm_ui_fill(int x, int y, int w, int h, char ch, uint8_t attr)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    if (!s_have_gfx) {
        ag_fill((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, ch, attr);
        return;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > FM_COLS) {
        w = FM_COLS - x;
    }
    if (y + h > FM_ROWS) {
        h = FM_ROWS - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    int16_t  px, py;
    uint16_t pw, ph;
    cell_rect(x, y, w, h, &px, &py, &pw, &ph);
    const uint32_t bg = bg_of(attr);

    if (ch == ' ' || ch == '\0') {
        if (attr == FM_ATTR_DIALOG && w >= 6 && h >= 3) {
            ag_gfx_fill_round_rect(px, py, pw, ph, 6, bg);
        } else {
            ag_gfx_fill_rect(px, py, pw, ph, bg);
        }
        return;
    }

    ag_gfx_fill_rect(px, py, pw, ph, bg);
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            fm_ui_poke(x + col, y + row, ch, attr);
        }
    }
}

void fm_ui_frame(int x, int y, int w, int h, uint8_t attr)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    if (!s_have_gfx) {
        /* Text path should not reach here without fm_ui_text.c linked. */
        return;
    }

    int16_t  px, py;
    uint16_t pw, ph;
    cell_rect(x, y, w, h, &px, &py, &pw, &ph);
    const uint32_t fg = fg_of(attr);
    const uint32_t bg = bg_of(attr);

    /* Interior: real fill (rounded for dialogs). Border: pixel strokes. */
    if (attr == FM_ATTR_DIALOG && w >= 6 && h >= 3) {
        ag_gfx_fill_round_rect(px, py, pw, ph, 6, bg);
    } else {
        ag_gfx_fill_rect(px, py, pw, ph, bg);
    }

    ag_gfx_stroke_rect(px, py, pw, ph, fg);
    if (pw > 4 && ph > 4) {
        ag_gfx_stroke_rect((int16_t)(px + 1), (int16_t)(py + 1),
                           (uint16_t)(pw - 2), (uint16_t)(ph - 2), fg);
    }
}

void fm_ui_present(void)
{
    if (!s_have_gfx) {
        return;
    }
    ag_gfx_flush(0, 0, 0, 0);
}

void fm_ui_set_cursor_visible(bool on)
{
    if (!s_have_gfx) {
        ag_cursor(on);
    }
}
