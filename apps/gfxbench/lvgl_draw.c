/*
 * LVGL widget backend for LVGLBENCH — same layout as native_draw.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "gfxbench.h"

#include "lvgl.h"

static lv_display_t *s_disp;
static lv_obj_t     *s_title;
static lv_obj_t     *s_time;
static lv_obj_t     *s_seek;
static lv_obj_t     *s_vol;
static lv_obj_t     *s_eq[GFXBENCH_EQ_N];
static lv_obj_t     *s_spec[GFXBENCH_SPEC_N];
static lv_obj_t     *s_pl[GFXBENCH_PL_N];
static lv_obj_t     *s_btn[GFXBENCH_BTN_N];
static char          s_time_buf[8];

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)px_map;
    gfxbench_flush_union_add((int16_t)area->x1, (int16_t)area->y1,
                             (uint16_t)(area->x2 - area->x1 + 1),
                             (uint16_t)(area->y2 - area->y1 + 1));
    lv_display_flush_ready(disp);
}

static lv_obj_t *mk_panel(lv_obj_t *parent, const gfxbench_rect_t *r)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_pos(o, r->x, r->y);
    lv_obj_set_size(o, r->w, r->h);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *mk_bar(lv_obj_t *parent, const gfxbench_rect_t *r, int maxv)
{
    lv_obj_t *o = lv_bar_create(parent);
    lv_obj_set_pos(o, r->x, r->y);
    lv_obj_set_size(o, r->w, r->h);
    lv_bar_set_range(o, 0, maxv);
    lv_bar_set_orientation(o, LV_BAR_ORIENTATION_VERTICAL);
    return o;
}

static lv_obj_t *mk_slider(lv_obj_t *parent, const gfxbench_rect_t *r, int maxv,
                           int vertical)
{
    lv_obj_t *o = lv_slider_create(parent);
    lv_obj_set_pos(o, r->x, r->y);
    lv_obj_set_size(o, r->w, r->h);
    lv_slider_set_range(o, 0, maxv);
    if (vertical) {
        lv_slider_set_orientation(o, LV_SLIDER_ORIENTATION_VERTICAL);
    }
    return o;
}

int gfxbench_backend_init(const ag_gfxinfo_t *gi, const gfxbench_layout_t *L)
{
    lv_obj_t *scr;
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
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    (void)mk_panel(scr, &L->mainp);
    (void)mk_panel(scr, &L->eq);
    (void)mk_panel(scr, &L->pl);

    s_title = lv_label_create(scr);
    lv_obj_set_pos(s_title, L->title.x, L->title.y);
    lv_label_set_text(s_title, gfxbench_track_name(2));

    s_time = lv_label_create(scr);
    lv_obj_set_pos(s_time, L->time.x, L->time.y);
    lv_label_set_text(s_time, "00:00");

    for (i = 0; i < GFXBENCH_SPEC_N; i++) {
        s_spec[i] = mk_bar(scr, &L->spec[i], 100);
        lv_bar_set_value(s_spec[i], 20, LV_ANIM_OFF);
    }
    s_seek = mk_slider(scr, &L->seek, 1000, 0);
    s_vol = mk_slider(scr, &L->vol, 100, 0);
    lv_slider_set_value(s_vol, 70, LV_ANIM_OFF);

    for (i = 0; i < GFXBENCH_BTN_N; i++) {
        s_btn[i] = lv_button_create(scr);
        lv_obj_set_pos(s_btn[i], L->btn[i].x, L->btn[i].y);
        lv_obj_set_size(s_btn[i], L->btn[i].w, L->btn[i].h);
    }
    for (i = 0; i < GFXBENCH_EQ_N; i++) {
        s_eq[i] = mk_slider(scr, &L->eq_band[i], 100, 1);
        lv_slider_set_value(s_eq[i], 40, LV_ANIM_OFF);
    }
    for (i = 0; i < GFXBENCH_PL_N; i++) {
        s_pl[i] = lv_label_create(scr);
        lv_obj_set_pos(s_pl[i], L->pl_row[i].x, L->pl_row[i].y);
        lv_label_set_text(s_pl[i], gfxbench_track_name(i));
    }

    gfxbench_flush_union_reset();
    lv_refr_now(s_disp);
    (void)gfxbench_flush_union_present();
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
    (void)L;

    gfxbench_flush_union_reset();
    t0 = ag_micros();

    lv_slider_set_value(s_seek, st->seek, LV_ANIM_OFF);
    for (i = 0; i < GFXBENCH_SPEC_N; i++) {
        lv_bar_set_value(s_spec[i], st->spec[i], LV_ANIM_OFF);
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
        lv_slider_set_value(s_vol, st->vol, LV_ANIM_OFF);
        for (i = 0; i < GFXBENCH_EQ_N; i++) {
            lv_slider_set_value(s_eq[i], st->eq[i], LV_ANIM_OFF);
        }
        lv_label_set_text(s_title, gfxbench_track_name(st->sel));
        lv_obj_invalidate(lv_screen_active());
    } else if (mode == GFXBENCH_IDLE) {
        lv_obj_invalidate(lv_screen_active());
    }

    lv_refr_now(s_disp);
    t->draw_us = (uint32_t)(ag_micros() - t0);
    t->flush_us = gfxbench_flush_union_present();
}

void gfxbench_backend_shutdown(void)
{
    if (s_disp != NULL) {
        lv_display_delete(s_disp);
        s_disp = NULL;
    }
    lv_deinit();
}

const char *gfxbench_backend_name(void) { return "lvglbench"; }
