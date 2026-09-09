/*
 * ArgonOS - the desktop shell's geometry, checked without a display.
 *
 * Two things live here, and both are the kind that fail silently on a panel:
 * the rectangle algebra a damage-driven repaint is built on, and the layout
 * arithmetic that has to come out right on a 320x240 board and a 640x400
 * window from the same code.
 *
 * The damage list is the interesting half.  It is allowed to be lossy in one
 * direction only - it may repaint more than was asked for, never less - and
 * that asymmetry is exactly what a test can pin down: every rectangle ever
 * added must still be covered by something in the list.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"

#include "../apps/desktop/dsk.h"
#include "../apps/desktop/dsk_menu.h"
#include "../apps/desktop/dsk_paint.h"
#include "../apps/desktop/dsk_rect.h"
#include "../apps/desktop/dsk_wm.h"

#include <stdint.h>

/* Is every pixel of r accounted for by the pieces in d? */
static bool damage_covers(const dsk_damage_t *d, dsk_rect_t r)
{
    for (int16_t y = r.y; y < (int16_t)dsk_rect_y2(r); y++) {
        for (int16_t x = r.x; x < (int16_t)dsk_rect_x2(r); x++) {
            bool found = false;
            for (uint8_t i = 0; i < d->n && !found; i++) {
                found = dsk_rect_has(d->r[i], x, y);
            }
            if (!found) {
                return false;
            }
        }
    }
    return true;
}

static void test_rect_basics(void)
{
    const dsk_rect_t a = dsk_rect(10, 10, 20, 20);

    AG_CHECK(!dsk_rect_empty(a));
    AG_CHECK(dsk_rect_empty(dsk_rect(10, 10, 0, 5)));
    AG_CHECK(dsk_rect_empty(dsk_rect(10, 10, 5, 0)));
    /* A negative extent is nothing, not a rectangle pointing backwards. */
    AG_CHECK(dsk_rect_empty(dsk_rect(10, 10, -4, 5)));

    AG_CHECK_INT(dsk_rect_area(a), 400);
    AG_CHECK_INT(dsk_rect_area(dsk_rect(0, 0, 640, 400)), 256000);
    AG_CHECK_INT(dsk_rect_area(dsk_rect_none()), 0);

    /* Half-open: the far edge is outside. */
    AG_CHECK(dsk_rect_has(a, 10, 10));
    AG_CHECK(dsk_rect_has(a, 29, 29));
    AG_CHECK(!dsk_rect_has(a, 30, 20));
    AG_CHECK(!dsk_rect_has(a, 20, 30));
    AG_CHECK(!dsk_rect_has(a, 9, 10));
    AG_CHECK(!dsk_rect_has(dsk_rect_none(), 0, 0));
}

static void test_rect_overlap(void)
{
    const dsk_rect_t a = dsk_rect(0, 0, 10, 10);

    AG_CHECK(dsk_rect_overlaps(a, dsk_rect(5, 5, 10, 10)));
    /* Touching edges do not overlap: [0,10) and [10,20) share no pixel. */
    AG_CHECK(!dsk_rect_overlaps(a, dsk_rect(10, 0, 10, 10)));
    AG_CHECK(!dsk_rect_overlaps(a, dsk_rect(0, 10, 10, 10)));
    AG_CHECK(!dsk_rect_overlaps(a, dsk_rect_none()));

    AG_CHECK(dsk_rect_covers(a, dsk_rect(2, 2, 4, 4)));
    AG_CHECK(dsk_rect_covers(a, a));
    AG_CHECK(!dsk_rect_covers(a, dsk_rect(2, 2, 20, 4)));
    /* Nothing is inside anything, including inside nothing. */
    AG_CHECK(dsk_rect_covers(a, dsk_rect_none()));
    AG_CHECK(dsk_rect_covers(dsk_rect_none(), dsk_rect_none()));
    AG_CHECK(!dsk_rect_covers(dsk_rect_none(), a));
}

static void test_rect_clip_union(void)
{
    const dsk_rect_t a = dsk_rect(0, 0, 10, 10);
    const dsk_rect_t b = dsk_rect(5, 5, 10, 10);
    dsk_rect_t       c = dsk_rect_clip(a, b);

    AG_CHECK_INT(c.x, 5);
    AG_CHECK_INT(c.y, 5);
    AG_CHECK_INT(c.w, 5);
    AG_CHECK_INT(c.h, 5);

    AG_CHECK(dsk_rect_empty(dsk_rect_clip(a, dsk_rect(20, 20, 5, 5))));
    AG_CHECK(dsk_rect_empty(dsk_rect_clip(a, dsk_rect_none())));

    c = dsk_rect_union(a, b);
    AG_CHECK_INT(c.x, 0);
    AG_CHECK_INT(c.y, 0);
    AG_CHECK_INT(c.w, 15);
    AG_CHECK_INT(c.h, 15);

    /* An empty operand yields the other, not a box that reaches the origin. */
    AG_CHECK(dsk_rect_eq(dsk_rect_union(b, dsk_rect_none()), b));
    AG_CHECK(dsk_rect_eq(dsk_rect_union(dsk_rect_none(), b), b));
    AG_CHECK(dsk_rect_empty(dsk_rect_union(dsk_rect_none(), dsk_rect_none())));

    c = dsk_rect_inset(dsk_rect(10, 10, 20, 20), 4);
    AG_CHECK_INT(c.x, 14);
    AG_CHECK_INT(c.w, 12);
    /* Inset past the middle is nothing, not an inverted rectangle. */
    AG_CHECK(dsk_rect_empty(dsk_rect_inset(dsk_rect(0, 0, 6, 6), 3)));
    c = dsk_rect_inset(dsk_rect(10, 10, 4, 4), -2);
    AG_CHECK_INT(c.x, 8);
    AG_CHECK_INT(c.w, 8);

    c = dsk_rect_offset(dsk_rect(1, 2, 3, 4), 10, 20);
    AG_CHECK_INT(c.x, 11);
    AG_CHECK_INT(c.y, 22);
    AG_CHECK_INT(c.w, 3);
}

