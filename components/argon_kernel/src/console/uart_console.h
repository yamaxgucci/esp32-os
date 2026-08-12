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

#endif /* ARGON_UART_CONSOLE_H */
