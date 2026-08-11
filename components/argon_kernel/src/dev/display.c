/*
 * ArgonOS - soft local display: RGB565 front + optional back buffer.
 *
 * Front is what QEMU / fb0 / the console present.  While graphics is acquired
 * and a back buffer exists, apps draw there; flush/swap copy to front and kick
 * the QEMU RGB window (no busy-wait on UPDATE_STATUS.ENA).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/display.h>

#include <stdio.h>
#include <string.h>

#include <argon/board.h>
#include <argon/console.h>
#include <argon/device.h>
#include <argon/draw.h>
#include <argon/log.h>
#include <argon/path.h>
#include <argon/vfs.h>

#include "dev/font8x16.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_qemu_rgb.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Match the text console: 80×25 cells at 8×16 px. */
#define AG_DISPLAY_DEFAULT_W 640
#define AG_DISPLAY_DEFAULT_H 400
#define AG_DISPLAY_MAX_W     800
#define AG_DISPLAY_MAX_H     480

/* CGA / BIOS text colours as RGB565. */
static const uint16_t k_cga565[16] = {
    0x0000, /* black        */
    0x0015, /* blue         */
    0x0540, /* green        */
    0x0555, /* cyan         */
    0xA800, /* red          */
    0xA815, /* magenta      */
    0xAAA0, /* brown        */
    0xAD55, /* light grey   */
    0x52AA, /* dark grey    */
    0x52BF, /* light blue   */
    0x57EA, /* light green  */
    0x57FF, /* light cyan   */
    0xFAAA, /* light red    */
    0xFABF, /* light magenta*/
    0xFFE0, /* yellow       */
    0xFFFF, /* white        */
};

static uint16_t *s_front; /* presented: QEMU FB or owned PSRAM          */
static uint16_t *s_back;  /* draw target while acquired; NULL = single */
static uint16_t *s_draw;  /* s_back while acquired with DB, else front */
static uint16_t *s_snap;  /* last released graphics frame, for gfxdump */
static uint16_t  s_w;
static uint16_t  s_h;
static uint32_t  s_stride; /* bytes per row */
static bool      s_ready;
static bool      s_acquired;
static bool      s_have_snap;
static bool      s_front_owned; /* false when front is QEMU's dedicated FB */
static uint32_t  s_console_gen;
static ag_device_t *s_dev;
static esp_lcd_panel_handle_t s_qemu_panel; /* NULL on real hardware */

static size_t fb_bytes(void)
{
    return (size_t)s_stride * (size_t)s_h;
}

/* ---------------------------------------------------------------------- */

/*
 * QEMU RGB MMIO (see espressif/qemu hw/display/esp_rgb.c).  Do NOT call
 * esp_lcd_rgb_qemu_refresh: it busy-waits on UPDATE_STATUS.ENA, and that bit
 * only clears from QEMU's display thread — a guest spin deadlocks SMS/console.
 */
enum {
    RGB_MMIO_UPDATE_FROM = 0x08u / 4u,
    RGB_MMIO_UPDATE_TO = 0x0cu / 4u,
    RGB_MMIO_UPDATE_CONTENT = 0x10u / 4u,
    RGB_MMIO_UPDATE_STATUS = 0x14u / 4u,
};

/*
 * Present rows y .. y+h of the framebuffer.
 *
 * Rows, not rectangles, and that is the whole point: UPDATE_CONTENT is a
 * *tightly packed* bitmap of exactly the region being updated - the same
 * contract as esp_lcd_panel_draw_bitmap, whose caller always hands over a
 * standalone w*h buffer.  A framebuffer is only tightly packed when the region
 * spans full rows; for anything narrower QEMU would read w*h pixels straight
 * from the pointer and paint the top-left corner of the frame into the region.
 * That is not theoretical - it looked like the console text reappearing in the
 * middle of the screen in diagonal bands.
 *
 * A caller that touched a narrow rectangle therefore still gets its rows
 * presented whole.  The extra columns cost nothing on this side: they are read
 * by QEMU's display thread, while the copy the guest pays for stays narrow.
 */
