/*
 * ArgonOS - the screen's code page.
 *
 * A cell of the text screen holds one byte, exactly like the video memory of a
 * PC, and the code page says what that byte means.  Conversion happens at the
 * edges: a terminal speaks UTF-8, so the renderer converts each cell byte to a
 * code point on the way out, and the input decoder converts a typed code point
 * back to a byte on the way in.  Everything in between - the screen, the line
 * editor, poke and fill, an application counting columns - deals in bytes and
 * needs to know nothing about Unicode.
 *
 * There is one code page for the machine, not one per screen or per process.
 * That is how DOS did it, and it is the only arrangement in which a byte written
 * by one thing and read by another means the same thing to both.
 *
 * This file is free of kernel and ESP-IDF dependencies so it can be unit
 * tested on a host.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_CODEPAGE_H
#define ARGON_CODEPAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AG_CP_437 = 0, /* the PC's original: box drawing, no Cyrillic          */
    AG_CP_866,     /* MS-DOS Cyrillic, with the box drawing of 437 kept    */
    AG_CP_1251,    /* Windows Cyrillic: what a Russian text file usually is */
    AG_CP_COUNT,
} ag_cp_t;

/* The byte's code point, or 0 when the page leaves that byte undefined. */
uint32_t ag_cp_to_unicode(ag_cp_t cp, uint8_t byte);

/* The byte for a code point, or -1 when this page has no such character. */
int32_t ag_cp_from_unicode(ag_cp_t cp, uint32_t codepoint);

/* Writes 1..4 bytes and returns how many; 0 for a code point outside Unicode. */
size_t ag_utf8_encode(uint32_t codepoint, char out[4]);

/* The number everyone knows the page by: 437, 866, 1251. */
uint16_t ag_cp_number(ag_cp_t cp);
bool     ag_cp_from_number(uint16_t number, ag_cp_t *out);

/* One line about the page, for `chcp` with no argument. */
const char *ag_cp_title(ag_cp_t cp);

/*
 * The machine's code page.  Changing it changes what the bytes already on the
 * screen mean, which is not a bug: it is the same as changing the font on a PC,
 * and the alternative - rewriting every cell - would be a guess about what the
 * old bytes were meant to be.
 */
ag_cp_t ag_cp_active(void);
void    ag_cp_set_active(ag_cp_t cp);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_CODEPAGE_H */
