/*
 * ArgonOS - processes.
 *
 * A process is an application plus everything the system has given it: a task
 * to run on, a stack, an arena to allocate from, a working directory, and a
 * resource list.  It exists so that ending an application - because it returned,
 * because it was killed, or because it hung - gives all of that back.
 *
 * The execution model stays DOS-like: one application in the foreground owning
 * the console, at most AG_PROC_MAX loaded at once, no address-space isolation.
 * What is added over DOS is accounting, and accounting is what makes a hung
 * application removable instead of a reason to reboot (architecture §5).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_PROC_H
#define ARGON_PROC_H

#include <argon/abi.h>
#include <argon/loader.h>
#include <argon/reslist.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Four, as the architecture says.  The code arena is shared between them, so in
 * practice the limit that bites first is how much code fits in it.
 */
#define AG_PROC_MAX 4

/* The pid the kernel's own work belongs to: the shell, the console, drivers. */
#define AG_PID_KERNEL 0

ag_err_t ag_proc_init(void);

/*
 * Loads `path` and runs it to completion, returning its exit code.  This is what
 * the shell's run command does, and what an application's exec() does.
 *
 * Negative return values are the loader's errors, not the application's exit
 * code; the two are told apart by the caller, which knows whether it ever
 * started.  `exit_code` receives the application's own value when it ran.
 */
ag_err_t ag_proc_exec(const char *path, int argc, char **argv, uint32_t flags,
                      int32_t *exit_code);

/* Starts it and returns its pid without waiting. */
ag_err_t ag_proc_spawn(const char *path, int argc, char **argv, uint32_t flags,
                       ag_pid_t *out_pid);

ag_err_t ag_proc_wait(ag_pid_t pid, int32_t *exit_code, uint32_t timeout_ms);

/*
 * Ends a process that is not going to end by itself.  Refuses with -AG_EBUSY
 * when it cannot be done safely - that is, when the process is inside the kernel
 * holding a lock the rest of the system needs - because deleting a task in that
 * state trades a hung application for a hung system.  `reason` goes in the
 * journal next to the crash record.
 */
ag_err_t ag_proc_kill(ag_pid_t pid, const char *reason);

/* Asks a process to stop by itself: what Ctrl-C does.  It may ignore this. */
ag_err_t ag_proc_signal(ag_pid_t pid);

ag_pid_t ag_proc_self(void);
ag_pid_t ag_proc_foreground(void);
ag_err_t ag_proc_set_foreground(ag_pid_t pid);
uint32_t ag_proc_count(void);

/* Iterates live processes; -AG_ENOENT past the last one. */
ag_err_t ag_proc_info(uint32_t index, ag_procinfo_t *out);

/* "running", "background", "finished" - for ps and for the journal. */
const char *ag_proc_state_name(ag_proc_state_t state);

/* Collects processes that have finished and were not waited for. */
uint32_t ag_proc_reap_finished(void);

/* ---------------------------------------------------------------------- */
/* What the syscall table forwards here                                   */
/* ---------------------------------------------------------------------- */

/*
 * Memory comes from the calling process's arena, so that killing it reclaims
 * everything at once and no application can fragment the kernel's heap.  Called
 * outside a process - by the shell, say - these fall back to the system heap,
 * which is the right answer for the kernel's own work.
 */
void  *ag_proc_alloc(size_t bytes, uint32_t caps);
void  *ag_proc_realloc(void *ptr, size_t bytes);
void   ag_proc_free(void *ptr);
size_t ag_proc_usable_size(const void *ptr);
void   ag_proc_meminfo(ag_meminfo_t *out);

/* The calling process's working directory, or the shell's outside a process. */
const char *ag_proc_cwd(void);
ag_err_t    ag_proc_set_cwd(const char *absolute_path);

/* Ends the calling process.  Does not return.  What the ABI's exit() does. */
void ag_proc_exit(int code);

/*
 * True once, when a signal has arrived and has not been read yet.  An
 * application that polls this is one that can be asked to stop; one that does
 * not is one that has to be killed.
 */
bool ag_proc_interrupted(void);

/*
 * True while the calling process has been asked to stop or marked killed, and
 * the flag has not been cleared by ag_proc_interrupted().  HostFS polls this
 * during long RPCs so a kill can proceed instead of waiting on the VFS lock.
 */
bool ag_proc_stopping(void);

/* True when this process's session slot currently has input/display focus. */
bool ag_proc_focused(void);

/*
 * Pending AG_EV_FOCUS_LOST / GAINED for `pid` (one deep).  Consumed by
 * ag_proc_take_focus_event() from the inp poll path.
 */
void ag_proc_post_focus_event(ag_pid_t pid, bool gained);
bool ag_proc_take_focus_event(ag_event_t *out);

/* Says the process is alive, for the watchdog.  What the ABI's heartbeat does. */
void ag_proc_heartbeat(void);

/*
 * Arms or disarms the calling process's deadline: it promises a heartbeat every
 * `ms`.  0 disarms, and is the default - a deadline nobody asked for would be
 * wrong for every application that legitimately waits.
 */
void ag_proc_watchdog(uint32_t ms);

/*
 * The first process that has missed its deadline, or AG_PID_KERNEL when none
 * has.  `late_by_ms` receives how long ago it should have reported.  The
 * supervisor asks; ending it is the supervisor's job, not this one's.
 */
ag_pid_t ag_proc_overdue(uint32_t *late_by_ms);

/*
 * Takes the last crash record, if one is waiting, and clears it.  The record is
 * formatted where the crash happened - which may be a task that is about to stop
 * existing, or one whose stack is nearly gone - and written to disk by the
 * supervisor, which is neither.  Returns false when there is nothing waiting.
 */
bool ag_proc_take_crash_record(char *out, size_t len);

/*
 * The threads-and-synchronisation subtable of the ABI, defined by the process
 * layer because everything in it belongs to a process.  A constant, so the
 * syscall table can point at it without a call at start-up.
 */
extern const ag_task_api_t ag_task_api_table;

#ifdef __cplusplus
}
#endif

#endif /* ARGON_PROC_H */
