/*
 * ArgonOS DESKTOP - rectangles and the damage list.
 *
 * Pure C on purpose: no argon.h, no display, nothing that needs a board.  This
 * is the file the host tests compile as it stands (host-tests/test_desktop.c),
 * because it is where an off-by-one costs an afternoon of squinting at a panel.
 *
 * A rectangle with a zero width or height is empty and every operation treats
 * it as nothing rather than as a point: a window shrunk to nothing must not
 * repaint one pixel of itself.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_DSK_RECT_H
#define ARGON_DSK_RECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int16_t x, y;
    int16_t w, h; /* zero in either means empty */
} dsk_rect_t;

/*
 * How many pieces of damage are remembered before they start being merged.
 *
 * Eight is chosen against what actually happens: a mouse move damages two
 * (where the pointer was, where it is), a window activation three (the frame
 * that lost focus, the one that gained it, and the strip the z-order change
 * uncovered), a menu two.  A number this small also bounds the worst case of
 * the repaint itself, which walks every window for every rectangle.
 */
#define DSK_DAMAGE_MAX 8

typedef struct {
    dsk_rect_t r[DSK_DAMAGE_MAX];
    uint8_t    n;
} dsk_damage_t;

static inline dsk_rect_t dsk_rect(int16_t x, int16_t y, int16_t w, int16_t h)
{
    dsk_rect_t r;
    r.x = x;
    r.y = y;
    r.w = (w > 0) ? w : 0;
    r.h = (h > 0) ? h : 0;
    return r;
}

static inline dsk_rect_t dsk_rect_none(void) { return dsk_rect(0, 0, 0, 0); }

static inline bool dsk_rect_empty(dsk_rect_t r)
{
    return r.w <= 0 || r.h <= 0;
}

/* Edges, as half-open intervals: [x, x2) and [y, y2). */
static inline int32_t dsk_rect_x2(dsk_rect_t r) { return (int32_t)r.x + r.w; }
static inline int32_t dsk_rect_y2(dsk_rect_t r) { return (int32_t)r.y + r.h; }

static inline int32_t dsk_rect_area(dsk_rect_t r)
{
    return dsk_rect_empty(r) ? 0 : (int32_t)r.w * (int32_t)r.h;
}

bool       dsk_rect_eq(dsk_rect_t a, dsk_rect_t b);
bool       dsk_rect_has(dsk_rect_t r, int16_t x, int16_t y);
bool       dsk_rect_overlaps(dsk_rect_t a, dsk_rect_t b);
/* True when b is wholly inside a (an empty b is inside anything). */
bool       dsk_rect_covers(dsk_rect_t a, dsk_rect_t b);
dsk_rect_t dsk_rect_clip(dsk_rect_t a, dsk_rect_t b);
/* Bounding box of the two; an empty operand yields the other. */
dsk_rect_t dsk_rect_union(dsk_rect_t a, dsk_rect_t b);
/* Shrink (n > 0) or grow (n < 0) by n on every side. */
dsk_rect_t dsk_rect_inset(dsk_rect_t r, int16_t n);
dsk_rect_t dsk_rect_offset(dsk_rect_t r, int16_t dx, int16_t dy);

void dsk_damage_clear(dsk_damage_t *d);

/*
 * Remember that this rectangle has to be repainted.
 *
 * The list is kept small and slightly lossy, which is the right trade here: a
 * rectangle repainted that did not need to be costs a fill, while a rectangle
 * forgotten costs a visible artefact that stays on the glass until something
 * else happens to cover it.  So this merges freely and never drops.
 *
 * Merging happens when the bounding box of two pieces is no larger than the
 * two pieces added together - overlapping or flush-abutting rectangles - and,
 * when the list is full, into whichever pair wastes the fewest pixels.
 */
void dsk_damage_add(dsk_damage_t *d, dsk_rect_t r);

/* Clip every piece to `bounds`, dropping what falls outside it entirely. */
void dsk_damage_clip(dsk_damage_t *d, dsk_rect_t bounds);

/* Bounding box of everything in the list, empty when the list is empty. */
dsk_rect_t dsk_damage_bounds(const dsk_damage_t *d);

#endif /* ARGON_DSK_RECT_H */
