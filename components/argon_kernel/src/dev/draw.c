/*
 * ArgonOS - integer soft-draw (Bresenham / midpoint / convex scanline).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/draw.h>

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
