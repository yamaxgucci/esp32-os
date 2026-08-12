/*
 * ArgonOS - command line splitting.
 *
 * Separate and dependency-free because argument parsing is exactly the kind of
 * code that quietly gets edge cases wrong.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_CMDLINE_H
#define ARGON_CMDLINE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AG_ARGV_MAX 16

/*
 * Splits `line` in place into arguments.  Whitespace separates; double quotes
 * group, may appear mid-argument ("a"b c gives `ab` and `c`), and an unclosed
 * quote runs to the end of the line rather than failing - a shell that refuses
 * to act on a missing quote is more annoying than one that guesses.
 *
 * Backslash is NOT an escape character: paths are written A:\APPS\X.AXE and
 * escaping them would be absurd.  Use quotes for arguments with spaces.
 *
 * Returns the number of arguments, at most `max_argv`; anything beyond that is
 * dropped.  argv entries point into `line`.
 */
int ag_cmdline_split(char *line, char **argv, int max_argv);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_CMDLINE_H */