static void test_damage_merge(void)
{
    dsk_damage_t d;

    dsk_damage_clear(&d);
    AG_CHECK_INT(d.n, 0);
    AG_CHECK(dsk_rect_empty(dsk_damage_bounds(&d)));

    /* An empty rectangle is not damage. */
    dsk_damage_add(&d, dsk_rect_none());
    AG_CHECK_INT(d.n, 0);

    /* Two rectangles that share an edge exactly become one, for free. */
    dsk_damage_add(&d, dsk_rect(0, 0, 10, 10));
    dsk_damage_add(&d, dsk_rect(10, 0, 10, 10));
    AG_CHECK_INT(d.n, 1);
    AG_CHECK_INT(d.r[0].w, 20);
    AG_CHECK_INT(d.r[0].h, 10);

    /* One inside another leaves the list alone. */
    dsk_damage_clear(&d);
    dsk_damage_add(&d, dsk_rect(0, 0, 100, 100));
    dsk_damage_add(&d, dsk_rect(10, 10, 5, 5));
    AG_CHECK_INT(d.n, 1);
    AG_CHECK_INT(d.r[0].w, 100);

    /* The other way round: the big one swallows the small one already there. */
    dsk_damage_clear(&d);
    dsk_damage_add(&d, dsk_rect(10, 10, 5, 5));
    dsk_damage_add(&d, dsk_rect(40, 40, 5, 5));
    dsk_damage_add(&d, dsk_rect(0, 0, 100, 100));
    AG_CHECK_INT(d.n, 1);
    AG_CHECK_INT(d.r[0].w, 100);

    /*
     * Far apart in both axes: merging them would repaint the whole diagonal,
     * so they stay two.  This is the case the whole scheme exists for - a
     * pointer at one corner and a status line at the other.
     */
    dsk_damage_clear(&d);
    dsk_damage_add(&d, dsk_rect(0, 0, 16, 16));
    dsk_damage_add(&d, dsk_rect(300, 220, 16, 16));
    AG_CHECK_INT(d.n, 2);
}

static void test_damage_never_forgets(void)
{
    /*
     * Twenty pieces into eight slots.  Whatever the merging did, every pixel
     * that was ever added has to still be covered - the list may cost more
     * than it needs, never less.
     */
    dsk_rect_t   in[20];
    dsk_damage_t d;
    int          n = 0;

    for (int i = 0; i < 20; i++) {
        in[n++] = dsk_rect((int16_t)((i * 37) % 300), (int16_t)((i * 53) % 220),
                           (int16_t)(4 + (i % 7) * 3),
                           (int16_t)(4 + (i % 5) * 3));
    }

    dsk_damage_clear(&d);
    for (int i = 0; i < n; i++) {
        dsk_damage_add(&d, in[i]);
    }
    AG_CHECK(d.n <= DSK_DAMAGE_MAX);
    for (int i = 0; i < n; i++) {
        AG_CHECK(damage_covers(&d, in[i]));
    }

    /* And the bounding box of the list contains every one of them. */
    const dsk_rect_t box = dsk_damage_bounds(&d);
    for (int i = 0; i < n; i++) {
        AG_CHECK(dsk_rect_covers(box, in[i]));
    }
}

static void test_damage_clip(void)
{
    dsk_damage_t     d;
    const dsk_rect_t screen = dsk_rect(0, 0, 320, 240);

    dsk_damage_clear(&d);
    dsk_damage_add(&d, dsk_rect(-8, -8, 16, 16)); /* half off the corner */
    dsk_damage_add(&d, dsk_rect(400, 100, 16, 16)); /* wholly outside */
    dsk_damage_add(&d, dsk_rect(100, 100, 16, 16));
    dsk_damage_clip(&d, screen);

    AG_CHECK_INT(d.n, 2);
    for (uint8_t i = 0; i < d.n; i++) {
        AG_CHECK(dsk_rect_covers(screen, d.r[i]));
    }
    AG_CHECK(damage_covers(&d, dsk_rect(0, 0, 8, 8)));
    AG_CHECK(damage_covers(&d, dsk_rect(100, 100, 16, 16)));
}

