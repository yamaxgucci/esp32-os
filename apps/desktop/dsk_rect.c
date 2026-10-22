/*
 * ArgonOS DESKTOP - rectangles and the damage list.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "dsk_rect.h"

static int32_t min32(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t max32(int32_t a, int32_t b) { return a > b ? a : b; }

static int16_t clamp16(int32_t v)
{
    if (v < INT16_MIN) {
        return INT16_MIN;
    }
    if (v > INT16_MAX) {
        return INT16_MAX;
    }
    return (int16_t)v;
}

bool dsk_rect_eq(dsk_rect_t a, dsk_rect_t b)
{
    if (dsk_rect_empty(a) && dsk_rect_empty(b)) {
        return true;
    }
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

bool dsk_rect_has(dsk_rect_t r, int16_t x, int16_t y)
{
    if (dsk_rect_empty(r)) {
        return false;
    }
    return x >= r.x && y >= r.y && (int32_t)x < dsk_rect_x2(r) &&
           (int32_t)y < dsk_rect_y2(r);
}

bool dsk_rect_overlaps(dsk_rect_t a, dsk_rect_t b)
{
    if (dsk_rect_empty(a) || dsk_rect_empty(b)) {
        return false;
    }
    return a.x < dsk_rect_x2(b) && b.x < dsk_rect_x2(a) &&
           a.y < dsk_rect_y2(b) && b.y < dsk_rect_y2(a);
}

bool dsk_rect_covers(dsk_rect_t a, dsk_rect_t b)
{
    if (dsk_rect_empty(b)) {
        return true;
    }
    if (dsk_rect_empty(a)) {
        return false;
    }
    return b.x >= a.x && b.y >= a.y && dsk_rect_x2(b) <= dsk_rect_x2(a) &&
           dsk_rect_y2(b) <= dsk_rect_y2(a);
}

dsk_rect_t dsk_rect_clip(dsk_rect_t a, dsk_rect_t b)
{
    if (dsk_rect_empty(a) || dsk_rect_empty(b)) {
        return dsk_rect_none();
    }
    const int32_t x1 = max32(a.x, b.x);
    const int32_t y1 = max32(a.y, b.y);
    const int32_t x2 = min32(dsk_rect_x2(a), dsk_rect_x2(b));
    const int32_t y2 = min32(dsk_rect_y2(a), dsk_rect_y2(b));

    if (x2 <= x1 || y2 <= y1) {
        return dsk_rect_none();
    }
    return dsk_rect(clamp16(x1), clamp16(y1), clamp16(x2 - x1),
                    clamp16(y2 - y1));
}

dsk_rect_t dsk_rect_union(dsk_rect_t a, dsk_rect_t b)
{
    if (dsk_rect_empty(a)) {
        return dsk_rect_empty(b) ? dsk_rect_none() : b;
    }
    if (dsk_rect_empty(b)) {
        return a;
    }
    const int32_t x1 = min32(a.x, b.x);
    const int32_t y1 = min32(a.y, b.y);
    const int32_t x2 = max32(dsk_rect_x2(a), dsk_rect_x2(b));
    const int32_t y2 = max32(dsk_rect_y2(a), dsk_rect_y2(b));

    return dsk_rect(clamp16(x1), clamp16(y1), clamp16(x2 - x1),
                    clamp16(y2 - y1));
}

dsk_rect_t dsk_rect_inset(dsk_rect_t r, int16_t n)
{
    if (dsk_rect_empty(r)) {
        return dsk_rect_none();
    }
    const int32_t w = (int32_t)r.w - 2 * (int32_t)n;
    const int32_t h = (int32_t)r.h - 2 * (int32_t)n;

    if (w <= 0 || h <= 0) {
        return dsk_rect_none();
    }
    return dsk_rect(clamp16((int32_t)r.x + n), clamp16((int32_t)r.y + n),
                    clamp16(w), clamp16(h));
}

dsk_rect_t dsk_rect_offset(dsk_rect_t r, int16_t dx, int16_t dy)
{
    if (dsk_rect_empty(r)) {
        return dsk_rect_none();
    }
    return dsk_rect(clamp16((int32_t)r.x + dx), clamp16((int32_t)r.y + dy), r.w,
                    r.h);
}

/* ---- damage ------------------------------------------------------------ */

