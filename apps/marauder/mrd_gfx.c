/*
 * MARAUDER - the band renderer.
 *
 * The UI is drawn one horizontal band at a time into a small RGB565 buffer and
 * handed to the panel.  Which way it is handed over depends on the machine:
 *
 *   - No system surface (the CYD board, [display] driver = panel): gfx_acquire
 *     returns fb = NULL and the only way to the glass is gfx->present, a band at
 *     a time.  This is the path that matters on hardware and the reason the whole
 *     app is written this way - there is no 150 KB to spare for a framebuffer
 *     with the radio up.
 *
 *   - A system surface exists (QEMU -Gfx): gfx->present in that mode goes to a
 *     hardware panel driver that is not there, so instead we copy each band into
 *     the acquired surface with gfx->blit and flush once at the end.  Same band
 *     pixels, so the picture is identical and gfxdump can capture it without a
 *     board.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mrd.h"
#include "mrd_font.h"

/* One band, reused for every push.  Static so it costs no arena; 5 KB of bss. */
static uint16_t s_band[MRD_UI_W * MRD_BAND_H];

/*
 * One scratch buffer shared by every screen's working state.
 *
 * Only one screen runs at a time - the menu dispatches into a screen and waits
 * for it to return - so their per-screen structs never overlap in time, and
 * giving each its own static would spend bss on states that are never both
 * live.  On this no-PSRAM board, with the radio taking ~36 KB, that bss is the
 * difference between the app loading and not: it is part of the data segment the
 * loader has to place in one contiguous block after Wi-Fi is up.  So each big
 * screen state lives here instead, cast from this buffer.  Sized to the largest
 * such struct (checked with _Static_assert at each use).
 */
static uint8_t s_scratch[MRD_SCRATCH_BYTES]
    __attribute__((aligned(8)));

void *mrd_scratch(void) { return s_scratch; }

bool mrd_gfx_begin(mrd_disp_t *d)
{
    if (ag_api()->gfx == NULL) {
        return false;
    }
    if (ag_gfx_acquire(&d->info) != AG_OK) {
        return false;
    }

    d->surfaceless = (d->info.fb == NULL);
    if (d->surfaceless && !AG_HAS(ag_api()->gfx, present)) {
        ag_gfx_release();
        return false;
    }

    d->W = d->info.width < MRD_UI_W ? d->info.width : MRD_UI_W;
    d->H = d->info.height < MRD_UI_H ? d->info.height : MRD_UI_H;

    /*
     * The grid touch events arrive on.  The XPT2046 driver reports cells of
     * 8 px on the panel (its COLS/ROWS = panel/8, so 40x30 on this 320x240
     * board) - NOT the text console's grid, which is a different number of rows
     * (40x25 here).  Using con->info was the bug: it stretched every tap
     * vertically, so a press landed on the button below and the bottom of the
     * screen mapped off the end.  Derive the grid from the surface and the 8 px
     * cell instead, which matches what the driver actually sends.
     */
    d->cols = d->W / 8;
    d->rows = d->H / 8;
    if (d->cols < 1) {
        d->cols = 1;
    }
    if (d->rows < 1) {
        d->rows = 1;
    }
    return true;
}

void mrd_gfx_end(mrd_disp_t *d)
{
    (void)d;
    ag_gfx_release();
}

void mrd_gfx_paint(mrd_disp_t *d, uint16_t bg,
                   void (*paint)(mrd_band_t *b, void *ctx), void *ctx)
{
    mrd_band_t band;
    band.px = s_band;
    band.w = d->W;

    for (int y0 = 0; y0 < d->H; y0 += MRD_BAND_H) {
        int h = d->H - y0;
        if (h > MRD_BAND_H) {
            h = MRD_BAND_H;
        }
        band.y0 = y0;
        band.h = h;

        /* Clear the band to the background, then let the screen paint over it. */
        for (int i = 0; i < d->W * h; i++) {
            s_band[i] = bg;
        }
        paint(&band, ctx);

        if (d->surfaceless) {
            const ag_blit_t b = {
                .px = s_band,
                .stride = MRD_UI_W * sizeof(uint16_t),
                .surf_w = d->W,
                .surf_h = d->H,
                .x = 0,
                .y = (uint16_t)y0,
                .w = d->W,
                .h = (uint16_t)h,
            };
            (void)ag_gfx_present(&b);
        } else {
            ag_gfx_blit(0, (int16_t)y0, d->W, (uint16_t)h, s_band,
                        MRD_UI_W * sizeof(uint16_t), AG_PIX_RGB565);
        }
    }

    if (!d->surfaceless) {
        ag_gfx_flush(0, 0, d->W, d->H);
    }
}

