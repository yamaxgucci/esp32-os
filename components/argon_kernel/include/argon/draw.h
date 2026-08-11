/*
 * ArgonOS - integer soft-draw helpers (RGB565 surface).
 *
 * Pure geometry; no display ownership.  Used by the gfx ABI and host tests.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_DRAW_H
#define ARGON_DRAW_H

#include <stdint.h>

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t *pix;
    uint16_t  w;
    uint16_t  h;
    /* Inclusive-exclusive clip; when clip_w/clip_h are 0, the whole surface. */
    int16_t   clip_x;
    int16_t   clip_y;
    uint16_t  clip_w;
    uint16_t  clip_h;
} ag_draw_surf_t;

/* Colour is already RGB565. */
void ag_draw_pixel(ag_draw_surf_t *s, int32_t x, int32_t y, uint16_t c565);
void ag_draw_line(ag_draw_surf_t *s, int32_t x0, int32_t y0, int32_t x1,
                  int32_t y1, uint16_t c565);
void ag_draw_circle(ag_draw_surf_t *s, int32_t cx, int32_t cy, int32_t r,
                    uint16_t c565);
void ag_draw_fill_circle(ag_draw_surf_t *s, int32_t cx, int32_t cy, int32_t r,
                         uint16_t c565);
void ag_draw_stroke_convex(ag_draw_surf_t *s, const ag_point_t *pts, int n,
                           uint16_t c565);
void ag_draw_fill_convex(ag_draw_surf_t *s, const ag_point_t *pts, int n,
                         uint16_t c565);
void ag_draw_stroke_rect(ag_draw_surf_t *s, int32_t x, int32_t y, int32_t w,
                         int32_t h, uint16_t c565);
void ag_draw_fill_round_rect(ag_draw_surf_t *s, int32_t x, int32_t y, int32_t w,
                             int32_t h, int32_t r, uint16_t c565);
void ag_draw_fill_rect(ag_draw_surf_t *s, int32_t x, int32_t y, int32_t w,
                       int32_t h, uint16_t c565);

/*
 * Copy an RGB565 (or RGB565_BE) rectangle onto the surface.  When use_key is
 * non-zero, pixels equal to key565 are skipped (sprite chroma key).
 */
void ag_draw_blit(ag_draw_surf_t *s, int32_t x, int32_t y, int32_t w, int32_t h,
                  const void *src, uint32_t src_stride, int src_be, int use_key,
                  uint16_t key565);

/* 0x00RRGGBB → RGB565 */
uint16_t ag_draw_rgb_to_565(uint32_t color);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_DRAW_H */
