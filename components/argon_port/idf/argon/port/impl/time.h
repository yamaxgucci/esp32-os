/*
 * ArgonOS port: ESP-IDF - time.
 *
 * ag_port_cycles is esp_cpu_get_cycle_count and not esp_timer_get_time, and the
 * difference is the whole point of the call existing: the two used to return the
 * same number, which made the cycle counter useless for the one job it has, and
 * nobody noticed because nothing called it (pitfall 33 in docs/05-status.md).
 *
 * Under QEMU this counter is a virtual clock at the core frequency and does not
 * count work.  That is a property of the emulator, not of this port; measuring
 * DSP cost in the emulator is done with -icount instead (`argon test -Icount`).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_TIME_H
#define ARGON_PORT_IMPL_TIME_H

#include "esp_cpu.h"
#include "esp_timer.h"

static inline int64_t ag_port_us(void)
{
    return esp_timer_get_time();
}

static inline uint32_t ag_port_cycles(void)
{
    return (uint32_t)esp_cpu_get_cycle_count();
}

#endif /* ARGON_PORT_IMPL_TIME_H */
