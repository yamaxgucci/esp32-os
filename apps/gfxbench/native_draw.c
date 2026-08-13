/*
 * Native ag_gfx_* backend for GFXBENCH.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "gfxbench.h"

#define COL_BG     0x00101820u
#define COL_PANEL  0x00202838u
#define COL_EDGE   0x00406070u
#define COL_TEXT   0x00D0FFE0u
#define COL_MUTED  0x0060A080u
#define COL_BAR    0x0000E068u
#define COL_THUMB  0x00E8E8F0u
#define COL_BTN    0x00B8B8C0u
#define COL_SEL    0x0030A060u
#define COL_ACCENT 0x00E8A54Bu

static uint16_t s_fb_w, s_fb_h;
static int s_need_full;

static void hslider(const gfxbench_rect_t *r, int permille)
{
    int t, x;
    ag_gfx_fill_rect(r->x, r->y, r->w, r->h, COL_EDGE);
    t = r->w > 8 ? (int)r->w - 8 : 1;
    x = r->x + 2 + (permille * t) / 1000;
    ag_gfx_fill_rect((int16_t)x, (int16_t)(r->y - 2), 8, (uint16_t)(r->h + 4),
                     COL_THUMB);
}

static void vslider(const gfxbench_rect_t *r, int permille)
{
    int t, y;
    ag_gfx_fill_rect(r->x, r->y, r->w, r->h, COL_EDGE);
    t = r->h > 8 ? (int)r->h - 8 : 1;
    y = r->y + 2 + ((1000 - permille) * t) / 1000;
    ag_gfx_fill_rect((int16_t)(r->x - 1), (int16_t)y, (uint16_t)(r->w + 2), 8,
                     COL_THUMB);
}

static void panel(const gfxbench_rect_t *r)
{
    ag_gfx_fill_round_rect(r->x, r->y, r->w, r->h, 6, COL_PANEL);
    ag_gfx_stroke_rect(r->x, r->y, r->w, r->h, COL_EDGE);
}

static void draw_spectrum(const gfxbench_layout_t *L, const gfxbench_state_t *st)
{
    int i;
    for (i = 0; i < GFXBENCH_SPEC_N; i++) {
        const gfxbench_rect_t *b = &L->spec[i];
        int h = ((int)b->h * st->spec[i]) / 100;
        if (h < 1) {
            h = 1;
        }
        ag_gfx_fill_rect(b->x, b->y, b->w, b->h, COL_BG);
        ag_gfx_fill_rect(b->x, (int16_t)(b->y + (int)b->h - h), b->w,
                         (uint16_t)h, COL_BAR);
    }
}

static void draw_time(const gfxbench_layout_t *L, const gfxbench_state_t *st)
{
    char buf[8];
    unsigned sec = (st->frame / 30u) % 3600u;
    buf[0] = (char)('0' + (sec / 600u) % 10u);
    buf[1] = (char)('0' + (sec / 60u) % 10u);
    buf[2] = ':';
    buf[3] = (char)('0' + (sec % 60u) / 10u);
    buf[4] = (char)('0' + (sec % 10u));
    buf[5] = '\0';
    ag_gfx_fill_rect(L->time.x, L->time.y, L->time.w, L->time.h, COL_PANEL);
    (void)ag_gfx_text(L->time.x, L->time.y, buf, COL_TEXT, COL_PANEL);
}

static void draw_static(const gfxbench_layout_t *L, const gfxbench_state_t *st)
{
    int i;
    ag_gfx_fill_rect(0, 0, s_fb_w, s_fb_h, COL_BG);
    panel(&L->mainp);
    panel(&L->eq);
    panel(&L->pl);
    (void)ag_gfx_text_fit(L->title.x, L->title.y, L->title.w,
                          gfxbench_track_name(st->sel), COL_TEXT, COL_PANEL);
    for (i = 0; i < GFXBENCH_BTN_N; i++) {
        const gfxbench_rect_t *b = &L->btn[i];
        uint32_t col = (st->playing && i == 2) ? COL_ACCENT : COL_BTN;
        ag_gfx_fill_round_rect(b->x, b->y, b->w, b->h, 3, col);
    }
    for (i = 0; i < GFXBENCH_EQ_N; i++) {
        vslider(&L->eq_band[i], st->eq[i] * 10);
    }
    hslider(&L->vol, st->vol * 10);
    for (i = 0; i < GFXBENCH_PL_N; i++) {
        const gfxbench_rect_t *r = &L->pl_row[i];
        uint32_t bg = (i == st->sel) ? COL_SEL : COL_PANEL;
        uint32_t fg = (i == st->sel) ? COL_TEXT : COL_MUTED;
        ag_gfx_fill_rect(r->x, r->y, r->w, r->h, bg);
        (void)ag_gfx_text_fit((int16_t)(r->x + 2), r->y,
                              (uint16_t)(r->w > 2 ? r->w - 2 : 0),
                              gfxbench_track_name(i), fg, bg);
    }
}

static void draw_dynamic(const gfxbench_layout_t *L, const gfxbench_state_t *st)
{
    draw_spectrum(L, st);
    hslider(&L->seek, st->seek);
    draw_time(L, st);
}

static void flush_union(const gfxbench_rect_t *a, const gfxbench_rect_t *b,
                        const gfxbench_rect_t *c)
{
    int x0 = a->x, y0 = a->y;
    int x1 = a->x + a->w, y1 = a->y + a->h;
    const gfxbench_rect_t *rs[2];
    int i;
    rs[0] = b;
    rs[1] = c;
    for (i = 0; i < 2; i++) {
        const gfxbench_rect_t *r = rs[i];
        if (r->x < x0) {
            x0 = r->x;
        }
        if (r->y < y0) {
            y0 = r->y;
        }
        if (r->x + r->w > x1) {
            x1 = r->x + r->w;
        }
        if (r->y + r->h > y1) {
            y1 = r->y + r->h;
        }
    }
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    ag_gfx_flush((uint16_t)x0, (uint16_t)y0, (uint16_t)(x1 - x0),
                 (uint16_t)(y1 - y0));
}

int gfxbench_backend_init(const ag_gfxinfo_t *gi, const gfxbench_layout_t *L)
{
    gfxbench_state_t st;
    s_fb_w = gi->width;
    s_fb_h = gi->height;
    gfxbench_state_reset(&st);
    draw_static(L, &st);
    draw_dynamic(L, &st);
    ag_gfx_flush(0, 0, 0, 0);
    s_need_full = 0;
    return 0;
}

void gfxbench_backend_on_reacquire(const ag_gfxinfo_t *gi)
{
    s_fb_w = gi->width;
    s_fb_h = gi->height;
    s_need_full = 1;
}

void gfxbench_backend_frame(const gfxbench_state_t *st,
                            const gfxbench_layout_t *L, gfxbench_mode_t mode,
                            gfxbench_timing_t *t)
{
    int full_present = (mode != GFXBENCH_DIRTY) || s_need_full;
    ag_time_t t0 = ag_micros();
    if (full_present) {
        draw_static(L, st);
        draw_dynamic(L, st);
        s_need_full = 0;
    } else {
        draw_dynamic(L, st);
    }
    t->draw_us = (uint32_t)(ag_micros() - t0);
    gfxbench_flush_begin();
    if (!full_present) {
        gfxbench_rect_t spec_all = L->spec[0];
        const gfxbench_rect_t *last = &L->spec[GFXBENCH_SPEC_N - 1];
        spec_all.w = (uint16_t)(last->x + last->w - spec_all.x);
        spec_all.h = last->h;
        flush_union(&L->seek, &spec_all, &L->time);
    } else {
        ag_gfx_flush(0, 0, 0, 0);
    }
    t->flush_us = gfxbench_flush_end();
}

void gfxbench_backend_shutdown(void) {}

const char *gfxbench_backend_name(void) { return "gfxbench"; }
