/*
 * ArgonOS port: bare metal - mutexes, semaphores and queues.
 *
 * Falls out of the scheduler in task.h and should be written with it.  Two
 * things here are not decoration:
 *
 * The mutex must invert priority.  The kernel takes locks from tasks of very
 * different priority as a matter of course - the supervisor runs at 15 and an
 * application at 5 - and a mutex without inheritance turns that into a latency
 * bug nobody will find by reading the code.
 *
 * ag_port_mutex_holder must really answer.  The supervisor asks who holds a
 * lock before deciding whether it may kill a task, because taking a mutex away
 * from a deleted task is not possible on any RTOS worth the name.  A port that
 * returns NULL makes the supervisor believe every lock is free, and it will
 * then kill a task holding one - which is a system that hangs instead of one
 * that says no (pitfall 21 in docs/05-status.md).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_SYNC_H
#define ARGON_PORT_IMPL_SYNC_H

#error "bare: no synchronisation yet.  See argon/port/sync.h for the contract."

#endif /* ARGON_PORT_IMPL_SYNC_H */
