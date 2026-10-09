/*
 * The port's clock, for the host test of net/wlink.c only: the test moves it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WLINK_STUB_TIME_H
#define WLINK_STUB_TIME_H

#include <stdint.h>

extern int64_t wlink_stub_now_us;

static inline int64_t ag_port_us(void) { return wlink_stub_now_us; }

#endif /* WLINK_STUB_TIME_H */
