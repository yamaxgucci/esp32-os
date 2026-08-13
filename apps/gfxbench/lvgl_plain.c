/*
 * Bare LVGL backend: lv_obj rects + labels, no theme / slider / bar.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "gfxbench.h"

#include "lvgl.h"

#define COL_BG     0x101820u
#define COL_PANEL  0x202838u
#define COL_EDGE   0x406070u
#define COL_TEXT   0xD0FFE0u
#define COL_MUTED  0x60A080u
#define COL_BAR    0x00E068u
#define COL_THUMB  0xE8E8F0u
#define COL_BTN    0xB8B8C0u
#define COL_SEL    0x30A060u
#define COL_ACCENT 0xE8A54Bu

static lv_display_t *s_disp;
static lv_obj_t     *s_title;
static lv_obj_t     *s_time;
static lv_obj_t     *s_seek_thumb;
static lv_obj_t     *s_vol_thumb;
static lv_obj_t     *s_spec_fill[GFXBENCH_SPEC_N];
static lv_obj_t     *s_eq_thumb[GFXBENCH_EQ_N];
static lv_obj_t     *s_btn[GFXBENCH_BTN_N];
static lv_obj_t     *s_pl_row[GFXBENCH_PL_N];
static lv_obj_t     *s_pl[GFXBENCH_PL_N];
static uint32_t      s_flush_acc;
static char          s_time_buf[8];
static gfxbench_rect_t s_seek_r;
static gfxbench_rect_t s_vol_r;
static gfxbench_rect_t s_eq_r[GFXBENCH_EQ_N];
static gfxbench_rect_t s_spec_r[GFXBENCH_SPEC_N];

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint16_t w, h;
    (void)px_map;
    w = (uint16_t)(area->x2 - area->x1 + 1);
    h = (uint16_t)(area->y2 - area->y1 + 1);
    gfxbench_flush_begin();
    ag_gfx_flush((uint16_t)area->x1, (uint16_t)area->y1, w, h);
    s_flush_acc += gfxbench_flush_end();
    lv_display_flush_ready(disp);
}

static lv_obj_t *rect(lv_obj_t *parent, const gfxbench_rect_t *r, uint32_t col)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, r->x, r->y);
    lv_obj_set_size(o, r->w, r->h);
    lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *label_at(lv_obj_t *parent, const gfxbench_rect_t *r,
                          const char *txt, uint32_t fg, uint32_t bg)
{
    lv_obj_t *o = lv_label_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, r->x, r->y);
    lv_obj_set_size(o, r->w, r->h);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(fg), 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_label_set_text(o, txt);
    return o;
}

static void set_hthumb(lv_obj_t *thumb, const gfxbench_rect_t *track, int permille)
{
    int t = track->w > 8 ? (int)track->w - 8 : 1;
    int x = 2 + (permille * t) / 1000;
    lv_obj_set_x(thumb, x);
}

static void set_vthumb(lv_obj_t *thumb, const gfxbench_rect_t *track, int permille)
{
    int t = track->h > 8 ? (int)track->h - 8 : 1;
    int y = 2 + ((1000 - permille) * t) / 1000;
    lv_obj_set_y(thumb, y);
}

static void set_spec(int i, int pct)
{
    int h = ((int)s_spec_r[i].h * pct) / 100;
    if (h < 1) {
        h = 1;
    }
    lv_obj_set_height(s_spec_fill[i], h);
    lv_obj_align(s_spec_fill[i], LV_ALIGN_BOTTOM_MID, 0, 0);
}

int gfxbench_backend_init(const ag_gfxinfo_t *gi, const gfxbench_layout_t *L)
{
    lv_obj_t *scr;
    lv_obj_t *track;
    gfxbench_rect_t tr;
    int i;
    uint32_t buf_sz;

    lv_init();
    s_disp = lv_display_create((int32_t)gi->width, (int32_t)gi->height);
    if (s_disp == NULL || gi->fb == NULL) {
        return -1;
    }
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, flush_cb);
    buf_sz = (uint32_t)gi->stride * (uint32_t)gi->height;
    lv_display_set_buffers_with_stride(s_disp, gi->fb, NULL, buf_sz,
                                       (uint32_t)gi->stride,
                                       LV_DISPLAY_RENDER_MODE_DIRECT);

    scr = lv_screen_active();
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    (void)rect(scr, &L->mainp, COL_PANEL);
    (void)rect(scr, &L->eq, COL_PANEL);
    (void)rect(scr, &L->pl, COL_PANEL);

    s_title = label_at(scr, &L->title, gfxbench_track_name(2), COL_TEXT, COL_PANEL);
    s_time = label_at(scr, &L->time, "00:00", COL_TEXT, COL_PANEL);

    for (i = 0; i < GFXBENCH_SPEC_N; i++) {
        gfxbench_rect_t fill = L->spec[i];
        s_spec_r[i] = L->spec[i];
        track = rect(scr, &L->spec[i], COL_BG);
        fill.x = 0;
        fill.y = 0;
        fill.h = 1;
        s_spec_fill[i] = rect(track, &fill, COL_BAR);
        lv_obj_set_width(s_spec_fill[i], L->spec[i].w);
        set_spec(i, 20);
    }

    s_seek_r = L->seek;
    track = rect(scr, &L->seek, COL_EDGE);
    tr.x = 2;
    tr.y = 0;
    tr.w = 8;
    tr.h = L->seek.h;
    s_seek_thumb = rect(track, &tr, COL_THUMB);
    set_hthumb(s_seek_thumb, &L->seek, 0);

    s_vol_r = L->vol;
    track = rect(scr, &L->vol, COL_EDGE);
    tr.h = L->vol.h;
    s_vol_thumb = rect(track, &tr, COL_THUMB);
    set_hthumb(s_vol_thumb, &L->vol, 700);

    for (i = 0; i < GFXBENCH_BTN_N; i++) {
        uint32_t col = (i == 2) ? COL_ACCENT : COL_BTN;
        s_btn[i] = rect(scr, &L->btn[i], col);
    }
    for (i = 0; i < GFXBENCH_EQ_N; i++) {
        s_eq_r[i] = L->eq_band[i];
        track = rect(scr, &L->eq_band[i], COL_EDGE);
        tr.x = 0;
        tr.y = 2;
        tr.w = L->eq_band[i].w;
        tr.h = 8;
        s_eq_thumb[i] = rect(track, &tr, COL_THUMB);
        set_vthumb(s_eq_thumb[i], &L->eq_band[i], 400 + (i * 70) % 500);
    }
    for (i = 0; i < GFXBENCH_PL_N; i++) {
        uint32_t bg = (i == 2) ? COL_SEL : COL_PANEL;
        uint32_t fg = (i == 2) ? COL_TEXT : COL_MUTED;
        s_pl_row[i] = rect(scr, &L->pl_row[i], bg);
        s_pl[i] = label_at(scr, &L->pl_row[i], gfxbench_track_name(i), fg, bg);
    }

    s_flush_acc = 0;
    lv_refr_now(s_disp);
    return 0;
}

void gfxbench_backend_on_reacquire(const ag_gfxinfo_t *gi)
{
    uint32_t buf_sz;
    if (s_disp == NULL || gi->fb == NULL) {
        return;
    }
    buf_sz = (uint32_t)gi->stride * (uint32_t)gi->height;
    lv_display_set_buffers_with_stride(s_disp, gi->fb, NULL, buf_sz,
                                       (uint32_t)gi->stride,
                                       LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_obj_invalidate(lv_screen_active());
}

void gfxbench_backend_frame(const gfxbench_state_t *st,
                            const gfxbench_layout_t *L, gfxbench_mode_t mode,
                            gfxbench_timing_t *t)
{
    int i;
    unsigned sec;
    ag_time_t t0;
    uint32_t total;
    (void)L;

    s_flush_acc = 0;
    t0 = ag_micros();

    set_hthumb(s_seek_thumb, &s_seek_r, st->seek);
    for (i = 0; i < GFXBENCH_SPEC_N; i++) {
        set_spec(i, st->spec[i]);
    }
    sec = (st->frame / 30u) % 3600u;
    s_time_buf[0] = (char)('0' + (sec / 600u) % 10u);
    s_time_buf[1] = (char)('0' + (sec / 60u) % 10u);
    s_time_buf[2] = ':';
    s_time_buf[3] = (char)('0' + (sec % 60u) / 10u);
    s_time_buf[4] = (char)('0' + (sec % 10u));
    s_time_buf[5] = '\0';
    lv_label_set_text(s_time, s_time_buf);

    if (mode == GFXBENCH_FULL) {
        set_hthumb(s_vol_thumb, &s_vol_r, st->vol * 10);
        for (i = 0; i < GFXBENCH_EQ_N; i++) {
            set_vthumb(s_eq_thumb[i], &s_eq_r[i], st->eq[i] * 10);
        }
        lv_label_set_text(s_title, gfxbench_track_name(st->sel));
        for (i = 0; i < GFXBENCH_BTN_N; i++) {
            uint32_t col = (st->playing && i == 2) ? COL_ACCENT : COL_BTN;
            lv_obj_set_style_bg_color(s_btn[i], lv_color_hex(col), 0);
        }
        for (i = 0; i < GFXBENCH_PL_N; i++) {
            uint32_t bg = (i == st->sel) ? COL_SEL : COL_PANEL;
            uint32_t fg = (i == st->sel) ? COL_TEXT : COL_MUTED;
            lv_obj_set_style_bg_color(s_pl_row[i], lv_color_hex(bg), 0);
            lv_obj_set_style_bg_color(s_pl[i], lv_color_hex(bg), 0);
            lv_obj_set_style_text_color(s_pl[i], lv_color_hex(fg), 0);
        }
        lv_obj_invalidate(lv_screen_active());
    } else if (mode == GFXBENCH_IDLE) {
        lv_obj_invalidate(lv_screen_active());
    }

    lv_refr_now(s_disp);

    total = (uint32_t)(ag_micros() - t0);
    t->flush_us = s_flush_acc;
    t->draw_us = (total > s_flush_acc) ? (total - s_flush_acc) : 0u;
}

void gfxbench_backend_shutdown(void)
{
    if (s_disp != NULL) {
        lv_display_delete(s_disp);
        s_disp = NULL;
    }
    lv_deinit();
}

const char *gfxbench_backend_name(void) { return "lvglplain"; }