static void test_metrics(void)
{
    dsk_metrics_t m;

    dsk_metrics_init(&m, 320, 240);
    AG_CHECK_INT(m.menubar_h, 18);
    AG_CHECK_INT(m.menubar.w, 320);
    AG_CHECK_INT(m.menubar.y, 0);
    AG_CHECK_INT(m.statusbar.h, 12);
    AG_CHECK_INT(m.statusbar.y, 228);
    /* The work area is exactly what the two strips left. */
    AG_CHECK_INT(m.work.y, 18);
    AG_CHECK_INT(m.work.h, 240 - 18 - 12);
    AG_CHECK(!dsk_rect_overlaps(m.work, m.menubar));
    AG_CHECK(!dsk_rect_overlaps(m.work, m.statusbar));
    /* Nothing is left over: the three strips are the whole screen. */
    AG_CHECK_INT(m.menubar.h + m.work.h + m.statusbar.h, 240);

    dsk_metrics_init(&m, 640, 400);
    AG_CHECK_INT(m.menubar_h, 18);
    AG_CHECK_INT(m.work.w, 640);
    AG_CHECK_INT(m.work.h, 400 - 18 - 12);
    AG_CHECK_INT(m.menubar.h + m.work.h + m.statusbar.h, 400);

    /* The CYD's surface still has room for both strips, with ninety to spare. */
    dsk_metrics_init(&m, 160, 120);
    AG_CHECK(!dsk_rect_empty(m.statusbar));
    AG_CHECK(!dsk_rect_empty(m.menubar));
    AG_CHECK_INT(m.work.h, 120 - 18 - 12);
    AG_CHECK_INT(m.menubar.h + m.work.h + m.statusbar.h, 120);

    /*
     * A surface too short to be a desktop at all.  The strips give way before
     * the work area does, and neither ends up overlapping anything.
     */
    dsk_metrics_init(&m, 160, 60);
    AG_CHECK(dsk_rect_empty(m.statusbar));
    AG_CHECK(!dsk_rect_empty(m.menubar));
    AG_CHECK_INT(m.work.h, 60 - 18);
    AG_CHECK(!dsk_rect_overlaps(m.work, m.menubar));

    dsk_metrics_init(&m, 160, 20);
    AG_CHECK(dsk_rect_empty(m.statusbar));
    AG_CHECK(dsk_rect_empty(m.menubar));
    AG_CHECK_INT(m.work.y, 0);
    AG_CHECK_INT(m.work.h, 20);
}

/* ---- a painter that draws nothing and remembers everything ------------- */

/*
 * The window manager talks to the screen through six function pointers, and
 * that is what makes it testable: bind these instead and the manager can be
 * driven through a whole session of opening, dragging and closing windows on a
 * machine with no display, while the test reads back what it tried to paint.
 *
 * What is worth checking here is not the pixels - a fill of the right colour
 * proves little - but the order and the extent: which windows a damage
 * rectangle causes to be painted, bottom to top, and that nothing is painted
 * outside the clip it was given.
 */
#define REC_MAX 512

typedef struct {
    dsk_rect_t r;
    dsk_rect_t clip;
} rec_fill_t;

static rec_fill_t s_fills[REC_MAX];
static int        s_nfills;
static dsk_rect_t s_rec_clip;

static void rec_reset(void)
{
    s_nfills = 0;
    s_rec_clip = dsk_rect_none();
}

static void rec_fill(dsk_rect_t r, uint32_t rgb)
{
    (void)rgb;
    if (s_nfills < REC_MAX) {
        s_fills[s_nfills].r = r;
        s_fills[s_nfills].clip = s_rec_clip;
        s_nfills++;
    }
}

static void rec_blit(dsk_rect_t r, const uint16_t *src, uint32_t stride)
{
    (void)src;
    (void)stride;
    rec_fill(r, 0);
}

static void rec_text(int16_t x, int16_t y, int16_t max_w, const char *s,
                     uint32_t fg, uint32_t bg)
{
    (void)max_w;
    (void)fg;
    (void)bg;
    rec_fill(dsk_rect(x, y, (int16_t)(8 * (s != NULL ? (int)strlen(s) : 0)),
                      16),
             0);
}

static void rec_clip(dsk_rect_t r) { s_rec_clip = r; }

static void rec_read(dsk_rect_t r, uint16_t *dst)
{
    const int32_t n = (int32_t)r.w * r.h;
    for (int32_t i = 0; i < n; i++) {
        dst[i] = 0;
    }
}

static void rec_flush(dsk_rect_t r) { (void)r; }

static const dsk_painter_t k_recorder = {
    .fill = rec_fill,
    .blit = rec_blit,
    .text = rec_text,
    .clip = rec_clip,
    .read = rec_read,
    .flush = rec_flush,
};

/* Damage the manager asked for, in order. */
static dsk_rect_t s_dmg[64];
static int        s_ndmg;

static void note_damage(dsk_rect_t r)
{
    if (s_ndmg < 64) {
        s_dmg[s_ndmg++] = r;
    }
}

static bool damaged_covers(dsk_rect_t r)
{
    dsk_damage_t d;
    dsk_damage_clear(&d);
    for (int i = 0; i < s_ndmg; i++) {
        dsk_damage_add(&d, s_dmg[i]);
    }
    return damage_covers(&d, r);
}

static dsk_metrics_t s_wm_m;

static int s_draw_calls[DSK_WIN_MAX];
static int s_draw_order[DSK_WIN_MAX];
static int s_draw_n;

static void t_draw(dsk_win_t *w, dsk_rect_t client)
{
    (void)client;
    const int n = (int)(intptr_t)w->user;
    if (n >= 0 && n < DSK_WIN_MAX) {
        s_draw_calls[n]++;
    }
    if (s_draw_n < DSK_WIN_MAX) {
        s_draw_order[s_draw_n++] = n;
    }
}

