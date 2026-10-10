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
 * Twenty-four: eight for drivers, as before - a module gets two tasks and the
 * machine does not carry four drivers doing real work at once - and the rest
 * for applications' tasks, watched for fairness (2 Oct 2026).  Taken from the
 * heap at start-up: the S3's static memory has no 700 bytes to spare.
 * Overflowing this is not an error: a task nobody is watching is a task that
 * behaves the way it always did.
 */
#define AG_BUDGET_SLOTS 24u

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
    bool                    fair;      /* an application's: next window by the last */
    uint8_t volatile        level;     /* a fair one's: 0 down, 1 ordinary, 2 up */
} slot_t;

static slot_t  *s_slots;
static uint32_t s_nslots; /* 0 until the table is made */

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
 * THE BOARD'S SPARE TIME (27 Sep 2026), for whatever may only use what is
 * left - a file relayed across the C3 beside a picture and sound, whose one
 * core it had filled to 98.5%.  Ticks that found the idle task, and all
 * ticks, over the last AG_IDLE_WINDOW_MS of both cores together.
 */
#define AG_IDLE_WINDOW_MS 20u
static ag_port_task_t    s_idle_task[2];
static uint32_t volatile s_idle_now[2], s_all_now[2];
static uint32_t volatile s_idle_last, s_all_last;
static uint32_t          s_idle_tick; /* ticks into the idle window, core 0 */

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
            for (uint32_t i = 0; i < s_nslots; i++) {
                if (s_slots[i].task != NULL) {
                    s_slots[i].last_ms = s_slots[i].used_ms;
                    s_slots[i].used_ms = 0;
                    s_slots[i].fresh = true;
                }
            }
        }
    }

    if (core >= 0 && core < 2) {
        s_all_now[core]++;
        if (running != NULL && running == s_idle_task[core]) {
            s_idle_now[core]++;
        }
        if (core == 0 && ++s_idle_tick >= AG_IDLE_WINDOW_MS) {
            s_idle_tick = 0;
            s_idle_last = s_idle_now[0] + s_idle_now[1];
            s_all_last = s_all_now[0] + s_all_now[1];
            s_idle_now[0] = s_idle_now[1] = 0;
            s_all_now[0] = s_all_now[1] = 0;
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
    for (uint32_t i = 0; i < s_nslots; i++) {
        if (s_slots[i].task != running) {
            continue;
        }
        s_slots[i].used_ms++;
        /* a fair one raised for being light loses the step the moment it is
         * not: the supervisor brings it down within the window */
        if (s_slots[i].fair && s_slots[i].level == 2u &&
            s_slots[i].used_ms > AG_BUDGET_LIGHT_MS && s_slots[i].over == 0u) {
            s_slots[i].over = s_slots[i].used_ms;
            return;
        }
        if (!s_slots[i].fair && s_slots[i].used_ms > s_slots[i].budget_ms &&
            s_slots[i].over == 0u) {
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
    s_slots = (slot_t *)ag_port_alloc(AG_BUDGET_SLOTS * sizeof(slot_t),
                                      AG_MEM_FAST | AG_MEM_BYTE);
    if (s_slots != NULL) {
        memset(s_slots, 0, AG_BUDGET_SLOTS * sizeof(slot_t));
        s_nslots = AG_BUDGET_SLOTS;
    }
    s_idle_task[0] = ag_port_task_idle_of(0);
    s_idle_task[1] = ag_port_task_idle_of(1);
    s_hooked = ag_port_tick_hook(budget_tick);
    if (!s_hooked) {
        ag_log(AG_LOG_WARN, "budget",
               "no tick hook: drivers keep whatever priority they ask for");
    }
}

static bool slot_take(ag_port_task_t task, const char *name, unsigned base_prio,
                      uint32_t budget_ms, bool fair);

bool ag_budget_watch(ag_port_task_t task, const char *name, unsigned base_prio,
                     uint32_t budget_ms)
{
    if (task == NULL || base_prio <= AG_BUDGET_WATCH_ABOVE) {
        return false; /* it cannot outrank an application, so cannot starve one */
    }

    return slot_take(task, name, base_prio, budget_ms, false);
}

static bool slot_take(ag_port_task_t task, const char *name, unsigned base_prio,
                      uint32_t budget_ms, bool fair)
{
    for (uint32_t i = 0; i < s_nslots; i++) {
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
        s->fair = fair;
        s->level = 1u;
        s->last_log = 0;
        s->task = task; /* last: this is what makes the slot live */
        return true;
    }

    ag_log(AG_LOG_WARN, "budget", "no slot for %s; it runs unwatched",
           (name != NULL) ? name : "a driver task");
    return false;
}

bool ag_budget_fair(ag_port_task_t task, const char *name)
{
    if (task == NULL || ag_budget_watched(task)) {
        return false;
    }
    return slot_take(task, name, AG_BUDGET_FAIR_PRIO, AG_BUDGET_FAIR_MS, true);
}

bool ag_budget_watched(ag_port_task_t task)
{
    for (uint32_t i = 0; task != NULL && i < s_nslots; i++) {
        if (s_slots[i].task == task) {
            return true;
        }
    }
    return false;
}

void ag_budget_forget(ag_port_task_t task)
{
    if (task == NULL) {
        return;
    }
    for (uint32_t i = 0; i < s_nslots; i++) {
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
    for (uint32_t i = 0; i < s_nslots; i++) {
        if (s_slots[i].task != NULL) {
            return true;
        }
    }
    return false;
}

bool ag_budget_service(void)
{
    bool changed = false;

    for (uint32_t i = 0; i < s_nslots; i++) {
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

        if (s->fair) {
            uint8_t want = s->level;
            if (s->fresh) {
                s->fresh = false;
                s->over = 0;
                want = (s->last_ms > s->budget_ms)         ? 0u
                       : (s->last_ms <= AG_BUDGET_LIGHT_MS) ? 2u
                                                           : 1u;
            } else if (s->over != 0u && s->level == 2u) {
                want = 1u; /* light no longer: back to the ordinary */
            }
            if (want != s->level) {
                s->level = want;
                s->demoted = (want != 1u);
                ag_port_task_prio_set(task, (want == 0u)   ? AG_BUDGET_FAIR_LOW
                                            : (want == 2u) ? AG_BUDGET_FAIR_HIGH
                                                           : s->base_prio);
                changed = true;
            }
            continue;
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

uint32_t ag_cpu_idle_percent(void)
{
    const uint32_t all = s_all_last;
    if (!s_hooked || all == 0u) {
        return 100u; /* not counted: nothing is held back on a guess */
    }
    return s_idle_last * 100u / all;
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
                      uint32_t *budget_ms, int *standing)
{
    uint32_t seen = 0;

    for (uint32_t i = 0; i < s_nslots; i++) {
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
        if (standing != NULL) {
            *standing = s_slots[i].fair ? (int)s_slots[i].level - 1
                                        : (s_slots[i].demoted ? -1 : 0);
        }
        return true;
    }
    return false;
}
