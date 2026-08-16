/*
 * ArgonOS port contract - the log that belongs to the layer below.
 *
 * ArgonOS has its own journal (argon/log.h) and that is not what this is.  This
 * is the other direction: whatever runs underneath - an RTOS, a vendor driver -
 * prints too, and those lines have to end up in the same journal rather than
 * racing the screen for the UART.  Two calls are enough for that.
 *
 * ag_port_log_level exists because of a real defect, not tidiness: scanning an
 * empty I2C bus asks 112 addresses, the driver below calls each timeout an
 * error, and the answer being looked for is lost among them (pitfall 28 in
 * docs/05-status.md).  The scan silences the tag while it runs.
 *
 * What a port must supply:
 *
 *   AG_PORT_LOG_NONE .. AG_PORT_LOG_VERBOSE   levels, ordered, NONE == silent
 *
 *   void ag_port_log_redirect(int (*sink)(const char *fmt, va_list ap))
 *   void ag_port_log_level(const char *tag, int level)
 *   int  ag_port_log_level_get(const char *tag)
 *
 * A port whose lower layer never prints implements both as no-ops - that is a
 * complete implementation, not a stub.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_LOG_H
#define ARGON_PORT_LOG_H

#include <stdarg.h>

#include <argon/port/impl/log.h>

#endif /* ARGON_PORT_LOG_H */
