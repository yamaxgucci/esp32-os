/*
 * ArgonOS DESKTOP - the pointer, drawn in software.
 *
 * There is no hardware cursor on any of these panels and there is no second
 * layer to put one in, so the pointer is pixels like everything else: the
 * sixteen-by-sixteen square under it is read out, the shape is composed over
 * the copy, and the copy is put back when it moves.  Five hundred and twelve
 * bytes, and the flush of two small squares per movement.
 *
 * The discipline, and the whole of what makes a software cursor leave no
 * trail: nothing may be drawn under the pointer while it is shown, because the
 * saved pixels would then be stale and putting them back would paint over what
 * was drawn.  So a repaint is hide, draw, show - in that order, every time.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_DSK_CURSOR_H
#define ARGON_DSK_CURSOR_H

#include "dsk.h"

#define DSK_CUR_W 16
#define DSK_CUR_H 16

typedef enum {
    DSK_CUR_ARROW = 0,
    DSK_CUR_WAIT,
    DSK_CUR_COUNT,
} dsk_cursor_id_t;

/*
 * Unpacks the shapes.  Call once, before the first show.
 *
 * `damage_fn` is wanted for band mode only, where the pointer is scenery: a
 * change of shape there is not drawing but a rectangle that owes a repaint,
 * and without it the hourglass would wait for the next unrelated repaint to
 * appear.  May be NULL.
 */
void dsk_cursor_init(int16_t screen_w, int16_t screen_h,
                     void (*damage_fn)(dsk_rect_t r));

/* Where the pointer is, in surface pixels.  Clamped to the screen. */
void    dsk_cursor_place(int16_t x, int16_t y);
int16_t dsk_cursor_x(void);
int16_t dsk_cursor_y(void);

void dsk_cursor_shape(dsk_cursor_id_t id);

/* Neither of these flushes: the caller owns the damage they belong to. */
void dsk_cursor_hide(void);
void dsk_cursor_show(void);

/* Hide, move, show, and flush both squares.  For a pointer that only moved. */
void dsk_cursor_move(int16_t x, int16_t y);

/*
 * Band mode: the pointer is part of the scene.
 *
 * There is no frame to save a patch out of and put back, so moving the
 * pointer is not a blit - it is two damaged rectangles, which `place_moved`
 * hands back as one box for the caller to damage, and the drawing happens in
 * `paint`, called while the strip under the pointer is still in hand.
 */
dsk_rect_t dsk_cursor_place_moved(int16_t x, int16_t y);
void       dsk_cursor_paint(void);

/* The square the pointer occupies now, for adding to a damage list. */
dsk_rect_t dsk_cursor_rect(void);

#endif /* ARGON_DSK_CURSOR_H */
