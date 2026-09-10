/*
 * ArgonOS DESKTOP - the icons, sixteen by sixteen.
 *
 * Written as pictures, one hexadecimal digit per pixel indexing the sixteen
 * VGA colours, and '.' for the pixels that are not there.  Two and a half
 * kilobytes of readable text against six hundred bytes of packed nibbles, and
 * the difference buys an icon somebody can edit without counting in hex - the
 * same trade the cursor shapes make in dsk_cursor.c.
 *
 * Nothing is transparent by the time it reaches the panel.  An icon is
 * composed into a scratch buffer over whatever colour the caller says is
 * behind it and handed over as one opaque blit, which is why this needs no
 * read-back and will work unchanged on the band rasteriser the touch board
 * will need (docs/plans/desktop.md §9.2).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_DSK_ICONS_H
#define ARGON_DSK_ICONS_H

#include "dsk.h"

/*
 * The desk's patterns, eight rows of eight bits each.
 *
 * Windows called these wallpapers and they are nothing of the kind: a
 * picture on a 320x240 panel is a hundred and fifty kilobytes on a machine
 * with a hundred and eleven, so what went behind the icons there - and here
 * - is a tile.  Named after what they look like rather than after Windows,
 * because the shapes are ours.
 */
#define DSK_PATTERNS 5

/* Index into dsk_pattern_rows(); 0 is a plain desk and draws no tile. */
const uint8_t *dsk_pattern_rows(int which);
const char    *dsk_pattern_name(int which);

#define DSK_ICON_W 16
#define DSK_ICON_H 16

typedef enum {
    DSK_ICON_DRIVE = 0, /* the fixed disk: C:, and anything not removable */
    DSK_ICON_FLOPPY,    /* removable media: the card, A:                  */
    DSK_ICON_HOST,      /* the development machine's folder, H:           */
    DSK_ICON_FOLDER,
    DSK_ICON_UP,        /* the ".." that goes back                        */
    DSK_ICON_PROGRAM,   /* .AXE                                           */
    DSK_ICON_DRIVER,    /* .SYS - a program the shell will not run        */
    DSK_ICON_TEXT,
    DSK_ICON_FILE,      /* anything else                                  */
    DSK_ICON_COUNT,
} dsk_icon_t;

/* Which icon a name deserves, by its extension.  Directories are the caller's. */
dsk_icon_t dsk_icon_for(const char *name);

/*
 * Draw one, at 1:1 or doubled.  `bg` is what shows through the parts of the
 * picture that are not there - so the caller has to know what is behind it,
 * which on a desktop or in a list it always does.
 */
/*
 * An icon that came out of a file rather than out of this table.
 *
 * `px` is DSK_ICON_W * DSK_ICON_H bytes, one per pixel: a palette index, or
 * 0xFF for "leave what is behind" (ag_axe_icon_t in argon/axe.h).  Drawn
 * exactly like a built-in one, because from here it IS one - which is the
 * point of a program carrying its own picture.
 */
void dsk_icon_draw_px(const uint8_t *px, int16_t x, int16_t y, int scale,
                      uint32_t bg);

void dsk_icon_draw(dsk_icon_t id, int16_t x, int16_t y, int scale,
                   uint32_t bg);

/* The palette the digits index, for anything that wants to match it. */
uint32_t dsk_icon_colour(int index);

#endif /* ARGON_DSK_ICONS_H */