static const dsk_win_ops_t k_test_ops = {
    .draw = t_draw,
    .key = NULL,
    .pointer = NULL,
    .closed = NULL,
};

/* ---- how a repaint is cut into strips ---------------------------------- */
/*
 * The band backend itself needs a display, but the loop that drives it does
 * not: it is pure, and it is where a whole rectangle can quietly go missing.
 * What must hold is simple and is exactly what a photograph cannot tell you
 * in one shot: the strips tile the rectangle, in order, with no gap and no
 * overlap, and every strip that was drawn was also presented.
 */
#define BAND_PX (320 * 16)
#define BAND_H_MAX 16

static dsk_rect_t s_band_drawn[64];
static int        s_band_n;
static int        s_band_presented;
static dsk_rect_t s_band_at; /* what begin() last set */

static int16_t fake_begin(dsk_rect_t r, int16_t y)
{
    /* The same arithmetic dsk_paint_band.c uses. */
    if (r.w <= 0 || r.w > (int16_t)BAND_PX) {
        return 0;
    }
    int16_t h = (int16_t)(BAND_PX / r.w);
    if (h > BAND_H_MAX) {
        h = BAND_H_MAX;
    }
    if (h > (int16_t)(r.y + r.h - y)) {
        h = (int16_t)(r.y + r.h - y);
    }
    if (h <= 0) {
        return 0;
    }
    s_band_at = dsk_rect(r.x, y, r.w, h);
    return h;
}

static void fake_present(void) { s_band_presented++; }

static const dsk_bander_t k_fake_bander = {
    .begin = fake_begin,
    .present = fake_present,
};

static void band_draw(dsk_rect_t r)
{
    if (s_band_n < 64) {
        s_band_drawn[s_band_n++] = r;
    }
}

static void check_bands_tile(dsk_rect_t r)
{
    s_band_n = 0;
    s_band_presented = 0;
    dsk_paint_bind(&k_recorder, 640, 400);
    dsk_paint_bind_bander(&k_fake_bander);
    AG_CHECK_INT(dsk_paint_banded(), 1);

    dsk_paint_region(r, band_draw);

    /* Every strip drawn went to the glass. */
    AG_CHECK_INT(s_band_presented, s_band_n);
    AG_CHECK_INT(s_band_n > 0, 1);

    int16_t at = r.y;
    for (int i = 0; i < s_band_n; i++) {
        AG_CHECK_INT(s_band_drawn[i].x, r.x);
        AG_CHECK_INT(s_band_drawn[i].w, r.w);
        AG_CHECK_INT(s_band_drawn[i].y, at);
        AG_CHECK_INT(s_band_drawn[i].h > 0, 1);
        at = (int16_t)(at + s_band_drawn[i].h);
    }
    /* No gap at the end either - the last row of the rectangle is covered. */
    AG_CHECK_INT(at, (int16_t)(r.y + r.h));

    dsk_paint_bind(&k_recorder, 640, 400); /* clears the bander */
    AG_CHECK_INT(dsk_paint_banded(), 0);
}

static void test_bands_cover_the_whole_rectangle(void)
{
    /*
     * Heights around the strip height, and the two that a real run produced
     * as missing regions on the glass - nineteen rows of a window caption and
     * thirty-nine of a dialog's lower half.
     */
    const int16_t heights[] = {1, 2, 15, 16, 17, 19, 31, 32, 33, 39, 200, 400};
    const int16_t widths[] = {1, 3, 8, 56, 68, 100, 319, 320};

    for (size_t hi = 0; hi < sizeof(heights) / sizeof(heights[0]); hi++) {
        for (size_t wi = 0; wi < sizeof(widths) / sizeof(widths[0]); wi++) {
            const int16_t h = heights[hi];
            const int16_t w = widths[wi];
            if (h > 400 || w > 640) {
                continue;
            }
            check_bands_tile(dsk_rect(0, 0, w, h));
            check_bands_tile(dsk_rect(3, 30, w, h));
        }
    }

    /* With no bander bound, the rectangle is drawn once and flushed once. */
    s_band_n = 0;
    dsk_paint_bind(&k_recorder, 640, 400);
    dsk_paint_region(dsk_rect(0, 0, 100, 100), band_draw);
    AG_CHECK_INT(s_band_n, 1);
    AG_CHECK_INT(s_band_drawn[0].h, 100);

    /* An empty rectangle draws nothing at all rather than one empty strip. */
    s_band_n = 0;
    dsk_paint_bind_bander(&k_fake_bander);
    dsk_paint_region(dsk_rect_none(), band_draw);
    AG_CHECK_INT(s_band_n, 0);
    dsk_paint_bind(&k_recorder, 640, 400);
}

static void wm_fixture(void)
{
    dsk_metrics_init(&s_wm_m, 640, 400);
    dsk_paint_bind(&k_recorder, 640, 400);
    dsk_wm_init(&s_wm_m, note_damage);
    rec_reset();
    s_ndmg = 0;
    s_draw_n = 0;
    for (int i = 0; i < DSK_WIN_MAX; i++) {
        s_draw_calls[i] = 0;
    }
}

/* ---- the window manager ------------------------------------------------ */

