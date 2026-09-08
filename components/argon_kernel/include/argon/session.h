/*
 * ArgonOS - session slots (per-slot shell + optional app) and system shell.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
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

/*
 * How deep one slot may nest.  A slot is not one process but a stack of them:
 * the shell at the bottom, the application it started above that, and whatever
 * the application starts above that - a shell running a file manager running
 * an editor.  Whoever is on top has the keyboard and the console; the ones
 * beneath are waiting for it and get their slot back when it goes.
 *
 * Four is what a Program-Manager-shaped stack needs plus one, and the cost is
 * a pid and a name per level in a table that already lives in slow memory.
 */
#define AG_SESSION_DEPTH 4

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

/*
 * Push `pid` on top of whatever is in that slot already, suspending it there
 * rather than refusing.  This is what a process starting another process
 * needs: the child must have the keyboard and the console, and the parent must
 * get them back when the child ends - which is what ag_session_unbind does
 * when it pops.
 *
 * -AG_ENFILE when the slot is AG_SESSION_DEPTH deep.  Binding an empty slot
 * is the same thing as ag_session_bind_to, so this is safe to call either way.
 */
ag_err_t ag_session_push_to(ag_pid_t pid, const char *name, int slot);

/*
 * Take `pid` out of its slot.  When it was on top of a stack the process
 * beneath becomes the top and is told it has the focus again; when it was
 * buried (a parent killed while its child still runs) it is simply removed and
 * the top does not change.
 */
void ag_session_unbind(ag_pid_t pid);

/*
 * The pids in a slot, deepest first, `out` holding up to AG_SESSION_DEPTH.
 * Returns how many there are.  For `tasklist` and for the tests: the nesting
 * is otherwise invisible, and a slot that quietly holds three processes is
 * exactly the kind of thing worth being able to look at.
 */
int ag_session_stack(int slot, ag_pid_t out[AG_SESSION_DEPTH]);

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
