/*
 * ArgonOS - processes.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "proc/proc_internal.h"

#include <setjmp.h>
#include <string.h>

#include <argon/console.h>
#include <argon/hostfs.h>
#include <argon/session.h>
#include <argon/audio.h>
#include <argon/display.h>
#include <argon/kernel.h>
#include <argon/lineedit.h>
#include <argon/loader.h>
#include <argon/power.h>
#include <argon/log.h>
#include <argon/path.h>
#include <argon/shell.h>
#include <argon/vfs.h>

#include <argon/port/config.h>
#include <argon/port/fault.h>
#include <argon/port/mem.h>
#include <argon/port/sync.h>
#include <argon/port/task.h>
#include <argon/port/time.h>

#include "dev/io.h"
#include "fs/storage.h"
#include "proc/supervisor.h"

/* Stack, in bytes, and the arena an application allocates from. */
#define AG_PROC_STACK_MIN (2u * 1024u)
#define AG_PROC_STACK_MAX (64u * 1024u)

/*
 * The floor for an image that names its own stack.  The load runs on the
 * process's own task - it opens a file, walks a filesystem and relocates - so
 * the number an application would be happy with is not necessarily one it can
 * be loaded on.
 */
#define AG_PROC_LOAD_STACK_MIN (8u * 1024u)
#ifndef CONFIG_ARGON_APP_STACK_KB
#define CONFIG_ARGON_APP_STACK_KB 16
#endif
#ifndef CONFIG_ARGON_APP_HEAP_KB
#define CONFIG_ARGON_APP_HEAP_KB 1024
#endif
#define AG_PROC_HEAP_MIN (16u * 1024u)

/* How long a process is given to leave the kernel before it is killed anyway. */
#define AG_PROC_KILL_GRACE_MS 500

static proc_t            s_procs[AG_PROC_MAX];
static ag_port_mutex_t        s_lock;
static ag_pid_t          s_next_pid = 1;
static ag_pid_t          s_foreground = AG_PID_KERNEL;

/* ---------------------------------------------------------------------- */

static void lock(void)
{
    if (s_lock != NULL) {
        ag_port_mutex_take_recursive(s_lock, AG_PORT_FOREVER);
    }
}

static void unlock(void)
{
    if (s_lock != NULL) {
        ag_port_mutex_give_recursive(s_lock);
    }
}

void ag_proc_table_lock(void) { lock(); }
void ag_proc_table_unlock(void) { unlock(); }

static uint32_t now_ms(void) { return (uint32_t)(ag_port_us() / 1000); }

