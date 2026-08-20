/*
 * ArgonOS port contract - the machine itself.
 *
 * What the system needs to know about the part it is running on, and the two
 * things it needs to do to it: restart it, and keep a handful of bytes across
 * that restart.
 *
 * The counter that survives a reset is not a convenience.  A board that reboots
 * in a loop has to be able to notice that it is doing so and come up in
 * recovery instead, and it cannot ask a filesystem it may be failing to mount.
 * A port that has no such memory may place AG_PORT_NOINIT in ordinary RAM: the
 * consequence is that repeated crashes are not counted, which loses recovery
 * mode but nothing else.
 *
 * What a port must supply:
 *
 *   AG_PORT_NOINIT          attribute: survives a warm reset, not zeroed at start
 *   AG_PORT_TARGET_NAME     string, e.g. "esp32s3" - printed by `ver`
 *
 *   ag_reset_t ag_port_reset_reason(void)
 *   const char *ag_port_reset_name(void)
 *
 * reset_reason answers the one question the boot logic asks - was this clean -
 * and deliberately answers nothing else, because three cases are all the policy
 * needs.  reset_name answers the question a person asks, which is a different
 * one: "prochee" covers a panic, a watchdog and a supply that sagged, and those
 * call for entirely different next steps.  A name costs nothing and turns a
 * board that "seems to have restarted" into a board that says why.
 *   void       ag_port_restart(void)         does not return
 *
 *   uint8_t  ag_port_cpu_cores(void)
 *   uint8_t  ag_port_cpu_revision(void)      major revision, 0 if unknown
 *   uint32_t ag_port_cpu_hz(void)
 *
 *   size_t   ag_port_psram_size(void)        0 when there is none
 *   bool     ag_port_psram_executable(void)  can instructions be fetched from it
 *
 * There is no separate "is there any PSRAM" call: a size of zero says so, and
 * one question that cannot disagree with itself is better than two that can.
 *
 * There is deliberately no way to hand the kernel an error code from below.
 * Every port function returns either a plain answer or an ag_err_t, and the
 * translation happens inside the port where the meaning is known.  An
 * application cannot do anything with a vendor error code and the ABI promises
 * it will never see one; the same restraint one layer down keeps that promise
 * cheap to keep.
 *
 * ag_reset_t has three values because the kernel distinguishes three cases and
 * no more: a cold start clears the crash streak, a deliberate restart keeps it,
 * and anything else increments it.  Map an unknown reason to AG_RESET_OTHER -
 * being counted as a crash is the safe direction.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_SYS_H
#define ARGON_PORT_SYS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h>

typedef enum {
    AG_RESET_POWERON = 0, /* cold start: no history to carry */
    AG_RESET_SOFTWARE,    /* asked for it: `reboot`, or a restart after update */
    AG_RESET_OTHER        /* watchdog, panic, brownout - something went wrong */
} ag_reset_t;

#include <argon/port/impl/sys.h>

#endif /* ARGON_PORT_SYS_H */
