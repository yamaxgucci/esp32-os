/*
 * ArgonOS port contract - what it costs this machine to be switched on.
 *
 * There is exactly one thing here, and that is deliberate: the clock.  Nothing
 * else about saving power belongs below this line - a dark screen is a panel
 * driver's business, a radio that is off is the radio's, and an application
 * that decides to stop working when the machine slows down is nobody's but its
 * own.  What only the machine can answer is how fast its processor is allowed
 * to run, and whether that can be changed while it is running at all.
 *
 * What a port must supply:
 *
 *   uint32_t ag_port_power_caps(void)
 *   uint32_t ag_port_power_cpu_mhz(void)
 *   uint32_t ag_port_power_cpu_steps(uint16_t *out, uint32_t max)
 *   ag_err_t ag_port_power_cpu_band(uint32_t min_mhz, uint32_t max_mhz)
 *
 * A port that cannot move its clock says so with caps() == 0 and answers
 * -AG_ENOTSUP from band().  It still has to answer cpu_mhz() and steps(),
 * because "this machine runs at 240 MHz and has no other setting" is an answer
 * and the shell prints it.  This is not a subsystem that can be absent: every
 * machine has a clock rate, so there is no AG_PORT_HAS_POWER and no #if around
 * the caller.
 *
 * Contract, not advice:
 *
 * - steps() writes the frequencies this machine will actually accept, in
 *   megahertz, highest first, and returns how many there are - which may be
 *   more than `max`.  out[0] is the maximum, and on a part whose clock is
 *   fixed it is the only entry.
 *
 * - band() takes two of those numbers and nothing else.  A value that is not
 *   in the list is -AG_EINVAL rather than the nearest one that would have
 *   worked: a caller that asked for 100 MHz and silently got 80 has no way to
 *   find that out, and the one place that knows the list is right here.
 *
 * - min == max pins the clock.  min < max hands it to whatever dynamic scaling
 *   the machine has, and then cpu_mhz() is the only way to know where it
 *   actually is - it is a reading, not a setting, and it changes without
 *   anybody asking.
 *
 * - band() is not a promise about power.  Halving the clock does not halve the
 *   current: on this family the radio and the display backlight each cost more
 *   than the whole processor does, and a board measured at 240 MHz and at 80
 *   differs by rather less than the arithmetic suggests.  The number that
 *   matters is measured on a board, and this call is only what makes the
 *   measurement possible.
 *
 * - Lowering the clock can move the peripheral bus with it, and everything
 *   that latched a divider off that bus keeps its divider: a serial port set
 *   up for 115200 baud at one bus frequency talks at another when the bus
 *   moves, which arrives as garbage on the console rather than as an error.
 *   A port must not offer a step it cannot keep the console readable across - a
 *   system that cannot be talked to cannot be told to speed up again.
 *
 *   This is not hypothetical.  The ESP-IDF port offers 240, 160 and 80, where
 *   the bus stays where it is, and does not offer the crystal, where it does
 *   not: on the board that step ended the conversation mid-line and only a
 *   reset brought it back.  The list is the port's promise about what survives,
 *   not an inventory of what the silicon can be set to.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_POWER_H
#define ARGON_PORT_POWER_H

#include <stdint.h>

#include <argon/abi.h>

enum ag_port_power_caps {
    /* The clock can be moved while the system is running. */
    AG_PORT_PWR_CPU_BAND = 1u << 0,
};

/*
 * Enough for every part in this family: four settings and two spare.  A bound
 * rather than a list that grows, so the shell can put it on the stack.
 */
#define AG_PORT_PWR_STEPS_MAX 6

uint32_t ag_port_power_caps(void);

/* Where the clock is now, megahertz.  Never zero. */
uint32_t ag_port_power_cpu_mhz(void);

/* Descending, out[0] is the maximum.  Returns the count, capped writes. */
uint32_t ag_port_power_cpu_steps(uint16_t *out, uint32_t max);

ag_err_t ag_port_power_cpu_band(uint32_t min_mhz, uint32_t max_mhz);

#endif /* ARGON_PORT_POWER_H */
