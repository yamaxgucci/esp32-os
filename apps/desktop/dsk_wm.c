/*
 * ArgonOS DESKTOP - the window manager.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "dsk_wm.h"

#include "dsk_paint.h"

/* ---- state ------------------------------------------------------------- */

static dsk_metrics_t s_m;
static void (*s_damage)(dsk_rect_t r);

static dsk_win_t s_win[DSK_WIN_MAX];
/* Bottom to top.  Holds slot numbers, not pointers, so it survives a memmove. */
static uint8_t s_z[DSK_WIN_MAX];
static uint8_t s_nz;

/*
 * What is being dragged, and what it looked like before.
 *
 * The outline is drawn as four one-pixel strips with the pixels underneath
 * saved, exactly like the pointer, and for the same reason: a window dragged
 * by its body would repaint the whole of it at every step, which over the wire
 * to the second board is three and a half frames a second (see
 * docs/plans/desktop.md §3.3).  Windows 3.x dragged an outline; so does this.
 */
typedef enum { TRACK_NONE = 0, TRACK_MOVE, TRACK_SIZE } track_t;

static track_t    s_track;
static dsk_win_t *s_track_win;
static dsk_hit_t  s_track_edge;  /* which edge or corner, when sizing */
static dsk_rect_t s_track_rect;  /* where the outline is now          */
static int16_t    s_track_dx;    /* pointer offset inside the frame   */
static int16_t    s_track_dy;
static bool       s_outline_up;

/*
 * The saved pixels under the outline: four strips, top and bottom the width of
 * the frame, left and right its height.  Sized for the whole screen at bind
 * time, because a window may be as large as one.
 */
#define OUTLINE_MAX 704 /* the longest strip is the widest screen, 640 */
static uint16_t s_under[4][OUTLINE_MAX];
static dsk_rect_t s_under_r[4];

/* ---- helpers ----------------------------------------------------------- */

static void damage(dsk_rect_t r)
{
    if (s_damage != NULL) {
        s_damage(r);
    }
}

static int slot_z(const dsk_win_t *w)
{
    for (uint8_t i = 0; i < s_nz; i++) {
        if (s_z[i] == w->slot) {
            return (int)i;
        }
    }
    return -1;
}

static void z_remove(const dsk_win_t *w)
{
    const int at = slot_z(w);
    if (at < 0) {
        return;
    }
    for (uint8_t i = (uint8_t)at; i + 1 < s_nz; i++) {
        s_z[i] = s_z[i + 1];
    }
    s_nz--;
}

static void z_to_top(const dsk_win_t *w)
{
    z_remove(w);
    if (s_nz < DSK_WIN_MAX) {
        s_z[s_nz++] = w->slot;
    }
}

static void copy_title(dsk_win_t *w, const char *title)
{
    size_t n = 0;
    if (title != NULL) {
        while (title[n] != '\0' && n + 1 < DSK_TITLE_MAX) {
            w->title[n] = title[n];
            n++;
        }
    }
    w->title[n] = '\0';
}

/* The number of minimised windows below this one in the z order. */
static int icon_index(const dsk_win_t *w)
{
    int n = 0;
    for (uint8_t i = 0; i < s_nz; i++) {
        const dsk_win_t *o = &s_win[s_z[i]];
        if (o == w) {
            return n;
        }
        if (o->state == DSK_WIN_MINIMISED) {
            n++;
        }
    }
    return n;
}

/* ---- geometry ---------------------------------------------------------- */

#define ICON_W 72
#define ICON_H 40
#define BOX_W  16 /* the caption's little raised squares */

dsk_rect_t dsk_wm_client(const dsk_win_t *w)
{
    if (w == NULL || w->state == DSK_WIN_MINIMISED) {
        return dsk_rect_none();
    }
    const int16_t b = s_m.border;
    const int16_t t = (int16_t)(b + s_m.title_h);
    const int16_t cw = (int16_t)(w->frame.w - 2 * b);
    const int16_t ch = (int16_t)(w->frame.h - t - b);
    if (cw <= 0 || ch <= 0) {
        return dsk_rect_none();
    }
    return dsk_rect((int16_t)(w->frame.x + b), (int16_t)(w->frame.y + t), cw,
                    ch);
}

dsk_rect_t dsk_wm_title(const dsk_win_t *w)
{
    if (w == NULL || w->state == DSK_WIN_MINIMISED) {
        return dsk_rect_none();
    }
    const int16_t b = s_m.border;
    const int16_t tw = (int16_t)(w->frame.w - 2 * b);
    if (tw <= 0) {
        return dsk_rect_none();
    }
    return dsk_rect((int16_t)(w->frame.x + b), (int16_t)(w->frame.y + b), tw,
                    s_m.title_h);
}