static void qemu_present_rows(int32_t y, int32_t h)
{
    if (s_qemu_panel == NULL || s_front == NULL || s_w == 0 || s_h == 0) {
        return;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (y + h > (int32_t)s_h) {
        h = (int32_t)s_h - y;
    }
    if (h <= 0) {
        return;
    }

    volatile uint32_t *const rgb = (volatile uint32_t *)0x21000000u;
    /* X in the high half, Y in the low one, per rgb_qemu_dev_t. Ends are
     * exclusive. */
    rgb[RGB_MMIO_UPDATE_FROM] = (uint32_t)y;
    rgb[RGB_MMIO_UPDATE_TO] = ((uint32_t)s_w << 16) | (uint32_t)(y + h);
    rgb[RGB_MMIO_UPDATE_CONTENT] =
        (uint32_t)(uintptr_t)((const uint8_t *)s_front + (size_t)y * s_stride);
    rgb[RGB_MMIO_UPDATE_STATUS] = 1u; /* ENA; QEMU clears asynchronously */
    taskYIELD();
}

static void qemu_present(void)
{
    qemu_present_rows(0, (int32_t)s_h);
}

/*
 * Copy draw → front within a rectangle, then present the rows it touched.
 * An empty rectangle after clipping still presents: callers use flush as the
 * "show what I drew" signal, and the QEMU window needs the kick regardless.
 *
 * The rectangle is what makes an emulator affordable: a 320x224 frame is 140 KB
 * to copy against 500 KB for a full 640x400 frame, and this copy is CPU, not
 * DMA.  Only the copy narrows, though - see qemu_present_rows for why the
 * presented region cannot.
 */
static void present_rect_to_front(int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (s_front == NULL || s_draw == NULL) {
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
    if (x + w > (int32_t)s_w) {
        w = (int32_t)s_w - x;
    }
    if (y + h > (int32_t)s_h) {
        h = (int32_t)s_h - y;
    }
    if (w <= 0 || h <= 0) {
        qemu_present();
        return;
    }

    if (s_draw != s_front) {
        if (x == 0 && w == (int32_t)s_w) {
            /* Whole rows are one contiguous run. */
            const size_t off = (size_t)y * s_stride;
            memcpy((uint8_t *)s_front + off, (const uint8_t *)s_draw + off,
                   (size_t)h * s_stride);
        } else {
            const size_t row_bytes = (size_t)w * sizeof(uint16_t);
            for (int32_t row = 0; row < h; row++) {
                const size_t off =
                    (size_t)(y + row) * s_stride + (size_t)x * sizeof(uint16_t);
                memcpy((uint8_t *)s_front + off, (const uint8_t *)s_draw + off,
                       row_bytes);
            }
        }
    }
    qemu_present_rows(y, h);
}

/* Copy draw → front (full frame). No-op when drawing already targets front. */
static void present_draw_to_front(void)
{
    if (s_front == NULL || s_draw == NULL) {
        return;
    }
    if (s_draw != s_front) {
        memcpy(s_front, s_draw, fb_bytes());
    }
    qemu_present();
}

/* On success, *out_fb is QEMU's dedicated RGB565 buffer (do not free). */
static bool qemu_try_attach(uint16_t w, uint16_t h, uint16_t **out_fb)
{
    const esp_lcd_rgb_qemu_config_t cfg = {
        .width = w,
        .height = h,
        .bpp = RGB_QEMU_BPP_16,
    };
    esp_lcd_panel_handle_t panel = NULL;
    if (esp_lcd_new_rgb_qemu(&cfg, &panel) != ESP_OK || panel == NULL) {
        return false;
    }
    (void)esp_lcd_panel_reset(panel);
    (void)esp_lcd_panel_init(panel);

    void *qfb = NULL;
    if (esp_lcd_rgb_qemu_get_frame_buffer(panel, &qfb) != ESP_OK ||
        qfb == NULL) {
        (void)esp_lcd_panel_del(panel);
        return false;
    }

    s_qemu_panel = panel;
    *out_fb = (uint16_t *)qfb;
    return true;
}

#define AG_POLY_MAX_VERTS 32

static ag_point_t s_poly[AG_POLY_MAX_VERTS];
static int        s_poly_n;
static bool       s_poly_active;

/* Stateful RGB565 blit (ABI 0.17) — for Argon CC's 6-arg call limit. */
static const void *s_blit_src;
static uint32_t    s_blit_stride;

/* Soft-draw clip (ABI 0.16).  Zero size means "whole framebuffer". */
static int16_t  s_clip_x;
static int16_t  s_clip_y;
static uint16_t s_clip_w;
static uint16_t s_clip_h;

static uint16_t rgb_to_565(uint32_t color)
{
    return ag_draw_rgb_to_565(color);
}

static ag_draw_surf_t draw_surf(void)
{
    ag_draw_surf_t s = {
        .pix = s_draw,
        .w = s_w,
        .h = s_h,
        .clip_x = s_clip_x,
        .clip_y = s_clip_y,
        .clip_w = s_clip_w,
        .clip_h = s_clip_h,
    };
    return s;
}

static void put_pixel(int32_t x, int32_t y, uint16_t c)
{
    ag_draw_surf_t s = draw_surf();
    ag_draw_pixel(&s, x, y, c);
}

static void fill_rect_raw(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    if (w <= 0 || h <= 0 || s_draw == NULL) {
        return;
    }
    ag_draw_surf_t s = draw_surf();
    ag_draw_fill_rect(&s, x, y, w, h, c);
}

static void draw_glyph(int32_t x, int32_t y, uint8_t ch, uint16_t fg, uint16_t bg)
{
    const uint8_t *rows = ag_font8x16[ch];
    for (int32_t row = 0; row < AG_FONT8X16_H; row++) {
        const uint8_t bits = rows[row];
        /* font8x8: bit 0 (LSB) is the leftmost pixel of the row. */
        for (int32_t col = 0; col < AG_FONT8X16_W; col++) {
            const uint16_t c =
                (bits & (uint8_t)(1u << col)) != 0 ? fg : bg;
            put_pixel(x + col, y + row, c);
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Device ops: raw RGB565 framebuffer as a byte stream                     */
/* ---------------------------------------------------------------------- */

static int32_t fb_read(ag_device_t *dev, void *buf, size_t len, uint64_t off)
{
    (void)dev;
    if (s_front == NULL || buf == NULL) {
        return -AG_EINVAL;
    }
    const uint64_t total = (uint64_t)s_stride * s_h;
    if (off >= total) {
        return 0;
    }
    if (off + len > total) {
        len = (size_t)(total - off);
    }
    memcpy(buf, (const uint8_t *)s_front + (size_t)off, len);
    return (int32_t)len;
}

static int32_t fb_write(ag_device_t *dev, const void *buf, size_t len,
                        uint64_t off)
{
    (void)dev;
    if (s_front == NULL || buf == NULL) {
        return -AG_EINVAL;
    }
    const uint64_t total = (uint64_t)s_stride * s_h;
    if (off >= total) {
        return -AG_EINVAL;
    }
    if (off + len > total) {
        len = (size_t)(total - off);
    }
    memcpy((uint8_t *)s_front + (size_t)off, buf, len);
    return (int32_t)len;
}

static uint64_t fb_size(ag_device_t *dev)
{
    (void)dev;
    return (uint64_t)s_stride * s_h;
}

static const ag_dev_ops_t k_fb_ops = {
    .read = fb_read,
    .write = fb_write,
    .size = fb_size,
};

/* ---------------------------------------------------------------------- */
/* gfx API                                                                 */
/* ---------------------------------------------------------------------- */

static ag_err_t gfx_acquire(ag_gfxinfo_t *out)
{
    if (!s_ready || s_front == NULL) {
        return -AG_ENODEV;
    }
    if (s_acquired) {
        return -AG_EBUSY;
    }
    s_acquired = true;
    s_clip_x = 0;
    s_clip_y = 0;
    s_clip_w = 0;
    s_clip_h = 0;
    if (s_back != NULL) {
        /* Start the back buffer from the last presented frame. */
        memcpy(s_back, s_front, fb_bytes());
        s_draw = s_back;
    } else {
        s_draw = s_front;
    }
    if (out != NULL) {
        out->width = s_w;
        out->height = s_h;
        out->fmt = AG_PIX_RGB565;
        out->stride = s_stride;
        out->fb = s_draw;
        out->double_buf = (s_back != NULL);
        out->direct = (s_back == NULL);
    }
    return AG_OK;
}

static void gfx_release(void)
{
    if (!s_acquired) {
        return;
    }
    /* Show the last drawn frame and keep a snapshot for gfxdump. */
    present_draw_to_front();
    if (s_snap != NULL && s_front != NULL) {
        memcpy(s_snap, s_front, fb_bytes());
        s_have_snap = true;
    }
    s_draw = s_front;
    s_acquired = false;
    s_console_gen = 0; /* force a full console redraw on the next tick */
    if (ag_console_ready()) {
        ag_console_lock();
        ag_screen_mark_all_dirty(ag_console_screen());
        ag_console_unlock();
    }
}

static void gfx_flush(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if (w == 0 || h == 0) {
        present_draw_to_front();
        return;
    }
    present_rect_to_front((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h);
}

static void gfx_swap(void)
{
    present_draw_to_front();
}

/*
 * Clear means clear the screen, so the front buffer goes too.  An app that
 * clears once and then flushes a sub-rectangle every frame - which is exactly
 * what the emulators do - would otherwise keep whatever the console left around
 * its window on display for as long as it runs.
 */
static void gfx_clear(uint32_t color)
{
    if (s_draw == NULL) {
        return;
    }
    const uint16_t c = rgb_to_565(color);
    fill_rect_raw(0, 0, (int32_t)s_w, (int32_t)s_h, c);
    if (s_front != NULL && s_front != s_draw) {
        const size_t pixels = (size_t)s_w * s_h;
        for (size_t i = 0; i < pixels; i++) {
            s_front[i] = c;
        }
    }
    qemu_present();
}

static void gfx_fill_rect(int16_t x, int16_t y, uint16_t w, uint16_t h,
                          uint32_t color)
{
    fill_rect_raw(x, y, (int32_t)w, (int32_t)h, rgb_to_565(color));
}

static void gfx_blit(int16_t x, int16_t y, uint16_t w, uint16_t h,
                     const void *src, uint32_t src_stride, ag_pixfmt_t src_fmt)
{
    if (s_draw == NULL || src == NULL || w == 0 || h == 0) {
        return;
    }
    if (src_fmt != AG_PIX_RGB565 && src_fmt != AG_PIX_RGB565_BE) {
        return; /* soft path only knows 16-bit RGB for now */
    }
    ag_draw_surf_t surf = draw_surf();
    ag_draw_blit(&surf, x, y, (int32_t)w, (int32_t)h, src, src_stride,
                 src_fmt == AG_PIX_RGB565_BE, 0, 0);
}

static void gfx_blit_key(int16_t x, int16_t y, uint16_t w, uint16_t h,
                         const void *src, uint32_t src_stride,
                         ag_pixfmt_t src_fmt, uint32_t key_rgb)
{
    if (s_draw == NULL || src == NULL || w == 0 || h == 0) {
        return;
    }
    if (src_fmt != AG_PIX_RGB565 && src_fmt != AG_PIX_RGB565_BE) {
        return;
    }
    ag_draw_surf_t surf = draw_surf();
    ag_draw_blit(&surf, x, y, (int32_t)w, (int32_t)h, src, src_stride,
                 src_fmt == AG_PIX_RGB565_BE, 1, rgb_to_565(key_rgb));
}

static void gfx_blit_bind(const void *src, uint32_t src_stride)
{
    s_blit_src = src;
    s_blit_stride = src_stride;
}

static void gfx_blit_copy(int16_t x, int16_t y, uint16_t w, uint16_t h)
{
    if (s_draw == NULL || s_blit_src == NULL || w == 0 || h == 0) {
        return;
    }
    ag_draw_surf_t surf = draw_surf();
    ag_draw_blit(&surf, x, y, (int32_t)w, (int32_t)h, s_blit_src, s_blit_stride,
                 0, 0, 0);
}

static void gfx_blit_keyed(int16_t x, int16_t y, uint16_t w, uint16_t h,
                           uint32_t key_rgb)
{
    if (s_draw == NULL || s_blit_src == NULL || w == 0 || h == 0) {
        return;
    }
    ag_draw_surf_t surf = draw_surf();
    ag_draw_blit(&surf, x, y, (int32_t)w, (int32_t)h, s_blit_src, s_blit_stride,
                 0, 1, rgb_to_565(key_rgb));
}

static int32_t gfx_text(int16_t x, int16_t y, const char *s, uint32_t fg,
                        uint32_t bg)
{
    if (s == NULL || s_draw == NULL) {
        return 0;
    }
    const uint16_t fgc = rgb_to_565(fg);
    const uint16_t bgc = rgb_to_565(bg);
    int32_t advance = 0;
    int32_t cx = x;
    int32_t cy = y;
    for (const char *p = s; *p != '\0'; p++) {
        const uint8_t ch = (uint8_t)*p;
        if (ch == '\n') {
            cx = x;
            cy += AG_FONT8X16_H;
            continue;
        }
        draw_glyph(cx, cy, ch, fgc, bgc);
        cx += AG_FONT8X16_W;
        advance += AG_FONT8X16_W;
    }
    return advance;
}

static void gfx_backlight(uint8_t percent)
{
    (void)percent; /* soft display has no backlight */
}

static void gfx_pixel(int16_t x, int16_t y, uint32_t color)
{
    ag_draw_surf_t s = draw_surf();
    ag_draw_pixel(&s, x, y, rgb_to_565(color));
}

static void gfx_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                     uint32_t color)
{
    ag_draw_surf_t s = draw_surf();
    ag_draw_line(&s, x0, y0, x1, y1, rgb_to_565(color));
}

static void gfx_circle(int16_t cx, int16_t cy, uint16_t r, uint32_t color)
{
    ag_draw_surf_t s = draw_surf();
    ag_draw_circle(&s, cx, cy, (int32_t)r, rgb_to_565(color));
}

static void gfx_fill_circle(int16_t cx, int16_t cy, uint16_t r, uint32_t color)
{
    ag_draw_surf_t s = draw_surf();
    ag_draw_fill_circle(&s, cx, cy, (int32_t)r, rgb_to_565(color));
}

static void gfx_poly_begin(void)
{
    s_poly_n = 0;
    s_poly_active = true;
}

static ag_err_t gfx_poly_vertex(int16_t x, int16_t y)
{
    if (!s_poly_active) {
        return -AG_EINVAL;
    }
    if (s_poly_n >= AG_POLY_MAX_VERTS) {
        return -AG_ENOMEM;
    }
    s_poly[s_poly_n].x = x;
    s_poly[s_poly_n].y = y;
    s_poly_n++;
    return AG_OK;
}

static void gfx_poly_fill(uint32_t color)
{
    if (!s_poly_active || s_poly_n < 3) {
        return;
    }
    ag_draw_surf_t s = draw_surf();
    ag_draw_fill_convex(&s, s_poly, s_poly_n, rgb_to_565(color));
    s_poly_active = false;
}

static void gfx_poly_stroke(uint32_t color)
{
    if (!s_poly_active || s_poly_n < 2) {
        return;
    }
    ag_draw_surf_t s = draw_surf();
    ag_draw_stroke_convex(&s, s_poly, s_poly_n, rgb_to_565(color));
    s_poly_active = false;
}

static void gfx_fill_convex(const ag_point_t *pts, int32_t n, uint32_t color)
{
    if (pts == NULL || n < 3) {
        return;
    }
    ag_draw_surf_t s = draw_surf();
    ag_draw_fill_convex(&s, pts, (int)n, rgb_to_565(color));
}

static void gfx_stroke_convex(const ag_point_t *pts, int32_t n, uint32_t color)
{
    if (pts == NULL || n < 2) {
        return;
    }
    ag_draw_surf_t s = draw_surf();
    ag_draw_stroke_convex(&s, pts, (int)n, rgb_to_565(color));
}

static void gfx_clip(int16_t x, int16_t y, uint16_t w, uint16_t h)
{
    s_clip_x = x;
    s_clip_y = y;
    s_clip_w = w;
    s_clip_h = h;
}

static void gfx_clip_reset(void)
{
    s_clip_x = 0;
    s_clip_y = 0;
    s_clip_w = 0;
    s_clip_h = 0;
}

static void gfx_stroke_rect(int16_t x, int16_t y, uint16_t w, uint16_t h,
                            uint32_t color)
{
    ag_draw_surf_t s = draw_surf();
    ag_draw_stroke_rect(&s, x, y, (int32_t)w, (int32_t)h, rgb_to_565(color));
}

static void gfx_fill_round_rect(int16_t x, int16_t y, uint16_t w, uint16_t h,
                                uint16_t r, uint32_t color)
{
    ag_draw_surf_t s = draw_surf();
    ag_draw_fill_round_rect(&s, x, y, (int32_t)w, (int32_t)h, (int32_t)r,
                            rgb_to_565(color));
}

const ag_gfx_api_t ag_gfx_api_table = {
    .size = sizeof(ag_gfx_api_t),
    .acquire = gfx_acquire,
    .release = gfx_release,
    .flush = gfx_flush,
    .swap = gfx_swap,
    .clear = gfx_clear,
    .fill_rect = gfx_fill_rect,
    .blit = gfx_blit,
    .text = gfx_text,
    .backlight = gfx_backlight,
    .pixel = gfx_pixel,
    .line = gfx_line,
    .circle = gfx_circle,
    .fill_circle = gfx_fill_circle,
    .poly_begin = gfx_poly_begin,
    .poly_vertex = gfx_poly_vertex,
    .poly_fill = gfx_poly_fill,
    .poly_stroke = gfx_poly_stroke,
    .fill_convex = gfx_fill_convex,
    .stroke_convex = gfx_stroke_convex,
    .clip = gfx_clip,
    .clip_reset = gfx_clip_reset,
    .stroke_rect = gfx_stroke_rect,
    .fill_round_rect = gfx_fill_round_rect,
    .blit_key = gfx_blit_key,
    .blit_bind = gfx_blit_bind,
    .blit_copy = gfx_blit_copy,
    .blit_keyed = gfx_blit_keyed,
};

/* ---------------------------------------------------------------------- */

bool ag_display_ready(void) { return s_ready; }

bool ag_display_acquired(void) { return s_acquired; }

void ag_display_render_console(const ag_screen_t *screen)
{
    if (!s_ready || s_acquired || s_front == NULL || screen == NULL) {
        return;
    }

    const bool full = (s_console_gen == 0);
    if (!full && !ag_screen_any_dirty(screen)) {
        return;
    }

    const uint16_t cols = screen->cols;
    const uint16_t rows = screen->rows;
    const uint16_t max_cols = (uint16_t)(s_w / AG_FONT8X16_W);
    const uint16_t max_rows = (uint16_t)(s_h / AG_FONT8X16_H);
    const uint16_t draw_cols = cols < max_cols ? cols : max_cols;
    const uint16_t draw_rows = rows < max_rows ? rows : max_rows;

    for (uint16_t row = 0; row < draw_rows; row++) {
        if (!full && !ag_screen_row_dirty(screen, row)) {
            continue;
        }
        for (uint16_t col = 0; col < draw_cols; col++) {
            const ag_cell_t cell = ag_screen_at(screen, col, row);
            const uint8_t fg = (uint8_t)(cell.attr & 0x0Fu);
            const uint8_t bg = (uint8_t)((cell.attr >> 4) & 0x0Fu);
            draw_glyph((int32_t)col * AG_FONT8X16_W,
                       (int32_t)row * AG_FONT8X16_H, (uint8_t)cell.ch,
                       k_cga565[fg], k_cga565[bg]);
        }
    }

    s_console_gen = screen->generation != 0 ? screen->generation : 1;
    qemu_present();
}

ag_err_t ag_display_dump_ppm(const char *path, const char *cwd, bool live)
{
    if (!s_ready || s_front == NULL || path == NULL) {
        return -AG_ENODEV;
    }

    /*
     * Prefer the last graphics snapshot when text has already been painted
     * back over the live buffer - that is the usual case right after `run`.
     */
    const uint16_t *src = s_front;
    if (!live && !s_acquired && s_have_snap && s_snap != NULL) {
        src = s_snap;
    }

    const ag_handle_t h =
        ag_vfs_open(path, cwd, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (h < 0) {
        return (ag_err_t)h;
    }

    char header[64];
    const int hdr_len =
        snprintf(header, sizeof(header), "P6\n%u %u\n255\n", (unsigned)s_w,
                 (unsigned)s_h);
    if (hdr_len <= 0 ||
        ag_vfs_write(h, header, (size_t)hdr_len) != (int32_t)hdr_len) {
        ag_vfs_close(h);
        return -AG_EIO;
    }

    /* Convert RGB565 → RGB888 in small chunks so we do not need another fb. */
    uint8_t chunk[96]; /* 32 pixels */
    size_t  fill = 0;
    const uint32_t pixels = (uint32_t)s_w * (uint32_t)s_h;
    for (uint32_t i = 0; i < pixels; i++) {
        const uint16_t c = src[i];
        const uint8_t  r = (uint8_t)(((c >> 11) & 0x1Fu) * 255u / 31u);
        const uint8_t  g = (uint8_t)(((c >> 5) & 0x3Fu) * 255u / 63u);
        const uint8_t  b = (uint8_t)((c & 0x1Fu) * 255u / 31u);
        chunk[fill++] = r;
        chunk[fill++] = g;
        chunk[fill++] = b;
        if (fill == sizeof(chunk)) {
            if (ag_vfs_write(h, chunk, fill) != (int32_t)fill) {
                ag_vfs_close(h);
                return -AG_EIO;
            }
            fill = 0;
        }
    }
    if (fill > 0) {
        if (ag_vfs_write(h, chunk, fill) != (int32_t)fill) {
            ag_vfs_close(h);
            return -AG_EIO;
        }
    }

    ag_vfs_close(h);
    return AG_OK;
}

ag_err_t ag_display_init(void)
{
    const ag_board_t *board = ag_board();
    const char       *driver = board->display.driver;

    if (driver[0] == '\0' || ag_path_icmp(driver, "none") == 0) {
        ag_log(AG_LOG_INFO, "display", "disabled (display.driver=none)");
        return AG_OK;
    }

    /* Soft is the only backend without a board; unknown names fall back to it. */
    if (ag_path_icmp(driver, "soft") != 0) {
        ag_log(AG_LOG_WARN, "display",
               "driver '%s' not built in; using soft framebuffer", driver);
    }

    uint16_t w = board->display.width;
    uint16_t h = board->display.height;
    if (w == 0) {
        w = AG_DISPLAY_DEFAULT_W;
    }
    if (h == 0) {
        h = AG_DISPLAY_DEFAULT_H;
    }
    if (w > AG_DISPLAY_MAX_W) {
        w = AG_DISPLAY_MAX_W;
    }
    if (h > AG_DISPLAY_MAX_H) {
        h = AG_DISPLAY_MAX_H;
    }

    const size_t bytes = (size_t)w * (size_t)h * sizeof(uint16_t);
    uint16_t *front = NULL;
    bool      owned = false;

    /* Prefer QEMU's dedicated FB so flush/refresh stays address-valid. */
    if (!qemu_try_attach(w, h, &front)) {
        front = (uint16_t *)heap_caps_malloc(
            bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (front == NULL) {
            front = (uint16_t *)heap_caps_malloc(
                bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (front == NULL) {
            ag_log(AG_LOG_ERROR, "display", "no memory for %ux%u framebuffer",
                   (unsigned)w, (unsigned)h);
            return -AG_ENOMEM;
        }
        owned = true;
    }

    uint16_t *back = (uint16_t *)heap_caps_malloc(
        bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (back == NULL) {
        back = (uint16_t *)heap_caps_malloc(
            bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    /* Back buffer optional: without it we stay single-buffered (direct). */

    uint16_t *snap = (uint16_t *)heap_caps_malloc(
        bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (snap == NULL) {
        snap = (uint16_t *)heap_caps_malloc(
            bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    /* Snapshot is optional: gfxdump then falls back to the live buffer. */

    memset(front, 0, bytes);
    if (back != NULL) {
        memset(back, 0, bytes);
    }
    s_front = front;
    s_back = back;
    s_draw = front;
    s_front_owned = owned;
    s_snap = snap;
    s_w = w;
    s_h = h;
    s_stride = (uint32_t)w * sizeof(uint16_t);
    s_acquired = false;
    s_have_snap = false;
    s_console_gen = 0;

    const ag_dev_desc_t desc = {
        .name = "fb0",
        .driver = "soft",
        .cls = AG_DEV_DISPLAY,
        .flags = 0,
        .ops = &k_fb_ops,
        .priv = front,
    };
    const ag_err_t err = ag_dev_register(&desc, &s_dev);
    if (err != AG_OK) {
        if (owned) {
            heap_caps_free(front);
        }
        if (back != NULL) {
            heap_caps_free(back);
        }
        if (snap != NULL) {
            heap_caps_free(snap);
        }
        s_front = NULL;
        s_back = NULL;
        s_draw = NULL;
        s_snap = NULL;
        return err;
    }

    s_ready = true;
    if (s_qemu_panel != NULL) {
        ag_log(AG_LOG_INFO, "display",
               "soft %ux%u RGB565 (%u KB)%s as fb0 + QEMU RGB window",
               (unsigned)w, (unsigned)h, (unsigned)(bytes / 1024u),
               back != NULL ? " + back" : "");
        qemu_present();
    } else {
        ag_log(AG_LOG_INFO, "display", "soft %ux%u RGB565 (%u KB)%s as fb0",
               (unsigned)w, (unsigned)h, (unsigned)(bytes / 1024u),
               back != NULL ? " + back" : "");
    }
    return AG_OK;
}
