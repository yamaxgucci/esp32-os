/*
 * ArgonOS - execution budgets: the accounting, and the punishment.
 *
 * The tick charges, the supervisor punishes.  They are separate because a
 * priority cannot be changed from an interrupt, and they must be: the whole
 * value of charging on the tick is that it costs a table walk of a few entries
 * and nothing else.
 *
 * The table is written from two places and read from a third, and there is no
 * lock in any of them.  That is deliberate and it is safe here:
 *
 *   - `task` is what makes an entry live.  It is published last when an entry
 *     is filled and cleared first when one is dropped, so a tick that lands in
 *     the middle sees either an empty slot or a complete one.
 *   - `used_ms` is only ever touched by the tick, and a task runs on one core
 *     at a time, so the two cores cannot be charging the same entry at once.
 *   - the flags are set by the tick and cleared by the supervisor, one writer
 *     each way; a flag seen a tick late costs a millisecond of accuracy.
 *
 * A spinlock here would be correct and would also be taken a thousand times a
 * second on both cores to protect arithmetic that cannot tear.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/budget.h>

#include <string.h>

#include <argon/log.h>

#include <argon/port/mem.h>
#include <argon/port/time.h>


/*
 * Eight, because a module gets two tasks and the machine does not carry four
 * drivers doing real work at once.  Overflowing this is not an error: a task
 * nobody is watching is a task that behaves the way it always did.
 */
#define AG_BUDGET_SLOTS 8u

/* Above this and a task can starve an ordinary application, so above this is
 * what gets watched.  It is AG_PRIO_NORMAL's number in proc.c. */
#define AG_BUDGET_WATCH_ABOVE 5u

typedef struct {
    ag_port_task_t volatile task; /* NULL means the slot is free           */
    char                    name[12];
    unsigned                base_prio;
    uint32_t                budget_ms;
    uint32_t volatile       used_ms;    /* this window, charged by the tick */
    uint32_t                last_ms;    /* the window that just closed      */
    uint64_t                last_log;   /* so a repeat offender is not a flood */
    uint32_t volatile       over;       /* the tick says: it has overrun    */
    bool volatile           fresh;      /* the tick says: a new window      */
    bool                    demoted;
} slot_t;

static slot_t   s_slots[AG_BUDGET_SLOTS];

/*
 * `cpu`: every task that was running when a tick landed, counted, per core.
 * A table per core so that each is written by one core only and a slot is
 * never claimed by both at once; a task that runs on both cores has a line in
 * each.  Handles only - a name cannot be read here (see budget_tick) - and
 * matched to names afterwards, in task context.
 */
#define AG_CPU_SLOTS 40u

typedef struct {
    ag_port_task_t volatile task[AG_CPU_SLOTS];
    uint32_t volatile       ticks[AG_CPU_SLOTS];
    uint32_t volatile       total;
    uint32_t volatile       lost; /* a tick the table had no room for */
} cpu_table_t;

/* On the heap, the first time `cpu` is asked: 650 bytes of static memory
 * were more than the S3's window had left (27 Sep 2026). */
static cpu_table_t *volatile s_cpu;
static volatile bool         s_cpu_on;
static uint32_t s_window_tick; /* ticks into the current window            */
static bool     s_hooked;

/*
 * Interrupt context, both cores, every millisecond - and in IRAM, calling
 * nothing at all.
 *
 * The first version of this rang the supervisor's doorbell the moment it found
 * an overrun, which is one function call and cost the board its first boot:
 * the tick also fires while a flash write has the instruction cache off, and a
 * handler that reaches into flash then dies with "Cache disabled but cached
 * memory region accessed".  So it only marks, and somebody in task context
 * comes round to look.
 */
static void AG_PORT_IRAM budget_tick(int core, ag_port_task_t running)
{
    (void)core;

    /*
     * The window is counted on core 0 alone.  Both cores tick, and a window
     * that advanced twice as fast on a dual-core machine would give every
     * driver half the budget it was promised.
     */
    if (core == 0) {
        if (++s_window_tick >= AG_BUDGET_WINDOW_MS) {
            s_window_tick = 0;
            for (uint32_t i = 0; i < AG_BUDGET_SLOTS; i++) {
                if (s_slots[i].task != NULL) {
                    s_slots[i].last_ms = s_slots[i].used_ms;
                    s_slots[i].used_ms = 0;
                    s_slots[i].fresh = true;
                }
            }
        }
    }

    if (s_cpu_on && s_cpu != NULL && core >= 0 && core < 2) {
        cpu_table_t *const c = &s_cpu[core];
        c->total++;
        uint32_t i = 0;
        while (i < AG_CPU_SLOTS && c->task[i] != running &&
               c->task[i] != NULL) {
            i++;
        }
        if (i == AG_CPU_SLOTS) {
            c->lost++;
        } else {
            c->task[i] = running;
            c->ticks[i]++;
        }
    }

    if (running == NULL) {
        return;
    }
    for (uint32_t i = 0; i < AG_BUDGET_SLOTS; i++) {
        if (s_slots[i].task != running) {
            continue;
        }
        s_slots[i].used_ms++;
        if (s_slots[i].used_ms > s_slots[i].budget_ms && s_slots[i].over == 0u) {
            /* The figure is kept here rather than read later: by the time the
             * supervisor looks, the window may have turned and the counter
             * gone back to zero, which is how the first version of this came
             * to report "used 0 ms of its 20 ms". */
            s_slots[i].over = s_slots[i].used_ms;
        }
        return;
    }
}