/* Truncating copy: a name or a path from a file is never allowed to run over. */
static void set_string(char *dst, size_t len, const char *src)
{
    if (dst == NULL || len == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n > len - 1) {
        n = len - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/*
 * Which process the calling task belongs to, or NULL for the kernel's own tasks.
 * A scan rather than thread-local storage: there are four slots, the comparison
 * is a pointer, and it costs nothing to be independent of how many thread-local
 * slots ESP-IDF happens to reserve for itself.
 *
 * The main task of each process is checked first, because that is the common case
 * by a wide margin - most applications have no threads at all.  Only if none
 * matches are the threads looked through, which is the slower path and is paid
 * for only by applications that have some.
 */
static proc_t *current(void)
{
    const ag_port_task_t me = ag_port_task_self();

    for (uint32_t i = 0; i < AG_PROC_MAX; i++) {
        if (s_procs[i].used && s_procs[i].task == me) {
            return &s_procs[i];
        }
    }

    for (uint32_t i = 0; i < AG_PROC_MAX; i++) {
        proc_t *p = &s_procs[i];
        if (!p->used) {
            continue;
        }
        for (uint32_t j = 0; j < AG_PROC_RES_MAX; j++) {
            if (p->res_slots[j].type == AG_RES_THREAD &&
                ag_thread_owns(p->res_slots[j].ref, me)) {
                return p;
            }
        }
    }
    return NULL;
}

proc_t *ag_proc_current(void) { return current(); }

static proc_t *by_pid(ag_pid_t pid)
{
    if (pid == AG_PID_KERNEL) {
        return NULL;
    }
    for (uint32_t i = 0; i < AG_PROC_MAX; i++) {
        if (s_procs[i].used && s_procs[i].pid == pid) {
            return &s_procs[i];
        }
    }
    return NULL;
}

const char *ag_proc_state_name(ag_proc_state_t state)
{
    switch (state) {
    case AG_PS_LOADING:    return "loading";
    case AG_PS_RUNNING:    return "running";
    case AG_PS_BACKGROUND: return "background";
    case AG_PS_STOPPED:    return "stopped";
    case AG_PS_ZOMBIE:     return "finished";
    default:               return "unknown";
    }
}

/* ---------------------------------------------------------------------- */
/* The process arena                                                      */
/* ---------------------------------------------------------------------- */

/*
 * Each process allocates from a region of its own rather than from the system
 * heap.  Three things come out of that: killing it reclaims everything at once,
 * an application cannot fragment the kernel's heap, and the numbers an
 * application sees in ag_meminfo are about itself rather than about the machine.
 */
static ag_err_t heap_create(proc_t *p, uint32_t requested)
{
    const bool required = requested != 0;
    size_t     size = required ? requested
                               : (size_t)CONFIG_ARGON_APP_HEAP_KB * 1024u;

    /*
     * A default is a guess, and this one is a guess about a machine with PSRAM:
     * a megabyte out of eight is nothing, a megabyte out of a quarter of one is
     * the machine.  The halving below only reacts to an allocation that fails,
     * and taking 128 KB of the 232 KB a board without PSRAM has does not fail -
     * it succeeds, and then the image's own data has nowhere to go.  That is
     * how it presented: an application refused with -AG_ENOMEM while `mem`
     * showed plenty free.
     *
     * So the default is also capped at a quarter of the pool it comes out of.
     * On the S3 that quarter is megabytes and this line does nothing.  An image
     * that asked for a size is not touched: it gets what it asked for or it
     * does not start.
     */
    if (!required) {
        size_t pool = ag_port_mem_free(AG_MEM_SLOW);
        if (pool == 0) {
            pool = ag_port_mem_free(AG_MEM_FAST);
        }
        if (size > pool / 4u) {
            size = pool / 4u;
        }
    }

    if (size < AG_PROC_HEAP_MIN) {
        size = AG_PROC_HEAP_MIN;
    }

    for (;;) {
        void *mem = ag_port_alloc(size, AG_MEM_SLOW | AG_MEM_BYTE);
        if (mem == NULL) {
            mem = ag_port_alloc(size, AG_MEM_FAST | AG_MEM_BYTE);
        }
        if (mem != NULL) {
            p->heap = ag_port_heap_register(mem, size);
            if (p->heap == NULL) {
                ag_port_free(mem);
                return -AG_ENOMEM;
            }
            p->heap_mem = mem;
            p->heap_size = size;
            return AG_OK;
        }

        /*
         * An image that asked for a size needs that size: refusing to start is
         * better than starting something that will fail halfway through.  One
         * that took the default gets whatever is available instead.
         */
        if (required || size <= AG_PROC_HEAP_MIN) {
            break;
        }
        size /= 2;
        if (size < AG_PROC_HEAP_MIN) {
            size = AG_PROC_HEAP_MIN;
        }
    }

    if (required) {
        ag_log(AG_LOG_ERROR, "proc", "%s asked for a %u KB arena; there is none",
               p->name, (unsigned)(requested / 1024u));
        return -AG_ENOMEM;
    }

    ag_log(AG_LOG_WARN, "proc",
           "%s starts with no arena: every allocation it makes will fail",
           p->name);
    return AG_OK;
}

static bool in_heap(const proc_t *p, const void *ptr)
{
    if (p == NULL || p->heap_mem == NULL || ptr == NULL) {
        return false;
    }
    return (const uint8_t *)ptr >= (const uint8_t *)p->heap_mem &&
           (const uint8_t *)ptr < (const uint8_t *)p->heap_mem + p->heap_size;
}

/* ---------------------------------------------------------------------- */
/* Ending a process                                                       */
/* ---------------------------------------------------------------------- */

static void release_resource(ag_res_type_t type, void *ref, uint32_t extra,
                             void *ctx)
{
    proc_t *p = (proc_t *)ctx;
    (void)extra;

    switch (type) {
    case AG_RES_MEM:
        /* Memory outside the arena: fast, DMA or executable. */
        ag_port_free(ref);
        break;
    case AG_RES_FILE:
        (void)ag_vfs_close((ag_handle_t)(intptr_t)ref);
        break;
    case AG_RES_THREAD:
        ag_thread_release(ref);
        break;
    case AG_RES_MUTEX:
    case AG_RES_SEM:
        ag_port_sem_free((ag_port_sem_t)ref);
        break;
    case AG_RES_QUEUE:
        ag_port_queue_free((ag_queue_t)ref);
        break;
    default:
        ag_log(AG_LOG_WARN, "proc", "%s: no way to release a %s yet", p->name,
               ag_res_type_name(type));
        break;
    }
}

/*
 * Gives everything back and frees the slot.  Called with the table locked, and
 * only once the process's task is gone - there is no such thing as reclaiming
 * the memory of something that is still running in it.
 */
static void reap(proc_t *p)
{
    const uint32_t held = ag_reslist_reclaim(&p->res, release_resource, p);
    const uint32_t files = ag_vfs_close_owned_by(p->pid);
    /*
     * Pins last, and never skipped.  An interrupt handler belonging to code
     * that is about to be freed is not a leak - it is a board that stops at the
     * next edge on that pin, with nothing in the journal to say why.
     */
    const uint32_t pins = ag_io_reclaim(p->pid);

    /*
     * And whatever it asked of the clock.  A hold left behind by a process
     * that is gone would pin the machine at full speed with nothing to show
     * for it, and no way to find out whose hold it was.
     */
    ag_power_forget(p->pid);

    if (held > 0 || files > 0 || pins > 0) {
        ag_log(AG_LOG_INFO, "proc",
               "%s (pid %u) left %u resource(s), %u open file(s) and %u pin(s); "
               "reclaimed",
               p->name, (unsigned)p->pid, (unsigned)held, (unsigned)files,
               (unsigned)pins);
    }

    /* Graphics: only release if this pid still owns the display. */
    if (ag_display_owner() == p->pid) {
        ag_gfx_api_table.release();
    }
    if (ag_audio_opened()) {
        ag_audio_api_table.close();
    }

    const bool restore_tty = (ag_session_focused_pid() == p->pid);
    ag_session_unbind(p->pid);
    if (restore_tty) {
        ag_console_restore_tty();
    }

    ag_loader_unload(&p->app);

    if (p->heap_mem != NULL) {
        ag_port_free(p->heap_mem);
    }
    if (p->done != NULL) {
        ag_port_sem_free(p->done);
    }
    if (s_foreground == p->pid) {
        /* Back to whoever had it, if they are still there. */
        s_foreground = (by_pid(p->prev_foreground) != NULL) ? p->prev_foreground
                                                           : AG_PID_KERNEL;
    }

    memset(p, 0, sizeof(*p));
}

/* Whatever the application did to the console, the next thing to print gets it
 * back in a known state.  Done here as well as in the shell, because a killed
 * process never reaches the shell's tidying up — and neither does a normal
 * `run` exit (background spawn, supervisor reaps). */
static void console_restore(void)
{
    ag_console_restore_tty();
}

/*
 * True while this task is inside the kernel holding a lock the rest of the system
 * needs.  Asked of FreeRTOS rather than tracked with a flag set on every
 * syscall: it costs nothing per call, and the answer is the truth rather than a
 * flag that some path forgot to set.
 */
static bool holds_kernel_lock(ag_port_task_t task)
{
    if (task == NULL) {
        return false;
    }
    return ag_console_lock_holder() == (void *)task ||
           ag_storage_vfs_lock_holder() == (void *)task ||
           ag_hostfs_rpc_holder() == (void *)task;
}

bool ag_proc_task_in_kernel(ag_port_task_t task)
{
    return holds_kernel_lock(task);
}

/*
 * Written down in an exception context, so: no locks, no logging, no allocation.
 * Answers whether this fault belongs to an application that can be unwound.
 */
bool ag_proc_note_fault(uint32_t cause, uint32_t pc, uint32_t vaddr, uint32_t sp)
{
    proc_t *p = current();

    if (p == NULL) {
        return false; /* the kernel's own fault: not ours to paper over */
    }
    if (p->fault.pending) {
        return false; /* faulting while being unwound: let it go to the panic */
    }
    if (holds_kernel_lock(ag_port_task_self())) {
        return false; /* unwinding would leave a kernel lock held forever */
    }

    p->fault.pending = true;
    p->fault.cause = cause;
    p->fault.pc = pc;
    p->fault.vaddr = vaddr;
    p->fault.sp = sp;
    return true;
}

/*
 * The last crash, formatted and waiting to be written to disk by the supervisor.
 * One slot: a second crash before the first is written overwrites it, which is
 * the right trade - the journal has both, and a queue of crash records is a
 * feature nobody needs on a machine that has just lost a process.
 */
#define AG_CRASH_TEXT_MAX 512
static char s_crash_text[AG_CRASH_TEXT_MAX];
static bool s_crash_waiting;

/* Appends to the pending record, never overrunning it. */
static void crash_printf(const char *fmt, ...)
{
    const size_t at = strlen(s_crash_text);
    if (at + 1 >= sizeof(s_crash_text)) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(&s_crash_text[at], sizeof(s_crash_text) - at, fmt, ap);
    va_end(ap);
}

bool ag_proc_take_crash_record(char *out, size_t len)
{
    if (out == NULL || len == 0) {
        return false;
    }

    lock();
    const bool waiting = s_crash_waiting;
    if (waiting) {
        set_string(out, len, s_crash_text);
        s_crash_text[0] = '\0';
        s_crash_waiting = false;
    }
    unlock();
    return waiting;
}

static void crash_record(proc_t *p, const char *reason)
{
    const uint32_t up_ms = now_ms() - (uint32_t)(p->started / 1000);
    size_t         used = 0;

    if (p->heap != NULL) {
        ag_port_heap_info_t info;
        ag_port_heap_info(p->heap, &info);
        used = info.allocated;
    }

    ag_log(AG_LOG_ERROR, "proc", "%s (pid %u) killed: %s", p->name,
           (unsigned)p->pid, (reason != NULL) ? reason : "no reason given");
    ag_log(AG_LOG_ERROR, "proc",
           "  %s for %u ms, %u B of a %u KB arena in use, %u resource(s) held, "
           "%u file(s) open system-wide",
           ag_proc_state_name(p->state), (unsigned)up_ms, (unsigned)used,
           (unsigned)(p->heap_size / 1024u), (unsigned)ag_reslist_count(&p->res),
           (unsigned)ag_vfs_open_count());

    /* The same thing again, for the file the supervisor will write. */
    s_crash_text[0] = '\0';
    crash_printf("%s (pid %u) killed at %u ms uptime: %s\n", p->name,
                 (unsigned)p->pid, (unsigned)now_ms(),
                 (reason != NULL) ? reason : "no reason given");
    crash_printf("  %s for %u ms, %u B of a %u KB arena, %u resource(s), "
                 "%u B code at %p\n",
                 ag_proc_state_name(p->state), (unsigned)up_ms, (unsigned)used,
                 (unsigned)(p->heap_size / 1024u),
                 (unsigned)ag_reslist_count(&p->res),
                 (unsigned)p->app.header.code.size, p->app.place.code);
    s_crash_waiting = true;

    /*
     * A process whose thread faulted is reported twice on the way down - once by
     * the thread, once by the kill that follows - and the second copy of the same
     * addresses is noise.
     */
    if (!p->fault.pending || p->fault.reported) {
        return;
    }
    p->fault.reported = true;

    /*
     * Where the fault was, said in terms the author of the application can act
     * on: an offset into its own code is a line they can find, and an address
     * outside it says the program had already gone somewhere it should not.
     */
    const uintptr_t code = (uintptr_t)p->app.place.code;
    const uintptr_t code_end = code + p->app.header.code.size;

    ag_log(AG_LOG_ERROR, "proc", "  %s at pc %08x, address %08x, sp %08x",
           ag_port_fault_cause_name(p->fault.cause), (unsigned)p->fault.pc,
           (unsigned)p->fault.vaddr, (unsigned)p->fault.sp);

    crash_printf("  %s at pc %08x, address %08x, sp %08x\n",
                 ag_port_fault_cause_name(p->fault.cause), (unsigned)p->fault.pc,
                 (unsigned)p->fault.vaddr, (unsigned)p->fault.sp);

    if (p->fault.pc >= code && p->fault.pc < code_end) {
        ag_log(AG_LOG_ERROR, "proc",
               "  pc is offset 0x%x in %s's own code (%u bytes at %08x)",
               (unsigned)(p->fault.pc - code), p->name,
               (unsigned)p->app.header.code.size, (unsigned)code);
        crash_printf("  pc is offset 0x%x in its own code\n",
                     (unsigned)(p->fault.pc - code));
    } else {
        ag_log(AG_LOG_ERROR, "proc",
               "  pc is outside %s's code, which starts at %08x", p->name,
               (unsigned)code);
        crash_printf("  pc is outside its own code\n");
    }
}

/* ---------------------------------------------------------------------- */
/* Starting a process                                                     */
/* ---------------------------------------------------------------------- */

bool ag_proc_task_create(ag_port_task_fn fn, const char *name,
                         uint32_t stack_bytes, void *arg, unsigned prio,
                         int core, ag_port_task_t *out)
{
#if AG_PORT_TASK_STACK_CAPS
    /*
     * Internal SRAM first, PSRAM only if there is not enough of it.  Which of
     * the two it landed in is worth a line in the log: a task whose stack is in
     * PSRAM cannot run while the flash cache is disabled, so if that ever
     * becomes a problem the evidence is already in the journal.
     */
    if (ag_port_task_create(fn, name, stack_bytes, arg, prio, core,
                            AG_MEM_FAST | AG_MEM_BYTE, out)) {
        return true;
    }
    if (ag_port_task_create(fn, name, stack_bytes, arg, prio, core,
                            AG_MEM_SLOW | AG_MEM_BYTE, out)) {
        ag_log(AG_LOG_INFO, "proc",
               "%s: task stack in PSRAM (%u bytes; internal SRAM was short)",
               name != NULL ? name : "?", (unsigned)stack_bytes);
        return true;
    }
    return false;
#else
    return ag_port_task_create(fn, name, stack_bytes, arg, prio, core, 0, out);
#endif
}

static uint32_t clamp_stack_bytes(uint32_t want)
{
    if (want == 0) {
        want = (uint32_t)CONFIG_ARGON_APP_STACK_KB * 1024u;
    }
    if (want < AG_PROC_STACK_MIN) {
        want = AG_PROC_STACK_MIN;
    }
    if (want > AG_PROC_STACK_MAX) {
        want = AG_PROC_STACK_MAX;
    }
    return want;
}

static uint32_t stack_bytes_for(const ag_axe_header_t *header)
{
    return clamp_stack_bytes(header->stack_size);
}

/* Scheduler priorities, all below AG_SUP_PRIORITY (15). */
static unsigned prio_for(uint8_t sched_class)
{
    switch (sched_class) {
    case AG_PRIO_LOW:
        return 2u;
    case AG_PRIO_HIGH:
        return 8u;
    case AG_PRIO_NORMAL:
    default:
        return 5u;
    }
}

static void apply_task_priority(proc_t *p)
{
    if (p == NULL || p->task == NULL) {
        return;
    }
    ag_port_task_prio_set(p->task, prio_for(p->sched_class));
}

static void name_from_path(char *dst, size_t dst_len, const char *path)
{
    const char *base = path;
    if (path != NULL) {
        for (const char *s = path; *s != '\0'; s++) {
            if (*s == '/' || *s == '\\' || *s == ':') {
                base = s + 1;
            }
        }
    }
    set_string(dst, dst_len, (base != NULL && base[0] != '\0') ? base : "app");
}

const char *ag_proc_prio_name(ag_proc_prio_t prio)
{
    switch (prio) {
    case AG_PRIO_LOW:
        return "low";
    case AG_PRIO_HIGH:
        return "high";
    case AG_PRIO_NORMAL:
    default:
        return "normal";
    }
}

ag_err_t ag_proc_get_priority(ag_pid_t pid, ag_proc_prio_t *out)
{
    if (out == NULL) {
        return -AG_EINVAL;
    }
    lock();
    proc_t *p = by_pid(pid);
    if (p == NULL) {
        unlock();
        return -AG_ENOENT;
    }
    *out = (ag_proc_prio_t)p->sched_class;
    unlock();
    return AG_OK;
}

ag_err_t ag_proc_set_priority(ag_pid_t pid, ag_proc_prio_t prio)
{
    if (prio > AG_PRIO_HIGH) {
        return -AG_EINVAL;
    }
    lock();
    proc_t *p = by_pid(pid);
    if (p == NULL) {
        unlock();
        return -AG_ENOENT;
    }
    p->sched_class = (uint8_t)prio;
    apply_task_priority(p);
    unlock();
    return AG_OK;
}

/*
 * Finish AXE load on the process task.  On failure the task becomes a zombie
 * with a negative exit_code; the slot is reaped by wait/reap_finished.
 */
static ag_err_t proc_finish_load(proc_t *p)
{
    /*
     * What is left before the image is read.
     *
     * The stack and the session's screens are already taken by the time this
     * runs, and on a small machine that is most of the difference between what
     * `mem` shows at a prompt and what a load actually has to work with.  It was
     * fifty kilobytes here, and looking for it in the wrong place cost an hour.
     */
    ag_log(AG_LOG_INFO, "proc", "%s: loading with %u free, largest %u",
           p->name, (unsigned)ag_port_mem_free(AG_MEM_FAST | AG_MEM_BYTE),
           (unsigned)ag_port_mem_largest(AG_MEM_FAST | AG_MEM_BYTE));

    ag_err_t err = ag_loader_load(p->path, p->cwd, &p->app);
    if (err != AG_OK) {
        ag_log(AG_LOG_ERROR, "proc", "pid %u: load %s failed (%d)",
               (unsigned)p->pid, p->path, (int)err);
        return err;
    }

    if ((p->app.header.flags & AG_AXE_DRIVER) != 0) {
        ag_log(AG_LOG_ERROR, "proc", "%s: is a driver; use 'drv load'", p->path);
        ag_loader_unload(&p->app);
        return -AG_EINVAL;
    }
    if ((p->app.header.flags & AG_AXE_NEEDS_GFX) != 0 && !ag_display_ready()) {
        ag_log(AG_LOG_ERROR, "proc", "%s: needs a display", p->path);
        ag_loader_unload(&p->app);
        return -AG_ENODEV;
    }
    if ((p->app.header.flags & AG_AXE_NEEDS_NET) != 0 &&
        ag_loader_api()->net == NULL) {
        ag_log(AG_LOG_ERROR, "proc", "%s: needs networking", p->path);
        ag_loader_unload(&p->app);
        return -AG_ENODEV;
    }
    if ((p->app.header.flags & AG_AXE_NEEDS_AUDIO) != 0 &&
        (ag_loader_api()->audio == NULL ||
         ag_loader_api()->audio->present == NULL ||
         !ag_loader_api()->audio->present())) {
        ag_log(AG_LOG_ERROR, "proc", "%s: needs audio", p->path);
        ag_loader_unload(&p->app);
        return -AG_ENODEV;
    }

    if (p->app.header.name[0] != '\0') {
        set_string(p->name, sizeof(p->name), p->app.header.name);
        const int slot = ag_session_slot_of(p->pid);
        if (slot >= 0) {
            (void)ag_session_bind_to(p->pid, p->name, slot);
        }
    }

    if ((p->app.header.flags & AG_AXE_NEEDS_AUDIO) != 0) {
        p->sched_class = (uint8_t)AG_PRIO_HIGH;
    }

    err = heap_create(p, p->app.header.heap_size);
    if (err != AG_OK) {
        ag_loader_unload(&p->app);
        return err;
    }

    /* Stack was sized at create; header may ask for more — warn only. */
    const uint32_t want = stack_bytes_for(&p->app.header);
    if (want > p->stack_bytes) {
        ag_log(AG_LOG_WARN, "proc",
               "%s: header wants %u stack bytes; running with %u", p->name,
               (unsigned)want, (unsigned)p->stack_bytes);
    }

    return AG_OK;
}

/*
 * The arguments are copied into the process, because the caller's are on its
 * stack or in its line buffer and a background process outlives both.
 */
static ag_err_t copy_args(proc_t *p, int argc, char **argv)
{
    size_t at = 0;

    p->argc = 0;
    if (argc > AG_PROC_ARGS_MAX) {
        argc = AG_PROC_ARGS_MAX;
    }

    for (int i = 0; i < argc; i++) {
        const char  *src = (argv != NULL) ? argv[i] : NULL;
        const size_t len = (src != NULL) ? strlen(src) : 0;

        if (at + len + 1 > sizeof(p->argbuf)) {
            return -AG_ERANGE;
        }
        memcpy(&p->argbuf[at], (src != NULL) ? src : "", len);
        p->argbuf[at + len] = '\0';
        p->argv[i] = &p->argbuf[at];
        at += len + 1;
        p->argc++;
    }
    return AG_OK;
}

static void proc_task(void *arg)
{
    proc_t *p = (proc_t *)arg;

    /*
     * Taken here rather than from xTaskCreate so that it is certainly set before
     * any of the application runs: exit() and the current-process lookup both
     * compare against it.
     */
    p->task = ag_port_task_self();
    p->heartbeat_ms = now_ms();
    apply_task_priority(p);

    if (p->load_pending) {
        const ag_err_t load_err = proc_finish_load(p);
        p->load_pending = false;
        if (load_err != AG_OK) {
            p->exit_code = (int32_t)load_err;
            p->state = AG_PS_ZOMBIE;
            ag_log(AG_LOG_ERROR, "proc", "%s (pid %u) failed to load: %d",
                   p->name, (unsigned)p->pid, (int)load_err);
            ag_port_sem_t done_fail = p->done;
            ag_port_sem_give(done_fail);
            ag_port_task_delete(NULL);
            return;
        }
        apply_task_priority(p);
    }

    /*
     * BACKGROUND at spawn means "do not steal focus at create".  If the user
     * already focused this slot while the image was loading, we are foreground
     * now and must not stay marked background.
     */
    const bool background = (p->flags & (uint32_t)AG_SPAWN_BACKGROUND) != 0;
    if (s_foreground == p->pid) {
        p->state = AG_PS_RUNNING;
    } else {
        p->state = background ? AG_PS_BACKGROUND : AG_PS_RUNNING;
    }

    typedef int (*entry_fn)(int, char **);
    const entry_fn entry = (entry_fn)p->app.binding.entry;
    if (entry == NULL) {
        p->exit_code = (int32_t)-AG_EINVAL;
        p->state = AG_PS_ZOMBIE;
        ag_port_sem_t done_bad = p->done;
        ag_port_sem_give(done_bad);
        ag_port_task_delete(NULL);
        return;
    }

    if (setjmp(p->exit_jump) == 0) {
        p->exit_code = (int32_t)entry(p->argc, p->argv);
    }
    /* else: came back through ag_proc_exit, which set exit_code already */

    p->stack_unused = (uint32_t)ag_port_task_stack_unused(NULL);
    p->state = AG_PS_ZOMBIE;

    ag_log(AG_LOG_INFO, "proc", "%s (pid %u) returned %d, %u stack bytes unused",
           p->name, (unsigned)p->pid, (int)p->exit_code,
           (unsigned)p->stack_unused);

    /* Nothing may touch the process after this: whoever waits for it owns it. */
    ag_port_sem_t done = p->done;
    ag_port_sem_give(done);
    ag_port_task_delete(NULL);
}

static proc_t *free_slot(void)
{
    for (uint32_t i = 0; i < AG_PROC_MAX; i++) {
        if (!s_procs[i].used) {
            return &s_procs[i];
        }
    }
    return NULL;
}

/*
 * Common path: reserve a slot, create the task, return immediately.
 * AXE images set load_pending; builtins set binding.entry and heap already.
 */
static ag_err_t spawn_common(proc_t *p, uint32_t flags, ag_pid_t *out_pid)
{
    p->done = ag_port_sem_new_binary();
    if (p->done == NULL) {
        return -AG_ENOMEM;
    }

    const bool background = (flags & (uint32_t)AG_SPAWN_BACKGROUND) != 0;
    p->state = AG_PS_LOADING;
    p->prev_foreground = s_foreground;
    if (!background) {
        s_foreground = p->pid;
    }

    /*
     * App core for interactive starts; system core for background.  Priority
     * comes from sched_class (applied inside proc_task); create uses normal.
     */
    const int      core = background ? 0 : (int)ag_sysinfo()->app_core;
    const unsigned prio = prio_for(p->sched_class);

    if (!ag_proc_task_create(proc_task, p->name, p->stack_bytes, p, prio, core,
                             NULL)) {
        /*
         * Tried internal SRAM, then PSRAM (when enabled).  Failure here means
         * both are exhausted or the TCB (always internal) could not be taken.
         */
        ag_log(AG_LOG_ERROR, "proc",
               "%s: no memory for a %u byte task stack "
               "(internal SRAM and PSRAM both failed)",
               p->name, (unsigned)p->stack_bytes);
        if (s_foreground == p->pid) {
            s_foreground = p->prev_foreground;
        }
        ag_port_sem_free(p->done);
        p->done = NULL;
        return -AG_ENOMEM;
    }

    if (out_pid != NULL) {
        *out_pid = p->pid;
    }

    const ag_pid_t bound_pid = p->pid;
    char           bound_name[32];
    const bool     skip_bind = (flags & (uint32_t)AG_SPAWN_NO_SESSION) != 0;
    set_string(bound_name, sizeof(bound_name), p->name);
    unlock();
    if (!skip_bind) {
        const ag_err_t bind_err = ag_session_bind(bound_pid, bound_name);
        if (bind_err != AG_OK) {
            ag_log(AG_LOG_WARN, "proc", "pid %u: no free session slot (%d)",
                   (unsigned)bound_pid, (int)bind_err);
        }
    }
    return AG_OK;
}

ag_err_t ag_proc_spawn(const char *path, int argc, char **argv, uint32_t flags,
                       ag_pid_t *out_pid)
{
    if (path == NULL) {
        return -AG_EINVAL;
    }

    lock();

    proc_t *p = free_slot();
    if (p == NULL) {
        unlock();
        ag_log(AG_LOG_ERROR, "proc", "all %u process slots are in use",
               (unsigned)AG_PROC_MAX);
        return -AG_EBUSY;
    }

    memset(p, 0, sizeof(*p));
    p->used = true;
    p->pid = s_next_pid++;
    p->flags = flags;
    p->started = (ag_time_t)ag_port_us();
    p->sched_class = (uint8_t)AG_PRIO_NORMAL;
    p->load_pending = true;
    ag_reslist_init(&p->res, p->res_slots, AG_PROC_RES_MAX);

    const char *parent_cwd = ag_proc_cwd();
    set_string(p->cwd, sizeof(p->cwd), parent_cwd);
    set_string(p->path, sizeof(p->path), path);
    name_from_path(p->name, sizeof(p->name), path);

    ag_err_t err = copy_args(p, argc, argv);
    if (err != AG_OK) {
        memset(p, 0, sizeof(*p));
        unlock();
        return err;
    }

    /*
     * What the image asks for, read before the task exists.
     *
     * A stack cannot be resized once a task is running on it, so the number has
     * to come from somewhere before then, and the only honest source is the
     * image.  A failure to read it here is not fatal - the default is a working
     * answer and the real load will report the same problem properly - so the
     * header is a hint at this point and nothing more.
     */
    {
        ag_axe_header_t peek;
        uint32_t        want = 0;
        if (ag_loader_peek(p->path, p->cwd, &peek) == AG_OK) {
            want = peek.stack_size;
        }
        /*
         * Never below what loading itself needs: the image is read on this very
         * task, through the filesystem, and an image that asks for two
         * kilobytes would overflow before its first instruction ran.
         */
        if (want != 0 && want < AG_PROC_LOAD_STACK_MIN) {
            want = AG_PROC_LOAD_STACK_MIN;
        }
        p->stack_bytes = clamp_stack_bytes(want);
    }

    err = spawn_common(p, flags, out_pid);
    if (err != AG_OK) {
        memset(p, 0, sizeof(*p));
        unlock();
        return err;
    }
    /* spawn_common unlocked on success */
    return AG_OK;
}

ag_err_t ag_proc_spawn_builtin(const char *name, ag_proc_entry_fn entry,
                               int argc, char **argv, uint32_t flags,
                               uint32_t stack_bytes, uint32_t heap_bytes,
                               ag_pid_t *out_pid)
{
    if (name == NULL || entry == NULL) {
        return -AG_EINVAL;
    }

    lock();

    proc_t *p = free_slot();
    if (p == NULL) {
        unlock();
        ag_log(AG_LOG_ERROR, "proc", "all %u process slots are in use",
               (unsigned)AG_PROC_MAX);
        return -AG_EBUSY;
    }

    memset(p, 0, sizeof(*p));
    p->used = true;
    p->pid = s_next_pid++;
    p->flags = flags;
    p->started = (ag_time_t)ag_port_us();
    p->sched_class = (uint8_t)AG_PRIO_NORMAL;
    p->load_pending = false;
    p->app.binding.entry = (void *)entry;
    set_string(p->name, sizeof(p->name), name);
    ag_reslist_init(&p->res, p->res_slots, AG_PROC_RES_MAX);

    /*
     * The system says this one is fit for any power mode, on its behalf.
     *
     * A built-in is the file manager, the editor, the shell's own tools: code
     * in this tree, text on a screen, no deadline anywhere in it.  Left to the
     * ordinary rule - answer or be ended - `power eco` would kill the file
     * manager somebody is standing in front of, and teaching each of these
     * loops to answer a question whose answer is always yes would be four
     * copies of the same three lines.  Where the system wrote the program, the
     * system can vouch for it.
     */
    (void)ag_power_declare(p->pid, AG_POWER_FIT_ANY, "system tool");

    const char *parent_cwd = ag_proc_cwd();
    set_string(p->cwd, sizeof(p->cwd), parent_cwd);

    ag_err_t err = copy_args(p, argc, argv);
    if (err != AG_OK) {
        memset(p, 0, sizeof(*p));
        unlock();
        return err;
    }

    p->stack_bytes = clamp_stack_bytes(stack_bytes);
    err = heap_create(p, heap_bytes);
    if (err != AG_OK) {
        memset(p, 0, sizeof(*p));
        unlock();
        return err;
    }

    err = spawn_common(p, flags, out_pid);
    if (err != AG_OK) {
        if (p->heap_mem != NULL) {
            ag_port_free(p->heap_mem);
        }
        memset(p, 0, sizeof(*p));
        unlock();
        return err;
    }
    return AG_OK;
}

ag_err_t ag_proc_exec(const char *path, int argc, char **argv, uint32_t flags,
                      int32_t *exit_code)
{
    ag_pid_t pid = 0;
    ag_err_t err = ag_proc_spawn(path, argc, argv,
                                 flags & ~(uint32_t)AG_SPAWN_BACKGROUND, &pid);
    if (err != AG_OK) {
        return err;
    }

    err = ag_proc_wait(pid, exit_code, UINT32_MAX);
    console_restore();
    return err;
}

/* ---------------------------------------------------------------------- */

ag_err_t ag_proc_wait(ag_pid_t pid, int32_t *exit_code, uint32_t timeout_ms)
{
    lock();
    proc_t *p = by_pid(pid);
    if (p == NULL) {
        unlock();
        return -AG_ENOENT;
    }
    ag_port_sem_t done = p->done;
    /*
     * Said out loud, because the supervisor collects what nobody is waiting for
     * and must not take this token out from under us: a stolen token is a wait
     * that never ends.
     */
    p->has_waiter = true;
    unlock();

    if (done == NULL) {
        return -AG_EINVAL;
    }

    const ag_port_ticks_t ticks = (timeout_ms == UINT32_MAX)
                                 ? AG_PORT_FOREVER
                                 : ag_port_ms_to_ticks(timeout_ms);
    if (!ag_port_sem_take(done, ticks)) {
        lock();
        p = by_pid(pid);
        if (p != NULL) {
            p->has_waiter = false;
        }
        unlock();
        return -AG_ETIMEDOUT;
    }

    lock();
    /* Found again by pid: the pointer could have been reaped while we waited. */
    p = by_pid(pid);
    if (p == NULL) {
        unlock();
        return -AG_ENOENT;
    }
    if (exit_code != NULL) {
        *exit_code = p->exit_code;
    }
    const bool was_killed = p->killed;
    reap(p);
    unlock();

    return was_killed ? -AG_EKILLED : AG_OK;
}

ag_err_t ag_proc_signal(ag_pid_t pid)
{
    lock();
    proc_t *p = by_pid(pid);
    if (p == NULL) {
        unlock();
        return -AG_ENOENT;
    }
    p->signalled = true;
    ag_log(AG_LOG_INFO, "proc", "%s (pid %u) asked to stop", p->name,
           (unsigned)p->pid);
    unlock();
    return AG_OK;
}

ag_err_t ag_proc_kill(ag_pid_t pid, const char *reason)
{
    lock();
    proc_t *p = by_pid(pid);
    if (p == NULL) {
        unlock();
        return -AG_ENOENT;
    }

    /* Already finished: there is nothing to kill, only something to collect. */
    if (p->state == AG_PS_ZOMBIE) {
        unlock();
        return AG_OK;
    }

    const ag_port_task_t task = p->task;
    p->killed = true;
    p->signalled = true; /* ask first, in case it is listening */
    unlock();

    /*
     * Wait for it to be outside the kernel.  A task deleted while it holds a
     * kernel lock leaves that lock held forever, and the two locks that matter -
     * the console and the filesystem - are how anything else gets done.
     */
    for (uint32_t waited = 0; waited < AG_PROC_KILL_GRACE_MS; waited += 10) {
        if (!holds_kernel_lock(task)) {
            break;
        }
        ag_port_task_delay(ag_port_ms_to_ticks(10));
    }

    lock();
    p = by_pid(pid);
    if (p == NULL) {
        unlock();
        return AG_OK; /* it finished by itself while we waited */
    }
    if (p->state == AG_PS_ZOMBIE) {
        unlock();
        return AG_OK;
    }

    if (holds_kernel_lock(task)) {
        /*
         * Refusing rather than pretending.  Killing it here would trade a hung
         * application for a hung system, and saying so gives the operator
         * something to act on.
         */
        ag_log(AG_LOG_ERROR, "proc",
               "%s (pid %u) is inside the kernel holding a lock; not killed - "
               "the system would hang. Reboot to recover.",
               p->name, (unsigned)p->pid);
        unlock();
        return -AG_EBUSY;
    }

    crash_record(p, reason);

    ag_port_task_delete(task);
    p->task = NULL;
    p->exit_code = -AG_EKILLED;
    p->state = AG_PS_ZOMBIE;

    /*
     * Whoever is waiting for it owns the collecting, so it is woken; with nobody
     * waiting, the killer does it here and now, so that ps right after a kill
     * shows the truth rather than a corpse the supervisor has not swept up yet.
     */
    ag_port_sem_t done = p->done;
    const bool        waited_for = p->has_waiter;
    if (!waited_for) {
        reap(p);
    }
    unlock();

    console_restore();

    if (waited_for && done != NULL) {
        ag_port_sem_give(done);
    }
    return AG_OK;
}

uint32_t ag_proc_reap_finished(void)
{
    uint32_t reaped = 0;

    lock();
    for (uint32_t i = 0; i < AG_PROC_MAX; i++) {
        proc_t *p = &s_procs[i];
        if (!p->used || p->state != AG_PS_ZOMBIE || p->has_waiter) {
            continue;
        }
        reap(p);
        reaped++;
    }
    unlock();
    return reaped;
}

/* ---------------------------------------------------------------------- */

ag_pid_t ag_proc_self(void)
{
    const proc_t *p = current();
    return (p != NULL) ? p->pid : AG_PID_KERNEL;
}

ag_pid_t ag_proc_foreground(void) { return s_foreground; }

ag_err_t ag_proc_set_foreground(ag_pid_t pid)
{
    if (pid == AG_PID_KERNEL) {
        s_foreground = AG_PID_KERNEL;
        return AG_OK;
    }

    lock();
    proc_t *p = by_pid(pid);
    if (p == NULL) {
        unlock();
        return -AG_ENOENT;
    }
    s_foreground = pid;
    if (p->state == AG_PS_BACKGROUND) {
        p->state = AG_PS_RUNNING;
    }
    unlock();
    return AG_OK;
}

uint32_t ag_proc_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < AG_PROC_MAX; i++) {
        if (s_procs[i].used) {
            n++;
        }
    }
    return n;
}