static void test_wm_geometry(void)
{
    wm_fixture();
    dsk_win_t *w = dsk_wm_open("Title", dsk_rect(100, 60, 200, 120),
                               &k_test_ops, (void *)(intptr_t)0);
    AG_CHECK(w != NULL);
    if (w == NULL) {
        return;
    }

    AG_CHECK_INT(w->frame.x, 100);
    AG_CHECK_INT(w->frame.w, 200);

    const dsk_rect_t t = dsk_wm_title(w);
    const dsk_rect_t c = dsk_wm_client(w);
    AG_CHECK_INT(t.x, 100 + s_wm_m.border);
    AG_CHECK_INT(t.y, 60 + s_wm_m.border);
    AG_CHECK_INT(t.w, 200 - 2 * s_wm_m.border);
    AG_CHECK_INT(t.h, s_wm_m.title_h);
    /* The client starts below the caption and the two never overlap. */
    AG_CHECK_INT(c.y, 60 + s_wm_m.border + s_wm_m.title_h);
    AG_CHECK(!dsk_rect_overlaps(t, c));
    /* Caption plus client plus two borders is the whole frame, exactly. */
    AG_CHECK_INT(s_wm_m.border + t.h + c.h + s_wm_m.border, w->frame.h);
    AG_CHECK(dsk_rect_covers(w->frame, t));
    AG_CHECK(dsk_rect_covers(w->frame, c));

    /* The three caption buttons are inside the caption and do not overlap. */
    const dsk_rect_t sys = dsk_wm_sysmenu(w);
    const dsk_rect_t mn = dsk_wm_minbox(w);
    const dsk_rect_t mx = dsk_wm_maxbox(w);
    AG_CHECK(dsk_rect_covers(t, sys));
    AG_CHECK(dsk_rect_covers(t, mn));
    AG_CHECK(dsk_rect_covers(t, mx));
    AG_CHECK(!dsk_rect_overlaps(sys, mn));
    AG_CHECK(!dsk_rect_overlaps(mn, mx));
    AG_CHECK(!dsk_rect_overlaps(sys, mx));
    /* Minimise is to the left of maximise, as it has been since 3.0. */
    AG_CHECK(mn.x < mx.x);

    /* A frame smaller than the window allows is widened, not left unusable. */
    dsk_win_t *tiny = dsk_wm_open("t", dsk_rect(0, 60, 8, 8), &k_test_ops,
                                  (void *)(intptr_t)1);
    AG_CHECK(tiny != NULL);
    if (tiny != NULL) {
        AG_CHECK(tiny->frame.w >= tiny->min_w);
        AG_CHECK(tiny->frame.h >= tiny->min_h);
        AG_CHECK(!dsk_rect_empty(dsk_wm_client(tiny)));
    }

    /* No window is allowed to start above the work area. */
    dsk_win_t *high = dsk_wm_open("h", dsk_rect(20, 0, 200, 120), &k_test_ops,
                                  (void *)(intptr_t)2);
    AG_CHECK(high != NULL);
    if (high != NULL) {
        AG_CHECK(high->frame.y >= s_wm_m.work.y);
        AG_CHECK(!dsk_rect_overlaps(high->frame, s_wm_m.menubar));
    }
}

static void test_wm_hit(void)
{
    wm_fixture();
    dsk_win_t *w = dsk_wm_open("Hit", dsk_rect(100, 60, 200, 120),
                               &k_test_ops, (void *)(intptr_t)0);
    if (w == NULL) {
        AG_CHECK(false);
        return;
    }
    dsk_hit_t what = DSK_HIT_NONE;

    AG_CHECK(dsk_wm_hit(50, 50, &what) == NULL);
    AG_CHECK_INT(what, DSK_HIT_NONE);

    const dsk_rect_t c = dsk_wm_client(w);
    AG_CHECK(dsk_wm_hit((int16_t)(c.x + 5), (int16_t)(c.y + 5), &what) == w);
    AG_CHECK_INT(what, DSK_HIT_CLIENT);

    const dsk_rect_t sys = dsk_wm_sysmenu(w);
    AG_CHECK(dsk_wm_hit((int16_t)(sys.x + 2), (int16_t)(sys.y + 2), &what) ==
             w);
    AG_CHECK_INT(what, DSK_HIT_SYSMENU);

    const dsk_rect_t mn = dsk_wm_minbox(w);
    (void)dsk_wm_hit((int16_t)(mn.x + 2), (int16_t)(mn.y + 2), &what);
    AG_CHECK_INT(what, DSK_HIT_MINIMISE);

    const dsk_rect_t mx = dsk_wm_maxbox(w);
    (void)dsk_wm_hit((int16_t)(mx.x + 2), (int16_t)(mx.y + 2), &what);
    AG_CHECK_INT(what, DSK_HIT_MAXIMISE);

    /* The middle of the caption, clear of every button. */
    (void)dsk_wm_hit((int16_t)(sys.x + sys.w + 4),
                     (int16_t)(sys.y + sys.h / 2), &what);
    AG_CHECK_INT(what, DSK_HIT_TITLE);

    /* Corners beat edges: the very corner pixel is a corner, not a side. */
    (void)dsk_wm_hit(w->frame.x, w->frame.y, &what);
    AG_CHECK_INT(what, DSK_HIT_CORNER_NW);
    (void)dsk_wm_hit((int16_t)(dsk_rect_x2(w->frame) - 1),
                     (int16_t)(dsk_rect_y2(w->frame) - 1), &what);
    AG_CHECK_INT(what, DSK_HIT_CORNER_SE);
    /* Half way down the left border is an edge. */
    (void)dsk_wm_hit(w->frame.x, (int16_t)(w->frame.y + w->frame.h / 2),
                     &what);
    AG_CHECK_INT(what, DSK_HIT_EDGE_W);

    /* An unresizable window's border drags it instead of resizing it. */
    w->resizable = false;
    (void)dsk_wm_hit(w->frame.x, (int16_t)(w->frame.y + w->frame.h / 2),
                     &what);
    AG_CHECK_INT(what, DSK_HIT_TITLE);
}

