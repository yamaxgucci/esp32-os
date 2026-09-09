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
 * Open what is picked, the way Enter and a double-click do.
 *
 * For the context menu, whose first item has to be the obvious one: a finger
 * that has just held a row down is not in a position to double-tap it.
 */
bool dsk_folder_open_sel(dsk_win_t *w);

#endif /* ARGON_DSK_FOLDER_H */
