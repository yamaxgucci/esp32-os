/*
 * ArgonOS port: bare metal - tasks.
 *
 * The largest single piece of work on this list, and the one to be honest
 * about: ArgonOS does not bring a scheduler.  It needs pre-emptive tasks with
 * priorities, and on a machine with no RTOS that means writing one - context
 * switch, tick, ready queues, priority inheritance in the mutex below.
 *
 * This is where "drop ESP-IDF" stops being a refactor.  Everything else on this
 * list is a driver; this is a kernel.  It is perfectly possible - it is a few
 * thousand lines for two cores on Xtensa - but it is not an afternoon, and the
 * shape of the contract in argon/port/task.h is deliberately the smallest set
 * that ArgonOS actually uses, so that the scheduler underneath can be simple.
 *
 * An alternative worth considering before writing one: FreeRTOS itself is not
 * ESP-IDF.  The kernel proper is a handful of files with a portable core and a
 * per-architecture port layer, and using it here without the rest of ESP-IDF is
 * a legitimate answer to this file.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_TASK_H
#define ARGON_PORT_IMPL_TASK_H

#error "bare: no scheduler yet.  See argon/port/task.h for the contract."

#endif /* ARGON_PORT_IMPL_TASK_H */
