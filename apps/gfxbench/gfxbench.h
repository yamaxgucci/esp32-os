/*
 * Shared scene + stats for GFXBENCH / LVGLBENCH.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef GFXBENCH_H
#define GFXBENCH_H

#include <argon/argon.h>

#define GFXBENCH_EQ_N   10
#define GFXBENCH_SPEC_N 16
#define GFXBENCH_PL_N   12
#define GFXBENCH_BTN_N  5

typedef enum {
    GFXBENCH_FULL = 0, /* animate all, redraw everything, full flush */
    GFXBENCH_DIRTY,    /* animate seek+spectrum, partial update */
    GFXBENCH_IDLE      /* no animation, full redraw of static UI */
} gfxbench_mode_t;

typedef struct {
    int16_t  x, y;
    uint16_t w, h;
} gfxbench_rect_t;

typedef struct {
    gfxbench_rect_t screen;
    gfxbench_rect_t mainp;
    gfxbench_rect_t eq;
    gfxbench_rect_t pl;
    gfxbench_rect_t seek;
    gfxbench_rect_t vol;
    gfxbench_rect_t btn[GFXBENCH_BTN_N];
    gfxbench_rect_t eq_band[GFXBENCH_EQ_N];
    gfxbench_rect_t spec[GFXBENCH_SPEC_N];
    gfxbench_rect_t title;
    gfxbench_rect_t time;
    gfxbench_rect_t pl_row[GFXBENCH_PL_N];
} gfxbench_layout_t;

typedef struct {
    uint32_t frame;
    int      seek; /* 0..1000 */
    int      vol;  /* 0..100 */
    int      eq[GFXBENCH_EQ_N];
    int      spec[GFXBENCH_SPEC_N];
    int      sel;
    int      playing;
} gfxbench_state_t;

typedef struct {
    uint32_t draw_us;
    uint32_t flush_us;
} gfxbench_timing_t;

void gfxbench_layout_init(gfxbench_layout_t *L, uint16_t w, uint16_t h);
void gfxbench_state_reset(gfxbench_state_t *st);
void gfxbench_state_step(gfxbench_state_t *st, gfxbench_mode_t mode);
const char *gfxbench_track_name(int i);

int         gfxbench_backend_init(const ag_gfxinfo_t *gi,
                                  const gfxbench_layout_t *L);
void        gfxbench_backend_on_reacquire(const ag_gfxinfo_t *gi);
void        gfxbench_backend_frame(const gfxbench_state_t *st,
                                   const gfxbench_layout_t *L,
                                   gfxbench_mode_t mode, gfxbench_timing_t *t);
void        gfxbench_backend_shutdown(void);
const char *gfxbench_backend_name(void);

/* Accumulated inside LVGL flush_cb; native sets timing directly. */
void     gfxbench_flush_begin(void);
uint32_t gfxbench_flush_end(void);

/* Fold LVGL's many dirty rects into one present (QEMU waits if a kick shrinks). */
void     gfxbench_flush_union_reset(void);
void     gfxbench_flush_union_add(int16_t x, int16_t y, uint16_t w, uint16_t h);
uint32_t gfxbench_flush_union_present(void);

#endif /* GFXBENCH_H */
