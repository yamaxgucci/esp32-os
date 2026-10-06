/*
 * ArgonOS - what the process layer shares between its own files (kernel
 * private).
 *
 * Only proc.c and threads.c include this.  Everything else talks to processes
 * through argon/proc.h, which is deliberately free of FreeRTOS.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PROC_INTERNAL_H
#define ARGON_PROC_INTERNAL_H

#include <setjmp.h>

#include <argon/lineedit.h>
#include <argon/path.h>
#include <argon/console.h>
#include <argon/proc.h>

#include <argon/port/mem.h>
#include <argon/port/sync.h>
#include <argon/port/task.h>

/* ag_port_panic_t: the record a panic leaves behind, for the shape of it. */
#include <argon/port/fault.h>

/* Bounds on what one process may hold.  A bound that is reached is a diagnosable
 * event; an unbounded list is a leak with extra steps (see argon/reslist.h). */
#define AG_PROC_RES_MAX 32
/*
 * Sixteen arguments and a 256-byte line were a shell's measure, not an
 * application's: a game started with twenty-seven words got fifteen, and the
 * switches past the cut looked broken rather than missing.
 */
#define AG_PROC_ARGS_MAX 48
#define AG_PROC_ARGBUF   512

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
    uint32_t ra;    /* where the faulting function would have returned to */
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
    ag_port_task_t      task;
    ag_port_sem_t   done;
    int32_t           exit_code;
    bool              killed;
    /* Its data guard has already been reported: say it once, not every tick. */
    bool              guard_said;
    bool              has_waiter; /* somebody is in ag_proc_wait for it      */
    /*
     * The process that spawned this one, or AG_PID_KERNEL when the shell, a
     * driver or the kernel did.  While that process lives, this one's zombie
     * is kept for it - see held_for_collecting in proc.c.
     */
    ag_pid_t          parent;
    volatile bool     signalled;
    jmp_buf           exit_jump;

    int   argc;
    char *argv[AG_PROC_ARGS_MAX];
    char  argbuf[AG_PROC_ARGBUF];

    char cwd[AG_PATH_MAX];
    /* Path for deferred AXE load inside proc_task; empty for builtins. */
    char path[AG_PATH_MAX];
    bool load_pending;

    void               *heap_mem;
    ag_port_heap_t heap;
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

    /*
     * Where THIS process's output goes, if not to the screen.
     *
     * A property of the process, deliberately, and set before its task runs.
     * The alternative - one global redirect on the console - was in place and
     * was wrong in a way that only showed up over a wire: a node running a
     * guest program redirected EVERYTHING into the link for as long as the
     * guest lived, so the board stopped answering its own user.  For a remote
     * run that lasted a second; for a migrated program, which never ends, it
     * lasted for ever.
     *
     * Set at spawn rather than afterwards because the loader logs from the new
     * task before spawn returns: anything installed after the fact has already
     * missed the lines that say why a load failed.
     */
    ag_con_sink_fn out_sink;
    void          *out_ctx;
} proc_t;

/* The process the calling task belongs to, or NULL for the kernel's own tasks. */
proc_t *ag_proc_current(void);

/* A process by pid, or NULL.  Call with the table locked. */
proc_t *ag_proc_by_pid(ag_pid_t pid);

/* The process table lock.  Recursive; held while a slot is examined or changed. */
void ag_proc_table_lock(void);
void ag_proc_table_unlock(void);

/*
 * Create the task behind a process or a thread.  Tries an internal-SRAM stack
 * first and PSRAM only if the port can place stacks and SRAM is short — same
 * self-delete semantics as ag_port_task_create, which is to say a task that
 * ends itself with ag_port_task_delete(NULL).
 */
bool ag_proc_task_create(ag_port_task_fn fn, const char *name,
                         uint32_t stack_bytes, void *arg, unsigned prio,
                         int core, ag_port_task_t *out);
/* The same with the stack in PSRAM first: for a task that lives as long as
 * the board and must not hold internal memory for it. */
bool ag_proc_task_create_slow(ag_port_task_fn fn, const char *name,
                              uint32_t stack_bytes, void *arg, unsigned prio,
                              int core, ag_port_task_t *out);

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
    ag_port_task_t  task;
    void        (*fn)(void *);
    void         *arg;
    volatile bool finished;
} ag_thread_rec_t;

/* True when this record is that task; how a thread finds its own process. */
bool ag_thread_owns(const void *record, ag_port_task_t task);

/*
 * A thread inside a NAMED process rather than the caller's.  For work asked for
 * from outside any process - a cluster thread the node next door wants started,
 * which arrives on the link's task.  Same rules and same limits as the ABI's
 * create(); see thread_create_in.
 */
ag_thread_t ag_thread_create_in(ag_pid_t pid, void (*fn)(void *), void *arg,
                                const char *name, size_t stack, int priority,
                                uint32_t flags);

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
/* Faults                                                                 */
/* ---------------------------------------------------------------------- */

/*
 * Catching one is the port's job (argon/port/fault.h) because it is the most
 * architecture-bound thing the system does.  What belongs here is the other two
 * halves of the arrangement: what to write down when it happens, and where the
 * faulting task lands afterwards.  The supervisor hands both to the port.
 */

/*
 * True while this task is inside the kernel holding a lock the rest of the system
 * needs.  A process in that state cannot be unwound or deleted: the lock would
 * stay held.
 */
bool ag_proc_task_in_kernel(ag_port_task_t task);

/*
 * Suspends another task where it stands and keeps it suspended only if it
 * holds none of those locks there: true means it may now be deleted, false
 * that it has been let go again.  One attempt; asking ag_proc_task_in_kernel
 * and then deleting is two steps with the task running between them on the
 * other core, which is how SPIN was deleted holding the journal (1 Oct 2026).
 */
bool ag_proc_task_stop_outside_kernel(ag_port_task_t task);

/*
 * Called from an exception context: writes down what happened and nothing else.
 * Returns false when this process cannot be recovered, in which case the caller
 * must let the fault take its normal course.
 */
bool ag_proc_note_fault(uint32_t cause, uint32_t pc, uint32_t vaddr, uint32_t sp,
                        uint32_t ra);

/*
 * Where a faulted process resumes, in ordinary task context: reports what
 * happened and ends the process.  Does not return.
 */
void ag_proc_fault_exit(void);

/*
 * A panic the port refused to recover - the kernel's own fault, one taken while
 * a kernel lock was held, a watchdog - copied out of the memory that survived
 * the reboot it caused, and put into the same crash record as everything else.
 * Called once, at boot, by the supervisor; the port clears its copy as it hands
 * it over.  There is no process to blame, which is why it arrives here rather
 * than through ag_proc_note_fault.
 */
void ag_proc_record_panic(const ag_port_panic_t *panic);

#endif /* ARGON_PROC_INTERNAL_H */