ag_err_t ag_proc_state_of(ag_pid_t pid, ag_proc_state_t *out)
{
    if (out == NULL) {
        return -AG_EINVAL;
    }
    lock();
    const proc_t *p = by_pid(pid);
    if (p == NULL) {
        unlock();
        return -AG_ENOENT;
    }
    *out = p->state;
    unlock();
    return AG_OK;
}

ag_err_t ag_proc_info(uint32_t index, ag_procinfo_t *out)
{
    if (out == NULL) {
        return -AG_EINVAL;
    }

    lock();
    uint32_t seen = 0;
    for (uint32_t i = 0; i < AG_PROC_MAX; i++) {
        const proc_t *p = &s_procs[i];
        if (!p->used) {
            continue;
        }
        if (seen++ != index) {
            continue;
        }

        memset(out, 0, sizeof(*out));
        out->pid = p->pid;
        set_string(out->name, sizeof(out->name), p->name);
        out->state = p->state;
        out->foreground = (p->pid == s_foreground);
        out->started = p->started;
        out->cpu_permille = 0; /* needs run-time stats, which are off */

        /*
         * What the system has given it, not what it happens to be using: the
         * arena is reserved for this process whether it allocates from it or
         * not, and this number is meant to answer "how much comes back if it
         * ends".
         */
        out->mem_used = p->app.header.code.size + p->app.header.data.size +
                        p->stack_bytes + p->heap_size +
                        ag_reslist_total_of(&p->res, AG_RES_MEM);
        out->priority = p->sched_class;

        unlock();
        return AG_OK;
    }
    unlock();
    return -AG_ENOENT;
}