dsk_rect_t dsk_wm_sysmenu(const dsk_win_t *w)
{
    const dsk_rect_t t = dsk_wm_title(w);
    if (dsk_rect_empty(t) || t.w < 3 * BOX_W) {
        return dsk_rect_none();
    }
    return dsk_rect(t.x, t.y, BOX_W, t.h);
}

dsk_rect_t dsk_wm_minbox(const dsk_win_t *w)
{
    const dsk_rect_t t = dsk_wm_title(w);
    if (dsk_rect_empty(t) || t.w < 3 * BOX_W) {
        return dsk_rect_none();
    }
    return dsk_rect((int16_t)(t.x + t.w - 2 * BOX_W), t.y, BOX_W, t.h);
}

dsk_rect_t dsk_wm_maxbox(const dsk_win_t *w)
{
    const dsk_rect_t t = dsk_wm_title(w);
    if (dsk_rect_empty(t) || t.w < 3 * BOX_W) {
        return dsk_rect_none();
    }
    return dsk_rect((int16_t)(t.x + t.w - BOX_W), t.y, BOX_W, t.h);
}

dsk_rect_t dsk_wm_icon_rect(const dsk_win_t *w)
{
    if (w == NULL || w->state != DSK_WIN_MINIMISED) {
        return dsk_rect_none();
    }
    /*
     * Along the bottom of the work area, left to right, wrapping upwards when
     * the row is full - which is where Windows 3.11 put them, and it keeps
     * them off the part of the desktop the windows use.
     */
    const int16_t per_row = (int16_t)(s_m.work.w / ICON_W);
    const int idx = icon_index(w);
    const int16_t col = (per_row > 0) ? (int16_t)(idx % per_row) : 0;
    const int16_t row = (per_row > 0) ? (int16_t)(idx / per_row) : 0;
    return dsk_rect((int16_t)(s_m.work.x + col * ICON_W),
                    (int16_t)(s_m.work.y + s_m.work.h - (row + 1) * ICON_H),
                    ICON_W, ICON_H);
}

dsk_hit_t dsk_wm_hit_in(const dsk_win_t *w, int16_t x, int16_t y)
{
    if (w == NULL || !w->used) {
        return DSK_HIT_NONE;
    }
    if (w->state == DSK_WIN_MINIMISED) {
        return dsk_rect_has(dsk_wm_icon_rect(w), x, y) ? DSK_HIT_ICON
                                                       : DSK_HIT_NONE;
    }
    if (!dsk_rect_has(w->frame, x, y)) {
        return DSK_HIT_NONE;
    }
    if (dsk_rect_has(dsk_wm_sysmenu(w), x, y)) {
        return DSK_HIT_SYSMENU;
    }
    if (dsk_rect_has(dsk_wm_minbox(w), x, y)) {
        return DSK_HIT_MINIMISE;
    }
    if (dsk_rect_has(dsk_wm_maxbox(w), x, y)) {
        return DSK_HIT_MAXIMISE;
    }
    if (dsk_rect_has(dsk_wm_title(w), x, y)) {
        return DSK_HIT_TITLE;
    }
    if (dsk_rect_has(dsk_wm_client(w), x, y)) {
        return DSK_HIT_CLIENT;
    }

    /*
     * What is left is the border.  A corner is the border's own thickness plus
     * a little, because a four-pixel square is not a target anybody can hit -
     * Windows 3.1 made its corners longer than the frame for the same reason.
     */
    if (!w->resizable || w->state == DSK_WIN_MAXIMISED) {
        return DSK_HIT_TITLE; /* an unresizable border still drags */
    }
    const int16_t corner = (int16_t)(s_m.border + 8);
    const bool    west = (x < w->frame.x + corner);
    const bool    east = (x >= (int16_t)(dsk_rect_x2(w->frame) - corner));
    const bool    north = (y < w->frame.y + corner);
    const bool    south = (y >= (int16_t)(dsk_rect_y2(w->frame) - corner));

    if (north && west) {
        return DSK_HIT_CORNER_NW;
    }
    if (north && east) {
        return DSK_HIT_CORNER_NE;
    }
    if (south && west) {
        return DSK_HIT_CORNER_SW;
    }
    if (south && east) {
        return DSK_HIT_CORNER_SE;
    }
    if (y < w->frame.y + s_m.border) {
        return DSK_HIT_EDGE_N;
    }
    if (y >= (int16_t)(dsk_rect_y2(w->frame) - s_m.border)) {
        return DSK_HIT_EDGE_S;
    }
    if (x < w->frame.x + s_m.border) {
        return DSK_HIT_EDGE_W;
    }
    return DSK_HIT_EDGE_E;
}

