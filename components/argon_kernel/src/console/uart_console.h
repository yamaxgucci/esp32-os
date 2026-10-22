/*
 * ArgonOS - UART console endpoint (kernel private).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_UART_CONSOLE_H
#define ARGON_UART_CONSOLE_H

#include <argon/abi.h>

/*
 * Installs the UART driver on `port` and registers it as a console endpoint.
 * Pin assignment is left alone, so the port keeps whatever the bootloader and
 * the board pack already configured.
 */
ag_err_t ag_uart_console_attach(int port, int baud);

/*
 * Takes `port` back off the console: the console task stops reading it and
 * stops rendering to it, but the UART driver stays installed, so the caller can
 * go on using ag_port_uart_* on it directly.  This is what the raw serial
 * bridge (`uartbridge`) does to free UART0 - the console is silent afterwards,
 * by design, until a reset.  A no-op if the port was not a console endpoint.
 */
void ag_uart_console_detach(int port);

#endif /* ARGON_UART_CONSOLE_H */
