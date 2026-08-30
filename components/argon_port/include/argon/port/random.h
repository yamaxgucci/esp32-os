/*
 * ArgonOS port contract - cryptographically-strong random bytes.
 *
 * One call: fill a buffer with random the hardware RNG stands behind.  SSH (and
 * anything else that needs unpredictable bytes - nonces, padding, keys) goes
 * through here rather than a PRNG seeded from the clock.
 *
 *   void ag_port_random(void *buf, size_t len)
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_RANDOM_H
#define ARGON_PORT_RANDOM_H

#include <stddef.h>

void ag_port_random(void *buf, size_t len);

#endif /* ARGON_PORT_RANDOM_H */
