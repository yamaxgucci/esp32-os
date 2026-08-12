/*
 * ArgonOS - session slots (per-slot shell + optional app).
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

#define AG_SESSION_SLOTS 4
#define AG_SESSION_SYSTEM 0

typedef struct {
    ag_pid_t pid; /* AG_PID_KERNEL = shell owns this slot (no app) */
    char     name[32];
    char     cwd[AG_PATH_MAX];
} ag_session_slot_t;

void ag_session_init(void);

int      ag_session_focused(void);
int      ag_session_display_number(int slot);
ag_pid_t ag_session_focused_pid(void);
bool     ag_session_shell_owns_keyboard(void);

/* Bind into the first free slot (fallback for non-shell spawners). */
ag_err_t ag_session_bind(ag_pid_t pid, const char *name);

/*
 * Bind (or move) `pid` into an exact slot.  Fails with -AG_EBUSY if that slot
 * already has a different process.
 */
ag_err_t ag_session_bind_to(ag_pid_t pid, const char *name, int slot);

void ag_session_unbind(ag_pid_t pid);

ag_err_t ag_session_focus(int slot);
bool     ag_session_enter_system(void);
ag_err_t ag_session_alt_tab(void);

void ag_session_info(ag_session_slot_t out[AG_SESSION_SLOTS]);
int  ag_session_slot_of(ag_pid_t pid);

const char *ag_session_cwd(int slot);
ag_err_t    ag_session_set_cwd(int slot, const char *absolute_path);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_SESSION_H */
