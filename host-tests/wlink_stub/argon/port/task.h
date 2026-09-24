/*
 * The port's scheduler time, for the host test of net/wlink.c only: ticks
 * are milliseconds of the test's own clock (wlink_stub_now_us).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WLINK_STUB_TASK_H
#define WLINK_STUB_TASK_H

#include <stdint.h>

typedef uint32_t ag_port_ticks_t;
#define AG_PORT_FOREVER UINT32_MAX

extern int64_t wlink_stub_now_us;

static inline ag_port_ticks_t ag_port_ms_to_ticks(uint32_t ms) { return ms; }

static inline ag_port_ticks_t ag_port_ticks(void)
{
    return (ag_port_ticks_t)(wlink_stub_now_us / 1000);
}

static inline void ag_port_task_delay(ag_port_ticks_t t) { (void)t; }

#endif /* WLINK_STUB_TASK_H */
