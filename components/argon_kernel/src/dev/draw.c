/*
 * ArgonOS - integer soft-draw (Bresenham / midpoint / convex scanline).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/draw.h>

#include <math.h>
#include <stddef.h>

uint16_t ag_draw_rgb_to_565(uint32_t color)
{
    const uint32_t r = (color >> 16) & 0xFFu;
    const uint32_t g = (color >> 8) & 0xFFu;
    const uint32_t b = color & 0xFFu;
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

static bool in_clip(const ag_draw_surf_t *s, int32_t x, int32_t y)
{
    if (s == NULL || s->pix == NULL) {
        return false;
    }
    if ((uint32_t)x >= s->w || (uint32_t)y >= s->h) {
        return false;
    }
    if (s->clip_w == 0 || s->clip_h == 0) {
        return true;
    }
    if (x < s->clip_x || y < s->clip_y) {
        return false;
    }
    if (x >= (int32_t)s->clip_x + (int32_t)s->clip_w) {
        return false;
    }
    if (y >= (int32_t)s->clip_y + (int32_t)s->clip_h) {
        return false;
    }
    return true;
}

void ag_draw_pixel(ag_draw_surf_t *s, int32_t x, int32_t y, uint16_t c565)
{
    if (!in_clip(s, x, y)) {
        return;
    }
    s->pix[(uint32_t)y * s->w + (uint32_t)x] = c565;
}

void ag_draw_line(ag_draw_surf_t *s, int32_t x0, int32_t y0, int32_t x1,
                  int32_t y1, uint16_t c565)
{
    int32_t dx = x1 - x0;
    int32_t dy = y1 - y0;
    int32_t sx = 1;
    int32_t sy = 1;
    if (dx < 0) {
        dx = -dx;
        sx = -1;
    }
    if (dy < 0) {
        dy = -dy;
        sy = -1;
    }
    int32_t err = dx - dy;
    for (;;) {
        ag_draw_pixel(s, x0, y0, c565);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int32_t e2 = err * 2;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void ag_draw_circle(ag_draw_surf_t *s, int32_t cx, int32_t cy, int32_t r,
                    uint16_t c565)
{
    if (r < 0) {
        return;
    }
    if (r == 0) {
        ag_draw_pixel(s, cx, cy, c565);
        return;
    }
    int32_t x = r;
    int32_t y = 0;
    int32_t err = 1 - x;
    while (x >= y) {
        ag_draw_pixel(s, cx + x, cy + y, c565);
        ag_draw_pixel(s, cx + y, cy + x, c565);
        ag_draw_pixel(s, cx - y, cy + x, c565);
        ag_draw_pixel(s, cx - x, cy + y, c565);
        ag_draw_pixel(s, cx - x, cy - y, c565);
        ag_draw_pixel(s, cx - y, cy - x, c565);
        ag_draw_pixel(s, cx + y, cy - x, c565);
        ag_draw_pixel(s, cx + x, cy - y, c565);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

static void hline(ag_draw_surf_t *s, int32_t x0, int32_t x1, int32_t y,
                  uint16_t c565)
{
    if (s == NULL || s->pix == NULL || (uint32_t)y >= s->h) {
        return;
    }
    if (s->clip_w != 0 && s->clip_h != 0) {
        if (y < s->clip_y || y >= (int32_t)s->clip_y + (int32_t)s->clip_h) {
            return;
        }
        if (x0 < s->clip_x) {
            x0 = s->clip_x;
        }
        if (x1 >= (int32_t)s->clip_x + (int32_t)s->clip_w) {
            x1 = (int32_t)s->clip_x + (int32_t)s->clip_w - 1;
        }
    }
    if (x0 > x1) {
        const int32_t t = x0;
        x0 = x1;
        x1 = t;
    }
    if (x1 < 0 || x0 >= (int32_t)s->w) {
        return;
    }
    if (x0 < 0) {
        x0 = 0;
    }
    if (x1 >= (int32_t)s->w) {
        x1 = (int32_t)s->w - 1;
    }
    uint16_t *row = &s->pix[(uint32_t)y * s->w + (uint32_t)x0];
    for (int32_t x = x0; x <= x1; x++) {
        *row++ = c565;
    }
}

void ag_draw_fill_circle(ag_draw_surf_t *s, int32_t cx, int32_t cy, int32_t r,
                         uint16_t c565)
{
    if (r < 0) {
        return;
    }
    if (r == 0) {
        ag_draw_pixel(s, cx, cy, c565);
        return;
    }
    int32_t x = r;
    int32_t y = 0;
    int32_t err = 1 - x;
    while (x >= y) {
        hline(s, cx - x, cx + x, cy + y, c565);
        hline(s, cx - y, cx + y, cy + x, c565);
        hline(s, cx - x, cx + x, cy - y, c565);
        hline(s, cx - y, cx + y, cy - x, c565);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

void ag_draw_fill_rect(ag_draw_surf_t *s, int32_t x, int32_t y, int32_t w,
                       int32_t h, uint16_t c565)
{
    if (s == NULL || s->pix == NULL || w <= 0 || h <= 0) {
        return;
    }
    for (int32_t row = 0; row < h; row++) {
        hline(s, x, x + w - 1, y + row, c565);
    }
}

void ag_draw_stroke_rect(ag_draw_surf_t *s, int32_t x, int32_t y, int32_t w,
                         int32_t h, uint16_t c565)
{
    if (s == NULL || w <= 0 || h <= 0) {
        return;
    }
    const int32_t x1 = x + w - 1;
    const int32_t y1 = y + h - 1;
    ag_draw_line(s, x, y, x1, y, c565);
    ag_draw_line(s, x, y1, x1, y1, c565);
    ag_draw_line(s, x, y, x, y1, c565);
    ag_draw_line(s, x1, y, x1, y1, c565);
}

void ag_draw_fill_round_rect(ag_draw_surf_t *s, int32_t x, int32_t y, int32_t w,
                             int32_t h, int32_t r, uint16_t c565)
{
    if (s == NULL || w <= 0 || h <= 0) {
        return;
    }
    if (r < 0) {
        r = 0;
    }
    if (r * 2 > w) {
        r = w / 2;
    }
    if (r * 2 > h) {
        r = h / 2;
    }
    if (r == 0) {
        ag_draw_fill_rect(s, x, y, w, h, c565);
        return;
    }

    /* Centre and side slabs; circles cover the corners. */
    if (w > 2 * r) {
        ag_draw_fill_rect(s, x + r, y, w - 2 * r, h, c565);
    }
    if (h > 2 * r) {
        ag_draw_fill_rect(s, x, y + r, r, h - 2 * r, c565);
        ag_draw_fill_rect(s, x + w - r, y + r, r, h - 2 * r, c565);
    }
    ag_draw_fill_circle(s, x + r, y + r, r, c565);
    ag_draw_fill_circle(s, x + w - 1 - r, y + r, r, c565);
    ag_draw_fill_circle(s, x + r, y + h - 1 - r, r, c565);
    ag_draw_fill_circle(s, x + w - 1 - r, y + h - 1 - r, r, c565);
}

