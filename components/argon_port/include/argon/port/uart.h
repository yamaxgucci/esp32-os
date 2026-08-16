/*
 * ArgonOS port contract - serial ports.
 *
 * The first peripheral any board needs and the last one it gives up.  Three
 * unrelated parts of the system talk through this: the console (which is how
 * anybody sees anything at all before there is a display), the HostFS link to
 * the development machine, and an application asking for a serial port of its
 * own through the ABI.  One contract serves all three, because they want the
 * same thing.
 *
 * What a port must supply:
 *
 *   AG_PORT_UART_PORTS     how many there are
 *   AG_PORT_UART_CONSOLE   the one the boot console is on, or -1 for none
 *   AG_PORT_UART_PIN_KEEP  as a pin number: leave that pin as it is
 *
 *   ag_err_t ag_port_uart_open(int port, const ag_port_uart_cfg_t *cfg,
 *                              uint32_t rx_bytes, uint32_t tx_bytes)
 *   ag_err_t ag_port_uart_config(int port, const ag_port_uart_cfg_t *cfg)
 *   ag_err_t ag_port_uart_pins(int port, int tx, int rx)
 *   bool     ag_port_uart_is_open(int port)
 *   ag_err_t ag_port_uart_flush(int port)
 *   int32_t  ag_port_uart_write(int port, const void *buf, size_t len)
 *   int32_t  ag_port_uart_read(int port, void *buf, size_t len,
 *                              uint32_t timeout_ms)
 *   int32_t  ag_port_uart_pending(int port)
 *
 * Contract, not advice:
 *
 * - open() on a port that is already open is not an error.  Two subsystems may
 *   both want it up, and neither knows about the other.
 * - The receive buffer is not optional and its size is the caller's business.
 *   The console asks for 2 KB because that is what absorbs the bytes already on
 *   the line when it sends XOFF - a port that quietly gives it less will lose
 *   keystrokes on a burst, which reads as a broken terminal rather than as a
 *   full buffer.  This has already cost this project a day (pitfall 17 in
 *   docs/05-status.md), so it is written down rather than assumed.
 * - read() with timeout_ms == 0 returns what is already buffered and does not
 *   wait.  It returns 0, not an error, when there is nothing.
 * - write() may block until the bytes are handed to the hardware.
 * - pending() is what makes it possible to drain a port without blocking; a
 *   port that cannot tell returns -AG_ENOTSUP, and the caller then polls with a
 *   short read instead.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_UART_H
#define ARGON_PORT_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h>

typedef struct {
    uint32_t baud;
    uint8_t  data_bits; /* 5..8                        */
    uint8_t  parity;    /* 0 none, 1 odd, 2 even       */
    uint8_t  stop_bits; /* 1 or 2                      */
} ag_port_uart_cfg_t;

ag_err_t ag_port_uart_open(int port, const ag_port_uart_cfg_t *cfg,
                           uint32_t rx_bytes, uint32_t tx_bytes);
ag_err_t ag_port_uart_config(int port, const ag_port_uart_cfg_t *cfg);
ag_err_t ag_port_uart_pins(int port, int tx, int rx);
bool     ag_port_uart_is_open(int port);
ag_err_t ag_port_uart_flush(int port);
int32_t  ag_port_uart_write(int port, const void *buf, size_t len);
int32_t  ag_port_uart_read(int port, void *buf, size_t len,
                           uint32_t timeout_ms);
int32_t  ag_port_uart_pending(int port);

#include <argon/port/impl/uart.h>

#endif /* ARGON_PORT_UART_H */
