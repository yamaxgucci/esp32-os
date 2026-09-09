/*
 * ArgonOS DESKTOP - the software pointer.
 *
 * The shapes are written as pictures and unpacked at start-up.  Two hundred
 * and seventy-two bytes of text per shape against sixty-four of bitmask, and
 * the difference buys a cursor somebody can edit without counting in hex.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "dsk_cursor.h"

#include "dsk_paint.h"

/* '.' nothing, 'B' black, 'W' white. */
static const char *const k_arrow[DSK_CUR_H] = {
    "B...............", "BB..............", "BWB.............",
    "BWWB............", "BWWWB...........", "BWWWWB..........",
    "BWWWWWB.........", "BWWWWWWB........", "BWWWWWWWB.......",
    "BWWWWWWWWB......", "BWWWWWBBBBB.....", "BWWBWWB.........",
    "BWB.BWWB........", "BB...BWWB.......", "B.....BWWB......",
    ".......BB.......",
};

static const char *const k_wait[DSK_CUR_H] = {
    "..BBBBBBBBBB....", "..BWWWWWWWWB....", "..BWWWWWWWWB....",
    "..BWBBBBBBWB....", "...BWWWWWWB.....", "....BWWWWB......",
    ".....BWWB.......", ".....BWWB.......", ".....BWWB.......",
    "....BWBBWB......", "...BWBBBBWB.....", "..BWBBBBBBWB....",
    "..BWBBBBBBWB....", "..BBBBBBBBBB....", "................",
    "................",
};

typedef struct {
    uint16_t opaque[DSK_CUR_H];
    uint16_t white[DSK_CUR_H];
    int8_t   hot_x, hot_y;
} shape_t;

static shape_t s_shape[DSK_CUR_COUNT];

static uint16_t   s_save[DSK_CUR_W * DSK_CUR_H];
static uint16_t   s_compose[DSK_CUR_W * DSK_CUR_H];
static bool       s_shown;
static dsk_rect_t s_at; /* where the saved pixels came from */

static void (*s_damage)(dsk_rect_t r);

static int16_t         s_x, s_y;
static int16_t         s_screen_w, s_screen_h;
static dsk_cursor_id_t s_id;

static void unpack(shape_t *out, const char *const *art, int8_t hx, int8_t hy)
{
    for (int row = 0; row < DSK_CUR_H; row++) {
        uint16_t op = 0, wh = 0;
        for (int col = 0; col < DSK_CUR_W; col++) {
            const char c = art[row][col];
            if (c == 'B' || c == 'W') {
                op |= (uint16_t)(1u << col);
            }
            if (c == 'W') {
                wh |= (uint16_t)(1u << col);
            }
        }
        out->opaque[row] = op;
        out->white[row] = wh;
    }
    out->hot_x = hx;
    out->hot_y = hy;
}

void dsk_cursor_init(int16_t screen_w, int16_t screen_h,
                     void (*damage_fn)(dsk_rect_t r))
{
    s_damage = damage_fn;
    unpack(&s_shape[DSK_CUR_ARROW], k_arrow, 0, 0);
    /* The hourglass points at its middle; the arrow at its tip. */
    unpack(&s_shape[DSK_CUR_WAIT], k_wait, 7, 7);
    s_screen_w = screen_w;
    s_screen_h = screen_h;
    s_id = DSK_CUR_ARROW;
    s_shown = false;
    s_at = dsk_rect_none();
    s_x = (int16_t)(screen_w / 2);
    s_y = (int16_t)(screen_h / 2);
}

int16_t dsk_cursor_x(void) { return s_x; }
int16_t dsk_cursor_y(void) { return s_y; }

void dsk_cursor_place(int16_t x, int16_t y)
{
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (s_screen_w > 0 && x > (int16_t)(s_screen_w - 1)) {
        x = (int16_t)(s_screen_w - 1);
    }
    if (s_screen_h > 0 && y > (int16_t)(s_screen_h - 1)) {
        y = (int16_t)(s_screen_h - 1);
    }
    s_x = x;
    s_y = y;
}

dsk_rect_t dsk_cursor_rect(void)
{
    const shape_t *sh = &s_shape[s_id];
    return dsk_rect((int16_t)(s_x - sh->hot_x), (int16_t)(s_y - sh->hot_y),
                    DSK_CUR_W, DSK_CUR_H);
}

void dsk_cursor_hide(void)
{
    /* Nothing was saved, so there is nothing to put back; see dsk_cursor_paint. */
    if (dsk_paint_banded() || !s_shown) {
        return;
    }
    /*
     * The clip has to be off: the pointer is above every window, so restoring
     * what was under it must not be cut short by whatever rectangle the last
     * piece of drawing was confined to.
     */
    dsk_clip_reset();
    dsk_paint()->blit(s_at, s_save, DSK_CUR_W);
    s_shown = false;
}