static void test_wm_zorder(void)
{
    wm_fixture();
    /* Three windows on the same spot, so only the z order can decide. */
    dsk_win_t *a = dsk_wm_open("a", dsk_rect(100, 60, 200, 120), &k_test_ops,
                               (void *)(intptr_t)0);
    dsk_win_t *b = dsk_wm_open("b", dsk_rect(100, 60, 200, 120), &k_test_ops,
                               (void *)(intptr_t)1);
    dsk_win_t *c = dsk_wm_open("c", dsk_rect(100, 60, 200, 120), &k_test_ops,
                               (void *)(intptr_t)2);
    AG_CHECK(a != NULL && b != NULL && c != NULL);
    AG_CHECK_INT(dsk_wm_count(), 3);
    /* The newest is on top and is the active one. */
    AG_CHECK(dsk_wm_active() == c);
    AG_CHECK(dsk_wm_at(0) == a);
    AG_CHECK(dsk_wm_at(2) == c);
    AG_CHECK(dsk_wm_at(3) == NULL);

    dsk_hit_t what;
    AG_CHECK(dsk_wm_hit(150, 100, &what) == c);

    dsk_wm_activate(a);
    AG_CHECK(dsk_wm_active() == a);
    AG_CHECK(dsk_wm_at(2) == a);
    AG_CHECK(dsk_wm_hit(150, 100, &what) == a);

    /* Alt+Tab brings up the one directly below, and again returns. */
    dsk_wm_cycle();
    AG_CHECK(dsk_wm_active() == c);
    dsk_wm_cycle();
    AG_CHECK(dsk_wm_active() == a);

    dsk_wm_close(a);
    AG_CHECK_INT(dsk_wm_count(), 2);
    AG_CHECK(dsk_wm_active() == c);
    dsk_wm_close_all();
    AG_CHECK_INT(dsk_wm_count(), 0);
    AG_CHECK(dsk_wm_active() == NULL);

    /* Every slot came back: the full dozen can be opened again. */
    for (int i = 0; i < DSK_WIN_MAX; i++) {
        AG_CHECK(dsk_wm_open("x", dsk_rect(100, 60, 200, 120), &k_test_ops,
                             (void *)(intptr_t)0) != NULL);
    }
    AG_CHECK_INT(dsk_wm_count(), DSK_WIN_MAX);
    /* And the one after that is refused rather than overwriting a slot. */
    AG_CHECK(dsk_wm_open("x", dsk_rect(100, 60, 200, 120), &k_test_ops,
                         NULL) == NULL);
}

static void test_wm_damage(void)
{
    wm_fixture();
    dsk_win_t *w = dsk_wm_open("d", dsk_rect(100, 60, 200, 120), &k_test_ops,
                               (void *)(intptr_t)0);
    if (w == NULL) {
        AG_CHECK(false);
        return;
    }
    /* Opening asks for the new window's own area. */
    AG_CHECK(damaged_covers(w->frame));

    /* Moving asks for both places, or the old one is left on the glass. */
    const dsk_rect_t from = w->frame;
    s_ndmg = 0;
    dsk_wm_move(w, dsk_rect(300, 200, 200, 120));
    AG_CHECK(damaged_covers(from));
    AG_CHECK(damaged_covers(w->frame));

    /* Minimising asks for the frame it left and the plate it became. */
    const dsk_rect_t was = w->frame;
    s_ndmg = 0;
    dsk_wm_minimise(w);
    AG_CHECK_INT(w->state, DSK_WIN_MINIMISED);
    AG_CHECK(damaged_covers(was));
    AG_CHECK(damaged_covers(dsk_wm_icon_rect(w)));
    /* A minimised window has no client area to draw into. */
    AG_CHECK(dsk_rect_empty(dsk_wm_client(w)));
    /* Its plate is inside the work area, along the bottom. */
    AG_CHECK(dsk_rect_covers(s_wm_m.work, dsk_wm_icon_rect(w)));

    /* Restoring puts back exactly the frame it had before. */
    s_ndmg = 0;
    dsk_wm_restore(w);
    AG_CHECK_INT(w->state, DSK_WIN_NORMAL);
    AG_CHECK(dsk_rect_eq(w->frame, was));
    AG_CHECK(damaged_covers(was));

    /* Maximise fills the work area and leaves the strips alone. */
    dsk_wm_maximise(w);
    AG_CHECK(dsk_rect_eq(w->frame, s_wm_m.work));
    AG_CHECK(!dsk_rect_overlaps(w->frame, s_wm_m.menubar));
    AG_CHECK(!dsk_rect_overlaps(w->frame, s_wm_m.statusbar));
    dsk_wm_restore(w);
    AG_CHECK(dsk_rect_eq(w->frame, was));

    /* Two minimised windows get plates that do not overlap. */
    dsk_win_t *o = dsk_wm_open("o", dsk_rect(100, 60, 200, 120), &k_test_ops,
                               (void *)(intptr_t)1);
    AG_CHECK(o != NULL);
    dsk_wm_minimise(w);
    dsk_wm_minimise(o);
    AG_CHECK(!dsk_rect_overlaps(dsk_wm_icon_rect(w), dsk_wm_icon_rect(o)));
}

