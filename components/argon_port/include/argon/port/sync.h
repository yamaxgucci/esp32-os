/*
 * ArgonOS port contract - mutexes, semaphores and queues.
 *
 * Three types, not one, even on a system where they are the same object
 * underneath: a port is free to make them the same typedef, but the kernel says
 * which it means, and so does anyone reading it.
 *
 * The mutex must invert priority correctly - the kernel takes locks from tasks
 * of different priority as a matter of course, and a port without inheritance
 * turns that into a latency bug nobody will find by reading.
 *
 * ag_port_mutex_holder exists for one reason and it is worth stating: the
 * supervisor asks who holds a lock before deciding whether it may kill a task
 * (pitfall 21 in docs/05-status.md - taking a mutex away from a deleted task is
 * not possible, so the supervisor refuses instead).  A port that cannot answer
 * returns NULL, and the supervisor then behaves as if the lock were free, which
 * is the unsafe answer; implement it.
 *
 * What a port must supply:
 *
 *   ag_port_mutex_t, ag_port_sem_t, ag_port_queue_t     opaque handles, NULL on failure
 *
 *   ag_port_mutex_t ag_port_mutex_new(void)
 *   ag_port_mutex_t ag_port_mutex_new_recursive(void)
 *   bool       ag_port_mutex_take(ag_port_mutex_t m, ag_port_ticks_t ticks)
 *   bool       ag_port_mutex_give(ag_port_mutex_t m)
 *   bool       ag_port_mutex_take_recursive(ag_port_mutex_t m, ag_port_ticks_t ticks)
 *   bool       ag_port_mutex_give_recursive(ag_port_mutex_t m)
 *   ag_port_task_t  ag_port_mutex_holder(ag_port_mutex_t m)
 *   void       ag_port_mutex_free(ag_port_mutex_t m)
 *
 *   ag_port_sem_t ag_port_sem_new_binary(void)
 *   ag_port_sem_t ag_port_sem_new_counting(uint32_t max, uint32_t initial)
 *   bool     ag_port_sem_take(ag_port_sem_t s, ag_port_ticks_t ticks)
 *   bool     ag_port_sem_give(ag_port_sem_t s)
 *   void     ag_port_sem_free(ag_port_sem_t s)
 *
 *   ag_port_queue_t ag_port_queue_new(uint32_t items, uint32_t item_size)
 *   bool       ag_port_queue_send(ag_port_queue_t q, const void *item, ag_port_ticks_t t)
 *   bool       ag_port_queue_recv(ag_port_queue_t q, void *item, ag_port_ticks_t t)
 *   bool       ag_port_queue_peek(ag_port_queue_t q, void *item, ag_port_ticks_t t)
 *   uint32_t   ag_port_queue_space(ag_port_queue_t q)
 *   void       ag_port_queue_free(ag_port_queue_t q)
 *
 * ag_port_queue_space says how many more items would fit.  The console reads it
 * to decide when to ask the far end to stop sending: a queue that is allowed to
 * fill drops keystrokes, and a dropped keystroke looks like a broken terminal.
 *
 * A binary semaphore is created empty: the first take blocks until a give.
 * queue_peek copies the head without removing it - the kernel needs that to
 * look at a keystroke without consuming it, and getting this wrong cost the
 * oldest idiom there is (see docs/05-status.md, what the documentation cost).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_SYNC_H
#define ARGON_PORT_SYNC_H

#include <stdbool.h>
#include <stdint.h>

#include <argon/port/task.h>
#include <argon/port/impl/sync.h>

#endif /* ARGON_PORT_SYNC_H */