dsk_win_t *dsk_wm_hit(int16_t x, int16_t y, dsk_hit_t *what)
{
    for (int i = (int)s_nz - 1; i >= 0; i--) {
        dsk_win_t      *w = &s_win[s_z[i]];
        const dsk_hit_t h = dsk_wm_hit_in(w, x, y);
        if (h != DSK_HIT_NONE) {
            if (what != NULL) {
                *what = h;
            }
            return w;
        }
    }
    if (what != NULL) {
        *what = DSK_HIT_NONE;
    }
    return NULL;
}

/* ---- opening and closing ----------------------------------------------- */

void dsk_wm_init(const dsk_metrics_t *m, void (*damage_fn)(dsk_rect_t r))
{
    if (m != NULL) {
        s_m = *m;
    }
    s_damage = damage_fn;
    s_nz = 0;
    s_track = TRACK_NONE;
    s_track_win = NULL;
    s_outline_up = false;
    for (int i = 0; i < DSK_WIN_MAX; i++) {
        s_win[i].used = false;
        s_win[i].slot = (uint8_t)i;
    }
}

void dsk_wm_damage_rect(dsk_rect_t r) { damage(r); }

int dsk_wm_count(void) { return (int)s_nz; }

dsk_win_t *dsk_wm_at(int z)
{
    if (z < 0 || z >= (int)s_nz) {
        return NULL;
    }
    return &s_win[s_z[z]];
}

dsk_win_t *dsk_wm_active(void)
{
    return (s_nz == 0) ? NULL : &s_win[s_z[s_nz - 1]];
}