/* ---------------------------------------------------------------------- */
/* What the syscall table forwards here                                   */
/* ---------------------------------------------------------------------- */

void *ag_proc_alloc(size_t bytes, uint32_t caps)
{
    if (bytes == 0) {
        return NULL;
    }

    proc_t *p = current();

    /*
     * Fast, DMA-capable and executable memory cannot come from the arena, which
     * is ordinary extended memory.  Those go to the system heap and are written
     * down one by one - and if there is no room to write one down, the
     * allocation is refused rather than leaked past the end of the process.
     */
    const uint32_t special = AG_MEM_FAST | AG_MEM_DMA | AG_MEM_EXEC;
    if (p == NULL || (caps & special) != 0) {
        uint32_t want = AG_MEM_BYTE;
        if (caps & AG_MEM_FAST) {
            want |= AG_MEM_FAST;
        }
        if (caps & AG_MEM_DMA) {
            want |= AG_MEM_DMA;
        }
        if (caps & AG_MEM_EXEC) {
            want |= AG_MEM_EXEC;
        }
        if ((caps & special) == 0) {
            want |= AG_MEM_SLOW;
        }

        void *ptr = ag_port_alloc(bytes, want);
        if (ptr == NULL && (want & AG_MEM_SLOW)) {
            ptr = ag_port_alloc(bytes, AG_MEM_FAST | AG_MEM_BYTE);
        }
        if (ptr == NULL) {
            return NULL;
        }

        if (p != NULL) {
            if (ag_reslist_add(&p->res, AG_RES_MEM, ptr, (uint32_t)bytes) !=
                AG_OK) {
                ag_log(AG_LOG_WARN, "proc",
                       "%s holds %u resources already; allocation refused",
                       p->name, (unsigned)ag_reslist_count(&p->res));
                ag_port_free(ptr);
                return NULL;
            }
        }
        if (caps & AG_MEM_ZERO) {
            memset(ptr, 0, bytes);
        }
        return ptr;
    }

    if (p->heap == NULL) {
        return NULL;
    }

    void *ptr = ag_port_heap_alloc(p->heap, bytes);
    if (ptr != NULL && (caps & AG_MEM_ZERO)) {
        memset(ptr, 0, bytes);
    }
    return ptr;
}

