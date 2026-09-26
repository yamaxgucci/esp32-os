/*
 * The port's clock, for the host test of net/rdpaint.c only: the test moves
 * it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef RDPAINT_STUB_TIME_H
#define RDPAINT_STUB_TIME_H

#include <stdint.h>

extern int64_t rdpaint_stub_now_us;

static inline int64_t ag_port_us(void) { return rdpaint_stub_now_us; }

#endif /* RDPAINT_STUB_TIME_H */
