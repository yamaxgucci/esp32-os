/*
 * ArgonOS - soft local display: RGB565 framebuffer in PSRAM.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/display.h>

#include <stdio.h>
#include <string.h>

#include <argon/board.h>
#include <argon/console.h>
#include <argon/device.h>
#include <argon/log.h>
#include <argon/path.h>
#include <argon/vfs.h>

#include "dev/font8x16.h"
#include "esp_heap_caps.h"

#define AG_DISPLAY_DEFAULT_W 320
#define AG_DISPLAY_DEFAULT_H 240
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

static uint16_t *s_fb;
static uint16_t *s_snap; /* last released graphics frame, for gfxdump */
static uint16_t  s_w;
static uint16_t  s_h;
static uint32_t  s_stride; /* bytes per row */
static bool      s_ready;
static bool      s_acquired;
static bool      s_have_snap;
static uint32_t  s_console_gen;
static ag_device_t *s_dev;

/* ---------------------------------------------------------------------- */

/* Colour arguments are 0x00RRGGBB. */
static uint16_t rgb_to_565(uint32_t color)
{
    const uint32_t r = (color >> 16) & 0xFFu;
    const uint32_t g = (color >> 8) & 0xFFu;
    const uint32_t b = color & 0xFFu;
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

static void put_pixel(int32_t x, int32_t y, uint16_t c)
{
    if ((uint32_t)x >= s_w || (uint32_t)y >= s_h) {
        return;
    }
    s_fb[(uint32_t)y * s_w + (uint32_t)x] = c;
}

static void fill_rect_raw(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    if (w <= 0 || h <= 0 || s_fb == NULL) {
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
    if (x >= (int32_t)s_w || y >= (int32_t)s_h) {
        return;
    }
    if (x + w > (int32_t)s_w) {
        w = (int32_t)s_w - x;
    }
    if (y + h > (int32_t)s_h) {
        h = (int32_t)s_h - y;
    }
    for (int32_t row = 0; row < h; row++) {
        uint16_t *dst = &s_fb[(uint32_t)(y + row) * s_w + (uint32_t)x];
        for (int32_t col = 0; col < w; col++) {
            dst[col] = c;
        }
    }
}

static void draw_glyph(int32_t x, int32_t y, uint8_t ch, uint16_t fg, uint16_t bg)
{
    const uint8_t *rows = ag_font8x16[ch];
    for (int32_t row = 0; row < AG_FONT8X16_H; row++) {
        const uint8_t bits = rows[row];
        for (int32_t col = 0; col < AG_FONT8X16_W; col++) {
            const uint16_t c = (bits & (uint8_t)(0x80u >> col)) ? fg : bg;
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
    if (s_fb == NULL || buf == NULL) {
        return -AG_EINVAL;
    }
    const uint64_t total = (uint64_t)s_stride * s_h;
    if (off >= total) {
        return 0;
    }
    if (off + len > total) {
        len = (size_t)(total - off);
    }
    memcpy(buf, (const uint8_t *)s_fb + (size_t)off, len);
    return (int32_t)len;
}

static int32_t fb_write(ag_device_t *dev, const void *buf, size_t len,
                        uint64_t off)
{
    (void)dev;
    if (s_fb == NULL || buf == NULL) {
        return -AG_EINVAL;
    }
    const uint64_t total = (uint64_t)s_stride * s_h;
    if (off >= total) {
        return -AG_EINVAL;
    }
    if (off + len > total) {
        len = (size_t)(total - off);
    }
    memcpy((uint8_t *)s_fb + (size_t)off, buf, len);
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
    if (!s_ready || s_fb == NULL) {
        return -AG_ENODEV;
    }
    if (s_acquired) {
        return -AG_EBUSY;
    }
    s_acquired = true;
    if (out != NULL) {
        out->width = s_w;
        out->height = s_h;
        out->fmt = AG_PIX_RGB565;
        out->stride = s_stride;
        out->fb = s_fb;
        out->double_buf = false;
        out->direct = true; /* soft fb is the front buffer */
    }
    return AG_OK;
}

static void gfx_release(void)
{
    if (!s_acquired) {
        return;
    }
    /* Keep a copy so `gfxdump` after `run` still sees the demo frame. */
    if (s_snap != NULL && s_fb != NULL) {
        memcpy(s_snap, s_fb, (size_t)s_stride * s_h);
        s_have_snap = true;
    }
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
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    /* Soft display: pixels are already in s_fb. */
}

static void gfx_swap(void) { /* single buffer */ }

static void gfx_clear(uint32_t color)
{
    if (s_fb == NULL) {
        return;
    }
    fill_rect_raw(0, 0, (int32_t)s_w, (int32_t)s_h, rgb_to_565(color));
}

static void gfx_fill_rect(int16_t x, int16_t y, uint16_t w, uint16_t h,
                          uint32_t color)
{
    fill_rect_raw(x, y, (int32_t)w, (int32_t)h, rgb_to_565(color));
}

static void gfx_blit(int16_t x, int16_t y, uint16_t w, uint16_t h,
                     const void *src, uint32_t src_stride, ag_pixfmt_t src_fmt)
{
    if (s_fb == NULL || src == NULL || w == 0 || h == 0) {
        return;
    }
    if (src_fmt != AG_PIX_RGB565 && src_fmt != AG_PIX_RGB565_BE) {
        return; /* soft path only knows 16-bit RGB for now */
    }
    for (uint16_t row = 0; row < h; row++) {
        const int32_t dy = (int32_t)y + (int32_t)row;
        if ((uint32_t)dy >= s_h) {
            continue;
        }
        const uint8_t *srow = (const uint8_t *)src + (uint32_t)row * src_stride;
        for (uint16_t col = 0; col < w; col++) {
            const int32_t dx = (int32_t)x + (int32_t)col;
            if ((uint32_t)dx >= s_w) {
                continue;
            }
            uint16_t pix = (uint16_t)srow[col * 2] |
                           ((uint16_t)srow[col * 2 + 1] << 8);
            if (src_fmt == AG_PIX_RGB565_BE) {
                pix = (uint16_t)((pix << 8) | (pix >> 8));
            }
            s_fb[(uint32_t)dy * s_w + (uint32_t)dx] = pix;
        }
    }
}

static int32_t gfx_text(int16_t x, int16_t y, const char *s, uint32_t fg,
                        uint32_t bg)
{
    if (s == NULL || s_fb == NULL) {
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
};

/* ---------------------------------------------------------------------- */

bool ag_display_ready(void) { return s_ready; }

bool ag_display_acquired(void) { return s_acquired; }

void ag_display_render_console(const ag_screen_t *screen)
{
    if (!s_ready || s_acquired || s_fb == NULL || screen == NULL) {
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
}

ag_err_t ag_display_dump_ppm(const char *path, const char *cwd, bool live)
{
    if (!s_ready || s_fb == NULL || path == NULL) {
        return -AG_ENODEV;
    }

    /*
     * Prefer the last graphics snapshot when text has already been painted
     * back over the live buffer - that is the usual case right after `run`.
     */
    const uint16_t *src = s_fb;
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
    uint16_t *fb = (uint16_t *)heap_caps_malloc(bytes,
                                                MALLOC_CAP_SPIRAM |
                                                    MALLOC_CAP_8BIT);
    if (fb == NULL) {
        fb = (uint16_t *)heap_caps_malloc(bytes,
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (fb == NULL) {
        ag_log(AG_LOG_ERROR, "display", "no memory for %ux%u framebuffer",
               (unsigned)w, (unsigned)h);
        return -AG_ENOMEM;
    }

    uint16_t *snap = (uint16_t *)heap_caps_malloc(bytes,
                                                   MALLOC_CAP_SPIRAM |
                                                       MALLOC_CAP_8BIT);
    if (snap == NULL) {
        snap = (uint16_t *)heap_caps_malloc(
            bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    /* Snapshot is optional: gfxdump then falls back to the live buffer. */

    memset(fb, 0, bytes);
    s_fb = fb;
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
        .priv = fb,
    };
    const ag_err_t err = ag_dev_register(&desc, &s_dev);
    if (err != AG_OK) {
        heap_caps_free(fb);
        if (snap != NULL) {
            heap_caps_free(snap);
        }
        s_fb = NULL;
        s_snap = NULL;
        return err;
    }

    s_ready = true;
    ag_log(AG_LOG_INFO, "display", "soft %ux%u RGB565 (%u KB) as fb0",
           (unsigned)w, (unsigned)h, (unsigned)(bytes / 1024u));
    return AG_OK;
}