void *ag_proc_realloc(void *ptr, size_t bytes)
{
    proc_t *p = current();

    if (ptr == NULL) {
        return ag_proc_alloc(bytes, 0);
    }
    if (p == NULL) {
        return ag_port_realloc(ptr, bytes, AG_MEM_BYTE);
    }

    if (in_heap(p, ptr)) {
        return ag_port_heap_realloc(p->heap, ptr, bytes);
    }

    uint32_t old = 0;
    if (ag_reslist_remove(&p->res, AG_RES_MEM, ptr, &old)) {
        void *moved = ag_port_realloc(ptr, bytes, AG_MEM_BYTE);
        /* Whatever happened, what the process holds now has to be recorded. */
        if (ag_reslist_add(&p->res, AG_RES_MEM, (moved != NULL) ? moved : ptr,
                           (moved != NULL) ? (uint32_t)bytes : old) != AG_OK) {
            /* The slot it was just in cannot have been taken meanwhile. */
            ag_log(AG_LOG_ERROR, "proc", "%s: lost track of %p", p->name, moved);
        }
        return moved;
    }

    ag_log(AG_LOG_WARN, "proc", "%s tried to resize %p, which it does not own",
           p->name, ptr);
    return NULL;
}

void ag_proc_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    proc_t *p = current();
    if (p == NULL) {
        ag_port_free(ptr);
        return;
    }

    if (in_heap(p, ptr)) {
        ag_port_heap_free(p->heap, ptr);
        return;
    }
    if (ag_reslist_remove(&p->res, AG_RES_MEM, ptr, NULL)) {
        ag_port_free(ptr);
        return;
    }

    /*
     * A pointer the process does not hold is either a double free or somebody
     * else's memory.  Freeing it would corrupt a heap the application does not
     * own; saying so is the whole point of keeping the list.
     */
    ag_log(AG_LOG_WARN, "proc", "%s freed %p, which it does not own", p->name,
           ptr);
}

