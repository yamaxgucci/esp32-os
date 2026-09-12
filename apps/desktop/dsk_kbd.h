/*
 * ArgonOS DESKTOP - a keyboard drawn on the glass.
 *
 * The board this shell was written for has a touchscreen and no keys.  Copy
 * and Move stopped needing letters when the clipboard arrived, but Run,
 * Rename and Create directory are all a person telling the machine a NAME,
 * and there was no way for that person to say one.  Those three commands
 * were on the menu and could not be carried out.
 *
 * It is deliberately not a soft keyboard for the whole system: it belongs to
 * the dialog with a text box in it, appears with that box and goes with it.
 * A system-wide one would have to decide what has focus, where it may cover,
 * and what happens when an application wants the screen - none of which this
 * needs in order to let somebody type "hi.axe".
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_DSK_KBD_H
#define ARGON_DSK_KBD_H

#include "dsk.h"

/* What a press produced.  A character comes back as itself. */
#define DSK_KBD_NONE      0
#define DSK_KBD_BACKSPACE 8
#define DSK_KBD_SHIFT     1 /* handled inside; the caller only repaints */

/* How tall this wants to be for a box `w` wide.  Zero if it will not fit. */
int16_t dsk_kbd_height(int16_t w);

/*
 * The same, given only `avail` pixels to live in: the keys shrink rather
 * than the keyboard overflowing.  A caller with a fixed screen has to ask
 * this one - a box taller than the screen is cut down by the window manager
 * and then the keyboard, which sits at the bottom, climbs over what is above
 * it.
 */
int16_t dsk_kbd_height_for(int16_t w, int16_t avail);

void dsk_kbd_draw(dsk_rect_t r);

/*
 * The character under the point, or DSK_KBD_NONE.  Shift is answered as
 * DSK_KBD_SHIFT and has already been applied to this keyboard's own state,
 * so the caller repaints and does nothing else.
 */
int dsk_kbd_press(dsk_rect_t r, int16_t x, int16_t y);

/* Which key the point is over, for drawing it pressed.  -1 for none. */
int  dsk_kbd_key_at(dsk_rect_t r, int16_t x, int16_t y);
void dsk_kbd_hilite(int key); /* -1 to clear */

/* Back to lower case, for a keyboard that is about to be shown again. */
void dsk_kbd_reset(void);

#endif /* ARGON_DSK_KBD_H */
