/*
 * ArgonOS - threads inside a process, and the objects they synchronise with.
 *
 * A thin layer over FreeRTOS, and thin on purpose: what an application gets is
 * the scheduler that is already there, not a runtime of our own.  What this adds
 * is ownership.  Every thread, mutex, semaphore and queue is written down in the
 * process that created it, so ending that process ends them too - which is the
 * difference between a system that can stop a misbehaving application and one
 * that can only reboot.
 *
 * Everything here refuses rather than leaks: a resource that cannot be recorded
 * is not handed out, because a resource nobody owns outlives the process and
 * there is no second chance to notice.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "proc/proc_internal.h"

#include <stdio.h>
#include <string.h>

#include <argon/kernel.h>
#include <argon/log.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/*
 * Bounds, and why these ones.  A thread costs a stack (internal SRAM first,
 * PSRAM fallback); four per process at the default 8 KB is still a sum worth
 * being deliberate about.  The stack is clamped for the same reason: an
 * application that asks for a megabyte of stack has made a mistake, and failing
 * at create() is a better place to find out.
 */
#define AG_THREAD_STACK_MIN (2u * 1024u)
#define AG_THREAD_STACK_MAX (32u * 1024u)
#define AG_THREAD_STACK_DEFAULT (8u * 1024u)
#define AG_THREAD_MAX_PER_PROC 4

/* Below the supervisor, always: a thread must not be able to outrank it. */
#define AG_THREAD_PRIO_MIN 1
#define AG_THREAD_PRIO_MAX 10

static uint32_t thread_count(const proc_t *p)
{
    return ag_reslist_count_of(&p->res, AG_RES_THREAD);
}

bool ag_thread_owns(const void *record, TaskHandle_t task)
{
    const ag_thread_rec_t *rec = (const ag_thread_rec_t *)record;
    return rec != NULL && rec->task == task;
}

void ag_thread_release(void *record)
{
    ag_thread_rec_t *rec = (ag_thread_rec_t *)record;

    if (rec == NULL) {
        return;
    }
    /*
     * A thread that has already run off its own end has deleted itself; its
     * handle is stale and must not be deleted twice.
     */
    if (!rec->finished && rec->task != NULL) {
        vTaskDelete(rec->task);
    }
    heap_caps_free(rec);
}

/*
 * Between the application's function and FreeRTOS, so that a thread which simply
 * returns is tidied up rather than falling off the end of a task - which
 * FreeRTOS treats as a fatal error.
 */
static void thread_entry(void *arg)
{
    ag_thread_rec_t *rec = (ag_thread_rec_t *)arg;

    if (rec->fn != NULL) {
        rec->fn(rec->arg);
    }

    /*
     * Marked before the task goes away, so that whoever joins or reclaims knows
     * not to delete a handle that no longer exists.  The record itself stays in
     * the process's resource list and is freed with it: freeing it here would
     * race with a joiner reading the flag.
     */
    rec->finished = true;
    vTaskDelete(NULL);
}

