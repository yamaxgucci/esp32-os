/*
 * ArgonOS text editor - shared declarations for the built-in and .AXE builds.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_EDIT_H
#define ARGON_EDIT_H

#include <argon/argon.h>
#include <argon/keys.h>
#include <argon/libc.h>

/*
 * Same dual build as the file manager: with AG_BUILTIN this is the shell's
 * `edit` command; without it, an ordinary .AXE.  Either way it talks to the
 * system only through the syscall table.
 */
#ifdef AG_BUILTIN
#define EDIT_ENTRY ag_edit_main
#else
#define EDIT_ENTRY ag_main
#endif

int EDIT_ENTRY(int argc, char **argv);

#define EDIT_COLS 80
#define EDIT_ROWS 25
#define EDIT_TEXT_ROWS 23 /* rows 0..22; 23 = keys, 24 = status            */
#define EDIT_ROW_KEYS 23
#define EDIT_ROW_STATUS 24

/*
 * Bounded on purpose: the on-device compiler targets the same arena, and an
 * editor that quietly eats megabytes would only hide the real limit.  Hitting
 * either bound is reported on the status line.
 */
#define EDIT_MAX_BYTES (64u * 1024u)
#define EDIT_MAX_LINES 2048
#define EDIT_LINE_MAX 512

#define EDIT_ATTR_TEXT AG_ATTR(AG_LGRAY, AG_BLUE)
#define EDIT_ATTR_CURSOR AG_ATTR(AG_BLACK, AG_CYAN)
#define EDIT_ATTR_KEYS AG_ATTR(AG_BLACK, AG_CYAN)
#define EDIT_ATTR_STATUS AG_ATTR(AG_LGRAY, AG_BLACK)
#define EDIT_ATTR_MESSAGE AG_ATTR(AG_YELLOW, AG_BLACK)
#define EDIT_ATTR_ERROR AG_ATTR(AG_LRED, AG_BLACK)
#define EDIT_ATTR_DIALOG AG_ATTR(AG_BLACK, AG_LGRAY)

#endif /* ARGON_EDIT_H */
