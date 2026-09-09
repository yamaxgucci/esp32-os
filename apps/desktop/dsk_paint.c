/*
 * ArgonOS DESKTOP - the painter, and what the shell draws with.
 *
 * Pure C: no argon.h anywhere in this file.  The six operations that touch
 * real pixels arrive through dsk_painter_t, bound either to the system surface
 * (dsk_paint_surface.c) or, in the unit tests, to a recorder - which is how
 * the window manager above it can be checked without a display.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "dsk_paint.h"

#include "../common/font8x8.h"

static const dsk_painter_t *s_p;

/*
 * The clip the shell asked for, kept here rather than read back out of the
 * backend: dsk_visible has to answer the same way whichever backend is bound,
 * including the recorder, which has no surface to clip against.
 */
static dsk_rect_t s_shell_clip;
static int16_t    s_shell_w, s_shell_h;

static uint16_t to565(uint32_t rgb)
{
    const uint32_t r = (rgb >> 16) & 0xFFu;
    const uint32_t g = (rgb >> 8) & 0xFFu;
    const uint32_t b = rgb & 0xFFu;
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

/* Set by the band backend after it binds itself; see dsk_paint_bind_bander. */
static const dsk_bander_t *s_bander;

void dsk_paint_bind(const dsk_painter_t *p, int16_t w, int16_t h)
{
    /*
     * A new backend draws its own way, so how a repaint is cut belongs to it
     * and not to whoever was bound before - and the recorder in the host
     * tests must never inherit a band driver.
     */
    s_bander = NULL;
    s_p = p;
    s_shell_clip = dsk_rect_none();
    s_shell_w = w;
    s_shell_h = h;
}

const dsk_painter_t *dsk_paint(void) { return s_p; }

/* ---- how a repaint is cut into pieces ----------------------------------- */

void dsk_paint_bind_bander(const dsk_bander_t *b) { s_bander = b; }
bool dsk_paint_banded(void) { return s_bander != NULL; }

void dsk_paint_region(dsk_rect_t r, void (*draw)(dsk_rect_t r))
{
    if (draw == NULL || dsk_rect_empty(r)) {
        return;
    }
    if (s_bander == NULL) {
        draw(r);
        s_p->flush(r);
        return;
    }
    /*
     * Bottom of the rectangle recomputed each time rather than kept: `begin`
     * decides how tall a strip it can afford for this width, and a narrow
     * rectangle gets one strip where a screen-wide one gets fifteen.
     */
    int16_t y = r.y;
    while (y < (int16_t)(r.y + r.h)) {
        const int16_t h = s_bander->begin(r, y);
        if (h <= 0) {
            return;
        }
        draw(dsk_rect(r.x, y, r.w, h));
        s_bander->present();
        y = (int16_t)(y + h);
    }
}

/* ---- what the shell calls ---------------------------------------------- */

void dsk_fill(dsk_rect_t r, uint32_t rgb) { s_p->fill(r, rgb); }
void dsk_flush(dsk_rect_t r) { s_p->flush(r); }

void dsk_clip(dsk_rect_t r)
{
    s_shell_clip = r;
    s_p->clip(r);
}

void dsk_clip_reset(void)
{
    s_shell_clip = dsk_rect_none();
    s_p->clip(dsk_rect_none());
}

dsk_rect_t dsk_clip_now(void)
{
    return dsk_rect_empty(s_shell_clip) ? dsk_rect(0, 0, s_shell_w, s_shell_h)
                                        : s_shell_clip;
}

bool dsk_visible(dsk_rect_t r) { return dsk_rect_overlaps(r, dsk_clip_now()); }

void dsk_text(int16_t x, int16_t y, const char *s, uint32_t fg, uint32_t bg)
{
    s_p->text(x, y, 0, s, fg, bg);
}

void dsk_text_fit(int16_t x, int16_t y, int16_t max_w, const char *s,
                  uint32_t fg, uint32_t bg)
{
    if (max_w <= 0) {
        return;
    }
    s_p->text(x, y, max_w, s, fg, bg);
}

/*
 * The small font, one glyph at a time through blit.
 *
 * Rendering into an 8x8 scratch and handing it over costs a copy per character
 * and keeps the backend at six operations, which is the trade this file exists
 * to make.  A string of twenty characters is twenty blits of a hundred and
 * twenty-eight bytes; the flush that follows costs more than all of them.
 */
void dsk_text_small(int16_t x, int16_t y, const char *s, uint32_t fg,
                    uint32_t bg)
{
    if (s == NULL) {
        return;
    }
    const uint16_t f = to565(fg);
    const uint16_t b = to565(bg);
    uint16_t       cell[DSK_SMALL_W * DSK_SMALL_H];
    int16_t        at = x;

    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        const uint8_t *rows = k_font8x8[*p];
        for (int row = 0; row < DSK_SMALL_H; row++) {
            const uint8_t bits = rows[row];
            for (int col = 0; col < DSK_SMALL_W; col++) {
                /* Bit 0 is the leftmost pixel (see apps/common/font8x8.h). */
                cell[row * DSK_SMALL_W + col] =
                    ((bits >> col) & 1u) ? f : b;
            }
        }
        s_p->blit(dsk_rect(at, y, DSK_SMALL_W, DSK_SMALL_H), cell,
                  DSK_SMALL_W);
        at = (int16_t)(at + DSK_SMALL_W);
    }
}

int16_t dsk_text_small_width(const char *s)
{
    int16_t n = 0;
    if (s == NULL) {
        return 0;
    }
    while (s[n] != '\0') {
        n++;
    }
    return (int16_t)(n * DSK_SMALL_W);
}

void dsk_hline(int16_t x, int16_t y, int16_t w, uint32_t rgb)
{
    s_p->fill(dsk_rect(x, y, w, 1), rgb);
}

void dsk_vline(int16_t x, int16_t y, int16_t h, uint32_t rgb)
{
    s_p->fill(dsk_rect(x, y, 1, h), rgb);
}

void dsk_frame(dsk_rect_t r, uint32_t rgb)
{
    if (dsk_rect_empty(r)) {
        return;
    }
    dsk_hline(r.x, r.y, r.w, rgb);
    dsk_hline(r.x, (int16_t)(r.y + r.h - 1), r.w, rgb);
    dsk_vline(r.x, r.y, r.h, rgb);
    dsk_vline((int16_t)(r.x + r.w - 1), r.y, r.h, rgb);
}

void dsk_bevel(dsk_rect_t r, bool raised)
{
    if (dsk_rect_empty(r)) {
        return;
    }
    const uint32_t light = raised ? DSK_WHITE : DSK_DGRAY;
    const uint32_t shade = raised ? DSK_DGRAY : DSK_WHITE;
    const int16_t  x2 = (int16_t)(r.x + r.w - 1);
    const int16_t  y2 = (int16_t)(r.y + r.h - 1);

    dsk_hline(r.x, r.y, r.w, light);
    dsk_vline(r.x, r.y, r.h, light);
    /* The shadow stops one short so the corners are not drawn twice. */
    dsk_hline((int16_t)(r.x + 1), y2, (int16_t)(r.w - 1), shade);
    dsk_vline(x2, (int16_t)(r.y + 1), (int16_t)(r.h - 1), shade);
}

void dsk_panel(dsk_rect_t r, bool raised, uint32_t face)
{
    s_p->fill(r, face);
    dsk_bevel(r, raised);
}
