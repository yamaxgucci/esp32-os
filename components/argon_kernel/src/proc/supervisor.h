/*
 * ArgonOS - the supervisor (kernel private).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_SUPERVISOR_H
#define ARGON_SUPERVISOR_H

#include <argon/abi.h>

/* Starts the process table and the supervisor task.  A boot stage. */
ag_err_t ag_supervisor_init(void);

bool     ag_supervisor_running(void);
/* How many times something has been stopped from the keyboard, for ver/mem. */
uint32_t ag_supervisor_stops(void);

/*
 * Asks the supervisor to end a process, from somewhere that cannot do it itself -
 * a thread of the process being ended, for instance, which cannot outlive the
 * work.  Returns immediately; the killing happens on the supervisor's task.
 */
void ag_supervisor_kill_request(ag_pid_t pid);

/*
 * Ctrl+C pressed with nothing in the foreground - that is, aimed at the shell
 * itself.  A shell command that can run for a long time asks between chunks of
 * work; the shell clears it before every command, so the answer is about this
 * command and not about a key pressed at the prompt five minutes ago.
 *
 * This exists because a device can be endless.  `type d:\zero` produces bytes
 * for as long as anyone is willing to read them, and a command that cannot be
 * stopped is a hung machine.
 */
bool ag_supervisor_interrupted(void);
void ag_supervisor_clear_interrupt(void);

/* Session switch / break-in: wake long shell builtins (fm, copy, …). */
void ag_supervisor_raise_shell_interrupt(void);

#endif /* ARGON_SUPERVISOR_H */
