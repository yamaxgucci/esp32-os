/*
 * ArgonOS DESKTOP - the painter's backend for a board with no surface.
 *
 * The CYD holds 179 KB of RAM and its glass is 320x240, which is 153 600
 * bytes of RGB565: a framebuffer for it does not exist and never will, and
 * that board is the only one with a touchscreen.  So this backend keeps no
 * frame at all.  It rasterises a damage rectangle one horizontal band at a
 * time into ten kilobytes of scratch and hands each band straight to the panel
 * through `gfx->present`, which is the transport a surfaceless display offers
 * (`[display] driver = panel`).  See docs/plans/desktop.md §9.2.
 *
 * The shell above does not know.  It draws through the same six operations it
 * always did; what changes is that the repaint of one rectangle happens
 * several times over, once per band, with everything outside the band clipped
 * away.  That costs redrawing the furniture up to fifteen times for a
 * full-screen repaint - and on this board drawing was measured at 2 ms of a
 * 17 ms frame, all the rest being the panel, so the arithmetic is affordable
 * where a second framebuffer is not.
 *
 * `read` is not the exception the plan expected.  The band IS the backing
 * store while it is being built, so reading back out of it is both correct and
 * free - which means the pointer keeps its compose-over-the-background look
 * instead of becoming a flat square.  The rule that does change: whatever
 * reads the background must be drawn while the band is still in hand, so the
 * pointer and the drag outline are painted inside the band pass rather than
 * saved and restored around the frame.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "dsk_paint.h"

#include <argon/argon.h>

#include "../common/font8x16.h"

/*
 * Ten kilobytes, which is 320x16 - the width of the widest glass this runs on
 * by the height of one band.  A narrow damage rectangle gets a taller band out
 * of the same memory (the band is `rect.w` wide, not screen-wide), so the
 * common case of a small repaint is one band and one present.
 */
#define BAND_PX (320 * 16)
#define BAND_H_MAX 16

static uint16_t s_px[BAND_PX];

static int16_t s_w, s_h;      /* the glass */
static dsk_rect_t s_clip;     /* shell clip, screen coordinates; empty = all */
static dsk_rect_t s_band;     /* what s_px currently covers, screen coords */

static dsk_rect_t clip_now(void)
{
    return dsk_rect_empty(s_clip) ? dsk_rect(0, 0, s_w, s_h) : s_clip;
}

/* Where a screen rectangle lands in the band, or empty if it misses it. */
static dsk_rect_t in_band(dsk_rect_t r)
{
    return dsk_rect_clip(dsk_rect_clip(r, clip_now()), s_band);
}

static uint16_t *at(int16_t x, int16_t y)
{
    return &s_px[(size_t)(y - s_band.y) * (size_t)s_band.w +
                 (size_t)(x - s_band.x)];
}

