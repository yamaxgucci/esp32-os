/*
 * ArgonOS DESKTOP - the painter over the acquired system surface.
 *
 * The backend clips every write itself against its own rectangle rather than
 * trusting the display's clip, with one exception: text, because the 8x16 font
 * lives in the kernel and only the kernel can draw with it.  So clip() sets
 * both, and the two have to mean the same thing - they do, x/y/w/h either way.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "dsk_paint.h"

#include <argon/argon.h>

#include "../common/font8x8.h"

static uint16_t  *s_fb;
static uint32_t   s_stride_px;
static int16_t    s_w;
static int16_t    s_h;
static dsk_rect_t s_clip; /* empty means the whole surface */

static const dsk_painter_t *s_p;

/*
 * The clip the shell asked for, kept here rather than read back out of the
 * backend: dsk_visible has to answer the same way whichever backend is bound,
 * including the host test's recorder, which has no surface to clip against.
 */
static dsk_rect_t s_shell_clip;
static int16_t    s_shell_w, s_shell_h;

static dsk_rect_t clip_now(void)
{
    return dsk_rect_empty(s_clip) ? dsk_rect(0, 0, s_w, s_h) : s_clip;
}

static uint16_t to565(uint32_t rgb)
{
    const uint32_t r = (rgb >> 16) & 0xFFu;
    const uint32_t g = (rgb >> 8) & 0xFFu;
    const uint32_t b = rgb & 0xFFu;
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

/* ---- the surface backend ------------------------------------------------ */

static void surf_fill(dsk_rect_t r, uint32_t rgb)
{
    const dsk_rect_t c = dsk_rect_clip(r, clip_now());
    if (dsk_rect_empty(c)) {
        return;
    }
    ag_gfx_fill_rect(c.x, c.y, (uint16_t)c.w, (uint16_t)c.h, rgb);
}

static void surf_blit(dsk_rect_t r, const uint16_t *src, uint32_t stride_px)
{
    if (s_fb == NULL || src == NULL) {
        return;
    }
    const dsk_rect_t c = dsk_rect_clip(r, clip_now());
    if (dsk_rect_empty(c)) {
        return;
    }
    /* Which part of the source survived the clip. */
    const int32_t sx = (int32_t)c.x - r.x;
    const int32_t sy = (int32_t)c.y - r.y;

    for (int32_t row = 0; row < c.h; row++) {
        const uint16_t *in = src + (size_t)(sy + row) * stride_px + sx;
        uint16_t       *out =
            s_fb + (size_t)(c.y + row) * s_stride_px + (size_t)c.x;
        for (int32_t i = 0; i < c.w; i++) {
            out[i] = in[i];
        }
    }
}

static void surf_text(int16_t x, int16_t y, int16_t max_w, const char *s,
                      uint32_t fg, uint32_t bg)
{
    if (s == NULL) {
        return;
    }
    if (max_w > 0) {
        (void)ag_gfx_text_fit(x, y, (uint16_t)max_w, s, fg, bg);
    } else {
        (void)ag_gfx_text(x, y, s, fg, bg);
    }
}

static void surf_clip(dsk_rect_t r)
{
    const dsk_rect_t whole = dsk_rect(0, 0, s_w, s_h);
    const dsk_rect_t c = dsk_rect_empty(r) ? whole : dsk_rect_clip(r, whole);

    s_clip = c;
    if (dsk_rect_empty(c)) {
        /*
         * A clip outside the surface entirely.  Nothing may be drawn, and the
         * display's own reset would mean the opposite, so pin it to one pixel
         * off the edge instead of resetting.
         */
        ag_gfx_clip(s_w, s_h, 1, 1);
        return;
    }
    if (dsk_rect_eq(c, whole)) {
        ag_gfx_clip_reset();
        return;
    }
    ag_gfx_clip(c.x, c.y, (uint16_t)c.w, (uint16_t)c.h);
}

static void surf_read(dsk_rect_t r, uint16_t *dst)
{
    if (s_fb == NULL || dst == NULL || dsk_rect_empty(r)) {
        return;
    }
    /*
     * Reads are not clipped by the shell's clip - a save-under has to come back
     * with exactly r.w * r.h pixels or its restore writes the wrong ones - but
     * they are clamped to the surface, and what falls outside reads as black.
     */
    for (int32_t row = 0; row < r.h; row++) {
        uint16_t      *out = dst + (size_t)row * r.w;
        const int32_t  sy = r.y + row;
        if (sy < 0 || sy >= s_h) {
            for (int32_t i = 0; i < r.w; i++) {
                out[i] = 0;
            }
            continue;
        }
        const uint16_t *in = s_fb + (size_t)sy * s_stride_px;
        for (int32_t i = 0; i < r.w; i++) {
            const int32_t sx = r.x + i;
            out[i] = (sx < 0 || sx >= s_w) ? 0u : in[sx];
        }
    }
}

static void surf_flush(dsk_rect_t r)
{
    const dsk_rect_t c = dsk_rect_clip(r, dsk_rect(0, 0, s_w, s_h));
    if (dsk_rect_empty(c)) {
        return;
    }
    ag_gfx_flush((uint16_t)c.x, (uint16_t)c.y, (uint16_t)c.w, (uint16_t)c.h);
}

static const dsk_painter_t k_surface = {
    .fill = surf_fill,
    .blit = surf_blit,
    .text = surf_text,
    .clip = surf_clip,
    .read = surf_read,
    .flush = surf_flush,
};

void dsk_paint_bind_surface(void *fb, uint32_t stride_bytes, int16_t w,
                            int16_t h)
{
    s_fb = (uint16_t *)fb;
    s_stride_px = stride_bytes / sizeof(uint16_t);
    s_w = w;
    s_h = h;
    s_clip = dsk_rect_none();
    s_shell_clip = dsk_rect_none();
    s_shell_w = w;
    s_shell_h = h;
    s_p = &k_surface;
    ag_gfx_clip_reset();
}

void dsk_paint_bind(const dsk_painter_t *p, int16_t w, int16_t h)
{
    s_p = p;
    s_w = w;
    s_h = h;
    s_clip = dsk_rect_none();
    s_shell_clip = dsk_rect_none();
    s_shell_w = w;
    s_shell_h = h;
}

const dsk_painter_t *dsk_paint(void) { return s_p; }

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
