/*
 * ArgonOS - virtual text screen.
 *
 * The kernel keeps one grid of character cells, exactly like the video memory
 * of a PC in text mode.  Every console endpoint renders that same grid: a
 * VT100 stream over UART or telnet, a bitmap font blitted to a local display,
 * or nothing at all on a headless board.  Applications write to one screen and
 * do not care who is watching.
 *
 * The module also parses the ANSI/VT100 escape sequences that ported code
 * emits, so an application that just printf()s colour codes behaves sensibly.
 *
 * This file is free of kernel and ESP-IDF dependencies so it can be unit
 * tested on a host.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_SCREEN_H
#define ARGON_SCREEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AG_SCREEN_MAX_COLS 240
#define AG_SCREEN_MAX_ROWS 128

/* Light grey on black, the same value a PC BIOS leaves behind. */
#define AG_ATTR_DEFAULT AG_ATTR(AG_LGRAY, AG_BLACK)

typedef struct {
    char    ch;
    uint8_t attr;
} ag_cell_t;

/* Escape sequence parser state; private, exposed only so the struct is sized. */
typedef enum {
    AG_VT_GROUND = 0,
    AG_VT_ESC,
    AG_VT_ESC_SKIP1, /* charset selection and friends: swallow one byte */
    AG_VT_CSI,
    AG_VT_OSC,
} ag_vt_state_t;

#define AG_VT_MAX_PARAMS 8

typedef struct {
    ag_cell_t *cells; /* cols * rows, row major        */
    uint32_t  *dirty; /* one bit per row               */

    uint16_t cols;
    uint16_t rows;
    uint16_t cur_x;
    uint16_t cur_y;
    uint8_t  attr;
    bool     cursor_visible;
    bool     reverse; /* SGR 7 is active, so `attr` holds swapped nibbles */

    /*
     * Deferred wrap: writing to the last column leaves the cursor there and
     * arms this flag, so a line exactly as wide as the screen does not emit a
     * blank line after it.  Any explicit cursor move disarms it.
     */
    bool pending_wrap;

    uint16_t saved_x;
    uint16_t saved_y;
    uint8_t  saved_attr;

    /* Escape sequence parser. */
    ag_vt_state_t state;
    int32_t       params[AG_VT_MAX_PARAMS];
    uint8_t       nparams;
    bool          param_seen;
    bool          private_seq;

    /* Bumped on every change; lets a renderer skip work when nothing moved. */
    uint32_t generation;
} ag_screen_t;

/* Bytes ag_screen_init() needs for a grid of this size. */
size_t ag_screen_memsize(uint16_t cols, uint16_t rows);

/*
 * Binds `mem` (at least ag_screen_memsize() bytes) to the screen and clears
 * it.  The memory must outlive the screen; nothing is allocated here.
 */
ag_err_t ag_screen_init(ag_screen_t *s, void *mem, size_t memsize,
                        uint16_t cols, uint16_t rows);

/* Writes text, interpreting control characters and escape sequences. */
void ag_screen_write(ag_screen_t *s, const char *buf, size_t len);
void ag_screen_puts(ag_screen_t *s, const char *str);

/* Writes one character with no interpretation at all. */
void ag_screen_putc_raw(ag_screen_t *s, char ch);

void ag_screen_cls(ag_screen_t *s);
void ag_screen_gotoxy(ag_screen_t *s, uint16_t x, uint16_t y);
void ag_screen_set_attr(ag_screen_t *s, uint8_t attr);
void ag_screen_set_cursor(ag_screen_t *s, bool visible);

/* Scrolls the whole screen up by `lines`, filling with the current attribute. */
void ag_screen_scroll_up(ag_screen_t *s, uint16_t lines);

void ag_screen_poke(ag_screen_t *s, uint16_t x, uint16_t y, char ch,
                    uint8_t attr);
void ag_screen_fill(ag_screen_t *s, uint16_t x, uint16_t y, uint16_t w,
                    uint16_t h, char ch, uint8_t attr);

const ag_cell_t *ag_screen_row(const ag_screen_t *s, uint16_t y);
ag_cell_t ag_screen_at(const ag_screen_t *s, uint16_t x, uint16_t y);

/*
 * Dirty rows.  A renderer walks the rows, redraws the dirty ones and then
 * calls ag_screen_clear_dirty().  Rendering to several endpoints at different
 * rates needs one dirty set each; use ag_screen_snapshot_dirty() to take the
 * set without clearing it for the others.
 */
bool ag_screen_row_dirty(const ag_screen_t *s, uint16_t y);
void ag_screen_mark_row_dirty(ag_screen_t *s, uint16_t y);
void ag_screen_mark_all_dirty(ag_screen_t *s);
void ag_screen_clear_dirty(ag_screen_t *s);
bool ag_screen_any_dirty(const ag_screen_t *s);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_SCREEN_H */