static uint16_t to565(uint32_t rgb)
{
    const uint32_t r = (rgb >> 16) & 0xFFu;
    const uint32_t g = (rgb >> 8) & 0xFFu;
    const uint32_t b = rgb & 0xFFu;
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

/* ---- the six operations ------------------------------------------------- */

static void band_fill(dsk_rect_t r, uint32_t rgb)
{
    const dsk_rect_t c = in_band(r);
    if (dsk_rect_empty(c)) {
        return;
    }
    const uint16_t v = to565(rgb);
    for (int16_t row = 0; row < c.h; row++) {
        uint16_t *out = at(c.x, (int16_t)(c.y + row));
        for (int16_t i = 0; i < c.w; i++) {
            out[i] = v;
        }
    }
}

static void band_blit(dsk_rect_t r, const uint16_t *src, uint32_t stride_px)
{
    if (src == NULL) {
        return;
    }
    const dsk_rect_t c = in_band(r);
    if (dsk_rect_empty(c)) {
        return;
    }
    /* Which part of the source survived the clip and the band. */
    const int32_t sx = (int32_t)c.x - r.x;
    const int32_t sy = (int32_t)c.y - r.y;

    for (int16_t row = 0; row < c.h; row++) {
        const uint16_t *in = src + (size_t)(sy + row) * stride_px + sx;
        uint16_t *out = at(c.x, (int16_t)(c.y + row));
        for (int16_t i = 0; i < c.w; i++) {
            out[i] = in[i];
        }
    }
}

static void glyph(int16_t x, int16_t y, const uint8_t rows[16], uint16_t fg,
                  uint16_t bg, bool trans)
{
    const dsk_rect_t c = in_band(dsk_rect(x, y, AG_FONT8X16_W,
                                          AG_FONT8X16_H));
    if (dsk_rect_empty(c)) {
        return;
    }
    for (int16_t py = c.y; py < c.y + c.h; py++) {
        const uint8_t bits = rows[py - y];
        uint16_t     *out = at(c.x, py);
        for (int16_t px = c.x; px < c.x + c.w; px++, out++) {
            /* Bit 0 is the leftmost pixel; see apps/common/font8x16.h. */
            if ((bits & (uint8_t)(1u << (px - x))) != 0) {
                *out = fg;
            } else if (!trans) {
                *out = bg;
            }
        }
    }
}

/*
 * The same truncation the kernel's text_fit does, deliberately: the two
 * backends have to put the same pixels on the glass or the shell's own
 * checks - which compare a photograph taken through one against a photograph
 * taken through the other - would disagree for reasons that are not bugs.
 * See ag_draw_text8x16 in components/argon_kernel/src/dev/draw.c.
 */
static void band_text(int16_t x, int16_t y, int16_t max_w, const char *s,
                      uint32_t fg, uint32_t bg)
{
    if (s == NULL) {
        return;
    }
    const uint16_t f = to565(fg);
    const uint16_t b = (bg == DSK_TRANS) ? 0u : to565(bg);
    const bool     trans = (bg == DSK_TRANS);

    int n = 0;
    while (s[n] != '\0' && s[n] != '\n') {
        n++;
    }

    if (max_w <= 0 || n * AG_FONT8X16_W <= max_w) {
        for (int i = 0; i < n; i++) {
            glyph((int16_t)(x + i * AG_FONT8X16_W), y,
                  k_font8x16[(uint8_t)s[i]], f, b, trans);
        }
        return;
    }

    const bool dots = (max_w >= 24);
    int        nfit = dots ? (max_w - 24) / AG_FONT8X16_W
                           : max_w / AG_FONT8X16_W;
    if (nfit < 0) {
        nfit = 0;
    }
    for (int i = 0; i < nfit; i++) {
        glyph((int16_t)(x + i * AG_FONT8X16_W), y,
              k_font8x16[(uint8_t)s[i]], f, b, trans);
    }
    if (dots) {
        for (int i = 0; i < 3; i++) {
            glyph((int16_t)(x + (nfit + i) * AG_FONT8X16_W), y,
                  k_font8x16[(uint8_t)'.'], f, b, trans);
        }
    }
}

static void band_clip(dsk_rect_t r)
{
    const dsk_rect_t whole = dsk_rect(0, 0, s_w, s_h);
    s_clip = dsk_rect_empty(r) ? whole : dsk_rect_clip(r, whole);
}

/*
 * Read back out of the band.
 *
 * Legal here, unlike the plan's guess, because the band is the only backing
 * store there is and it holds everything drawn into this pass so far.  What it
 * cannot do is answer for pixels outside the band, and it says so by leaving
 * them at zero rather than pretending: nothing asks, because the two callers
 * (the pointer and the drag outline) are painted inside the band pass.
 */
static void band_read(dsk_rect_t r, uint16_t *dst)
{
    if (dst == NULL || dsk_rect_empty(r)) {
        return;
    }
    for (int16_t row = 0; row < r.h; row++) {
        const int16_t py = (int16_t)(r.y + row);
        for (int16_t col = 0; col < r.w; col++) {
            const int16_t px = (int16_t)(r.x + col);
            const bool inside = px >= s_band.x && px < s_band.x + s_band.w &&
                                py >= s_band.y && py < s_band.y + s_band.h;
            dst[(size_t)row * r.w + col] = inside ? *at(px, py) : 0u;
        }
    }
}

/*
 * Nothing.  A band reaches the panel when the band is finished, which the
 * repaint driver decides, not the shell: a flush per piece of furniture would
 * send the same band several times over.
 */
static void band_flush(dsk_rect_t r) { (void)r; }

static const dsk_painter_t s_painter = {
    .fill = band_fill,
    .blit = band_blit,
    .text = band_text,
    .clip = band_clip,
    .read = band_read,
    .flush = band_flush,
};

/* ---- the band driver ---------------------------------------------------- */

static int16_t band_begin(dsk_rect_t r, int16_t y)
{
    if (r.w <= 0 || r.w > (int16_t)BAND_PX) {
        return 0;
    }
    int16_t h = (int16_t)(BAND_PX / r.w);
    if (h > BAND_H_MAX) {
        h = BAND_H_MAX;
    }
    if (h > (int16_t)(r.y + r.h - y)) {
        h = (int16_t)(r.y + r.h - y);
    }
    if (h <= 0) {
        return 0;
    }
    s_band = dsk_rect(r.x, y, r.w, h);
    return h;
}

static void band_present(void)
{
    if (dsk_rect_empty(s_band)) {
        return;
    }
    const ag_blit_t b = {
        .px = (const uint8_t *)s_px,
        .stride = (uint32_t)s_band.w * sizeof(uint16_t),
        .surf_w = (uint16_t)s_w,
        .surf_h = (uint16_t)s_h,
        .x = (uint16_t)s_band.x,
        .y = (uint16_t)s_band.y,
        .w = (uint16_t)s_band.w,
        .h = (uint16_t)s_band.h,
    };
    /*
     * The answer is looked at, and said out loud once.
     *
     * A refused present is invisible: the drawing happened, the milliseconds
     * were spent, and the pixels went nowhere.  That is exactly how an hour
     * went - the kernel was rejecting every band narrower than the screen
     * (its stride check asked for a surface-wide buffer) and this call
     * discarded the code that said so.  Once, not per band: a broken
     * transport would otherwise print sixty times a second.
     */
    static bool s_complained;
    const ag_err_t err = ag_gfx_present(&b);
    if (err != AG_OK && !s_complained) {
        s_complained = true;
        ag_printf("desktop: present %dx%d at %d,%d refused (%d)\n",
                  (int)s_band.w, (int)s_band.h, (int)s_band.x, (int)s_band.y,
                  (int)err);
    }
}

/*
 * The frame is finished.
 *
 * In this mode there is no surface to copy from, so flush cannot mean "copy
 * what I drew" - it means "the strips I have handed over are a whole frame,
 * show them".  The kernel holds the rows until it hears this (or until a
 * frame's worth of time has passed, for a caller that never says it), and
 * then asks the panel once instead of once per strip.
 */
static void band_frame_done(void) { ag_gfx_flush(0, 0, 0, 0); }

static const dsk_bander_t s_bander = {
    .begin = band_begin,
    .present = band_present,
    .frame_done = band_frame_done,
};

void dsk_paint_bind_band(int16_t w, int16_t h)
{
    s_w = w;
    s_h = h;
    s_clip = dsk_rect_none();
    s_band = dsk_rect_none();
    dsk_paint_bind(&s_painter, w, h);
    dsk_paint_bind_bander(&s_bander);
}
