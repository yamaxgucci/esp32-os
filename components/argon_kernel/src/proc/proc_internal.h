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

/*
 * What the exception handler managed to write down.  Filled in an exception
 * context, where nothing may be locked and nothing may be logged, and read back
 * in ordinary task context where both are fine.
 */
typedef struct {
    bool     pending;
    bool     reported; /* so a fault is not written out twice on the way down */
    uint32_t cause;
    uint32_t pc;
    uint32_t vaddr;
    uint32_t sp;
} ag_proc_fault_t;

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
    /* Path for deferred AXE load inside proc_task; empty for builtins. */
    char path[AG_PATH_MAX];
    bool load_pending;

    void               *heap_mem;
    multi_heap_handle_t heap;
    size_t              heap_size;

    ag_res_t     res_slots[AG_PROC_RES_MAX];
    ag_reslist_t res;

    ag_time_t started;
    uint32_t  heartbeat_ms;
    uint32_t  watchdog_ms; /* 0 = not watched, which is the default          */
    uint32_t  stack_bytes;
    uint32_t  stack_unused;

    /* User/sched class: AG_PRIO_LOW / NORMAL / HIGH (see argon/proc.h). */
    uint8_t sched_class;

    ag_proc_fault_t fault;

    /*
     * Who had the console before this one took it.  A process started by another
     * process gives it back to its parent when it ends, not to the shell - which
     * is what makes exec() from inside an application behave the way the shell's
     * own run does.
     */
    ag_pid_t prev_foreground;

    /* One pending focus event: 0 = none, 1 = GAINED, 2 = LOST. */
    volatile uint8_t focus_ev;
} proc_t;

/* The process the calling task belongs to, or NULL for the kernel's own tasks. */
proc_t *ag_proc_current(void);

/* The process table lock.  Recursive; held while a slot is examined or changed. */
void ag_proc_table_lock(void);
void ag_proc_table_unlock(void);

/*
 * Create a process/thread FreeRTOS task.  Tries an internal-SRAM stack first,
 * then PSRAM when CONFIG_SPIRAM allows it — same self-delete semantics as
 * xTaskCreatePinnedToCore (use plain vTaskDelete).
 */
BaseType_t ag_proc_task_create(TaskFunction_t fn, const char *name,
                               uint32_t stack_bytes, void *arg,
                               UBaseType_t prio, BaseType_t core,
                               TaskHandle_t *out);

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

/*
 * Records that the calling thread has stopped running.  Every path that ends a
 * thread must call this before it deletes itself: the reclaim at the end of the
 * process deletes threads that are still marked running, and deleting a task
 * that is already gone is a freed TCB used again.
 */
void ag_thread_mark_self_finished(void);

/* ---------------------------------------------------------------------- */
/* Faults (fault.c)                                                       */
/* ---------------------------------------------------------------------- */

/* Installs the exception handlers, on every core.  Part of the supervisor. */
ag_err_t ag_fault_init(void);

/* "store to a prohibited address", "illegal instruction" - for the record. */
const char *ag_fault_cause_name(uint32_t cause);

/*
 * True while this task is inside the kernel holding a lock the rest of the system
 * needs.  A process in that state cannot be unwound or deleted: the lock would
 * stay held.
 */
bool ag_proc_task_in_kernel(TaskHandle_t task);

/*
 * Called from an exception context: writes down what happened and nothing else.
 * Returns false when this process cannot be recovered, in which case the caller
 * must let the fault take its normal course.
 */
bool ag_proc_note_fault(uint32_t cause, uint32_t pc, uint32_t vaddr, uint32_t sp);

/*
 * Where a faulted process resumes, in ordinary task context: reports what
 * happened and ends the process.  Does not return.
 */
void ag_proc_fault_exit(void);

#endif /* ARGON_PROC_INTERNAL_H */
