/*
 * ArgonOS - session slots (system shell + user app slots).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_SESSION_H
#define ARGON_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AG_SESSION_SLOTS 4
#define AG_SESSION_SYSTEM 0

typedef struct {
    ag_pid_t pid; /* AG_PID_KERNEL = empty (slot 0 is always system) */
    char     name[32];
} ag_session_slot_t;

void ag_session_init(void);

/* Currently focused slot index (0 = system shell). */
int ag_session_focused(void);

/* Pid that should own the keyboard for the focused slot. */
ag_pid_t ag_session_focused_pid(void);

/* Bind a newly spawned process into a free user slot (1..3). */
ag_err_t ag_session_bind(ag_pid_t pid, const char *name);

/* Clear a slot when its process ends. */
void ag_session_unbind(ag_pid_t pid);

/*
 * Switch focus to `slot`.  Releases gfx from the previous owner, posts focus
 * events, updates process foreground.  Slot 0 = system shell.
 */
ag_err_t ag_session_focus(int slot);

/*
 * Break-in: focus system shell without killing.  If already in system and a
 * second press arrives within ~1 s, hard-kills the last user foreground pid.
 * Returns true when a kill was performed.
 */
bool ag_session_enter_system(void);

/* Cycle live slots (system + occupied user).  Used by Alt+Tab. */
ag_err_t ag_session_alt_tab(void);

/* Fill slot table for `slots` command / overlay. */
void ag_session_info(ag_session_slot_t out[AG_SESSION_SLOTS]);

/* Slot index for pid, or -1. */
int ag_session_slot_of(ag_pid_t pid);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_SESSION_H */