void ag_draw_stroke_convex(ag_draw_surf_t *s, const ag_point_t *pts, int n,
                           uint16_t c565)
{
    if (s == NULL || pts == NULL || n < 2) {
        return;
    }
    for (int i = 0; i < n; i++) {
        const ag_point_t *a = &pts[i];
        const ag_point_t *b = &pts[(i + 1) % n];
        ag_draw_line(s, a->x, a->y, b->x, b->y, c565);
    }
}

void ag_draw_fill_convex(ag_draw_surf_t *s, const ag_point_t *pts, int n,
                         uint16_t c565)
{
    if (s == NULL || pts == NULL || n < 3) {
        return;
    }

    int32_t ymin = pts[0].y;
    int32_t ymax = pts[0].y;
    for (int i = 1; i < n; i++) {
        if (pts[i].y < ymin) {
            ymin = pts[i].y;
        }
        if (pts[i].y > ymax) {
            ymax = pts[i].y;
        }
    }
    if (ymax < 0 || ymin >= (int32_t)s->h) {
        return;
    }
    if (ymin < 0) {
        ymin = 0;
    }
    if (ymax >= (int32_t)s->h) {
        ymax = (int32_t)s->h - 1;
    }

    for (int32_t y = ymin; y <= ymax; y++) {
        int32_t x_min = 0x7fffffff;
        int32_t x_max = -0x7fffffff - 1;
        int hits = 0;
        for (int i = 0; i < n; i++) {
            const int32_t y0 = pts[i].y;
            const int32_t y1 = pts[(i + 1) % n].y;
            const int32_t x0 = pts[i].x;
            const int32_t x1 = pts[(i + 1) % n].x;
            if (y0 == y1) {
                continue;
            }
            if ((y < y0 && y < y1) || (y >= y0 && y >= y1)) {
                continue;
            }
            const int32_t dy = y1 - y0;
            const int32_t x =
                x0 + (int32_t)((int64_t)(y - y0) * (x1 - x0) / dy);
            if (x < x_min) {
                x_min = x;
            }
            if (x > x_max) {
                x_max = x;
            }
            hits++;
        }
        if (hits >= 2) {
            hline(s, x_min, x_max, y, c565);
        }
    }
}

