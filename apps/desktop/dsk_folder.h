/*
 * ArgonOS DESKTOP - a window onto a directory.
 *
 * One list, one line per entry, an icon and a name and a size.  Not two
 * panels: the two-panel manager already exists and is better at what it is
 * for (`fm`, and GFXFM beside it).  This is the other idea - a window per
 * place, several of them at once, dragged where you want them - which is the
 * one the desktop is for.
 *
 * The entries live on the heap, read once when the window opens and again on
 * F5.  Reading is slow enough to be worth saying so: a directory on HostFS
 * takes hundreds of milliseconds, so the pointer becomes an hourglass while it
 * happens rather than the shell appearing to have died.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_DSK_FOLDER_H
#define ARGON_DSK_FOLDER_H

#include "dsk.h"
#include "dsk_wm.h"

/* How the shell asks for a file to be opened, so this file runs nothing. */
typedef void (*dsk_folder_open_fn)(const char *path, bool is_dir);

void dsk_folder_init(const dsk_metrics_t *m, dsk_folder_open_fn open_file);

/* A window onto `path`, cascaded into the work area.  NULL when refused. */
dsk_win_t *dsk_folder_open(const char *path);

/* Point an existing folder window somewhere else (a subdirectory, or up). */
void dsk_folder_go(dsk_win_t *w, const char *path);

/* True when this window is a folder, so the shell can tell its own apart. */
bool dsk_folder_is(const dsk_win_t *w);

/* Re-read every folder window showing this directory.  NULL means all. */
void dsk_folder_refresh(const char *path);

/* The directory this window is showing, or NULL when it is not a folder. */
const char *dsk_folder_path(const dsk_win_t *w);

/*
 * What the cursor is on, as a whole path.  False when there is nothing to
 * point at, or when the cursor is on ".." - which is a place and not a file,
 * and an operation aimed at it would be aimed at the parent directory.
 */
bool dsk_folder_selected(const dsk_win_t *w, char *path, size_t len,
                         char *name, size_t name_len, bool *is_dir);

/*
 * Put the cursor on this name, if the window has it.  Called after an
 * operation so the thing just created or renamed is the thing selected -
 * without it a rename leaves the cursor on whatever happens to sort into that
 * row, which is not the file the user just named.
 */
void dsk_folder_select_name(dsk_win_t *w, const char *name);

/*
 * Marks: what an operation acts on when it is more than one thing.
 *
 * The cursor and the selection used to be the same row, and while nothing is
 * marked they still are - every operation asks dsk_folder_selected() first
 * and gets the cursor, exactly as before.  Marks are the other case: once
 * there are any, they are what Copy, Move and Delete work through.
 */
typedef enum {
    DSK_MARK_TOGGLE = 0, /* one row; -1 means the cursor's */
    DSK_MARK_ALL,
    DSK_MARK_NONE,
    DSK_MARK_INVERT,
} dsk_mark_t;

void dsk_folder_mark(dsk_win_t *w, int row, dsk_mark_t how);

/* How many rows carry a mark in this window (0 when none do). */
int dsk_folder_marked(const dsk_win_t *w);

/* The `which`th marked entry, in the order they are listed. */
bool dsk_folder_marked_at(const dsk_win_t *w, int which, char *path,
                          size_t len, char *name, size_t name_len,
                          bool *is_dir);

/*
 * Is this point on the row that is already picked?
 *
 * Which gesture a press begins depends on the answer: on the picked row it
 * is the start of dragging that file somewhere, and anywhere else in the
 * list it is the corner of a rubber band.  The two cannot both be armed -
 * the drag eats the release that ends the band - and this is the question
 * that separates them.
 */
bool dsk_folder_on_sel(const dsk_win_t *w, int16_t x, int16_t y);

/* Is this point in the list itself - not the caption, border, scroll bar
 * or status strip?  Asked by whoever wants to know whether a press was
 * aimed at the files or at the furniture. */
bool dsk_folder_in_list(const dsk_win_t *w, int16_t x, int16_t y);

/*
 * Did the press that just happened begin a rubber band?
 *
 * Asked by whoever else wants that press - the file drag, today.  The
 * window is the one that knows: it took the press, it decided which
 * gesture it was, and this is that decision rather than a second guess at
 * it.  Guessing was tried and got it exactly backwards: the caller asked
 * "was this the picked row?" AFTER the window had moved the selection onto
 * the row under the press, so the answer was always yes.
 */
bool dsk_folder_band_armed(const dsk_win_t *w);

/*
 * The window currently drawing a rubber band, or NULL.
 *
 * A band owns the pointer until the button comes up, for the same reason
 * a dragged icon does: the hand wanders off the window - upwards over the
 * caption, sideways onto the desk - and the manager, which routes by
 * what is under the pointer, stops delivering.  The band then freezes
 * with its marks half made and never hears the release at all.
 */
dsk_win_t *dsk_folder_banding(void);

/*
 * Hand a move or a release straight to a window that is drawing a band.
 * Returns true when it was used up.
 */
bool dsk_folder_band_event(dsk_win_t *w, dsk_ptr_t type, int16_t x, int16_t y,
                           uint8_t buttons);

/*
 * Open what is picked, the way Enter and a double-click do.
 *
 * For the context menu, whose first item has to be the obvious one: a finger
 * that has just held a row down is not in a position to double-tap it.
 */
bool dsk_folder_open_sel(dsk_win_t *w);

#endif /* ARGON_DSK_FOLDER_H */