dsk_win_t *dsk_wm_open(const char *title, dsk_rect_t frame,
                       const dsk_win_ops_t *ops, void *user)
{
    if (ops == NULL || ops->draw == NULL || dsk_rect_empty(frame)) {
        return NULL;
    }
    int slot = -1;
    for (int i = 0; i < DSK_WIN_MAX; i++) {
        if (!s_win[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return NULL;
    }

    dsk_win_t *w = &s_win[slot];
    const uint8_t keep = w->slot;
    dsk_win_t     fresh = {0};
    *w = fresh;
    w->slot = keep;
    w->used = true;
    w->ops = ops;
    w->user = user;
    w->state = DSK_WIN_NORMAL;
    w->resizable = true;
    w->min_w = (int16_t)(2 * s_m.border + 3 * BOX_W + 16);
    w->min_h = (int16_t)(2 * s_m.border + s_m.title_h + 16);
    copy_title(w, title);
    dsk_wm_move(w, frame);
    z_to_top(w);
    damage(w->frame);
    return w;
}

void dsk_wm_close(dsk_win_t *w)
{
    if (w == NULL || !w->used) {
        return;
    }
    if (s_track_win == w) {
        s_track = TRACK_NONE;
        s_track_win = NULL;
        s_outline_up = false;
    }
    /*
     * The hole it leaves has to be repainted, and for a minimised window that
     * is the plate rather than the frame - and then every other plate as well,
     * because they are packed left to right and closing one shuffles the rest
     * along.
     */
    if (w->state == DSK_WIN_MINIMISED) {
        damage(dsk_wm_icon_rect(w));
    } else {
        damage(w->frame);
    }
    const bool was_min = (w->state == DSK_WIN_MINIMISED);

    if (w->ops != NULL && w->ops->closed != NULL) {
        w->ops->closed(w);
    }
    z_remove(w);
    w->used = false;
    w->ops = NULL;
    w->user = NULL;

    if (was_min) {
        for (uint8_t i = 0; i < s_nz; i++) {
            dsk_win_t *o = &s_win[s_z[i]];
            if (o->state == DSK_WIN_MINIMISED) {
                damage(dsk_wm_icon_rect(o));
            }
        }
    }
    /* Whoever is on top now has a different caption colour. */
    if (s_nz > 0) {
        damage(dsk_wm_title(dsk_wm_active()));
    }
}

void dsk_wm_close_all(void)
{
    while (s_nz > 0) {
        dsk_wm_close(&s_win[s_z[s_nz - 1]]);
    }
}

void dsk_wm_activate(dsk_win_t *w)
{
    if (w == NULL || !w->used) {
        return;
    }
    dsk_win_t *was = dsk_wm_active();
    if (was == w) {
        return;
    }
    z_to_top(w);
    /*
     * Both captions change colour, and the newly raised window may have been
     * partly hidden - so it is the whole of it that needs paint, not its
     * caption.
     */
    if (was != NULL) {
        damage(was->state == DSK_WIN_MINIMISED ? dsk_wm_icon_rect(was)
                                               : dsk_wm_title(was));
    }
    damage(w->state == DSK_WIN_MINIMISED ? dsk_wm_icon_rect(w) : w->frame);
}

void dsk_wm_cycle(void)
{
    if (s_nz < 2) {
        return;
    }
    /* The one directly below the top comes up: Alt+Tab, held once. */
    dsk_wm_activate(&s_win[s_z[s_nz - 2]]);
}

void dsk_wm_set_title(dsk_win_t *w, const char *title)
{
    if (w == NULL || !w->used) {
        return;
    }
    copy_title(w, title);
    damage(w->state == DSK_WIN_MINIMISED ? dsk_wm_icon_rect(w)
                                         : dsk_wm_title(w));
}

/* Keep a frame inside the work area, and no smaller than the window allows. */
static dsk_rect_t sane_frame(const dsk_win_t *w, dsk_rect_t f)
{
    if (f.w < w->min_w) {
        f.w = w->min_w;
    }
    if (f.h < w->min_h) {
        f.h = w->min_h;
    }
    if (f.w > s_m.work.w) {
        f.w = s_m.work.w;
    }
    if (f.h > s_m.work.h) {
        f.h = s_m.work.h;
    }
    /*
     * A caption that has gone off the top or the side cannot be grabbed to
     * bring it back, so it is not allowed to go there.  The bottom is
     * different: a window may hang below the work area, because that is how a
     * long list gets read on a short screen.
     */
    if (f.x < s_m.work.x) {
        f.x = s_m.work.x;
    }
    if (f.y < s_m.work.y) {
        f.y = s_m.work.y;
    }
    if (dsk_rect_x2(f) > dsk_rect_x2(s_m.work)) {
        f.x = (int16_t)(dsk_rect_x2(s_m.work) - f.w);
    }
    if (f.y > (int16_t)(dsk_rect_y2(s_m.work) - s_m.title_h)) {
        f.y = (int16_t)(dsk_rect_y2(s_m.work) - s_m.title_h);
    }
    return f;
}

void dsk_wm_move(dsk_win_t *w, dsk_rect_t frame)
{
    if (w == NULL || !w->used) {
        return;
    }
    const dsk_rect_t was = w->frame;
    const dsk_rect_t now = sane_frame(w, frame);
    if (dsk_rect_eq(was, now)) {
        return;
    }
    w->frame = now;
    if (w->state == DSK_WIN_NORMAL) {
        w->restore = now;
    }
    damage(was);
    damage(now);
}

void dsk_wm_minimise(dsk_win_t *w)
{
    if (w == NULL || !w->used || w->state == DSK_WIN_MINIMISED) {
        return;
    }
    const dsk_rect_t was = w->frame;
    if (w->state == DSK_WIN_NORMAL) {
        w->restore = w->frame;
    }
    w->state = DSK_WIN_MINIMISED;
    damage(was);
    damage(dsk_wm_icon_rect(w));
}

void dsk_wm_maximise(dsk_win_t *w)
{
    if (w == NULL || !w->used || w->state == DSK_WIN_MAXIMISED) {
        return;
    }
    const dsk_rect_t was =
        (w->state == DSK_WIN_MINIMISED) ? dsk_wm_icon_rect(w) : w->frame;
    if (w->state == DSK_WIN_NORMAL) {
        w->restore = w->frame;
    }
    w->state = DSK_WIN_MAXIMISED;
    w->frame = s_m.work;
    damage(was);
    damage(w->frame);
}

void dsk_wm_restore(dsk_win_t *w)
{
    if (w == NULL || !w->used || w->state == DSK_WIN_NORMAL) {
        return;
    }
    const bool was_min = (w->state == DSK_WIN_MINIMISED);
    const dsk_rect_t was = was_min ? dsk_wm_icon_rect(w) : w->frame;

    w->state = DSK_WIN_NORMAL;
    w->frame = sane_frame(w, w->restore);
    damage(was);
    damage(w->frame);
    if (was_min) {
        /* The plates to the right of the one that left shuffle along. */
        for (uint8_t i = 0; i < s_nz; i++) {
            dsk_win_t *o = &s_win[s_z[i]];
            if (o->state == DSK_WIN_MINIMISED) {
                damage(dsk_wm_icon_rect(o));
            }
        }
    }
}

void dsk_wm_cascade(void)
{
    int16_t at = 0;
    for (uint8_t i = 0; i < s_nz; i++) {
        dsk_win_t *w = &s_win[s_z[i]];
        if (w->state == DSK_WIN_MINIMISED) {
            continue;
        }
        w->state = DSK_WIN_NORMAL;
        const int16_t step = (int16_t)(s_m.title_h + s_m.border);
        const int16_t w_w = (int16_t)(s_m.work.w * 3 / 4);
        const int16_t w_h = (int16_t)(s_m.work.h * 3 / 4);
        dsk_wm_move(w, dsk_rect((int16_t)(s_m.work.x + at * step),
                                (int16_t)(s_m.work.y + at * step), w_w, w_h));
        at++;
        /* Round back to the corner rather than march off the edge. */
        if (s_m.work.x + at * step + w_w > dsk_rect_x2(s_m.work)) {
            at = 0;
        }
    }
}

void dsk_wm_tile(void)
{
    int n = 0;
    for (uint8_t i = 0; i < s_nz; i++) {
        if (s_win[s_z[i]].state != DSK_WIN_MINIMISED) {
            n++;
        }
    }
    if (n == 0) {
        return;
    }
    /* Columns first, so two windows sit side by side as they do in 3.11. */
    int cols = 1;
    while (cols * cols < n) {
        cols++;
    }
    const int rows = (n + cols - 1) / cols;
    const int16_t cw = (int16_t)(s_m.work.w / cols);
    const int16_t ch = (int16_t)(s_m.work.h / rows);

    int at = 0;
    for (uint8_t i = 0; i < s_nz; i++) {
        dsk_win_t *w = &s_win[s_z[i]];
        if (w->state == DSK_WIN_MINIMISED) {
            continue;
        }
        w->state = DSK_WIN_NORMAL;
        const int16_t cx = (int16_t)(at % cols);
        const int16_t cy = (int16_t)(at / cols);
        dsk_wm_move(w, dsk_rect((int16_t)(s_m.work.x + cx * cw),
                                (int16_t)(s_m.work.y + cy * ch), cw, ch));
        at++;
    }
}

/* ---- drawing ----------------------------------------------------------- */

/* A tiny picture of a window, for a minimised one.  Drawn, not a bitmap. */
static void draw_mini_window(dsk_rect_t r, bool active)
{
    dsk_panel(r, true, DSK_LGRAY);
    const dsk_rect_t cap = dsk_rect((int16_t)(r.x + 2), (int16_t)(r.y + 2),
                                    (int16_t)(r.w - 4), 4);
    dsk_fill(cap, active ? DSK_NAVY : DSK_DGRAY);
    dsk_fill(dsk_rect((int16_t)(r.x + 2), (int16_t)(r.y + 7),
                      (int16_t)(r.w - 4), (int16_t)(r.h - 9)),
             DSK_WHITE);
}

static void draw_box(dsk_rect_t r, int glyph)
{
    if (dsk_rect_empty(r)) {
        return;
    }
    dsk_panel(r, true, DSK_LGRAY);
    const int16_t cx = (int16_t)(r.x + r.w / 2);
    const int16_t cy = (int16_t)(r.y + r.h / 2);
    switch (glyph) {
    case 0: /* system menu: a long bar, as in 3.x */
        dsk_fill(dsk_rect((int16_t)(r.x + 3), (int16_t)(cy - 1),
                          (int16_t)(r.w - 6), 3),
                 DSK_BLACK);
        break;
    case 1: /* minimise: a small bar low down */
        dsk_fill(dsk_rect((int16_t)(cx - 3), (int16_t)(cy + 2), 7, 2),
                 DSK_BLACK);
        break;
    default: /* maximise: an outlined box */
        dsk_frame(dsk_rect((int16_t)(cx - 4), (int16_t)(cy - 4), 9, 9),
                  DSK_BLACK);
        dsk_fill(dsk_rect((int16_t)(cx - 4), (int16_t)(cy - 4), 9, 2),
                 DSK_BLACK);
        break;
    }
}

static void draw_frame(dsk_win_t *w, bool active)
{
    const dsk_rect_t f = w->frame;

    /* The border: a raised plate with a dark line where the client begins. */
    dsk_panel(f, true, DSK_LGRAY);
    dsk_frame(dsk_rect_inset(f, (int16_t)(s_m.border - 1)), DSK_DGRAY);

    const dsk_rect_t t = dsk_wm_title(w);
    if (!dsk_rect_empty(t)) {
        dsk_fill(t, active ? DSK_NAVY : DSK_LGRAY);
        if (!active) {
            dsk_hline(t.x, (int16_t)(dsk_rect_y2(t) - 1), t.w, DSK_DGRAY);
        }
        draw_box(dsk_wm_sysmenu(w), 0);
        draw_box(dsk_wm_minbox(w), 1);
        draw_box(dsk_wm_maxbox(w), 2);

        const dsk_rect_t sys = dsk_wm_sysmenu(w);
        const int16_t    tx =
            (int16_t)(dsk_rect_empty(sys) ? t.x + 3 : dsk_rect_x2(sys) + 4);
        const dsk_rect_t mn = dsk_wm_minbox(w);
        const int16_t    right =
            (int16_t)(dsk_rect_empty(mn) ? dsk_rect_x2(t) - 3 : mn.x - 4);
        if (right > tx) {
            dsk_text_fit(tx, (int16_t)(t.y + 1), (int16_t)(right - tx),
                         w->title, active ? DSK_WHITE : DSK_DGRAY,
                         active ? DSK_NAVY : DSK_LGRAY);
        }
    }
}

void dsk_wm_draw(dsk_rect_t area)
{
    if (dsk_rect_empty(area)) {
        return;
    }
    const dsk_win_t *top = dsk_wm_active();

    for (uint8_t i = 0; i < s_nz; i++) {
        dsk_win_t       *w = &s_win[s_z[i]];
        const bool       active = (w == top);
        const dsk_rect_t box =
            (w->state == DSK_WIN_MINIMISED) ? dsk_wm_icon_rect(w) : w->frame;
        const dsk_rect_t part = dsk_rect_clip(box, area);
        if (dsk_rect_empty(part)) {
            continue;
        }
        dsk_clip(part);
        if (w->state == DSK_WIN_MINIMISED) {
            draw_mini_window(box, active);
            /* The caption goes under the plate, centred, in the small font. */
            const int16_t tw = dsk_text_small_width(w->title);
            const int16_t tx =
                (int16_t)(box.x + (box.w - (tw < box.w ? tw : box.w)) / 2);
            dsk_text_small(tx, (int16_t)(dsk_rect_y2(box) - DSK_SMALL_H),
                           w->title, DSK_WHITE, DSK_TEAL);
        } else {
            draw_frame(w, active);
            const dsk_rect_t client = dsk_wm_client(w);
            const dsk_rect_t cpart = dsk_rect_clip(client, area);
            if (!dsk_rect_empty(cpart)) {
                dsk_clip(cpart);
                w->ops->draw(w, client);
            }
        }
        dsk_clip_reset();
    }
}

/* ---- the drag outline -------------------------------------------------- */

/*
 * Four strips.  Kept as rectangles rather than a single frame so that the
 * saved pixels are contiguous per strip, which is what read() and blit() want.
 */
static void outline_strips(dsk_rect_t r, dsk_rect_t out[4])
{
    out[0] = dsk_rect(r.x, r.y, r.w, 1);                              /* top */
    out[1] = dsk_rect(r.x, (int16_t)(dsk_rect_y2(r) - 1), r.w, 1);    /* bot */
    out[2] = dsk_rect(r.x, (int16_t)(r.y + 1), 1, (int16_t)(r.h - 2)); /* left */
    out[3] = dsk_rect((int16_t)(dsk_rect_x2(r) - 1), (int16_t)(r.y + 1), 1,
                      (int16_t)(r.h - 2));
}

static bool outline_fits(dsk_rect_t r)
{
    dsk_rect_t s[4];
    outline_strips(r, s);
    for (int i = 0; i < 4; i++) {
        if ((int32_t)s[i].w * s[i].h > OUTLINE_MAX) {
            return false;
        }
    }
    return true;
}

static void outline_hide(void)
{
    if (!s_outline_up) {
        return;
    }
    dsk_clip_reset();
    for (int i = 0; i < 4; i++) {
        if (!dsk_rect_empty(s_under_r[i])) {
            dsk_paint()->blit(s_under_r[i], s_under[i],
                              (uint32_t)s_under_r[i].w);
        }
    }
    s_outline_up = false;
}

static void outline_show(dsk_rect_t r)
{
    if (s_outline_up || !outline_fits(r)) {
        return;
    }
    dsk_rect_t s[4];
    outline_strips(r, s);
    dsk_clip_reset();
    for (int i = 0; i < 4; i++) {
        s_under_r[i] = s[i];
        if (dsk_rect_empty(s[i])) {
            continue;
        }
        dsk_paint()->read(s[i], s_under[i]);
        dsk_fill(s[i], DSK_BLACK);
    }
    s_outline_up = true;
}

/*
 * One flush for the whole outline, not four.
 *
 * Four thin strips look like the cheap way round and are the expensive one:
 * a panel is handed whole rows (see panel_present_rows in the kernel), so the
 * left and right strips each cost every row the window spans, and eight
 * flushes per pointer move became eight requests QEMU's display thread had to
 * be waited for.  Measured, and it is not subtle: the pointer fell seconds
 * behind the mouse, the event queue backed up, and a drag arrived at the
 * wrong place because the button had come up long before the guest read it.
 *
 * The rows spanned are the same either way, so one flush of the bounding
 * rectangle sends exactly the same pixels for an eighth of the requests.
 */
static void outline_flush(dsk_rect_t r) { dsk_flush(r); }

bool dsk_wm_tracking(void) { return s_track != TRACK_NONE; }

/*
 * Take the outline off the glass for the duration of a repaint, and put it
 * back afterwards.
 *
 * The same contract the pointer has, and for the same reason: the pixels it
 * saved are only good until something else draws there.  A repaint underneath
 * a live outline leaves the outline's copy stale, and the next movement then
 * restores four strips of a picture that is no longer on the screen - which
 * looks like a black rectangle nailed to the desktop, surviving even a forced
 * full repaint, because the repaint is what made it stale in the first place.
 */
void dsk_wm_outline_off(void)
{
    outline_hide();
}

void dsk_wm_outline_on(void)
{
    if (s_track != TRACK_NONE) {
        outline_show(s_track_rect);
    }
}

/* Where the outline goes for a pointer at (x, y), given what is being done. */
static dsk_rect_t track_target(int16_t x, int16_t y)
{
    dsk_win_t *w = s_track_win;
    dsk_rect_t f = w->frame;

    if (s_track == TRACK_MOVE) {
        f.x = (int16_t)(x - s_track_dx);
        f.y = (int16_t)(y - s_track_dy);
        return sane_frame(w, f);
    }

    int32_t x1 = f.x, y1 = f.y, x2 = dsk_rect_x2(f), y2 = dsk_rect_y2(f);
    switch (s_track_edge) {
    case DSK_HIT_EDGE_N:
        y1 = y;
        break;
    case DSK_HIT_EDGE_S:
        y2 = y;
        break;
    case DSK_HIT_EDGE_W:
        x1 = x;
        break;
    case DSK_HIT_EDGE_E:
        x2 = x;
        break;
    case DSK_HIT_CORNER_NW:
        x1 = x;
        y1 = y;
        break;
    case DSK_HIT_CORNER_NE:
        x2 = x;
        y1 = y;
        break;
    case DSK_HIT_CORNER_SW:
        x1 = x;
        y2 = y;
        break;
    default:
        x2 = x;
        y2 = y;
        break;
    }
    if (x2 - x1 < w->min_w) {
        /* Push the edge that is not being dragged, not the one that is. */
        if (x1 != f.x) {
            x1 = x2 - w->min_w;
        } else {
            x2 = x1 + w->min_w;
        }
    }
    if (y2 - y1 < w->min_h) {
        if (y1 != f.y) {
            y1 = y2 - w->min_h;
        } else {
            y2 = y1 + w->min_h;
        }
    }
    return sane_frame(w, dsk_rect((int16_t)x1, (int16_t)y1, (int16_t)(x2 - x1),
                                  (int16_t)(y2 - y1)));
}

/* ---- input ------------------------------------------------------------- */

static void start_track(dsk_win_t *w, dsk_hit_t what, int16_t x, int16_t y)
{
    s_track_win = w;
    s_track_edge = what;
    s_track = (what == DSK_HIT_TITLE) ? TRACK_MOVE : TRACK_SIZE;
    s_track_dx = (int16_t)(x - w->frame.x);
    s_track_dy = (int16_t)(y - w->frame.y);
    s_track_rect = w->frame;
    outline_show(s_track_rect);
    outline_flush(s_track_rect);
}

static void end_track(bool keep)
{
    if (s_track == TRACK_NONE) {
        return;
    }
    dsk_win_t *w = s_track_win;
    const dsk_rect_t to = s_track_rect;

    outline_hide();
    outline_flush(to);
    s_track = TRACK_NONE;
    s_track_win = NULL;
    if (keep && w != NULL) {
        dsk_wm_move(w, to);
    }
}

bool dsk_wm_pointer(dsk_ptr_t type, int16_t x, int16_t y, uint8_t buttons,
                    bool dbl)
{
    /* A drag owns the pointer until the button comes up. */
    if (s_track != TRACK_NONE) {
        if (type == DSK_PTR_MOVE) {
            const dsk_rect_t to = track_target(x, y);
            if (!dsk_rect_eq(to, s_track_rect)) {
                const dsk_rect_t from = s_track_rect;
                outline_hide();
                s_track_rect = to;
                outline_show(to);
                /*
                 * One flush for both places.  They overlap for every
                 * ordinary movement, and where they do not the rows between
                 * them cost nothing to send and one request instead of two.
                 */
                outline_flush(dsk_rect_union(from, to));
            }
            return true;
        }
        if (type == DSK_PTR_UP) {
            end_track(true);
            return true;
        }
        return true;
    }

    /*
     * A modal window takes everything, and a click outside it is not passed
     * through to what is behind - it is swallowed, which is what modal means.
     */
    dsk_win_t *top = dsk_wm_active();
    if (top != NULL && top->modal) {
        const dsk_hit_t what = dsk_wm_hit_in(top, x, y);
        if (what == DSK_HIT_NONE) {
            return true;
        }
        if (top->ops->pointer != NULL &&
            top->ops->pointer(top, what, x, y, buttons,
                              type == DSK_PTR_DOWN, dbl)) {
            return true;
        }
        if (type == DSK_PTR_DOWN && what == DSK_HIT_TITLE) {
            start_track(top, what, x, y);
        }
        return true;
    }

    dsk_hit_t  what = DSK_HIT_NONE;
    dsk_win_t *w = dsk_wm_hit(x, y, &what);
    if (w == NULL) {
        return false;
    }
    if (type == DSK_PTR_MOVE) {
        /* Only the window's own handler cares about a move with no drag. */
        if (w->ops->pointer != NULL) {
            return w->ops->pointer(w, what, x, y, buttons, false, false);
        }
        return false;
    }
    if (type == DSK_PTR_UP) {
        if (w->ops->pointer != NULL) {
            (void)w->ops->pointer(w, what, x, y, buttons, false, dbl);
        }
        return true;
    }

    /* A press anywhere in a window raises it first. */
    dsk_wm_activate(w);

    switch (what) {
    case DSK_HIT_SYSMENU:
        /*
         * In 3.11 this opened a menu with Move / Size / Close on it; here it
         * closes, and the menu arrives when there is anything else to put on
         * it.  Double-click closed the window there too, so the gesture is
         * not being taken away from anybody.
         */
        dsk_wm_close(w);
        return true;
    case DSK_HIT_MINIMISE:
        dsk_wm_minimise(w);
        return true;
    case DSK_HIT_MAXIMISE:
        if (w->state == DSK_WIN_MAXIMISED) {
            dsk_wm_restore(w);
        } else {
            dsk_wm_maximise(w);
        }
        return true;
    case DSK_HIT_ICON:
        if (dbl) {
            dsk_wm_restore(w);
        }
        return true;
    case DSK_HIT_TITLE:
        if (dbl) {
            /* Double-click on the caption toggles maximise, as it always has. */
            if (w->state == DSK_WIN_MAXIMISED) {
                dsk_wm_restore(w);
            } else {
                dsk_wm_maximise(w);
            }
            return true;
        }
        if (w->state != DSK_WIN_MAXIMISED) {
            start_track(w, what, x, y);
        }
        return true;
    case DSK_HIT_CLIENT:
        if (w->ops->pointer != NULL) {
            (void)w->ops->pointer(w, what, x, y, buttons, true, dbl);
        }
        return true;
    default:
        if (w->resizable && w->state == DSK_WIN_NORMAL) {
            start_track(w, what, x, y);
        }
        return true;
    }
}

bool dsk_wm_key(uint16_t keycode, uint32_t unicode, uint16_t mods)
{
    if (s_track != TRACK_NONE) {
        /* Escape abandons a drag and puts the window back. */
        if (keycode == DSK_KEY_ESC) {
            s_track_rect = s_track_win->frame;
            end_track(false);
            return true;
        }
        return true;
    }
    dsk_win_t *w = dsk_wm_active();
    if (w == NULL) {
        return false;
    }
    if (w->ops->key != NULL && w->ops->key(w, keycode, unicode, mods)) {
        return true;
    }
    return false;
}