void ag_draw_blit(ag_draw_surf_t *s, int32_t x, int32_t y, int32_t w, int32_t h,
                  const void *src, uint32_t src_stride, int src_be, int use_key,
                  uint16_t key565)
{
    if (s == NULL || s->pix == NULL || src == NULL || w <= 0 || h <= 0) {
        return;
    }
    for (int32_t row = 0; row < h; row++) {
        const int32_t dy = y + row;
        const uint8_t *srow =
            (const uint8_t *)src + (uint32_t)row * src_stride;
        for (int32_t col = 0; col < w; col++) {
            const int32_t dx = x + col;
            uint16_t pix = (uint16_t)srow[(uint32_t)col * 2u] |
                           ((uint16_t)srow[(uint32_t)col * 2u + 1u] << 8);
            if (src_be) {
                pix = (uint16_t)((pix << 8) | (pix >> 8));
            }
            if (use_key && pix == key565) {
                continue;
            }
            ag_draw_pixel(s, dx, dy, pix);
        }
    }
}

static uint16_t pack_565(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

static uint16_t blend_565(uint16_t dst, uint8_t r, uint8_t g, uint8_t b,
                          uint8_t a)
{
    const uint32_t ia = 255u - (uint32_t)a;
    uint32_t dr = (uint32_t)((dst >> 11) & 31u);
    uint32_t dg = (uint32_t)((dst >> 5) & 63u);
    uint32_t db = (uint32_t)(dst & 31u);
    dr = (dr << 3) | (dr >> 2);
    dg = (dg << 2) | (dg >> 4);
    db = (db << 3) | (db >> 2);
    const uint32_t or = ((uint32_t)r * a + dr * ia) / 255u;
    const uint32_t og = ((uint32_t)g * a + dg * ia) / 255u;
    const uint32_t ob = ((uint32_t)b * a + db * ia) / 255u;
    return pack_565(or, og, ob);
}

void ag_draw_blit_argb8888(ag_draw_surf_t *s, int32_t x, int32_t y, int32_t w,
                           int32_t h, const void *src, uint32_t src_stride)
{
    if (s == NULL || s->pix == NULL || src == NULL || w <= 0 || h <= 0) {
        return;
    }
    for (int32_t row = 0; row < h; row++) {
        const int32_t dy = y + row;
        const uint8_t *srow =
            (const uint8_t *)src + (uint32_t)row * src_stride;
        for (int32_t col = 0; col < w; col++) {
            const int32_t dx = x + col;
            const uint8_t *p = srow + (uint32_t)col * 4u;
            const uint8_t b = p[0];
            const uint8_t g = p[1];
            const uint8_t r = p[2];
            const uint8_t a = p[3];
            if (a == 0 || !in_clip(s, dx, dy)) {
                continue;
            }
            if (a == 255) {
                ag_draw_pixel(s, dx, dy, pack_565(r, g, b));
                continue;
            }
            const uint16_t dst =
                s->pix[(uint32_t)dy * s->w + (uint32_t)dx];
            ag_draw_pixel(s, dx, dy, blend_565(dst, r, g, b, a));
        }
    }
}

void ag_draw_glyph8x16(ag_draw_surf_t *s, int32_t x, int32_t y,
                       const uint8_t rows[16], uint16_t fg, uint16_t bg,
                       int trans)
{
    int32_t row;
    if (s == NULL || rows == NULL) {
        return;
    }
    for (row = 0; row < 16; row++) {
        const uint8_t bits = rows[row];
        int32_t col;
        for (col = 0; col < 8; col++) {
            if ((bits & (uint8_t)(1u << col)) != 0) {
                ag_draw_pixel(s, x + col, y + row, fg);
            } else if (!trans) {
                ag_draw_pixel(s, x + col, y + row, bg);
            }
        }
    }
}

int32_t ag_draw_text8x16(ag_draw_surf_t *s, int32_t x, int32_t y,
                         const uint8_t font[256][16], const char *str,
                         uint16_t fg, uint16_t bg, int trans, int32_t max_w)
{
    int32_t cx;
    int32_t cy;
    int32_t advance;

    if (s == NULL || font == NULL || str == NULL) {
        return 0;
    }
    cx = x;
    cy = y;
    advance = 0;
    if (max_w < 0) {
        const char *p;
        for (p = str; *p != '\0'; p++) {
            const uint8_t ch = (uint8_t)*p;
            if (ch == '\n') {
                cx = x;
                cy += 16;
                continue;
            }
            ag_draw_glyph8x16(s, cx, cy, font[ch], fg, bg, trans);
            cx += 8;
            advance += 8;
        }
        return advance;
    }

    {
        int n = 0;
        int nfit;
        int dots;
        int i;
        const char *p;

        for (p = str; *p != '\0' && *p != '\n'; p++) {
            n++;
        }
        if (n * 8 <= max_w) {
            for (i = 0; i < n; i++) {
                ag_draw_glyph8x16(s, x + i * 8, y, font[(uint8_t)str[i]], fg, bg,
                                  trans);
            }
            return n * 8;
        }
        dots = max_w >= 24;
        nfit = dots ? (max_w - 24) / 8 : max_w / 8;
        if (nfit < 0) {
            nfit = 0;
        }
        for (i = 0; i < nfit; i++) {
            ag_draw_glyph8x16(s, x + i * 8, y, font[(uint8_t)str[i]], fg, bg,
                              trans);
        }
        if (dots) {
            for (i = 0; i < 3; i++) {
                ag_draw_glyph8x16(s, x + (nfit + i) * 8, y, font[(uint8_t)'.'],
                                  fg, bg, trans);
            }
            return (nfit + 3) * 8;
        }
        return nfit * 8;
    }
}

static uint16_t load_rgb565(const void *src, uint32_t stride, int32_t x,
                            int32_t y)
{
    const uint8_t *p =
        (const uint8_t *)src + (uint32_t)y * stride + (uint32_t)x * 2u;
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint16_t sample_clamp(const void *src, uint32_t stride, int32_t sx,
                             int32_t sy, int32_t sw, int32_t sh, int32_t u,
                             int32_t v)
{
    if (u < sx) {
        u = sx;
    }
    if (v < sy) {
        v = sy;
    }
    if (u >= sx + sw) {
        u = sx + sw - 1;
    }
    if (v >= sy + sh) {
        v = sy + sh - 1;
    }
    return load_rgb565(src, stride, u, v);
}

static int32_t wrap_dim(int32_t a, int32_t n)
{
    if (n <= 0) {
        return 0;
    }
    a %= n;
    if (a < 0) {
        a += n;
    }
    return a;
}

void ag_draw_blit_scaled(ag_draw_surf_t *s, int32_t dx, int32_t dy, int32_t dw,
                         int32_t dh, const void *src, uint32_t src_stride,
                         int32_t sx, int32_t sy, int32_t sw, int32_t sh)
{
    int32_t row;
    if (s == NULL || s->pix == NULL || src == NULL || dw <= 0 || dh <= 0 ||
        sw <= 0 || sh <= 0) {
        return;
    }
    for (row = 0; row < dh; row++) {
        int32_t col;
        const int32_t v = sy + (int32_t)((int64_t)row * sh / dh);
        for (col = 0; col < dw; col++) {
            const int32_t u = sx + (int32_t)((int64_t)col * sw / dw);
            ag_draw_pixel(s, dx + col, dy + row,
                          sample_clamp(src, src_stride, sx, sy, sw, sh, u, v));
        }
    }
}

void ag_draw_blit_tiled(ag_draw_surf_t *s, int32_t dx, int32_t dy, int32_t dw,
                        int32_t dh, const void *src, uint32_t src_stride,
                        int32_t sx, int32_t sy, int32_t sw, int32_t sh)
{
    int32_t row;
    if (s == NULL || s->pix == NULL || src == NULL || dw <= 0 || dh <= 0 ||
        sw <= 0 || sh <= 0) {
        return;
    }
    for (row = 0; row < dh; row++) {
        int32_t col;
        const int32_t v = sy + wrap_dim(row, sh);
        for (col = 0; col < dw; col++) {
            const int32_t u = sx + wrap_dim(col, sw);
            ag_draw_pixel(s, dx + col, dy + row,
                          load_rgb565(src, src_stride, u, v));
        }
    }
}

static int32_t lerp16(int32_t a, int32_t b, int32_t t, int32_t den)
{
    if (den == 0) {
        return a << 16;
    }
    return (a << 16) + (int32_t)(((int64_t)(b - a) << 16) * t / den);
}

/* Affine UV on one triangle.  n-gons are fanned; span-UV on n>3 disagrees. */
static void fill_tex_tri(ag_draw_surf_t *s, ag_draw_texvert_t a,
                         ag_draw_texvert_t b, ag_draw_texvert_t c,
                         const void *src, uint32_t src_stride, int32_t sx,
                         int32_t sy, int32_t sw, int32_t sh)
{
    const ag_draw_texvert_t pts[3] = {a, b, c};
    int32_t ymin = a.y;
    int32_t ymax = a.y;
    int i;

    if (b.y < ymin) {
        ymin = b.y;
    }
    if (c.y < ymin) {
        ymin = c.y;
    }
    if (b.y > ymax) {
        ymax = b.y;
    }
    if (c.y > ymax) {
        ymax = c.y;
    }
    if (ymax < 0 || ymin >= (int32_t)s->h) {
        return;
    }
    if (ymin < 0) {
        ymin = 0;
    }
    if (ymax >= (int32_t)s->h) {
        ymax = (int32_t)s->h - 1;
    }

    for (int32_t y = ymin; y <= ymax; y++) {
        int32_t x_min = 0x7fffffff;
        int32_t x_max = -0x7fffffff - 1;
        int32_t u_min = 0;
        int32_t v_min = 0;
        int32_t u_max = 0;
        int32_t v_max = 0;
        int hits = 0;
        for (i = 0; i < 3; i++) {
            const int32_t y0 = pts[i].y;
            const int32_t y1 = pts[(i + 1) % 3].y;
            const int32_t x0 = pts[i].x;
            const int32_t x1 = pts[(i + 1) % 3].x;
            const int32_t dy = y1 - y0;
            int32_t x;
            int32_t u;
            int32_t v;
            if (dy == 0) {
                continue;
            }
            if ((y < y0 && y < y1) || (y >= y0 && y >= y1)) {
                continue;
            }
            x = x0 + (int32_t)((int64_t)(y - y0) * (x1 - x0) / dy);
            u = lerp16(pts[i].u, pts[(i + 1) % 3].u, y - y0, dy);
            v = lerp16(pts[i].v, pts[(i + 1) % 3].v, y - y0, dy);
            if (x < x_min) {
                x_min = x;
                u_min = u;
                v_min = v;
            }
            if (x > x_max) {
                x_max = x;
                u_max = u;
                v_max = v;
            }
            hits++;
        }
        if (hits < 2) {
            continue;
        }
        if (x_max == x_min) {
            ag_draw_pixel(s, x_min, y,
                          sample_clamp(src, src_stride, sx, sy, sw, sh,
                                       u_min >> 16, v_min >> 16));
            continue;
        }
        for (int32_t x = x_min; x <= x_max; x++) {
            const int32_t den = x_max - x_min;
            const int32_t u =
                u_min + (int32_t)((int64_t)(u_max - u_min) * (x - x_min) / den);
            const int32_t v =
                v_min + (int32_t)((int64_t)(v_max - v_min) * (x - x_min) / den);
            ag_draw_pixel(s, x, y,
                          sample_clamp(src, src_stride, sx, sy, sw, sh, u >> 16,
                                       v >> 16));
        }
    }
}

static float cross2(float ax, float ay, float bx, float by)
{
    return ax * by - ay * bx;
}

/*
 * Inverse bilinear: p = mix(mix(A,B,s), mix(D,C,s), t) for quad A,B,C,D.
 * After Iñigo Quílez.  Returns 0 if the point is degenerate.
 */
static int inv_bilinear(int32_t px, int32_t py, const ag_draw_texvert_t q[4],
                        float *out_s, float *out_t)
{
    const float ax = (float)q[0].x;
    const float ay = (float)q[0].y;
    const float ex = (float)q[1].x - ax;
    const float ey = (float)q[1].y - ay;
    const float fx = (float)q[3].x - ax;
    const float fy = (float)q[3].y - ay;
    const float gx = ax - (float)q[1].x + (float)q[2].x - (float)q[3].x;
    const float gy = ay - (float)q[1].y + (float)q[2].y - (float)q[3].y;
    const float hx = (float)px - ax;
    const float hy = (float)py - ay;
    const float k2 = cross2(gx, gy, fx, fy);
    const float k1 = cross2(ex, ey, fx, fy) + cross2(hx, hy, gx, gy);
    const float k0 = cross2(hx, hy, ex, ey);
    const float eps = 1e-4f;
    float s;
    float t;

    if (k2 > -eps && k2 < eps) {
        float den;
        if (k1 > -eps && k1 < eps) {
            return 0;
        }
        t = -k0 / k1;
        den = ex * k1 - gx * k0;
        if (den > -eps && den < eps) {
            return 0;
        }
        s = (hx * k1 + fx * k0) / den;
    } else {
        float w = k1 * k1 - 4.f * k0 * k2;
        float ik2;
        float den;
        if (w < 0.f) {
            return 0;
        }
        w = sqrtf(w);
        ik2 = 0.5f / k2;
        t = (-k1 - w) * ik2;
        den = ex + gx * t;
        if (den > -eps && den < eps) {
            den = ey + gy * t;
            if (den > -eps && den < eps) {
                return 0;
            }
            s = (hy - fy * t) / den;
        } else {
            s = (hx - fx * t) / den;
        }
        if (s < -0.02f || s > 1.02f || t < -0.02f || t > 1.02f) {
            t = (-k1 + w) * ik2;
            den = ex + gx * t;
            if (den > -eps && den < eps) {
                den = ey + gy * t;
                if (den > -eps && den < eps) {
                    return 0;
                }
                s = (hy - fy * t) / den;
            } else {
                s = (hx - fx * t) / den;
            }
        }
    }
    *out_s = s;
    *out_t = t;
    return 1;
}

/* One bilinear map over a quad — no triangle-fan seam. */
static void fill_tex_quad(ag_draw_surf_t *s, const ag_draw_texvert_t q[4],
                          const void *src, uint32_t src_stride, int32_t sx,
                          int32_t sy, int32_t sw, int32_t sh)
{
    int32_t ymin = q[0].y;
    int32_t ymax = q[0].y;
    int i;

    for (i = 1; i < 4; i++) {
        if (q[i].y < ymin) {
            ymin = q[i].y;
        }
        if (q[i].y > ymax) {
            ymax = q[i].y;
        }
    }
    if (ymax < 0 || ymin >= (int32_t)s->h) {
        return;
    }
    if (ymin < 0) {
        ymin = 0;
    }
    if (ymax >= (int32_t)s->h) {
        ymax = (int32_t)s->h - 1;
    }

    for (int32_t y = ymin; y <= ymax; y++) {
        int32_t x_min = 0x7fffffff;
        int32_t x_max = -0x7fffffff - 1;
        int hits = 0;
        for (i = 0; i < 4; i++) {
            const int32_t y0 = q[i].y;
            const int32_t y1 = q[(i + 1) % 4].y;
            const int32_t x0 = q[i].x;
            const int32_t x1 = q[(i + 1) % 4].x;
            const int32_t dy = y1 - y0;
            int32_t x;
            if (dy == 0) {
                continue;
            }
            if ((y < y0 && y < y1) || (y >= y0 && y >= y1)) {
                continue;
            }
            x = x0 + (int32_t)((int64_t)(y - y0) * (x1 - x0) / dy);
            if (x < x_min) {
                x_min = x;
            }
            if (x > x_max) {
                x_max = x;
            }
            hits++;
        }
        if (hits < 2) {
            continue;
        }
        for (int32_t x = x_min; x <= x_max; x++) {
            float st_s;
            float st_t;
            float u;
            float v;
            if (!inv_bilinear(x, y, q, &st_s, &st_t)) {
                continue;
            }
            if (st_s < 0.f) {
                st_s = 0.f;
            }
            if (st_s > 1.f) {
                st_s = 1.f;
            }
            if (st_t < 0.f) {
                st_t = 0.f;
            }
            if (st_t > 1.f) {
                st_t = 1.f;
            }
            u = (1.f - st_s) * (1.f - st_t) * (float)q[0].u +
                st_s * (1.f - st_t) * (float)q[1].u +
                st_s * st_t * (float)q[2].u +
                (1.f - st_s) * st_t * (float)q[3].u;
            v = (1.f - st_s) * (1.f - st_t) * (float)q[0].v +
                st_s * (1.f - st_t) * (float)q[1].v +
                st_s * st_t * (float)q[2].v +
                (1.f - st_s) * st_t * (float)q[3].v;
            ag_draw_pixel(
                s, x, y,
                sample_clamp(src, src_stride, sx, sy, sw, sh,
                             (int32_t)(u + (u >= 0.f ? 0.5f : -0.5f)),
                             (int32_t)(v + (v >= 0.f ? 0.5f : -0.5f))));
        }
    }
}

void ag_draw_fill_convex_tex(ag_draw_surf_t *s, const ag_draw_texvert_t *pts,
                             int n, const void *src, uint32_t src_stride,
                             int32_t sx, int32_t sy, int32_t sw, int32_t sh)
{
    int i;
    if (s == NULL || s->pix == NULL || pts == NULL || src == NULL || n < 3 ||
        sw <= 0 || sh <= 0) {
        return;
    }
    if (n == 4) {
        fill_tex_quad(s, pts, src, src_stride, sx, sy, sw, sh);
        return;
    }
    for (i = 1; i < n - 1; i++) {
        fill_tex_tri(s, pts[0], pts[i], pts[i + 1], src, src_stride, sx, sy, sw,
                     sh);
    }
}