void dsk_cursor_show(void)
{
    if (dsk_paint_banded() || s_shown) {
        return;
    }
    const shape_t *sh = &s_shape[s_id];
    const dsk_rect_t r = dsk_cursor_rect();

    dsk_clip_reset();
    dsk_paint()->read(r, s_save);

    for (int row = 0; row < DSK_CUR_H; row++) {
        const uint16_t op = sh->opaque[row];
        const uint16_t wh = sh->white[row];
        uint16_t      *out = &s_compose[row * DSK_CUR_W];
        const uint16_t *in = &s_save[row * DSK_CUR_W];
        for (int col = 0; col < DSK_CUR_W; col++) {
            if ((op >> col) & 1u) {
                out[col] = ((wh >> col) & 1u) ? 0xFFFFu : 0x0000u;
            } else {
                out[col] = in[col];
            }
        }
    }

    dsk_paint()->blit(r, s_compose, DSK_CUR_W);
    s_at = r;
    s_shown = true;
}

/*
 * The band-mode pair, and why they are separate calls.
 *
 * With a surface, the pointer is a patch: read what is under it, draw over it,
 * and put the pixels back before anything else is drawn.  In band mode there
 * is nothing outside the current strip to read or put back, so the pointer
 * stops being a patch and becomes the last thing painted into each strip -
 * over a background that has just been drawn there, which is exactly what
 * `read` still answers for.  Moving it is then not drawing at all: it is two
 * rectangles that owe a repaint, which is what the caller does with the box
 * this hands back.
 */
dsk_rect_t dsk_cursor_place_moved(int16_t x, int16_t y)
{
    const dsk_rect_t was = dsk_cursor_rect();

    dsk_cursor_place(x, y);
    const dsk_rect_t now = dsk_cursor_rect();
    if (dsk_rect_eq(was, now)) {
        return dsk_rect_none();
    }
    return dsk_rect_union(was, now);
}

void dsk_cursor_paint(void)
{
    const shape_t   *sh = &s_shape[s_id];
    const dsk_rect_t r = dsk_cursor_rect();

    if (!dsk_visible(r)) {
        return;
    }
    /*
     * The clip stays as the strip's, unlike the surface path which resets it:
     * there the pointer is above every window and must not be cut short by
     * the last piece of drawing; here it must be cut by the strip, because
     * the strip is all the memory there is.
     */
    dsk_paint()->read(r, s_save);

    for (int row = 0; row < DSK_CUR_H; row++) {
        const uint16_t op = sh->opaque[row];
        const uint16_t wh = sh->white[row];
        uint16_t       *out = &s_compose[row * DSK_CUR_W];
        const uint16_t *in = &s_save[row * DSK_CUR_W];
        for (int col = 0; col < DSK_CUR_W; col++) {
            if ((op >> col) & 1u) {
                out[col] = ((wh >> col) & 1u) ? 0xFFFFu : 0x0000u;
            } else {
                out[col] = in[col];
            }
        }
    }
    dsk_paint()->blit(r, s_compose, DSK_CUR_W);
}

void dsk_cursor_shape(dsk_cursor_id_t id)
{
    if (id >= DSK_CUR_COUNT || id == s_id) {
        return;
    }
    if (dsk_paint_banded()) {
        const dsk_rect_t before = dsk_cursor_rect();
        s_id = id;
        if (s_damage != NULL) {
            s_damage(dsk_rect_union(before, dsk_cursor_rect()));
        }
        return;
    }

    const bool was = s_shown;
    const dsk_rect_t old = s_at;

    dsk_cursor_hide();
    s_id = id;
    if (was) {
        dsk_cursor_show();
        dsk_flush(dsk_rect_union(old, s_at));
    }
}

void dsk_cursor_move(int16_t x, int16_t y)
{
    const int16_t was_x = s_x;
    const int16_t was_y = s_y;

    dsk_cursor_place(x, y);
    if (s_x == was_x && s_y == was_y) {
        return;
    }

    const dsk_rect_t old = s_shown ? s_at : dsk_rect_none();
    dsk_cursor_hide();
    dsk_cursor_show();

    /*
     * Two squares, or one when they overlap - which they do for every
     * ordinary movement, and a pointer dragged across the screen in one jump
     * is the case where two separate flushes are worth it.
     */
    const dsk_rect_t box = dsk_rect_union(old, s_at);
    if (dsk_rect_area(box) <= dsk_rect_area(old) + dsk_rect_area(s_at)) {
        dsk_flush(box);
    } else {
        dsk_flush(old);
        dsk_flush(s_at);
    }
}
