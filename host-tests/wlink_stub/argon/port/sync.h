/*
 * The port's locks and wakes, for the host test of net/wlink.c only
 * (test_wlink.c): one thread, so a mutex is taken at once and a semaphore is
 * a count that a take finds or does not - nothing ever waits.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WLINK_STUB_SYNC_H
#define WLINK_STUB_SYNC_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <argon/port/task.h>

typedef struct stub_sem {
    uint32_t count;
} *ag_port_sem_t;
typedef struct stub_mutex {
    int held;
} *ag_port_mutex_t;

static inline ag_port_mutex_t ag_port_mutex_new(void)
{
    return (ag_port_mutex_t)calloc(1, sizeof(struct stub_mutex));
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

static inline ag_port_sem_t ag_port_sem_new_counting(uint32_t max, uint32_t n)
{
    (void)max;
    ag_port_sem_t s = (ag_port_sem_t)calloc(1, sizeof(struct stub_sem));
    if (s != NULL) {
        s->count = n;
    }
    return s;
}

static inline ag_port_sem_t ag_port_sem_new_binary(void)
{
    return ag_port_sem_new_counting(1u, 0u);
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
    s->count++;
    return true;
}

#endif /* WLINK_STUB_SYNC_H */