static ag_thread_t api_create(void (*fn)(void *), void *arg, const char *name,
                              size_t stack, int priority, uint32_t flags)
{
    proc_t *p = ag_proc_current();

    if (fn == NULL) {
        return NULL;
    }
    if (p == NULL) {
        /* The kernel does not create threads through the application's ABI. */
        return NULL;
    }

    ag_proc_table_lock();

    if (thread_count(p) >= AG_THREAD_MAX_PER_PROC) {
        ag_log(AG_LOG_WARN, "proc", "%s already has %u threads", p->name,
               (unsigned)AG_THREAD_MAX_PER_PROC);
        ag_proc_table_unlock();
        return NULL;
    }

    size_t want = (stack != 0) ? stack : AG_THREAD_STACK_DEFAULT;
    if (want < AG_THREAD_STACK_MIN) {
        want = AG_THREAD_STACK_MIN;
    }
    if (want > AG_THREAD_STACK_MAX) {
        want = AG_THREAD_STACK_MAX;
    }

    int prio = (priority != 0) ? priority : AG_THREAD_PRIO_MIN;
    if (prio < AG_THREAD_PRIO_MIN) {
        prio = AG_THREAD_PRIO_MIN;
    }
    if (prio > AG_THREAD_PRIO_MAX) {
        prio = AG_THREAD_PRIO_MAX;
    }

    /*
     * The record is in the kernel's internal heap, not the process arena: the
     * arena is released while these records are still being used to stop the
     * threads that were allocating from it.
     */
    ag_thread_rec_t *rec = (ag_thread_rec_t *)heap_caps_calloc(
        1, sizeof(*rec), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (rec == NULL) {
        ag_proc_table_unlock();
        return NULL;
    }
    rec->fn = fn;
    rec->arg = arg;

    /* Recorded before it exists: a thread that starts running before it is
     * written down is a thread the process might not own. */
    if (ag_reslist_add(&p->res, AG_RES_THREAD, rec, (uint32_t)want) != AG_OK) {
        ag_log(AG_LOG_WARN, "proc", "%s holds too many resources for a thread",
               p->name);
        heap_caps_free(rec);
        ag_proc_table_unlock();
        return NULL;
    }

    BaseType_t core;
    if (flags & AG_THREAD_ANY_CORE) {
        core = tskNO_AFFINITY;
    } else if (flags & AG_THREAD_SYS_CORE) {
        core = 0;
    } else {
        core = (BaseType_t)ag_sysinfo()->app_core;
    }

    char label[16];
    snprintf(label, sizeof(label), "%.11s/t", (name != NULL) ? name : p->name);

    TaskHandle_t task = NULL;
    if (ag_proc_task_create(thread_entry, label, (uint32_t)want, rec,
                            (UBaseType_t)prio, core, &task) != pdPASS) {
        (void)ag_reslist_remove(&p->res, AG_RES_THREAD, rec, NULL);
        heap_caps_free(rec);
        ag_proc_table_unlock();
        ag_log(AG_LOG_WARN, "proc", "%s: no memory for a %u byte thread stack",
               p->name, (unsigned)want);
        return NULL;
    }
    rec->task = task;

    ag_proc_table_unlock();
    return (ag_thread_t)rec;
}

/*
 * Marks the calling thread's record as done.  Every path by which a thread stops
 * running has to go through this before deleting itself, or the reclaim at the
 * end of the process will delete a task that no longer exists - which is not an
 * error FreeRTOS reports, it is a freed TCB used again, and the system stops
 * making sense silently.  That cost an evening.
 */
void ag_thread_mark_self_finished(void)
{
    proc_t *p = ag_proc_current();
    const TaskHandle_t me = xTaskGetCurrentTaskHandle();

    if (p == NULL) {
        return;
    }
    for (uint32_t i = 0; i < AG_PROC_RES_MAX; i++) {
        if (p->res_slots[i].type == AG_RES_THREAD &&
            ag_thread_owns(p->res_slots[i].ref, me)) {
            ((ag_thread_rec_t *)p->res_slots[i].ref)->finished = true;
            return;
        }
    }
}

static void api_exit(void)
{
    /*
     * Ending the calling thread.  The record stays in the process's list, marked
     * finished, and is freed when the process ends - the same path a thread that
     * simply returned takes.
     */
    ag_thread_mark_self_finished();
    vTaskDelete(NULL);
}

/*
 * Waiting for a thread to finish.  Polled rather than signalled: FreeRTOS has no
 * join, and a semaphore per thread would have to be taken by exactly one waiter
 * to work - whereas polling a flag is correct however many join it, and join is
 * not on anybody's fast path.
 */
static ag_err_t api_join(ag_thread_t t, uint32_t timeout_ms)
{
    ag_thread_rec_t *rec = (ag_thread_rec_t *)t;

    if (rec == NULL) {
        return -AG_EINVAL;
    }

    for (uint32_t waited = 0;; waited += 10) {
        if (rec->finished) {
            return AG_OK;
        }
        if (timeout_ms != UINT32_MAX && waited >= timeout_ms) {
            return -AG_ETIMEDOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void api_yield(void) { taskYIELD(); }

static void api_sleep_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

static ag_thread_t api_self(void)
{
    proc_t *p = ag_proc_current();
    const TaskHandle_t me = xTaskGetCurrentTaskHandle();

    if (p != NULL) {
        for (uint32_t i = 0; i < AG_PROC_RES_MAX; i++) {
            if (p->res_slots[i].type == AG_RES_THREAD &&
                ag_thread_owns(p->res_slots[i].ref, me)) {
                return (ag_thread_t)p->res_slots[i].ref;
            }
        }
    }
    return NULL; /* the process's main thread has no record of its own */
}

/* ---------------------------------------------------------------------- */
/* Synchronisation objects                                                */
/* ---------------------------------------------------------------------- */

/*
 * Each of these is recorded against the process, so that ending it takes them
 * with it.  An object that could not be recorded is deleted again and the call
 * fails: handing out something nobody owns is how a leak survives the process
 * that made it.
 */
static void *keep(ag_res_type_t type, void *object)
{
    proc_t *p = ag_proc_current();

    if (object == NULL) {
        return NULL;
    }
    if (p == NULL) {
        return object; /* the kernel's own objects are the kernel's business */
    }

    ag_proc_table_lock();
    const ag_err_t err = ag_reslist_add(&p->res, type, object, 0);
    ag_proc_table_unlock();

    if (err != AG_OK) {
        ag_log(AG_LOG_WARN, "proc", "%s holds %u resources already; no %s",
               p->name, (unsigned)ag_reslist_count(&p->res),
               ag_res_type_name(type));
        return NULL;
    }
    return object;
}

/* False when the process does not hold it, which is a double delete. */
static bool forget(ag_res_type_t type, void *object)
{
    proc_t *p = ag_proc_current();

    if (object == NULL) {
        return false;
    }
    if (p == NULL) {
        return true;
    }

    ag_proc_table_lock();
    const bool held = ag_reslist_remove(&p->res, type, object, NULL);
    ag_proc_table_unlock();

    if (!held) {
        ag_log(AG_LOG_WARN, "proc", "%s released a %s it does not own", p->name,
               ag_res_type_name(type));
    }
    return held;
}

static ag_mutex_t api_mutex_create(void)
{
    SemaphoreHandle_t m = xSemaphoreCreateMutex();
    void             *kept = keep(AG_RES_MUTEX, m);

    if (kept == NULL && m != NULL) {
        vSemaphoreDelete(m);
    }
    return (ag_mutex_t)kept;
}

static void api_mutex_delete(ag_mutex_t m)
{
    if (forget(AG_RES_MUTEX, m)) {
        vSemaphoreDelete((SemaphoreHandle_t)m);
    }
}

static bool api_mutex_lock(ag_mutex_t m, uint32_t timeout_ms)
{
    if (m == NULL) {
        return false;
    }
    const TickType_t ticks = (timeout_ms == UINT32_MAX)
                                 ? portMAX_DELAY
                                 : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake((SemaphoreHandle_t)m, ticks) == pdTRUE;
}

static void api_mutex_unlock(ag_mutex_t m)
{
    if (m != NULL) {
        (void)xSemaphoreGive((SemaphoreHandle_t)m);
    }
}

static ag_sem_t api_sem_create(uint32_t initial, uint32_t max)
{
    if (max == 0) {
        max = 1;
    }
    if (initial > max) {
        initial = max;
    }

    SemaphoreHandle_t s = xSemaphoreCreateCounting(max, initial);
    void             *kept = keep(AG_RES_SEM, s);

    if (kept == NULL && s != NULL) {
        vSemaphoreDelete(s);
    }
    return (ag_sem_t)kept;
}

static void api_sem_delete(ag_sem_t s)
{
    if (forget(AG_RES_SEM, s)) {
        vSemaphoreDelete((SemaphoreHandle_t)s);
    }
}

static bool api_sem_take(ag_sem_t s, uint32_t timeout_ms)
{
    if (s == NULL) {
        return false;
    }
    const TickType_t ticks = (timeout_ms == UINT32_MAX)
                                 ? portMAX_DELAY
                                 : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake((SemaphoreHandle_t)s, ticks) == pdTRUE;
}

static void api_sem_give(ag_sem_t s)
{
    if (s != NULL) {
        (void)xSemaphoreGive((SemaphoreHandle_t)s);
    }
}

static ag_queue_t api_queue_create(uint32_t items, size_t item_size)
{
    if (items == 0 || item_size == 0) {
        return NULL;
    }

    QueueHandle_t q = xQueueCreate(items, item_size);
    void         *kept = keep(AG_RES_QUEUE, q);

    if (kept == NULL && q != NULL) {
        vQueueDelete(q);
    }
    return (ag_queue_t)kept;
}

static void api_queue_delete(ag_queue_t q)
{
    if (forget(AG_RES_QUEUE, q)) {
        vQueueDelete((QueueHandle_t)q);
    }
}

static bool api_queue_send(ag_queue_t q, const void *item, uint32_t timeout_ms)
{
    if (q == NULL || item == NULL) {
        return false;
    }
    const TickType_t ticks = (timeout_ms == UINT32_MAX)
                                 ? portMAX_DELAY
                                 : pdMS_TO_TICKS(timeout_ms);
    return xQueueSend((QueueHandle_t)q, item, ticks) == pdTRUE;
}

static bool api_queue_recv(ag_queue_t q, void *item, uint32_t timeout_ms)
{
    if (q == NULL || item == NULL) {
        return false;
    }
    const TickType_t ticks = (timeout_ms == UINT32_MAX)
                                 ? portMAX_DELAY
                                 : pdMS_TO_TICKS(timeout_ms);
    return xQueueReceive((QueueHandle_t)q, item, ticks) == pdTRUE;
}

/*
 * "Disable preemption on the current core", which is what suspending the
 * scheduler does on this port - and unlike a spinlock it nests, so an
 * application that takes it twice gets what it expects instead of a panic.
 * Interrupts keep running: this is not a way to talk to hardware, it is a way to
 * finish a short piece of work without being switched out.
 */
static void api_critical_enter(void) { vTaskSuspendAll(); }
static void api_critical_exit(void) { (void)xTaskResumeAll(); }

const ag_task_api_t ag_task_api_table = {
    .size = sizeof(ag_task_api_t),
    .create = api_create,
    .exit = api_exit,
    .join = api_join,
    .yield = api_yield,
    .sleep_ms = api_sleep_ms,
    .self = api_self,
    .mutex_create = api_mutex_create,
    .mutex_delete = api_mutex_delete,
    .mutex_lock = api_mutex_lock,
    .mutex_unlock = api_mutex_unlock,
    .sem_create = api_sem_create,
    .sem_delete = api_sem_delete,
    .sem_take = api_sem_take,
    .sem_give = api_sem_give,
    .queue_create = api_queue_create,
    .queue_delete = api_queue_delete,
    .queue_send = api_queue_send,
    .queue_recv = api_queue_recv,
    .critical_enter = api_critical_enter,
    .critical_exit = api_critical_exit,
};