void ag_budget_init(void)
{
    if (s_hooked) {
        return;
    }
    s_hooked = ag_port_tick_hook(budget_tick);
    if (!s_hooked) {
        ag_log(AG_LOG_WARN, "budget",
               "no tick hook: drivers keep whatever priority they ask for");
    }
}

void ag_budget_watch(ag_port_task_t task, const char *name, unsigned base_prio,
                     uint32_t budget_ms)
{
    if (task == NULL || base_prio <= AG_BUDGET_WATCH_ABOVE) {
        return; /* it cannot outrank an application, so it cannot starve one */
    }

    for (uint32_t i = 0; i < AG_BUDGET_SLOTS; i++) {
        if (s_slots[i].task != NULL) {
            continue;
        }
        slot_t *s = &s_slots[i];
        memset(s->name, 0, sizeof(s->name));
        if (name != NULL) {
            size_t n = strlen(name);
            if (n >= sizeof(s->name)) {
                n = sizeof(s->name) - 1u;
            }
            memcpy(s->name, name, n);
        }
        s->base_prio = base_prio;
        s->budget_ms = (budget_ms != 0u) ? budget_ms : AG_BUDGET_DEFAULT_MS;
        s->used_ms = 0;
        s->last_ms = 0;
        s->over = 0;
        s->fresh = false;
        s->demoted = false;
        s->last_log = 0;
        s->task = task; /* last: this is what makes the slot live */
        return;
    }

    ag_log(AG_LOG_WARN, "budget", "no slot for %s; it runs unwatched",
           (name != NULL) ? name : "a driver task");
}

void ag_budget_forget(ag_port_task_t task)
{
    if (task == NULL) {
        return;
    }
    for (uint32_t i = 0; i < AG_BUDGET_SLOTS; i++) {
        if (s_slots[i].task == task) {
            s_slots[i].task = NULL; /* first: this is what makes it dead */
            s_slots[i].over = 0;
            s_slots[i].fresh = false;
            s_slots[i].demoted = false;
            return;
        }
    }
}

bool ag_budget_watching(void)
{
    for (uint32_t i = 0; i < AG_BUDGET_SLOTS; i++) {
        if (s_slots[i].task != NULL) {
            return true;
        }
    }
    return false;
}

bool ag_budget_service(void)
{
    bool changed = false;

    for (uint32_t i = 0; i < AG_BUDGET_SLOTS; i++) {
        slot_t *s = &s_slots[i];
        ag_port_task_t task = s->task;
        if (task == NULL) {
            continue;
        }

        if (s->over != 0u && !s->demoted) {
            const uint32_t spent = s->over;
            s->demoted = true;
            ag_port_task_prio_set(task, AG_BUDGET_DEMOTED_PRIO);
            changed = true;
            /*
             * At most once a second per task.  A driver that overruns every
             * window overruns ten times a second, and ten identical lines a
             * second is not a diagnosis, it is a denial of the journal to
             * everything else that might have something to say.
             */
            const uint64_t now = (uint64_t)ag_port_us();
            if (s->last_log == 0ull || now - s->last_log >= 1000000ull) {
                s->last_log = now;
                ag_log(AG_LOG_WARN, "budget",
                       "%s wants %u ms of every %u and is allowed %u; held "
                       "below applications for the rest of each window",
                       s->name, (unsigned)spent,
                       (unsigned)AG_BUDGET_WINDOW_MS, (unsigned)s->budget_ms);
            }
        }

        if (s->fresh) {
            s->fresh = false;
            s->over = 0;
            if (s->demoted) {
                s->demoted = false;
                ag_port_task_prio_set(task, s->base_prio);
                changed = true;
            }
        }
    }

    return changed;
}

void ag_cpu_sample(bool on)
{
    if (on) {
        s_cpu_on = false;
        if (s_cpu == NULL) {
            s_cpu = (cpu_table_t *)ag_port_alloc(2u * sizeof(cpu_table_t),
                                                 AG_MEM_FAST);
            if (s_cpu == NULL) {
                return;
            }
        }
        memset((void *)s_cpu, 0, 2u * sizeof(cpu_table_t));
    }
    s_cpu_on = on && s_cpu != NULL;
}

bool ag_cpu_sampled(int core, uint32_t index, ag_port_task_t *task,
                    uint32_t *ticks)
{
    if (s_cpu == NULL || core < 0 || core >= 2 || index >= AG_CPU_SLOTS ||
        s_cpu[core].task[index] == NULL) {
        return false;
    }
    *task = s_cpu[core].task[index];
    *ticks = s_cpu[core].ticks[index];
    return true;
}

uint32_t ag_cpu_sampled_total(int core, uint32_t *lost)
{
    if (s_cpu == NULL || core < 0 || core >= 2) {
        return 0;
    }
    if (lost != NULL) {
        *lost = s_cpu[core].lost;
    }
    return s_cpu[core].total;
}

bool ag_budget_report(uint32_t index, const char **name, uint32_t *used_ms,
                      uint32_t *budget_ms, bool *demoted)
{
    uint32_t seen = 0;

    for (uint32_t i = 0; i < AG_BUDGET_SLOTS; i++) {
        if (s_slots[i].task == NULL) {
            continue;
        }
        if (seen++ != index) {
            continue;
        }
        if (name != NULL) {
            *name = s_slots[i].name;
        }
        if (used_ms != NULL) {
            *used_ms = s_slots[i].last_ms;
        }
        if (budget_ms != NULL) {
            *budget_ms = s_slots[i].budget_ms;
        }
        if (demoted != NULL) {
            *demoted = s_slots[i].demoted;
        }
        return true;
    }
    return false;
}
