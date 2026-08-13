/*
 * Skin blit UI, overlays, mouse hit-testing for AMP.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "amp_app.h"

#include <string.h>

#define COL_BG     0x00101820u
#define COL_FOCUS  0x00E8A54Bu
#define COL_TEXT   0x00D0FFE0u
#define COL_MUTED  0x0060A080u
#define COL_BAR    0x0000E068u
#define COL_THUMB  0x00E8E8F0u
#define COL_SEL    0x0030A060u
#define COL_ICON   0x00101820u
#define COL_BTN_BG 0x00B8B8C0u /* match painted button face for text glyphs */
#define COL_CURSOR 0x00E8A54Bu

static void blit_panel(const amp_skin_panel_t *pan)
{
    if (pan == NULL || pan->pixels == NULL) {
        return;
    }
    ag_gfx_blit(pan->pos.x, pan->pos.y, pan->w, pan->h, pan->pixels,
                (uint32_t)pan->w * 2u, AG_PIX_RGB565);
}

static void draw_focus(amp_player_t *p, amp_panel_t pan)
{
    const amp_rect_t *r = &p->skin.panel[pan].pos;
    if (p->focus != pan) {
        return;
    }
    ag_gfx_stroke_rect(r->x - 1, r->y - 1, (uint16_t)(r->w + 2),
                       (uint16_t)(r->h + 2), COL_FOCUS);
}

static void draw_slider_thumb(const amp_rect_t *track_scr, int permille,
                              int vertical)
{
    int tw = 6, th = 10;
    int x, y;
    if (vertical) {
        int t = track_scr->h > 4 ? (int)track_scr->h - 4 : 1;
        y = track_scr->y + 2 + ((1000 - permille) * t) / 1000;
        x = track_scr->x + ((int)track_scr->w - tw) / 2;
        ag_gfx_fill_rect((int16_t)x, (int16_t)y, (uint16_t)tw, (uint16_t)th,
                         COL_THUMB);
    } else {
        int t = track_scr->w > 4 ? (int)track_scr->w - 4 : 1;
        x = track_scr->x + 2 + (permille * t) / 1000;
        y = track_scr->y + ((int)track_scr->h - th) / 2;
        ag_gfx_fill_rect((int16_t)x, (int16_t)y, (uint16_t)tw, (uint16_t)th,
                         COL_THUMB);
    }
}

static void ctrl_screen_rect(const amp_player_t *p, amp_ctrl_t c,
                             amp_rect_t *out)
{
    amp_panel_t pan = p->skin.ctrl_panel[c];
    const amp_rect_t *pos = &p->skin.panel[pan].pos;
    out->x = (int16_t)(pos->x + p->skin.ctrl[c].x);
    out->y = (int16_t)(pos->y + p->skin.ctrl[c].y);
    out->w = p->skin.ctrl[c].w;
    out->h = p->skin.ctrl[c].h;
}

static void icon_tri(int cx, int cy, int dir, uint32_t col)
{
    /* dir: -1 left, +1 right */
    ag_point_t pts[3];
    int s = 6;
    if (dir >= 0) {
        pts[0].x = (int16_t)(cx - s / 2);
        pts[0].y = (int16_t)(cy - s);
        pts[1].x = (int16_t)(cx + s);
        pts[1].y = (int16_t)cy;
        pts[2].x = (int16_t)(cx - s / 2);
        pts[2].y = (int16_t)(cy + s);
    } else {
        pts[0].x = (int16_t)(cx + s / 2);
        pts[0].y = (int16_t)(cy - s);
        pts[1].x = (int16_t)(cx - s);
        pts[1].y = (int16_t)cy;
        pts[2].x = (int16_t)(cx + s / 2);
        pts[2].y = (int16_t)(cy + s);
    }
    ag_gfx_fill_convex(pts, 3, col);
}

