/*
 * ArgonOS - session slots (per-slot shell + optional app) and system shell.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_SESSION_H
#define ARGON_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include <argon/abi.h>
#include <argon/path.h>

#ifdef __cplusplus
extern "C" {
#endif

/* User slots 0..3, shown as 1..4.  Apps bind only here. */
#define AG_SESSION_SLOTS 4

/* Dedicated OS shell; not a user slot.  Focus only via Ctrl+\ / Ctrl+Alt+Del. */
#define AG_SESSION_SYSTEM (-1)

typedef struct {
    ag_pid_t pid; /* AG_PID_KERNEL = shell owns this slot (no app) */
    char     name[32];
    char     cwd[AG_PATH_MAX];
} ag_session_slot_t;

void ag_session_init(void);

/* AG_SESSION_SYSTEM, or 0..AG_SESSION_SLOTS-1. */
int      ag_session_focused(void);
bool     ag_session_is_system(void);
int      ag_session_display_number(int slot); /* user: 1..4; sys: 0 */
ag_pid_t ag_session_focused_pid(void);
bool     ag_session_shell_owns_keyboard(void);

/* Bind into the first free user slot (fallback for non-shell spawners). */
ag_err_t ag_session_bind(ag_pid_t pid, const char *name);

/*
 * Bind (or move) `pid` into an exact user slot.  Fails with -AG_EBUSY if that
 * slot already has a different process.  Rejects AG_SESSION_SYSTEM.
 */
ag_err_t ag_session_bind_to(ag_pid_t pid, const char *name, int slot);

void ag_session_unbind(ag_pid_t pid);

ag_err_t ag_session_focus(int slot);
bool     ag_session_enter_system(void);
ag_err_t ag_session_alt_tab(void);

void ag_session_info(ag_session_slot_t out[AG_SESSION_SLOTS]);
int  ag_session_slot_of(ag_pid_t pid);

/* slot is AG_SESSION_SYSTEM or a user index. */
const char *ag_session_cwd(int slot);
ag_err_t    ag_session_set_cwd(int slot, const char *absolute_path);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_SESSION_H */
