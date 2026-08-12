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

/* Currently focused slot index (0..3). */
int ag_session_focused(void);

/* 1-based number for prompts / Alt+N labels (slot 0 → 1, …). */
int ag_session_display_number(int slot);

/* Pid that should own the keyboard: app if present, else KERNEL (shell). */
ag_pid_t ag_session_focused_pid(void);

/* True when the focused slot has no app — shell should read the console. */
bool ag_session_shell_owns_keyboard(void);

/* Bind a newly spawned process into a slot (prefer focused if empty). */
ag_err_t ag_session_bind(ag_pid_t pid, const char *name);

/* Clear a slot when its process ends. */
void ag_session_unbind(ag_pid_t pid);

/*
 * Switch focus to `slot` (0..3).  Empty slots are valid: gfx is released and
 * the shell owns the console for that slot.
 */
ag_err_t ag_session_focus(int slot);

/*
 * Break-in: focus slot 0 shell without killing.  Second press within ~1 s
 * hard-kills the last user app pid.
 */
bool ag_session_enter_system(void);

/* Cycle all four slots. */
ag_err_t ag_session_alt_tab(void);

void ag_session_info(ag_session_slot_t out[AG_SESSION_SLOTS]);

int ag_session_slot_of(ag_pid_t pid);

/* Per-slot working directory (shell syncs on focus / cd). */
const char *ag_session_cwd(int slot);
ag_err_t    ag_session_set_cwd(int slot, const char *absolute_path);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_SESSION_H */