size_t ag_proc_usable_size(const void *ptr)
{
    if (ptr == NULL) {
        return 0;
    }

    proc_t *p = current();
    if (p != NULL && in_heap(p, ptr)) {
        return ag_port_heap_alloc_size(p->heap, (void *)ptr);
    }
    return ag_port_alloc_size((void *)ptr);
}

void ag_proc_meminfo(ag_meminfo_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    const proc_t *p = current();

    if (p != NULL && p->heap != NULL) {
        ag_port_heap_info_t info;
        ag_port_heap_info(p->heap, &info);
        out->arena_total = p->heap_size;
        out->arena_free = info.free_bytes;
        out->arena_largest = info.largest_free;
    } else {
        /* The kernel's own view: there is no process arena to report. */
        out->arena_total = ag_port_mem_total(AG_MEM_SLOW);
        out->arena_free = ag_port_mem_free(AG_MEM_SLOW);
        out->arena_largest = ag_port_mem_largest(AG_MEM_SLOW);
    }

    out->fast_free = ag_port_mem_free(AG_MEM_FAST);
    out->system_free = out->fast_free;
}

const char *ag_proc_cwd(void)
{
    const proc_t *p = current();
    return (p != NULL) ? p->cwd : ag_shell_cwd();
}

ag_err_t ag_proc_set_cwd(const char *absolute_path)
{
    if (absolute_path == NULL) {
        return -AG_EINVAL;
    }

    proc_t *p = current();
    if (p == NULL) {
        return ag_shell_set_cwd(absolute_path);
    }
    if (strlen(absolute_path) + 1 > sizeof(p->cwd)) {
        return -AG_ERANGE;
    }
    set_string(p->cwd, sizeof(p->cwd), absolute_path);
    return AG_OK;
}

