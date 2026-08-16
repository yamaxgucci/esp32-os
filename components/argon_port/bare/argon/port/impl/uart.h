/*
 * ArgonOS port: bare metal - serial ports, the constants.
 *
 * Write this one early: it is how anybody sees anything at all before there is
 * a display, and the whole system is brought up over it.
 *
 * The receive buffer size is the caller's, not yours, and it matters.  The
 * console asks for 2 KB because that is what absorbs the bytes already on the
 * line when it sends XOFF; a driver that quietly gives it less loses keystrokes
 * on a burst, which reads as a broken terminal rather than a full buffer
 * (pitfall 17 in docs/05-status.md).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_UART_H
#define ARGON_PORT_IMPL_UART_H

#error "bare: no serial port yet.  See argon/port/uart.h for the contract."

#endif /* ARGON_PORT_IMPL_UART_H */
