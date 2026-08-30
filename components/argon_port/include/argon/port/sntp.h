/*
 * ArgonOS port contract - SNTP, setting the clock from the network.
 *
 * One thing: ask a time server what time it is and set the system clock to the
 * answer.  After that the wall clock the kernel already reads (time(), which
 * backs ag_get_datetime) returns real time instead of seconds since boot.
 *
 * This is deliberately not "keep a time daemon running": a one-shot sync is
 * what a board needs to stamp files and show the date, and it costs nothing
 * once it returns.  A board that wants the clock disciplined continuously can
 * call it again on a timer from above - the port keeps no state between calls.
 *
 * What a port must supply, when AG_PORT_HAS_SNTP is 1:
 *
 *   ag_err_t ag_port_sntp_sync(const char *server, uint32_t timeout_ms)
 *
 * Contract:
 * - Blocks until the clock is set or `timeout_ms` passes (0 -> a sane default).
 *   The network must be up first; with no lease it fails rather than waits.
 * - `server` NULL or empty means a default pool.  A name is resolved by the
 *   same resolver the rest of the net layer uses.
 * - On AG_OK the system clock is set; on failure it is left untouched.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_SNTP_H
#define ARGON_PORT_SNTP_H

#include <stdint.h>

#include <argon/abi.h>

#include <argon/port/impl/sntp.h>

#if AG_PORT_HAS_SNTP

ag_err_t ag_port_sntp_sync(const char *server, uint32_t timeout_ms);

#endif /* AG_PORT_HAS_SNTP */

#endif /* ARGON_PORT_SNTP_H */