static void test_wm_draw_order(void)
{
    wm_fixture();
    dsk_win_t *a = dsk_wm_open("a", dsk_rect(100, 60, 200, 120), &k_test_ops,
                               (void *)(intptr_t)0);
    dsk_win_t *b = dsk_wm_open("b", dsk_rect(150, 90, 200, 120), &k_test_ops,
                               (void *)(intptr_t)1);
    dsk_win_t *far = dsk_wm_open("far", dsk_rect(400, 250, 120, 80),
                                 &k_test_ops, (void *)(intptr_t)2);
    AG_CHECK(a != NULL && b != NULL && far != NULL);

    /* Damage over the two overlapping windows repaints both, a before b. */
    for (int i = 0; i < DSK_WIN_MAX; i++) {
        s_draw_calls[i] = 0;
    }
    s_draw_n = 0;
    rec_reset();
    const dsk_rect_t area = dsk_rect(150, 90, 60, 40);
    dsk_wm_draw(area);
    AG_CHECK_INT(s_draw_calls[0], 1);
    AG_CHECK_INT(s_draw_calls[1], 1);
    /* The window nowhere near it is not painted at all. */
    AG_CHECK_INT(s_draw_calls[2], 0);
    AG_CHECK_INT(s_draw_n, 2);
    AG_CHECK_INT(s_draw_order[0], 0); /* bottom first */
    AG_CHECK_INT(s_draw_order[1], 1);

    /*
     * And every fill was inside the clip it was given, and every clip inside
     * the damage.  This is the property the whole scheme rests on: the clip is
     * what stops a window drawing over the one above it, so a fill that
     * escapes one is a window that will overwrite its neighbour.
     */
    AG_CHECK(s_nfills > 0);
    for (int i = 0; i < s_nfills; i++) {
        AG_CHECK(dsk_rect_covers(area, s_fills[i].clip));
    }

    /* Raising a over b and repainting the shared corner now goes b, a. */
    dsk_wm_activate(a);
    s_draw_n = 0;
    dsk_wm_draw(area);
    AG_CHECK_INT(s_draw_n, 2);
    AG_CHECK_INT(s_draw_order[0], 1);
    AG_CHECK_INT(s_draw_order[1], 0);

    /* A minimised window is painted as a plate, not as a client area. */
    dsk_wm_minimise(a);
    s_draw_n = 0;
    for (int i = 0; i < DSK_WIN_MAX; i++) {
        s_draw_calls[i] = 0;
    }
    dsk_wm_draw(dsk_wm_icon_rect(a));
    AG_CHECK_INT(s_draw_calls[0], 0);
}

/* ---- the menu bar ------------------------------------------------------ */

static void fill_item(dsk_menu_t *m, const char *label, uint16_t id,
                      bool separator, bool enabled)
{
    dsk_menu_item_t *it = &m->items[m->n++];
    it->label = label;
    it->id = id;
    it->separator = separator;
    it->enabled = enabled;
    it->checked = false;
}