static void icon_bar(int x, int y, int w, int h, uint32_t col)
{
    ag_gfx_fill_rect((int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h, col);
}

static void draw_btn_press(amp_player_t *p, amp_ctrl_t c)
{
    amp_rect_t r;
    uint32_t age;
    if (p->pressed != c) {
        return;
    }
    age = ag_millis() - p->press_ms;
    if (age > 180u) {
        return;
    }
    ctrl_screen_rect(p, c, &r);
    /* Inset darkening while held/just clicked */
    ag_gfx_fill_rect((int16_t)(r.x + 1), (int16_t)(r.y + 1),
                     (uint16_t)(r.w > 2 ? r.w - 2 : 1),
                     (uint16_t)(r.h > 2 ? r.h - 2 : 1), 0x00686878u);
    ag_gfx_stroke_rect(r.x, r.y, r.w, r.h, COL_FOCUS);
}

static void draw_btn_icon(amp_player_t *p, amp_ctrl_t c)
{
    amp_rect_t r;
    int cx, cy;
    ctrl_screen_rect(p, c, &r);
    draw_btn_press(p, c);
    cx = r.x + (int)r.w / 2;
    cy = r.y + (int)r.h / 2;
    switch (c) {
    case AMP_CTRL_PREV:
        icon_bar(cx - 7, cy - 5, 2, 10, COL_ICON);
        icon_tri(cx + 2, cy, -1, COL_ICON);
        break;
    case AMP_CTRL_PLAY:
        icon_tri(cx - 1, cy, 1, COL_ICON);
        break;
    case AMP_CTRL_PAUSE:
        icon_bar(cx - 5, cy - 5, 3, 10, COL_ICON);
        icon_bar(cx + 2, cy - 5, 3, 10, COL_ICON);
        break;
    case AMP_CTRL_STOP:
        icon_bar(cx - 5, cy - 5, 10, 10, COL_ICON);
        break;
    case AMP_CTRL_NEXT:
        icon_tri(cx - 2, cy, 1, COL_ICON);
        icon_bar(cx + 5, cy - 5, 2, 10, COL_ICON);
        break;
    case AMP_CTRL_EJECT: {
        ag_point_t pts[3];
        pts[0].x = (int16_t)(cx - 6);
        pts[0].y = (int16_t)(cy + 2);
        pts[1].x = (int16_t)(cx + 6);
        pts[1].y = (int16_t)(cy + 2);
        pts[2].x = (int16_t)cx;
        pts[2].y = (int16_t)(cy - 6);
        ag_gfx_fill_convex(pts, 3, COL_ICON);
        icon_bar(cx - 6, cy + 4, 12, 2, COL_ICON);
        break;
    }
    case AMP_CTRL_EQ_TOGGLE:
        /* Simple EQ bars. */
        icon_bar(cx - 6, cy + 2, 2, 4, COL_ICON);
        icon_bar(cx - 2, cy - 2, 2, 8, COL_ICON);
        icon_bar(cx + 2, cy, 2, 6, COL_ICON);
        icon_bar(cx + 6, cy - 4, 2, 10, COL_ICON);
        break;
    case AMP_CTRL_PL_TOGGLE:
        icon_bar(cx - 6, cy - 5, 12, 2, COL_ICON);
        icon_bar(cx - 6, cy - 1, 12, 2, COL_ICON);
        icon_bar(cx - 6, cy + 3, 12, 2, COL_ICON);
        break;
    case AMP_CTRL_REPEAT:
        ag_gfx_circle((int16_t)cx, (int16_t)cy, 5, COL_ICON);
        if (p->repeat) {
            icon_bar(cx + 3, cy - 1, 4, 2, COL_ICON);
        }
        break;
    case AMP_CTRL_SHUFFLE:
        ag_gfx_line((int16_t)(cx - 6), (int16_t)(cy - 3), (int16_t)(cx + 6),
                    (int16_t)(cy + 3), COL_ICON);
        ag_gfx_line((int16_t)(cx - 6), (int16_t)(cy + 3), (int16_t)(cx + 6),
                    (int16_t)(cy - 3), COL_ICON);
        if (p->shuffle) {
            icon_bar(cx - 1, cy - 1, 3, 3, COL_ICON);
        }
        break;
    default:
        break;
    }
}

static void draw_transport_icons(amp_player_t *p)
{
    draw_btn_icon(p, AMP_CTRL_PREV);
    draw_btn_icon(p, AMP_CTRL_PLAY);
    draw_btn_icon(p, AMP_CTRL_PAUSE);
    draw_btn_icon(p, AMP_CTRL_STOP);
    draw_btn_icon(p, AMP_CTRL_NEXT);
    draw_btn_icon(p, AMP_CTRL_EJECT);
    draw_btn_icon(p, AMP_CTRL_EQ_TOGGLE);
    draw_btn_icon(p, AMP_CTRL_PL_TOGGLE);
    draw_btn_icon(p, AMP_CTRL_REPEAT);
    draw_btn_icon(p, AMP_CTRL_SHUFFLE);
}

static void draw_eq_btn_icons(amp_player_t *p)
{
    amp_rect_t r;
    int cx, cy;
    /* ON — power dot */
    ctrl_screen_rect(p, AMP_CTRL_EQ_ON, &r);
    draw_btn_press(p, AMP_CTRL_EQ_ON);
    cx = r.x + (int)r.w / 2;
    cy = r.y + (int)r.h / 2;
    ag_gfx_circle((int16_t)cx, (int16_t)cy, 5, COL_ICON);
    icon_bar(cx - 1, cy - 7, 2, 6, COL_ICON);
    if (!p->eq.enabled) {
        ag_gfx_stroke_rect(r.x, r.y, r.w, r.h, 0x00C04040u);
    }
    /* RST — circular arrow hint (X + arc-ish) */
    ctrl_screen_rect(p, AMP_CTRL_EQ_AUTO, &r);
    draw_btn_press(p, AMP_CTRL_EQ_AUTO);
    cx = r.x + (int)r.w / 2;
    cy = r.y + (int)r.h / 2;
    ag_gfx_circle((int16_t)cx, (int16_t)cy, 5, COL_ICON);
    ag_gfx_line((int16_t)(cx + 2), (int16_t)(cy - 5), (int16_t)(cx + 6),
                (int16_t)(cy - 2), COL_ICON);
    ag_gfx_line((int16_t)(cx + 6), (int16_t)(cy - 2), (int16_t)(cx + 3),
                (int16_t)(cy + 1), COL_ICON);
}

static void draw_cursor(const amp_player_t *p)
{
    int x, y;
    if (!p->mouse_live || p->mx < 0 || p->my < 0) {
        return;
    }
    x = p->mx;
    y = p->my;
    /* Arrow cursor */
    {
        ag_point_t pts[3];
        pts[0].x = (int16_t)x;
        pts[0].y = (int16_t)y;
        pts[1].x = (int16_t)(x + 10);
        pts[1].y = (int16_t)(y + 4);
        pts[2].x = (int16_t)(x + 4);
        pts[2].y = (int16_t)(y + 10);
        ag_gfx_fill_convex(pts, 3, COL_CURSOR);
    }
    ag_gfx_line((int16_t)x, (int16_t)y, (int16_t)(x + 12), (int16_t)(y + 12),
                COL_ICON);
}

static void draw_overlays_main(amp_player_t *p)
{
    const amp_rect_t *pos = &p->skin.panel[AMP_PANEL_MAIN].pos;
    amp_rect_t r;
    char buf[32];
    uint32_t pos_ms = 0, dur_ms = 0;
    int i;
    int permille = 0;

    if (p->skin.qvga && p->focus != AMP_PANEL_MAIN) {
        return;
    }

    /* spectrum */
    for (i = 0; i < AMP_EQ_BANDS; i++) {
        int bw = (int)p->skin.spectrum.w / AMP_EQ_BANDS;
        int h = (int)(p->eq.spectrum[i] * (float)p->skin.spectrum.h);
        if (h < 1) {
            h = 1;
        }
        if (h > (int)p->skin.spectrum.h) {
            h = (int)p->skin.spectrum.h;
        }
        ag_gfx_fill_rect((int16_t)(pos->x + p->skin.spectrum.x + i * bw),
                         (int16_t)(pos->y + p->skin.spectrum.y +
                                   (int)p->skin.spectrum.h - h),
                         (uint16_t)(bw > 1 ? bw - 1 : 1), (uint16_t)h, COL_BAR);
    }

    /* ticker: stay inside the LCD window; long titles become "..." */
    {
        int16_t tx = (int16_t)(pos->x + p->skin.ticker.x);
        int16_t ty = (int16_t)(pos->y + p->skin.ticker.y);
        uint16_t tw = p->skin.ticker.w;
        uint16_t th = p->skin.ticker.h;
        uint16_t fit_w = tw > 4 ? (uint16_t)(tw - 4) : tw;
        ag_gfx_clip(tx, ty, tw, th);
        (void)ag_gfx_text_fit((int16_t)(tx + 2), ty, fit_w,
                              p->title[0] ? p->title : "(no title)", COL_TEXT,
                              AG_GFX_TRANS);
        ag_gfx_clip_reset();
    }

    if (p->mp3) {
        pos_ms = ag_mp3_position_ms(p->mp3);
        dur_ms = ag_mp3_duration_ms(p->mp3);
        if (dur_ms > 0) {
            permille = (int)((pos_ms * 1000u) / dur_ms);
        }
    }
    {
        unsigned pm = (pos_ms / 1000u) / 60u, ps = (pos_ms / 1000u) % 60u;
        unsigned dm = (dur_ms / 1000u) / 60u, ds = (dur_ms / 1000u) % 60u;
        int n = 0;
        buf[n++] = (char)('0' + (pm / 10u) % 10u);
        buf[n++] = (char)('0' + pm % 10u);
        buf[n++] = ':';
        buf[n++] = (char)('0' + (ps / 10u) % 10u);
        buf[n++] = (char)('0' + ps % 10u);
        buf[n++] = '/';
        buf[n++] = (char)('0' + (dm / 10u) % 10u);
        buf[n++] = (char)('0' + dm % 10u);
        buf[n++] = ':';
        buf[n++] = (char)('0' + (ds / 10u) % 10u);
        buf[n++] = (char)('0' + ds % 10u);
        buf[n] = '\0';
    }
    {
        int16_t x = (int16_t)(pos->x + p->skin.timebox.x);
        int16_t y = (int16_t)(pos->y + p->skin.timebox.y);
        ag_gfx_clip(x, y, p->skin.timebox.w, p->skin.timebox.h);
        (void)ag_gfx_text_fit(x, y, p->skin.timebox.w, buf, COL_TEXT,
                              AG_GFX_TRANS);
        ag_gfx_clip_reset();
    }

    ctrl_screen_rect(p, AMP_CTRL_SEEK, &r);
    draw_slider_thumb(&r, permille, 0);
    ctrl_screen_rect(p, AMP_CTRL_VOL, &r);
    draw_slider_thumb(&r, p->volume * 10, 0);
    ag_gfx_text((int16_t)(r.x - 28), (int16_t)(r.y - 2), "VOL", COL_MUTED,
                AG_GFX_TRANS);
    ctrl_screen_rect(p, AMP_CTRL_BAL, &r);
    draw_slider_thumb(&r, (p->balance + 100) * 5, 0);
    ag_gfx_text((int16_t)(r.x - 28), (int16_t)(r.y - 2), "BAL", COL_MUTED,
                AG_GFX_TRANS);

    draw_transport_icons(p);
}

static void draw_overlays_eq(amp_player_t *p)
{
    amp_ctrl_t c;
    if (p->skin.qvga && p->focus != AMP_PANEL_EQ) {
        return;
    }
    for (c = AMP_CTRL_EQ_PREAMP; c <= AMP_CTRL_EQ_BAND9; c++) {
        amp_rect_t r;
        int8_t g;
        int permille;
        ctrl_screen_rect(p, c, &r);
        if (c == AMP_CTRL_EQ_PREAMP) {
            g = p->eq.preamp;
        } else {
            g = p->eq.gain[c - AMP_CTRL_EQ_BAND0];
        }
        permille = (int)(((int)g + 12) * 1000) / 24;
        draw_slider_thumb(&r, permille, 1);
        if (p->focus == AMP_PANEL_EQ) {
            int sel = p->eq.band_sel;
            int is_sel = (c == AMP_CTRL_EQ_PREAMP && sel == 0) ||
                         (c >= AMP_CTRL_EQ_BAND0 &&
                          sel == (int)(c - AMP_CTRL_EQ_BAND0 + 1));
            if (is_sel) {
                ag_gfx_stroke_rect(r.x - 1, r.y - 1, (uint16_t)(r.w + 2),
                                   (uint16_t)(r.h + 2), COL_FOCUS);
            }
        }
    }
    draw_eq_btn_icons(p);
}

static void draw_picker(amp_player_t *p)
{
    int i;
    int row_h = 16;
    int x = 40, y = 40;
    int w = (int)p->fb_w - 80;
    int h = (int)p->fb_h - 80;
    int visible;
    if (!p->picker) {
        return;
    }
    if (w < 160) {
        w = (int)p->fb_w - 16;
        x = 8;
    }
    if (h < 80) {
        h = (int)p->fb_h - 16;
        y = 8;
    }
    ag_gfx_fill_rect((int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h,
                     0x00101820u);
    ag_gfx_stroke_rect((int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h,
                       COL_FOCUS);
    ag_gfx_text((int16_t)(x + 8), (int16_t)(y + 4), "Open MP3 (Enter)", COL_TEXT,
                0x00101820u);
    visible = (h - 28) / row_h;
    if (visible < 1) {
        visible = 1;
    }
    if (p->pick_i < 0) {
        p->pick_i = 0;
    }
    for (i = 0; i < visible && i < p->pick_n; i++) {
        int idx = i;
        int16_t ty = (int16_t)(y + 24 + i * row_h);
        const char *base = p->pick[idx];
        const char *s;
        uint32_t bg = 0x00101820u;
        /* simple window: show around selection */
        if (p->pick_n > visible) {
            idx = p->pick_i - visible / 2 + i;
            if (idx < 0) {
                continue;
            }
            if (idx >= p->pick_n) {
                break;
            }
        }
        base = p->pick[idx];
        for (s = base; *s; s++) {
            if (*s == '\\' || *s == '/') {
                base = s + 1;
            }
        }
        if (idx == p->pick_i) {
            bg = COL_SEL;
            ag_gfx_fill_rect((int16_t)(x + 4), ty, (uint16_t)(w - 8),
                             (uint16_t)row_h, bg);
        }
        ag_gfx_text_fit((int16_t)(x + 8), ty,
                        (uint16_t)(w > 16 ? w - 16 : 0), base, COL_TEXT, bg);
    }
}

static void draw_overlays_pl(amp_player_t *p)
{
    amp_rect_t list;
    int row_h;
    int visible;
    int i;
    const amp_rect_t *pos;

    if (p->skin.qvga && p->focus != AMP_PANEL_PL) {
        return;
    }
    pos = &p->skin.panel[AMP_PANEL_PL].pos;
    ctrl_screen_rect(p, AMP_CTRL_PL_LIST, &list);
    row_h = p->skin.pl_row_h > 0 ? p->skin.pl_row_h : 14;
    visible = (int)list.h / row_h;
    if (visible < 1) {
        visible = 1;
    }
    if (p->pl.sel < p->pl.scroll) {
        p->pl.scroll = p->pl.sel;
    }
    if (p->pl.sel >= p->pl.scroll + visible) {
        p->pl.scroll = p->pl.sel - visible + 1;
    }
    ag_gfx_clip(list.x, list.y, list.w, list.h);
    for (i = 0; i < visible; i++) {
        int idx = p->pl.scroll + i;
        int16_t y = (int16_t)(list.y + i * row_h);
        const char *name;
        const char *base;
        if (idx >= p->pl.count) {
            break;
        }
        name = p->pl.paths[idx];
        base = name;
        {
            const char *s;
            for (s = name; *s; s++) {
                if (*s == '\\' || *s == '/') {
                    base = s + 1;
                }
            }
        }
        if (idx == p->pl.sel) {
            ag_gfx_fill_rect(list.x, y, list.w, (uint16_t)row_h, COL_SEL);
        }
        {
            uint32_t row_bg = (idx == p->pl.sel) ? COL_SEL : AG_GFX_TRANS;
            if (idx == p->pl.cur) {
                ag_gfx_text((int16_t)(list.x + 2), y, ">", COL_BAR, row_bg);
            }
            ag_gfx_text_fit((int16_t)(list.x + 12), y,
                            (uint16_t)(list.w > 14 ? list.w - 14 : 0), base,
                            COL_TEXT, row_bg);
        }
    }
    ag_gfx_clip_reset();
    ag_gfx_text((int16_t)(pos->x + 8), (int16_t)(pos->y + 1), "PLAYLIST",
                COL_MUTED, AG_GFX_TRANS);
    (void)pos;
}

void amp_ui_draw(amp_player_t *p)
{
    int i;
    if (p == NULL) {
        return;
    }
    ag_gfx_fill_rect(0, 0, p->fb_w, p->fb_h, COL_BG);
    if (p->skin.qvga) {
        blit_panel(&p->skin.panel[p->focus]);
        draw_focus(p, p->focus);
        if (p->focus == AMP_PANEL_MAIN) {
            draw_overlays_main(p);
        } else if (p->focus == AMP_PANEL_EQ) {
            draw_overlays_eq(p);
        } else {
            draw_overlays_pl(p);
        }
    } else {
        for (i = 0; i < AMP_PANEL_N; i++) {
            blit_panel(&p->skin.panel[i]);
            draw_focus(p, (amp_panel_t)i);
        }
        draw_overlays_main(p);
        draw_overlays_eq(p);
        draw_overlays_pl(p);
    }
    if (p->status[0]) {
        ag_gfx_text_fit(8, (int16_t)(p->fb_h - 18),
                        (uint16_t)(p->fb_w > 16 ? p->fb_w - 16 : 0), p->status,
                        COL_MUTED, COL_BG);
    } else {
        char nbuf[24];
        int n = 0;
        nbuf[n++] = (char)('0' + (p->pl.count / 10) % 10);
        nbuf[n++] = (char)('0' + p->pl.count % 10);
        nbuf[n++] = ' ';
        nbuf[n++] = 't';
        nbuf[n++] = 'r';
        nbuf[n++] = 'k';
        nbuf[n++] = ' ';
        nbuf[n++] = 'E';
        nbuf[n++] = 'j';
        nbuf[n++] = '/';
        nbuf[n++] = 'A';
        nbuf[n++] = '=';
        nbuf[n++] = 'o';
        nbuf[n++] = 'p';
        nbuf[n++] = 'e';
        nbuf[n++] = 'n';
        nbuf[n] = '\0';
        ag_gfx_text(8, (int16_t)(p->fb_h - 18), nbuf, COL_MUTED, COL_BG);
    }
    draw_picker(p);
    draw_cursor(p);
    ag_gfx_flush(0, 0, 0, 0);
}

static void apply_drag(amp_player_t *p, int x, int y)
{
    amp_rect_t r;
    if (p->drag == AMP_CTRL_NONE) {
        return;
    }
    ctrl_screen_rect(p, p->drag, &r);
    if (p->drag == AMP_CTRL_VOL) {
        int t = r.w > 1 ? r.w : 1;
        amp_cmd_volume(p, ((x - r.x) * 100) / t);
    } else if (p->drag == AMP_CTRL_BAL) {
        int t = r.w > 1 ? r.w : 1;
        amp_cmd_balance(p, ((x - r.x) * 200) / t - 100);
    } else if (p->drag == AMP_CTRL_SEEK) {
        int t = r.w > 1 ? r.w : 1;
        amp_cmd_seek_permille(p, ((x - r.x) * 1000) / t);
    } else if (p->drag >= AMP_CTRL_EQ_PREAMP && p->drag <= AMP_CTRL_EQ_BAND9) {
        int t = r.h > 1 ? r.h : 1;
        int permille = 1000 - ((y - r.y) * 1000) / t;
        int db = (permille * 24) / 1000 - 12;
        if (db < -12) {
            db = -12;
        }
        if (db > 12) {
            db = 12;
        }
        if (p->drag == AMP_CTRL_EQ_PREAMP) {
            p->eq.preamp = (int8_t)db;
            p->eq.band_sel = 0;
        } else {
            p->eq.gain[p->drag - AMP_CTRL_EQ_BAND0] = (int8_t)db;
            p->eq.band_sel = (int)(p->drag - AMP_CTRL_EQ_BAND0 + 1);
        }
        amp_eq_recalc(&p->eq);
        p->dirty = 1;
    }
}

void amp_ui_pointer(amp_player_t *p, const ag_event_t *ev)
{
    int x, y;
    if (p == NULL || ev == NULL) {
        return;
    }
    x = (int)ev->ptr.x;
    y = (int)ev->ptr.y;
    p->mx = x;
    p->my = y;
    p->mouse_live = 1;
    p->dirty = 1;

    if (ev->type == AG_EV_POINTER_DOWN) {
        amp_panel_t pan = AMP_PANEL_MAIN;
        amp_ctrl_t c;
        if (p->picker) {
            /* Click row in picker */
            int row = (y - 64) / 16;
            if (row >= 0 && row < p->pick_n) {
                p->pick_i = row;
                amp_btn_press(p, AMP_CTRL_EJECT);
                amp_cmd_picker_choose(p);
            }
            return;
        }
        c = amp_skin_hit(&p->skin, p->focus, x, y, &pan);
        p->mbtn = 1;
        p->focus = pan;
        if (c != AMP_CTRL_NONE) {
            amp_btn_press(p, c);
        }
        if (c == AMP_CTRL_PLAY || c == AMP_CTRL_PAUSE) {
            amp_cmd_play_pause(p);
        } else if (c == AMP_CTRL_STOP) {
            amp_cmd_stop(p);
        } else if (c == AMP_CTRL_NEXT) {
            amp_cmd_next(p);
        } else if (c == AMP_CTRL_PREV) {
            amp_cmd_prev(p);
        } else if (c == AMP_CTRL_EJECT) {
            amp_cmd_open_picker(p);
        } else if (c == AMP_CTRL_PL_TOGGLE) {
            p->focus = AMP_PANEL_PL;
            p->dirty = 1;
        } else if (c == AMP_CTRL_EQ_TOGGLE) {
            p->focus = AMP_PANEL_EQ;
            p->dirty = 1;
        } else if (c == AMP_CTRL_REPEAT) {
            p->repeat = !p->repeat;
            p->dirty = 1;
        } else if (c == AMP_CTRL_SHUFFLE) {
            p->shuffle = !p->shuffle;
            p->dirty = 1;
        } else if (c == AMP_CTRL_EQ_ON) {
            p->eq.enabled = !p->eq.enabled;
            p->dirty = 1;
        } else if (c == AMP_CTRL_EQ_AUTO) {
            amp_eq_reset(&p->eq);
            p->dirty = 1;
        } else if (c == AMP_CTRL_PL_LIST) {
            amp_rect_t list;
            int row;
            ctrl_screen_rect(p, AMP_CTRL_PL_LIST, &list);
            row = (y - list.y) / (p->skin.pl_row_h > 0 ? p->skin.pl_row_h : 14);
            row += p->pl.scroll;
            if (row >= 0 && row < p->pl.count) {
                if (p->pl.sel == row) {
                    amp_cmd_open_sel(p);
                } else {
                    p->pl.sel = row;
                    p->dirty = 1;
                }
            }
        } else if (c == AMP_CTRL_VOL || c == AMP_CTRL_BAL || c == AMP_CTRL_SEEK ||
                   (c >= AMP_CTRL_EQ_PREAMP && c <= AMP_CTRL_EQ_BAND9)) {
            p->drag = c;
            apply_drag(p, x, y);
        }
        p->dirty = 1;
    } else if (ev->type == AG_EV_POINTER_UP) {
        p->mbtn = 0;
        p->drag = AMP_CTRL_NONE;
    } else if (ev->type == AG_EV_POINTER_MOVE) {
        if (p->mbtn && p->drag != AMP_CTRL_NONE) {
            apply_drag(p, x, y);
        }
    } else if (ev->type == AG_EV_WHEEL) {
        int dy = (int)ev->ptr.dy;
        if (p->focus == AMP_PANEL_PL) {
            p->pl.sel -= (dy > 0 ? 1 : -1);
            if (p->pl.sel < 0) {
                p->pl.sel = 0;
            }
            if (p->pl.sel >= p->pl.count && p->pl.count > 0) {
                p->pl.sel = p->pl.count - 1;
            }
            p->dirty = 1;
        } else {
            amp_cmd_volume(p, p->volume + (dy > 0 ? 4 : -4));
        }
    }
}
