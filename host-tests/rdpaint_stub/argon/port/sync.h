/*
 * The port's locks and wakes, for the host test of net/rdpaint.c only: one
 * thread, so a mutex is taken at once and a semaphore is a count.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef RDPAINT_STUB_SYNC_H
#define RDPAINT_STUB_SYNC_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <argon/port/task.h>

typedef struct rdp_stub_sem {
    uint32_t count;
} *ag_port_sem_t;
typedef struct rdp_stub_mutex {
    int held;
} *ag_port_mutex_t;

static inline ag_port_mutex_t ag_port_mutex_new(void)
{
    return (ag_port_mutex_t)calloc(1, sizeof(struct rdp_stub_mutex));
}

static inline bool ag_port_mutex_take(ag_port_mutex_t m, ag_port_ticks_t t)
{
    (void)t;
    m->held++;
    return true;
}

static inline bool ag_port_mutex_give(ag_port_mutex_t m)
{
    m->held--;
    return true;
}

static inline ag_port_sem_t ag_port_sem_new_binary(void)
{
    return (ag_port_sem_t)calloc(1, sizeof(struct rdp_stub_sem));
}

static inline bool ag_port_sem_take(ag_port_sem_t s, ag_port_ticks_t t)
{
    (void)t;
    if (s->count > 0u) {
        s->count--;
        return true;
    }
    return false;
}

static inline bool ag_port_sem_give(ag_port_sem_t s)
{
    s->count = 1u;
    return true;
}

#endif /* RDPAINT_STUB_SYNC_H */