void dsk_damage_clear(dsk_damage_t *d)
{
    if (d != NULL) {
        d->n = 0;
    }
}

/* Pixels the bounding box of a and b would repaint that neither one asked for. */
static int32_t merge_waste(dsk_rect_t a, dsk_rect_t b)
{
    const int32_t box = dsk_rect_area(dsk_rect_union(a, b));
    const int32_t sum = dsk_rect_area(a) + dsk_rect_area(b) -
                        dsk_rect_area(dsk_rect_clip(a, b));
    return box - sum;
}

void dsk_damage_add(dsk_damage_t *d, dsk_rect_t r)
{
    if (d == NULL || dsk_rect_empty(r)) {
        return;
    }

    /* Already going to be repainted, or it swallows one that was. */
    for (uint8_t i = 0; i < d->n; i++) {
        if (dsk_rect_covers(d->r[i], r)) {
            return;
        }
    }
    for (uint8_t i = 0; i < d->n;) {
        if (dsk_rect_covers(r, d->r[i])) {
            d->r[i] = d->r[d->n - 1];
            d->n--;
        } else {
            i++;
        }
    }

    /* Free merge: the box costs no more than the two pieces did. */
    for (uint8_t i = 0; i < d->n; i++) {
        if (merge_waste(d->r[i], r) <= 0) {
            const dsk_rect_t box = dsk_rect_union(d->r[i], r);
            d->r[i] = d->r[d->n - 1];
            d->n--;
            dsk_damage_add(d, box); /* the box may now swallow others */
            return;
        }
    }

    if (d->n < DSK_DAMAGE_MAX) {
        d->r[d->n++] = r;
        return;
    }

    /*
     * Full.  Somebody has to be merged, so it is the pair that wastes least -
     * including the newcomer, because a small rectangle far from everything
     * else is exactly the one that should not drag a whole corner of the screen
     * into somebody else's box.
     */
    int32_t best = INT32_MAX;
    int     bi = 0, bj = -1; /* bj < 0 means "merge bi with the newcomer" */

    for (uint8_t i = 0; i < d->n; i++) {
        const int32_t w = merge_waste(d->r[i], r);
        if (w < best) {
            best = w;
            bi = i;
            bj = -1;
        }
        for (uint8_t j = (uint8_t)(i + 1); j < d->n; j++) {
            const int32_t w2 = merge_waste(d->r[i], d->r[j]);
            if (w2 < best) {
                best = w2;
                bi = i;
                bj = j;
            }
        }
    }

    if (bj < 0) {
        d->r[bi] = dsk_rect_union(d->r[bi], r);
        return;
    }
    d->r[bi] = dsk_rect_union(d->r[bi], d->r[bj]);
    d->r[bj] = d->r[d->n - 1];
    d->n--;
    d->r[d->n++] = r;
}

void dsk_damage_clip(dsk_damage_t *d, dsk_rect_t bounds)
{
    if (d == NULL) {
        return;
    }
    uint8_t out = 0;
    for (uint8_t i = 0; i < d->n; i++) {
        const dsk_rect_t c = dsk_rect_clip(d->r[i], bounds);
        if (!dsk_rect_empty(c)) {
            d->r[out++] = c;
        }
    }
    d->n = out;
}

dsk_rect_t dsk_damage_bounds(const dsk_damage_t *d)
{
    dsk_rect_t box = dsk_rect_none();

    if (d == NULL) {
        return box;
    }
    for (uint8_t i = 0; i < d->n; i++) {
        box = dsk_rect_union(box, d->r[i]);
    }
    return box;
}
