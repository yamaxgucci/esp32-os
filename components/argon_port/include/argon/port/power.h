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
 *   const char *ag_port_power_note(void)
 *   uint32_t ag_port_power_bus_floor_mhz(void)
 *   void ag_port_power_allow_crystal(bool on)
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
 *   "Actually accept" means now, not in principle: the list may be shorter
 *   while something on the machine needs the peripheral bus where it is.  A
 *   caller that cached it would offer a setting that has since become a way to
 *   break a radio, so nothing caches it - it is three comparisons.  note() says
 *   in one line what is being withheld and why, or NULL when nothing is.
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
 *   This is not hypothetical, and it is what the console has to be immune to
 *   before the lowest step can be offered at all.  On the ESP-IDF port the
 *   serial ports are clocked from something the processor's frequency does not
 *   move, which is why that port offers the crystal step - and withholds it
 *   while a radio is running, because a radio cannot be given a slower bus.
 *   The list is the port's promise about what survives, not an inventory of
 *   what the silicon can be set to.
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

/* One line about what this machine is holding back right now, or NULL. */
const char *ag_port_power_note(void);

/*
 * Offer the step below the bus floor, or stop offering it.
 *
 * There is exactly one such step on this family - the crystal - and it is the
 * one where the peripheral bus follows the processor down.  The console was
 * made immune to that (see uart_hw.c) and a session at 40 MHz then read
 * perfectly on the board; what has not been shown is the rest of the machine,
 * and the board wedged twice in that neighbourhood, once while starting a radio
 * on the slowed bus and once with the console going deaf after the switch.
 *
 * So it is off unless somebody asks for it: `[power] crystal = 1` in
 * SYSTEM.CFG, which is a decision an installation makes with a meter and a
 * board in front of it, not a default.  Off is not "unsupported" - everything
 * around it is written and tested - it is "not yet shown to be harmless".
 */
void ag_port_power_allow_crystal(bool on);

/*
 * The lowest setting at which the peripheral bus still runs at the rate it has
 * at full speed - and therefore the lowest one at which everything that divides
 * off that bus keeps meaning what it meant.  Zero on a machine where the bus
 * never follows the processor.
 *
 * It is a fact about the clock tree, not about what is running: a caller asks
 * it *before* starting something that cannot take a slower bus, when the answer
 * cannot yet be deduced from anything else.  That is the whole reason it is
 * separate from steps(), which reports what is safe right now and therefore
 * still lists the low step until the radio actually exists.
 */
uint32_t ag_port_power_bus_floor_mhz(void);

#endif /* ARGON_PORT_POWER_H */
