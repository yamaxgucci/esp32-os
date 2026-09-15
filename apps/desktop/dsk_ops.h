/*
 * ArgonOS DESKTOP - copying, moving, deleting, and the box that says so.
 *
 * The operations themselves are apps/common/fsops, shared with `fm`.  What is
 * here is the shell's side of them: the progress box, the Escape that stops
 * one, and turning an error into a message the user can read.
 *
 * ---- the one blocking thing in this shell ----------------------------------
 *
 * Everything else here is callback driven, on purpose: a dialog opens and
 * returns, so the shell keeps repainting and keeps answering while a question
 * is on the screen (see dsk_dlg.h).  These do not.  A copy runs to its end
 * inside one call, and the progress box is drawn from the tick that fsops
 * calls as it goes.
 *
 * That is not the same compromise.  A question waits for a person and may wait
 * for ever, so a shell that blocks on one is a shell that has stopped.  An
 * operation is bounded by itself, cannot be answered by anyone, and has
 * exactly one thing worth doing while it runs - saying how far it has got and
 * watching for Escape - which is what the tick does.  Making fsops resumable
 * instead would put a state machine across a tree walk to buy nothing anyone
 * can see.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_DSK_OPS_H
#define ARGON_DSK_OPS_H

#include "dsk.h"

/*
 * `repaint` puts the desktop back after the progress box has been over it -
 * the box is drawn straight onto the surface rather than as a window, because
 * it is up for the length of one call and a window would have to be created,
 * damaged and destroyed inside it.
 */
void dsk_ops_init(const dsk_metrics_t *m, void (*repaint)(void));

/*
 * Each of these reports its own errors and returns when the work is over.
 * `to` is the finished name for the copy and the move, not the directory to
 * put it in: which of those the user meant is decided where they typed it.
 */
void dsk_ops_copy(const char *from, const char *to, const char *label);
void dsk_ops_move(const char *from, const char *to, const char *label);
void dsk_ops_delete(const char *path, const char *label);

/* No progress box: these are one call to the filesystem and either work or
 * do not.  The error is reported the same way. */
bool dsk_ops_mkdir(const char *path, const char *label);
bool dsk_ops_rename(const char *from, const char *to, const char *label);

/*
 * Whether the last operation changed anything.  The shell re-reads the
 * folder windows only when it did, so a cancelled operation that got nowhere
 * does not make every open window flicker.
 */
bool dsk_ops_changed(void);

#endif /* ARGON_DSK_OPS_H */
