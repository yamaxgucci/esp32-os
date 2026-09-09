/*
 * ArgonOS DESKTOP - what the shell remembers between one run and the next.
 *
 * C:\DESKTOP.INI, in the shape an INI has had since before Windows had a
 * registry: sections in brackets, key=value, semicolons for comments.  A
 * person can read it, edit it and delete it, and deleting it is the way back
 * to the defaults - which is worth more here than any binary format's speed,
 * because there is nothing in it big enough to be slow.
 *
 * ---- what is remembered, and what is not -----------------------------------
 *
 * Remembered: where the drive icons were dragged to, which directories were
 * open in which windows and how big those windows were, the colour behind
 * everything, and how long a double click may take.
 *
 * NOT remembered, on purpose: which window was on top, where the selection
 * was in each list, and the scroll position.  All three are about a moment
 * rather than about an arrangement, and restoring a selection onto a directory
 * whose contents have changed underneath puts the cursor on a different file
 * from the one it was on - which is worse than putting it at the top.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_DSK_INI_H
#define ARGON_DSK_INI_H

#include "dsk.h"

/* Types and error codes.  This half is not the pure one - it opens files - so
 * it may name what the filesystem answers with. */
#include <argon/abi.h>

#define DSK_INI_PATH "c:\\DESKTOP.INI"

/* Kept small deliberately: this is an arrangement, not a session log. */
#define DSK_INI_ICONS 8
#define DSK_INI_WINS 8
#define DSK_INI_LABEL 8
#define DSK_INI_PATH_MAX 96

typedef struct {
    char    label[DSK_INI_LABEL]; /* "c:", the drive's own spelling */
    int16_t x, y;                 /* top left of its cell, in work-area pixels */
} dsk_ini_icon_t;

typedef struct {
    char       path[DSK_INI_PATH_MAX];
    dsk_rect_t frame;
    uint8_t    state; /* dsk_win_state_t, so a tile stays a tile */
} dsk_ini_win_t;

typedef struct {
    uint32_t background;
    uint16_t dblclick_ms;
    /*
     * The small font, for whoever wants twice the lines.
     *
     * Off by default, and that is Maxim's call after seeing both on the CYD:
     * 8x8 is readable there and 8x16 is nicer to read, so the choice belongs
     * to whoever is looking at it rather than to a threshold in the code.
     */
    bool     small_font;

    dsk_ini_icon_t icon[DSK_INI_ICONS];
    int            nicons;

    dsk_ini_win_t win[DSK_INI_WINS];
    int           nwins;

    /* False when there was no file, so a first run can tell that from a file
     * that says the defaults - the difference decides whether one is written. */
    bool loaded;
} dsk_ini_t;

/* The built-in arrangement: teal, 400 ms, no icons placed, no windows. */
void dsk_ini_defaults(dsk_ini_t *ini);

/*
 * Reads DESKTOP.INI over `ini`, which must already hold the defaults - so a
 * file that mentions half the keys leaves the other half alone, and a
 * malformed line loses that line and nothing else.  Missing file is not an
 * error: it is what a machine that has just been flashed looks like.
 */
void dsk_ini_load(dsk_ini_t *ini);

/* Writes it.  Returns the error rather than reporting it: the caller knows
 * whether this happened on the way out, where there is nobody left to tell. */
ag_err_t dsk_ini_save(const dsk_ini_t *ini);

/* Where this drive's icon was left, or false when it was never moved. */
bool dsk_ini_icon_of(const dsk_ini_t *ini, const char *label, int16_t *x,
                     int16_t *y);

/* Remembers one, replacing any place already held for that label. */
void dsk_ini_set_icon(dsk_ini_t *ini, const char *label, int16_t x, int16_t y);

#endif /* ARGON_DSK_INI_H */
