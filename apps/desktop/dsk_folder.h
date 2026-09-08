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

#endif /* ARGON_DSK_FOLDER_H */
