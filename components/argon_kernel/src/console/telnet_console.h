/*
 * ArgonOS - telnet console endpoint (kernel private).
 *
 * A second way onto the same console the UART carries: a TCP listener that
 * turns each connection into a console endpoint, so the shell, the editor and
 * the file manager appear over the network exactly as they do on the wire.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_TELNET_CONSOLE_H
#define ARGON_TELNET_CONSOLE_H

#include <stdbool.h>
#include <stdint.h>

#include <argon/abi.h>

#include <argon/port/config.h> /* CONFIG_ARGON_NET_TELNET */
#include <argon/port/net.h>

#if defined(CONFIG_ARGON_NET_TELNET) && CONFIG_ARGON_NET_TELNET

/* Start listening on `port` (0 -> 23) and accept console sessions.  Returns
 * AG_OK when the listener is up, -AG_EBUSY if one already is. */
ag_err_t ag_telnet_start(uint16_t port);

/* Stop the listener and drop every telnet session. */
void ag_telnet_stop(void);

bool     ag_telnet_running(void);
uint16_t ag_telnet_port(void);

#endif /* CONFIG_ARGON_NET_TELNET */

#endif /* ARGON_TELNET_CONSOLE_H */