void ag_proc_exit(int code)
{
    proc_t *p = current();

    if (p != NULL) {
        p->exit_code = (int32_t)code;
        longjmp(p->exit_jump, 1);
    }
    /* Called with nothing running: there is nowhere to go, so ignore it. */
}

/*
 * Where a faulted application resumes - ordinary task context again, on its own
 * stack, so locking and logging are allowed here and this is where all of it
 * happens.
 */
void ag_proc_fault_exit(void)
{
    proc_t *p = current();

    if (p == NULL) {
        /* Cannot happen: the handler only diverts application tasks. */
        ag_port_task_delete(NULL);
        return;
    }

    crash_record(p, "faulted");
    p->killed = true;
    p->exit_code = -AG_EKILLED;

    if (ag_port_task_self() == p->task) {
        /* The main task can unwind to where it was started from. */
        longjmp(p->exit_jump, 1);
    }

    /*
     * A thread cannot: exit_jump belongs to another task's stack, and jumping
     * onto it would land on a frame that is still in use.  So the thread ends
     * here and the supervisor takes the process down - a fault in one thread is
     * a fault in the process, since they share everything.
     *
     * Marked finished first: the reclaim that follows deletes threads that are
     * still marked running, and this one is about to stop existing.
     */
    ag_thread_mark_self_finished();
    ag_supervisor_kill_request(p->pid);
    ag_port_task_delete(NULL);
}

