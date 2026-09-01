/*
 * ArgonOS port contract - MCPWM, a complementary PWM pair with dead-time.
 *
 * Plain PWM the system already has through LEDC (io.h); what the motor-control
 * peripheral adds, and the only reason to reach for it, is a matched pair of
 * outputs that are never high at once.  A and B drive the two transistors of a
 * half-bridge: B is the inverse of A, and a dead-time is held on every edge so
 * that one has fully turned off before the other turns on - without it a bridge
 * shoot-through is a short across the supply.
 *
 * pair() starts the timer and both outputs and returns; the peripheral runs on
 * its own until stop() tears it down.  duty is in per-mille (0..1000) of the A
 * output, deadtime in nanoseconds on each edge.  Returns 0, or negative if the
 * pins, frequency, or dead-time could not be set up.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_MCPWM_H
#define ARGON_PORT_MCPWM_H

#include <stdint.h>

int  ag_port_mcpwm_pair(int gpio_a, int gpio_b, uint32_t hz,
                        uint32_t duty_permille, uint32_t deadtime_ns);
void ag_port_mcpwm_stop(void);

#endif /* ARGON_PORT_MCPWM_H */
