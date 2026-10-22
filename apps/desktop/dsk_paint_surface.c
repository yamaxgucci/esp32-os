/*
 * ArgonOS DESKTOP - the painter's backend for the acquired system surface.
 *
 * This is the only file in the shell that includes argon.h for drawing, and
 * that is the point of the split: everything above it - the window manager,
 * the menus, the geometry - draws through the six-function table in
 * dsk_paint.h and so compiles on the host, where the unit tests bind a
 * recorder in place of this.
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

static uint16_t  *s_fb;
static uint32_t   s_stride_px;
static int16_t    s_w;
static int16_t    s_h;
static dsk_rect_t s_clip; /* empty means the whole surface */

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
    ag_gfx_clip_reset();
    dsk_paint_bind(&k_surface, w, h);
}

