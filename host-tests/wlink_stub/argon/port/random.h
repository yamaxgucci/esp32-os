/*
 * The port's randomness, for the host test of net/wlink.c only: a sequence
 * the test can repeat, since a session number is all it is used for there.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WLINK_STUB_RANDOM_H
#define WLINK_STUB_RANDOM_H

#include <stddef.h>
#include <stdint.h>

extern uint32_t wlink_stub_rng;

static inline void ag_port_random(void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    for (size_t i = 0; i < len; i++) {
        wlink_stub_rng = wlink_stub_rng * 1664525u + 1013904223u;
        p[i] = (uint8_t)(wlink_stub_rng >> 24);
    }
}

#endif /* WLINK_STUB_RANDOM_H */
