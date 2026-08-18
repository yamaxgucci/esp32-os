/*
 * ArgonOS - the console on a panel that has no framebuffer.
 *
 * The pixel path (display.c) needs a surface: 320x240 in RGB565 is 150 KB, and
 * a board with no PSRAM does not have 150 KB in one piece.  This path sends
 * the console the other way round - the cells that changed, as characters -
 * and lets the driver turn a row of them into pixels with a buffer one row of
 * text long.
 *
 * It is deliberately not a second display subsystem.  There is no drawing
 * here, no geometry and no state beyond "which device, and what did it last
 * see": everything that knows what a glyph looks like lives in the driver,
 * which is also the only thing that knows what the panel is.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_TEXTPANEL_H
#define ARGON_TEXTPANEL_H

#include <stdbool.h>
#include <stdint.h>

#include <argon/abi.h>
#include <argon/screen.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How many cells the attached panel holds, or -AG_ENODEV when no device
 * offers the text path.  Asked once at boot, before the console is created,
 * so that a board whose only screen is 320 pixels wide runs a console that
 * fits it rather than one whose right half nobody can see.
 */
ag_err_t ag_textpanel_geometry(uint16_t *cols, uint16_t *rows);

/*
 * Send what changed.  Called from the console tick; cheap and safe when there
 * is no such device, which is the usual case.
 */
void ag_textpanel_render(const ag_screen_t *screen);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_TEXTPANEL_H */
