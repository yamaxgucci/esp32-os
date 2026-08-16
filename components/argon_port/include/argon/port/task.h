/*
 * ArgonOS port contract - tasks, and the tick they are scheduled on.
 *
 * ArgonOS does not bring its own scheduler.  It asks the machine below for
 * pre-emptive tasks with priorities, and builds processes, threads and the
 * supervisor on top of them; a port that cannot pre-empt cannot run this
 * system, and saying so plainly here is more useful than a layer that pretends
 * otherwise.
 *
 * Time here is counted in ticks rather than milliseconds on purpose.  The
 * kernel takes deadlines by subtracting two tick readings, and converting to
 * milliseconds at the boundary would silently change what those deadlines mean
 * on a port with a different tick.  A tick is whatever the port's scheduler
 * runs on; ag_port_ms_to_ticks is the only place that knows the ratio.
 *
 * What a port must supply:
 *
 *   ag_port_task_t              opaque handle to a task
 *   ag_port_ticks_t             the scheduler's time unit
 *   ag_port_task_fn        void (*)(void *) - a task entry point
 *   AG_PORT_FOREVER        a timeout that never expires
 *   AG_PORT_ANY_CORE       as `core`: let the scheduler choose
 *   AG_PORT_TASK_STACK_CAPS  1 if the port can place a task stack by memory
 *                            capability, 0 if the caps argument is ignored
 *
 *   ag_port_ticks_t ag_port_ms_to_ticks(uint32_t ms)
 *   ag_port_ticks_t ag_port_ticks(void)
 *
 *   bool ag_port_task_create(ag_port_task_fn fn, const char *name,
 *                            uint32_t stack_bytes, void *arg, unsigned prio,
 *                            int core, unsigned stack_caps, ag_port_task_t *out)
 *   void ag_port_task_delete(ag_port_task_t t)      NULL means "this task"
 *   void ag_port_task_delay(ag_port_ticks_t ticks)
 *   ag_port_task_t ag_port_task_self(void)
 *   void ag_port_task_prio_set(ag_port_task_t t, unsigned prio)
 *   uint32_t ag_port_task_stack_unused(ag_port_task_t t)   NULL means "this task"
 *
 *   void ag_port_notify_give(ag_port_task_t t)
 *   void ag_port_notify_take(bool clear, ag_port_ticks_t ticks)
 *
 *   void ag_port_task_yield(void)
 *   void ag_port_sched_lock(void)
 *   void ag_port_sched_unlock(void)
 *
 * Notes that are contract, not advice:
 *
 * - `core` pins the task to a CPU.  A single-core port ignores it; it must not
 *   refuse.  The application owns core 1 where there is one, and that is the
 *   whole reason the argument exists.
 * - `stack_caps` is a mask from argon/port/mem.h.  Where
 *   AG_PORT_TASK_STACK_CAPS is 0 the kernel does not ask twice, so a port that
 *   ignores the mask must say 0 rather than accept and disregard it.
 * - ag_port_task_delete(NULL) does not return.
 * - ag_port_sched_lock stops the scheduler; it is not a critical section and
 *   does not disable interrupts.  It must nest or the kernel must not nest it -
 *   today the kernel does not.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_TASK_H
#define ARGON_PORT_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include <argon/port/mem.h>
#include <argon/port/impl/task.h>

#endif /* ARGON_PORT_TASK_H */
