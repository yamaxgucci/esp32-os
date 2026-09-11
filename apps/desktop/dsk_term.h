/*
 * ArgonOS DESKTOP - the console, in a window.
 *
 * The kernel's console draws itself on whatever the machine has: a
 * framebuffer, a panel's text cells, a serial terminal.  None of those is any
 * use to a shell that owns the screen, so while the desktop is up the console
 * is invisible - and the console is where everything says what happened.  The
 * boot report, a driver refusing to load, the output of a program that has
 * already exited: all of it is there and none of it can be read without a
 * serial cable.
 *
 * So this window reads the console's cells (ag_con_peek_row, ABI 0.43) and
 * draws them itself.  It is a *view*: what it shows is the one console the
 * machine has, and typing goes to the desktop as it always did.
 *
 * What it is not, and why not yet: a Console, meaning a shell of its
 * own to type at.  That needs a console screen per session slot - there is
 * exactly one today, and switching slots clears it - and a second instance of
 * the shell.  Both are kernel work and neither is needed to read what the
 * machine said, which is the half worth having first.  See §9.4 of
 * docs/plans/desktop.md.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_DSK_TERM_H
#define ARGON_DSK_TERM_H

#include "dsk_wm.h"

/* Open the console window, or bring the one that exists to the front. */
dsk_win_t *dsk_term_open(const dsk_metrics_t *m);

/*
 * The same window, showing a session slot's own screen and feeding it the
 * keys it cannot read for itself.
 *
 * A slot with a shell of its own (ag_prompt_in_slot) has a screen that is
 * nowhere on the glass while this desktop holds it, and no claim on the
 * keyboard while this desktop is in front.  Both of those are what this
 * window is for.  A slot below zero is the system console, as a view.
 */
dsk_win_t *dsk_term_open_slot(const dsk_metrics_t *m, int slot);

/* How many keys a prompt window passed on, and how many the slot took. */
void dsk_term_fwd_stats(uint32_t *sent, uint32_t *taken);

/*
 * Whether this window is somebody's prompt rather than the view.
 *
 * The shell asks because a prompt is the one window here that a person
 * types a line into, and on a board whose only input is a stylus that
 * is when the keyboard has to come up.  The keyboard itself belongs to
 * the desk, not to this window: there is one of them, and it serves
 * whatever has focus.
 */
bool dsk_term_is_prompt(const dsk_win_t *w);

/*
 * A tap landed on a prompt's text since this was last asked.
 *
 * The desk raises its keyboard on that and on nothing else: a drag is
 * how the text is scrolled back, and the frame belongs to the window
 * manager, so neither of those should bring up a keyboard.
 */
bool dsk_term_take_tap(void);

/*
 * Re-read the console and damage the rows that changed.
 *
 * Called from the shell's idle rather than driven by an event, because the
 * console changes without asking anybody: a driver logs, a background process
 * prints.  Cheap when nothing moved - one row read and compared per row on
 * screen - and it does nothing at all when the window is not open.
 */
void dsk_term_tick(void);

/* How long until dsk_term_tick would do something, in milliseconds. */
uint32_t dsk_term_due_in(uint32_t now);

#endif /* ARGON_DSK_TERM_H */