ag_err_t mrd_gfx_dump_ppm(mrd_disp_t *d, const char *path, uint16_t bg,
                          void (*paint)(mrd_band_t *b, void *ctx), void *ctx)
{
    ag_handle_t f = ag_open(path, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (f < 0) {
        return (ag_err_t)f;
    }

    /* "P6\n<W> <H>\n255\n" without snprintf. */
    char hdr[32];
    int  n = 0;
    hdr[n++] = 'P';
    hdr[n++] = '6';
    hdr[n++] = '\n';
    unsigned dims[2] = {d->W, d->H};
    for (int k = 0; k < 2; k++) {
        char t[6];
        int  m = 0;
        unsigned v = dims[k];
        do {
            t[m++] = (char)('0' + v % 10u);
            v /= 10u;
        } while (v);
        while (m > 0) {
            hdr[n++] = t[--m];
        }
        hdr[n++] = (k == 0) ? ' ' : '\n';
    }
    const char *tail = "255\n";
    for (int i = 0; tail[i]; i++) {
        hdr[n++] = tail[i];
    }
    ag_write(f, hdr, (size_t)n);

    static uint8_t rgb[MRD_UI_W * 3];
    mrd_band_t band;
    band.px = s_band;
    band.w = d->W;

    for (int y0 = 0; y0 < d->H; y0 += MRD_BAND_H) {
        int h = d->H - y0;
        if (h > MRD_BAND_H) {
            h = MRD_BAND_H;
        }
        band.y0 = y0;
        band.h = h;
        for (int i = 0; i < d->W * h; i++) {
            s_band[i] = bg;
        }
        paint(&band, ctx);

        for (int r = 0; r < h; r++) {
            const uint16_t *row = s_band + r * MRD_UI_W;
            for (int x = 0; x < d->W; x++) {
                const uint16_t c = row[x];
                rgb[x * 3 + 0] = (uint8_t)(((c >> 11) & 0x1fu) * 255u / 31u);
                rgb[x * 3 + 1] = (uint8_t)(((c >> 5) & 0x3fu) * 255u / 63u);
                rgb[x * 3 + 2] = (uint8_t)((c & 0x1fu) * 255u / 31u);
            }
            ag_write(f, rgb, (size_t)d->W * 3u);
        }
    }
    ag_close(f);
    return AG_OK;
}

void mrd_band_fill(mrd_band_t *b, int x, int y, int w, int h, uint16_t color)
{
    /* Clip to the band and to the logical width. */
    int y1 = y + h;
    if (y < b->y0) {
        y = b->y0;
    }
    if (y1 > b->y0 + b->h) {
        y1 = b->y0 + b->h;
    }
    int x1 = x + w;
    if (x < 0) {
        x = 0;
    }
    if (x1 > b->w) {
        x1 = b->w;
    }
    for (int yy = y; yy < y1; yy++) {
        uint16_t *row = b->px + (yy - b->y0) * MRD_UI_W;
        for (int xx = x; xx < x1; xx++) {
            row[xx] = color;
        }
    }
}

void mrd_band_frame(mrd_band_t *b, int x, int y, int w, int h, uint16_t color)
{
    mrd_band_fill(b, x, y, w, 1, color);
    mrd_band_fill(b, x, y + h - 1, w, 1, color);
    mrd_band_fill(b, x, y, 1, h, color);
    mrd_band_fill(b, x + w - 1, y, 1, h, color);
}

int mrd_text_w(const char *s, int scale)
{
    int n = 0;
    while (s && *s) {
        n++;
        s++;
    }
    return n * MRD_FONT_W * scale;
}

int mrd_band_text(mrd_band_t *b, int x, int y, const char *s, uint16_t fg,
                  uint32_t bg, int scale)
{
    if (s == NULL || scale < 1) {
        return 0;
    }
    const int gw = MRD_FONT_W * scale;
    const int gh = MRD_FONT_H * scale;
    const int x0 = x;

    for (; *s; s++, x += gw) {
        unsigned char c = (unsigned char)*s;
        if (c < MRD_FONT_FIRST || c > MRD_FONT_LAST) {
            c = '?';
        }
        /* Skip glyphs entirely outside the band or the width. */
        if (x + gw <= 0 || x >= b->w || y + gh <= b->y0 ||
            y >= b->y0 + b->h) {
            continue;
        }
        const uint8_t *g = k_mrd_font[c - MRD_FONT_FIRST];

        for (int gy = 0; gy < MRD_FONT_H; gy++) {
            const uint8_t bits = g[gy];
            for (int sy = 0; sy < scale; sy++) {
                const int py = y + gy * scale + sy;
                if (py < b->y0 || py >= b->y0 + b->h) {
                    continue;
                }
                uint16_t *row = b->px + (py - b->y0) * MRD_UI_W;
                for (int gx = 0; gx < MRD_FONT_W; gx++) {
                    const bool on = (bits >> gx) & 1u; /* LSB = leftmost */
                    if (!on && bg == 0xFFFFFFFFu) {
                        continue; /* transparent background */
                    }
                    const uint16_t col = on ? fg : (uint16_t)bg;
                    for (int sx = 0; sx < scale; sx++) {
                        const int px = x + gx * scale + sx;
                        if (px >= 0 && px < b->w) {
                            row[px] = col;
                        }
                    }
                }
            }
        }
    }
    return x - x0;
}

/*
 * A tap, clamped to the screen.  It used to scale as well: a pointer event
 * carried console cells and every screen in here had to multiply back up to
 * pixels, which is why this is called from a dozen places.  As of ABI 0.42 the
 * event already carries the surface's pixels, so all that is left is refusing
 * a coordinate off the edge - kept as a function rather than removed from those
 * dozen call sites, because a clamp at the boundary is worth having anyway.
 */
void mrd_touch_to_px(const mrd_disp_t *d, int16_t cx, int16_t cy, int *px,
                     int *py)
{
    int x = (int)cx, y = (int)cy;
    if (x < 0) {
        x = 0;
    } else if (x >= d->W) {
        x = d->W - 1;
    }
    if (y < 0) {
        y = 0;
    } else if (y >= d->H) {
        y = d->H - 1;
    }
    *px = x;
    *py = y;
}
