/*
 * ArgonOS port contract - time, and the two different things it means.
 *
 * There are two clocks here and confusing them has already cost this project a
 * day (see pitfall 33 in docs/05-status.md).  They are not interchangeable:
 *
 *   ag_port_us()      wall time since boot, microseconds.  Monotonic, never
 *                     wraps in any run this system will see, and is what
 *                     timeouts and the `boot` report are measured with.  Signed,
 *                     so that a difference of two readings is a plain int64_t
 *                     and the caller does not have to think about it.
 *
 *   ag_port_cycles()  the CPU cycle counter.  Exists to measure an inner loop
 *                     where a microsecond is hundreds of instructions.  A port
 *                     that has no such counter must say so by returning 0
 *                     always, not by returning ag_port_us() again - a clock
 *                     that quietly answers in the wrong unit is worse than one
 *                     that is absent, because nobody checks it.
 *
 * Ticks are the third unit and they belong to the scheduler, not here: see
 * argon/port/task.h.
 *
 * What a port must supply:
 *
 *   int64_t  ag_port_us(void)
 *   uint32_t ag_port_cycles(void)
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_TIME_H
#define ARGON_PORT_TIME_H

#include <stdint.h>

#include <argon/port/impl/time.h>

#endif /* ARGON_PORT_TIME_H */
