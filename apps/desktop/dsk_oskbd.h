/*
 * ArgonOS DESKTOP - the shell's keyboard, one for everything.
 *
 * There used to be two: one drawn inside a dialog box, which existed so that
 * Run, Rename and Create directory could be carried out on a board with no
 * keys, and one drawn inside a prompt window for the same reason.  Two
 * keyboards is one too many - they were different sizes, only one of them
 * had an Enter, and which one a person got depended on which window they
 * were in.
 *
 * So it lives on the desk instead, above the windows and below an open menu,
 * and what it types goes wherever a key from a real keyboard would go: to
 * the menu if one is open, then to the window in front.  That is the whole
 * of the focus question dsk_kbd.h once declined to answer - it is not this
 * file's to answer either, it is already answered by whatever has focus.
 *
 * It comes up when somebody touches a prompt window, because a prompt is
 * where a person types a line, and it goes away by its own key.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_DSK_OSKBD_H
#define ARGON_DSK_OSKBD_H

#include "dsk.h"

/*
 * `damage` marks a rectangle for repaint; `key` is given what was pressed,
 * in the same shape a driver would have delivered it.
 */
void dsk_oskbd_init(const dsk_metrics_t *m, void (*damage)(dsk_rect_t r),
                    void (*key)(uint16_t keycode, uint32_t unicode));

/* The Options setting: false hides it and refuses to raise it again. */
void dsk_oskbd_allow(bool allowed);
bool dsk_oskbd_allowed(void);

/*
 * What the first two keys of the row are called: a box's OK and Cancel,
 * or a line's Enter and Esc.  They send the same two keys either way.
 */
void dsk_oskbd_mode(bool for_dialog);

void dsk_oskbd_show(bool on);
bool dsk_oskbd_visible(void);

/* How tall it is when up - answered whether or not it is up. */
int16_t dsk_oskbd_height(void);

/* Where it is, or an empty rectangle when it is not up. */
dsk_rect_t dsk_oskbd_rect(void);

/* What is left of the work area above it, for anything that must not hide. */
dsk_rect_t dsk_oskbd_work(void);

void dsk_oskbd_draw(dsk_rect_t clip);

/* Where the letters and the row of three are, up or not: for the test. */
void dsk_oskbd_probe(dsk_rect_t *keys, dsk_rect_t *row);

/* True when the point was the keyboard's, press or release alike. */
bool dsk_oskbd_pointer(dsk_ptr_t type, int16_t x, int16_t y);

#endif /* ARGON_DSK_OSKBD_H */
