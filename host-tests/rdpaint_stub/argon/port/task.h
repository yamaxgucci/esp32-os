/*
 * The port's tasks, for the host test of net/rdpaint.c only (test_rdpaint.c):
 * a task is created and never run - the test makes the painter's passes
 * itself - and a delay is nothing.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef RDPAINT_STUB_TASK_H
#define RDPAINT_STUB_TASK_H

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t ag_port_ticks_t;
typedef void    *ag_port_task_t;
typedef void (*ag_port_task_fn)(void *arg);
#define AG_PORT_FOREVER  UINT32_MAX
#define AG_PORT_ANY_CORE (-1)

static inline ag_port_ticks_t ag_port_ms_to_ticks(uint32_t ms) { return ms; }
static inline void            ag_port_task_delay(ag_port_ticks_t t) { (void)t; }

static inline bool ag_port_task_create(ag_port_task_fn fn, const char *name,
                                       uint32_t stack, void *arg, unsigned prio,
                                       int core, unsigned caps,
                                       ag_port_task_t *out)
{
    (void)fn;
    (void)name;
    (void)stack;
    (void)arg;
    (void)prio;
    (void)core;
    (void)caps;
    static int s_task;
    *out = &s_task;
    return true;
}

#endif /* RDPAINT_STUB_TASK_H */