bool ag_proc_interrupted(void)
{
    proc_t *p = current();

    if (p == NULL || !p->signalled) {
        return false;
    }
    p->signalled = false;
    return true;
}

bool ag_proc_stopping(void)
{
    proc_t *p = current();
    if (p == NULL) {
        return false;
    }
    return p->signalled || p->killed;
}

bool ag_proc_focused(void)
{
    proc_t *p = current();
    if (p == NULL) {
        return ag_session_shell_owns_keyboard();
    }
    return ag_session_focused_pid() == p->pid;
}

void ag_proc_post_focus_event(ag_pid_t pid, bool gained)
{
    lock();
    proc_t *p = by_pid(pid);
    if (p != NULL) {
        p->focus_ev = gained ? 1u : 2u;
    }
    unlock();
}

bool ag_proc_take_focus_event(ag_event_t *out)
{
    if (out == NULL) {
        return false;
    }
    proc_t *p = current();
    if (p == NULL || p->focus_ev == 0) {
        return false;
    }
    const uint8_t kind = p->focus_ev;
    p->focus_ev = 0;
    memset(out, 0, sizeof(*out));
    out->type = (kind == 1u) ? AG_EV_FOCUS_GAINED : AG_EV_FOCUS_LOST;
    out->ts = (ag_time_t)ag_port_us();
    return true;
}

void ag_proc_heartbeat(void)
{
    proc_t *p = current();
    if (p != NULL) {
        p->heartbeat_ms = now_ms();
    }
}

void ag_proc_watchdog(uint32_t ms)
{
    proc_t *p = current();

    if (p == NULL) {
        return;
    }
    /* Armed from now, not from whenever the last heartbeat happened to be. */
    p->heartbeat_ms = now_ms();
    p->watchdog_ms = ms;

    if (ms != 0) {
        ag_log(AG_LOG_INFO, "proc", "%s (pid %u) promises a heartbeat every %u ms",
               p->name, (unsigned)p->pid, (unsigned)ms);
    }
}

ag_pid_t ag_proc_overdue(uint32_t *late_by_ms)
{
    const uint32_t now = now_ms();
    ag_pid_t       found = AG_PID_KERNEL;
    uint32_t       late = 0;

    lock();
    for (uint32_t i = 0; i < AG_PROC_MAX; i++) {
        const proc_t *p = &s_procs[i];

        if (!p->used || p->watchdog_ms == 0) {
            continue;
        }
        if (p->state != AG_PS_RUNNING && p->state != AG_PS_BACKGROUND) {
            continue;
        }
        const uint32_t since = now - p->heartbeat_ms;
        if (since > p->watchdog_ms) {
            found = p->pid;
            late = since - p->watchdog_ms;
            break;
        }
    }
    unlock();

    if (late_by_ms != NULL) {
        *late_by_ms = late;
    }
    return found;
}

/* ---------------------------------------------------------------------- */

ag_err_t ag_proc_init(void)
{
    if (s_lock != NULL) {
        return AG_OK;
    }

    s_lock = ag_port_mutex_new_recursive();
    if (s_lock == NULL) {
        return -AG_ENOMEM;
    }

    memset(s_procs, 0, sizeof(s_procs));
    s_foreground = AG_PID_KERNEL;

    /* From now on the VFS knows whose files it is holding. */
    ag_vfs_set_owner_fn(ag_proc_self);
    return AG_OK;
}
