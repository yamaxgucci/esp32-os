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
#include "../apps/desktop/dsk_rect.h"

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

void run_desktop_tests(void)
{
    test_rect_basics();
    test_rect_overlap();
    test_rect_clip_union();
    test_damage_merge();
    test_damage_never_forgets();
    test_damage_clip();
    test_metrics();
}
