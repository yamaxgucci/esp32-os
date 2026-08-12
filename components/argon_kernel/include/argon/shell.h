/*
 * ArgonOS - built-in shell.
 *
 * The shell is part of the kernel image rather than a program on the card, so
 * a board with no readable media still comes up to a prompt and can be
 * inspected.  That is the difference between a system that is degraded and one
 * that is bricked.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_SHELL_H
#define ARGON_SHELL_H

#include <stddef.h>

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runs the read-eval-print loop on the console.  Does not return. */
void ag_shell_run(void);

/* Executes one command line, as if it had been typed.  Returns the exit code. */
int ag_shell_execute(const char *line);

const char *ag_shell_cwd(void);

/*
 * True when the operator pressed Ctrl+C during the command that is running now.
 * A command whose work is bounded need not ask; one that reads until the end of
 * a file has to, because a device has no end - `type d:\zero` would otherwise
 * be a machine that has to be reset.
 */
bool ag_shell_interrupted(void);

/* Clears the flag after a command has handled Ctrl+C (e.g. cancel a copy). */
void ag_shell_clear_interrupted(void);

/* Expects an already canonical absolute path; the caller checks it exists. */
ag_err_t ag_shell_set_cwd(const char *path);

/*
 * Renders a POSIX path the way DOS would have: /sd/apps becomes A:\APPS.
 * Presentation only - the kernel deals exclusively in POSIX paths.
 */
void ag_shell_dos_path(const char *posix_path, char *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_SHELL_H */