static void test_menu_geometry(void)
{
    static dsk_menu_t menus[2];
    dsk_metrics_t     m;

    dsk_metrics_init(&m, 640, 400);
    dsk_paint_bind(&k_recorder, 640, 400);
    dsk_menu_init(&m, note_damage, NULL);

    menus[0].title = "File";
    menus[0].n = 0;
    fill_item(&menus[0], "New window", 1, false, true);
    fill_item(&menus[0], "", 0, true, false);
    fill_item(&menus[0], "Exit", 2, false, true);

    menus[1].title = "Help";
    menus[1].n = 0;
    fill_item(&menus[1], "About...", 3, false, true);

    dsk_menu_set(menus, 2);

    /* The titles sit on the bar, in order, without overlapping. */
    const dsk_rect_t t0 = dsk_menu_title_rect(0);
    const dsk_rect_t t1 = dsk_menu_title_rect(1);
    AG_CHECK(!dsk_rect_empty(t0));
    AG_CHECK(!dsk_rect_empty(t1));
    AG_CHECK(t0.x < t1.x);
    AG_CHECK(!dsk_rect_overlaps(t0, t1));
    AG_CHECK(dsk_rect_covers(m.menubar, t0));
    AG_CHECK(dsk_rect_covers(m.menubar, t1));
    AG_CHECK_INT(dsk_menu_title_at((int16_t)(t1.x + 2), (int16_t)(t1.y + 2)),
                 1);
    AG_CHECK_INT(dsk_menu_title_at(600, 2), -1);

    /* Nothing is dropped down until something opens it. */
    AG_CHECK(!dsk_menu_open());
    AG_CHECK_INT(dsk_menu_item_at((int16_t)(t0.x + 2), 30), -1);

    AG_CHECK(dsk_menu_pointer(DSK_PTR_DOWN, (int16_t)(t0.x + 2),
                              (int16_t)(t0.y + 2)));
    AG_CHECK(dsk_menu_open());

    /* The drop-down hangs below the bar, under its own title. */
    const dsk_rect_t d = dsk_menu_drop_rect(0);
    AG_CHECK_INT(d.y, m.menubar.y + m.menubar.h);
    AG_CHECK_INT(d.x, t0.x);
    AG_CHECK(dsk_rect_x2(d) <= 640);

    /* The three items stack inside it and do not overlap. */
    const dsk_rect_t i0 = dsk_menu_item_rect(0, 0);
    const dsk_rect_t i1 = dsk_menu_item_rect(0, 1);
    const dsk_rect_t i2 = dsk_menu_item_rect(0, 2);
    AG_CHECK(dsk_rect_covers(d, i0));
    AG_CHECK(dsk_rect_covers(d, i2));
    AG_CHECK(!dsk_rect_overlaps(i0, i1));
    AG_CHECK(!dsk_rect_overlaps(i1, i2));
    AG_CHECK(i0.y < i1.y && i1.y < i2.y);
    /* The separator is shorter than an item, which is what makes it one. */
    AG_CHECK(i1.h < i0.h);

    AG_CHECK_INT(dsk_menu_item_at((int16_t)(i2.x + 2), (int16_t)(i2.y + 2)),
                 2);
    AG_CHECK_INT(dsk_menu_item_at(600, 300), -1);

    /* Escape puts it away. */
    AG_CHECK(dsk_menu_key(DSK_KEY_ESC, 0, 0));
    AG_CHECK(!dsk_menu_open());

    /* A press away from the bar with nothing open is not the menu's business. */
    AG_CHECK(!dsk_menu_pointer(DSK_PTR_DOWN, 300, 300));

    /* A drop-down near the right edge slides back rather than hanging off. */
    static dsk_menu_t wide[6];
    for (int i = 0; i < 6; i++) {
        wide[i].title = "Menu";
        wide[i].n = 0;
        fill_item(&wide[i], "a rather long item label here", 9, false, true);
    }
    dsk_menu_set(wide, 6);
    (void)dsk_menu_pointer(DSK_PTR_DOWN,
                           (int16_t)(dsk_menu_title_rect(5).x + 2), 2);
    const dsk_rect_t d5 = dsk_menu_drop_rect(5);
    AG_CHECK(!dsk_rect_empty(d5));
    AG_CHECK(d5.x >= 0);
    AG_CHECK(dsk_rect_x2(d5) <= 640);
    dsk_menu_close();
}

static uint16_t s_chosen;
static void     chose(uint16_t id) { s_chosen = id; }

static void test_menu_choosing(void)
{
    static dsk_menu_t menus[1];
    dsk_metrics_t     m;

    dsk_metrics_init(&m, 640, 400);
    dsk_paint_bind(&k_recorder, 640, 400);
    dsk_menu_init(&m, note_damage, chose);

    menus[0].title = "File";
    menus[0].n = 0;
    fill_item(&menus[0], "New", 7, false, true);
    fill_item(&menus[0], "Greyed", 8, false, false);
    dsk_menu_set(menus, 1);

    const dsk_rect_t t = dsk_menu_title_rect(0);
    (void)dsk_menu_pointer(DSK_PTR_DOWN, (int16_t)(t.x + 2),
                           (int16_t)(t.y + 2));
    s_chosen = 0;
    const dsk_rect_t i0 = dsk_menu_item_rect(0, 0);
    (void)dsk_menu_pointer(DSK_PTR_UP, (int16_t)(i0.x + 2),
                           (int16_t)(i0.y + 2));
    AG_CHECK_INT(s_chosen, 7);
    /* Choosing closes it, so a handler never draws under an open menu. */
    AG_CHECK(!dsk_menu_open());

    /* A disabled item cannot be chosen, and the menu stays open. */
    (void)dsk_menu_pointer(DSK_PTR_DOWN, (int16_t)(t.x + 2),
                           (int16_t)(t.y + 2));
    s_chosen = 0;
    const dsk_rect_t i1 = dsk_menu_item_rect(0, 1);
    (void)dsk_menu_pointer(DSK_PTR_UP, (int16_t)(i1.x + 2),
                           (int16_t)(i1.y + 2));
    AG_CHECK_INT(s_chosen, 0);
    AG_CHECK(dsk_menu_open());

    /* Down-arrow then Enter picks the first item that can be picked. */
    s_chosen = 0;
    AG_CHECK(dsk_menu_key(DSK_KEY_DOWN, 0, 0));
    AG_CHECK(dsk_menu_key(DSK_KEY_ENTER, 0, 0));
    AG_CHECK_INT(s_chosen, 7);
    AG_CHECK(!dsk_menu_open());

    /* A press on the open title closes it again rather than reopening. */
    (void)dsk_menu_pointer(DSK_PTR_DOWN, (int16_t)(t.x + 2),
                           (int16_t)(t.y + 2));
    AG_CHECK(dsk_menu_open());
    (void)dsk_menu_pointer(DSK_PTR_DOWN, (int16_t)(t.x + 2),
                           (int16_t)(t.y + 2));
    AG_CHECK(!dsk_menu_open());
}

void run_desktop_tests(void)
{
    test_bands_cover_the_whole_rectangle();
    test_rect_basics();
    test_rect_overlap();
    test_rect_clip_union();
    test_damage_merge();
    test_damage_never_forgets();
    test_damage_clip();
    test_metrics();
    test_wm_geometry();
    test_wm_hit();
    test_wm_zorder();
    test_wm_damage();
    test_wm_draw_order();
    test_menu_geometry();
    test_menu_choosing();
}
