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

    /* ticker */
    ag_gfx_text((int16_t)(pos->x + p->skin.ticker.x + 2),
                (int16_t)(pos->y + p->skin.ticker.y),
                p->title[0] ? p->title : "(no title)", COL_TEXT, 0);

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
    ag_gfx_text((int16_t)(pos->x + p->skin.timebox.x),
                (int16_t)(pos->y + p->skin.timebox.y), buf, COL_TEXT, 0);

    ctrl_screen_rect(p, AMP_CTRL_SEEK, &r);
    draw_slider_thumb(&r, permille, 0);
    ctrl_screen_rect(p, AMP_CTRL_VOL, &r);
    draw_slider_thumb(&r, p->volume * 10, 0);
    ctrl_screen_rect(p, AMP_CTRL_BAL, &r);
    draw_slider_thumb(&r, (p->balance + 100) * 5, 0);
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
    if (!p->eq.enabled) {
        amp_rect_t r;
        ctrl_screen_rect(p, AMP_CTRL_EQ_ON, &r);
        ag_gfx_stroke_rect(r.x, r.y, r.w, r.h, 0x00C04040u);
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
        if (idx == p->pl.cur) {
            ag_gfx_text((int16_t)(list.x + 2), y, ">", COL_BAR, 0);
        }
        ag_gfx_text((int16_t)(list.x + 12), y, base, COL_TEXT, 0);
    }
    ag_gfx_text((int16_t)(pos->x + 8), (int16_t)(pos->y + 1), "PLAYLIST",
                COL_MUTED, 0);
    (void)pos;
}

void amp_ui_draw(amp_player_t *p)
{
    int i;
    if (p == NULL) {
        return;
    }
    ag_gfx_clear(COL_BG);
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
        ag_gfx_text(8, (int16_t)(p->fb_h - 18), p->status, COL_MUTED, 0);
    }
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

    if (ev->type == AG_EV_POINTER_DOWN) {
        amp_panel_t pan = AMP_PANEL_MAIN;
        amp_ctrl_t c = amp_skin_hit(&p->skin, p->focus, x, y, &pan);
        p->mbtn = 1;
        p->focus = pan;
        if (c == AMP_CTRL_PLAY || c == AMP_CTRL_PAUSE) {
            amp_cmd_play_pause(p);
        } else if (c == AMP_CTRL_STOP) {
            amp_cmd_stop(p);
        } else if (c == AMP_CTRL_NEXT) {
            amp_cmd_next(p);
        } else if (c == AMP_CTRL_PREV) {
            amp_cmd_prev(p);
        } else if (c == AMP_CTRL_EJECT || c == AMP_CTRL_PL_TOGGLE) {
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
