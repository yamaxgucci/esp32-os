/*
 * ArgonOS port: ESP-IDF - mutexes, semaphores and queues.
 *
 * FreeRTOS makes all three the same handle, so on this port the three typedefs
 * are one type and the compiler will not catch a semaphore passed to a mutex
 * call.  They are still three names, because the kernel says which it means and
 * a port whose objects really are different needs the distinction to exist in
 * the contract rather than to be added to it later.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_SYNC_H
#define ARGON_PORT_IMPL_SYNC_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

typedef SemaphoreHandle_t ag_port_mutex_t;
typedef SemaphoreHandle_t ag_port_sem_t;
typedef QueueHandle_t     ag_port_queue_t;

static inline ag_port_mutex_t ag_port_mutex_new(void)
{
    return xSemaphoreCreateMutex();
}

static inline ag_port_mutex_t ag_port_mutex_new_recursive(void)
{
    return xSemaphoreCreateRecursiveMutex();
}

static inline bool ag_port_mutex_take(ag_port_mutex_t m, ag_port_ticks_t ticks)
{
    return xSemaphoreTake(m, ticks) == pdTRUE;
}

static inline bool ag_port_mutex_give(ag_port_mutex_t m)
{
    return xSemaphoreGive(m) == pdTRUE;
}

static inline bool ag_port_mutex_take_recursive(ag_port_mutex_t m, ag_port_ticks_t ticks)
{
    return xSemaphoreTakeRecursive(m, ticks) == pdTRUE;
}

static inline bool ag_port_mutex_give_recursive(ag_port_mutex_t m)
{
    return xSemaphoreGiveRecursive(m) == pdTRUE;
}

static inline ag_port_task_t ag_port_mutex_holder(ag_port_mutex_t m)
{
    return xSemaphoreGetMutexHolder(m);
}

static inline void ag_port_mutex_free(ag_port_mutex_t m) { vSemaphoreDelete(m); }

static inline ag_port_sem_t ag_port_sem_new_binary(void)
{
    return xSemaphoreCreateBinary();
}

static inline ag_port_sem_t ag_port_sem_new_counting(uint32_t max, uint32_t initial)
{
    return xSemaphoreCreateCounting(max, initial);
}

static inline bool ag_port_sem_take(ag_port_sem_t s, ag_port_ticks_t ticks)
{
    return xSemaphoreTake(s, ticks) == pdTRUE;
}

static inline bool ag_port_sem_give(ag_port_sem_t s)
{
    return xSemaphoreGive(s) == pdTRUE;
}

static inline void ag_port_sem_free(ag_port_sem_t s) { vSemaphoreDelete(s); }

static inline ag_port_queue_t ag_port_queue_new(uint32_t items, uint32_t item_size)
{
    return xQueueCreate(items, item_size);
}

static inline bool ag_port_queue_send(ag_port_queue_t q, const void *item,
                                      ag_port_ticks_t ticks)
{
    return xQueueSend(q, item, ticks) == pdTRUE;
}

static inline bool ag_port_queue_recv(ag_port_queue_t q, void *item,
                                      ag_port_ticks_t ticks)
{
    return xQueueReceive(q, item, ticks) == pdTRUE;
}

static inline bool ag_port_queue_peek(ag_port_queue_t q, void *item,
                                      ag_port_ticks_t ticks)
{
    return xQueuePeek(q, item, ticks) == pdTRUE;
}

static inline uint32_t ag_port_queue_space(ag_port_queue_t q)
{
    return (uint32_t)uxQueueSpacesAvailable(q);
}

static inline void ag_port_queue_free(ag_port_queue_t q) { vQueueDelete(q); }

#endif /* ARGON_PORT_IMPL_SYNC_H */
