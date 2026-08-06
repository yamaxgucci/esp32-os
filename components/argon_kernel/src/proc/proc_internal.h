/*
 * ArgonOS - what the process layer shares between its own files (kernel
 * private).
 *
 * Only proc.c and threads.c include this.  Everything else talks to processes
 * through argon/proc.h, which is deliberately free of FreeRTOS.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_PROC_INTERNAL_H
#define ARGON_PROC_INTERNAL_H

#include <setjmp.h>

#include <argon/lineedit.h>
#include <argon/path.h>
#include <argon/proc.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "multi_heap.h"

/* Bounds on what one process may hold.  A bound that is reached is a diagnosable
 * event; an unbounded list is a leak with extra steps (see argon/reslist.h). */
#define AG_PROC_RES_MAX 32
#define AG_PROC_ARGS_MAX 16

typedef struct {
    bool            used;
    ag_pid_t        pid;
    char            name[32];
    ag_proc_state_t state;
    uint32_t        flags;

    ag_loaded_app_t   app;
    TaskHandle_t      task;
    SemaphoreHandle_t done;
    int32_t           exit_code;
    bool              killed;
    bool              has_waiter; /* somebody is in ag_proc_wait for it      */
    volatile bool     signalled;
    jmp_buf           exit_jump;

    int   argc;
    char *argv[AG_PROC_ARGS_MAX];
    char  argbuf[AG_LINE_MAX];

    char cwd[AG_PATH_MAX];

    void               *heap_mem;
    multi_heap_handle_t heap;
    size_t              heap_size;

    ag_res_t     res_slots[AG_PROC_RES_MAX];
    ag_reslist_t res;

    ag_time_t started;
    uint32_t  heartbeat_ms;
    uint32_t  stack_bytes;
    uint32_t  stack_unused;

    /*
     * Who had the console before this one took it.  A process started by another
     * process gives it back to its parent when it ends, not to the shell - which
     * is what makes exec() from inside an application behave the way the shell's
     * own run does.
     */
    ag_pid_t prev_foreground;
} proc_t;

/* The process the calling task belongs to, or NULL for the kernel's own tasks. */
proc_t *ag_proc_current(void);

/* The process table lock.  Recursive; held while a slot is examined or changed. */
void ag_proc_table_lock(void);
void ag_proc_table_unlock(void);

/* ---------------------------------------------------------------------- */
/* Threads (threads.c)                                                    */
/* ---------------------------------------------------------------------- */

/*
 * A thread of a process.  Held in the process's resource list, so a process that
 * ends with threads still running takes them with it.  Allocated from the
 * kernel's internal heap rather than from the process arena, because the arena is
 * released while these are still being used to stop the threads.
 */
typedef struct {
    TaskHandle_t  task;
    void        (*fn)(void *);
    void         *arg;
    volatile bool finished;
} ag_thread_rec_t;

/* True when this record is that task; how a thread finds its own process. */
bool ag_thread_owns(const void *record, TaskHandle_t task);

/* Stops the thread if it is still running and frees the record. */
void ag_thread_release(void *record);

#endif /* ARGON_PROC_INTERNAL_H */
