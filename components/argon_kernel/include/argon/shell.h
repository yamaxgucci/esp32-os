/*
 * ArgonOS - built-in shell.
 *
 * The shell is part of the kernel image rather than a program on the card, so
 * a board with no readable media still comes up to a prompt and can be
 * inspected.  That is the difference between a system that is degraded and one
 * that is bricked.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_SHELL_H
#define ARGON_SHELL_H

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runs the read-eval-print loop on the console.  Does not return. */
void ag_shell_run(void);

/* Executes one command line, as if it had been typed.  Returns the exit code. */
int ag_shell_execute(const char *line);

const char *ag_shell_cwd(void);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_SHELL_H */
