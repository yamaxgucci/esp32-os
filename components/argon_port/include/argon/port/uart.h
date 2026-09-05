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

/*
 * The same bytes, carried by hardware instead of by this thread.
 *
 * A hundred and fifty kilobytes a frame through ag_port_uart_write is a task
 * copying into a ring and waking eighty times to do it, and on a busy machine
 * that leaves the wire half idle - measured: 172 KB/s of a possible 400 while
 * a game ran.  With DMA the peripheral pulls the buffer itself and the caller
 * sleeps until it is gone.
 *
 * Not every chip has it (the ESP32 and the S2 do not), so a port may answer
 * -AG_ENOTSUP and the caller must have the ordinary write to fall back on.
 * The buffer may live in external RAM where the hardware allows it; alignment
 * is the port's business to check, and a buffer it cannot take is
 * -AG_EINVAL rather than a silent copy.
 *
 * Blocking: it returns when the last byte has left, or on error.  Interleaving
 * it with ag_port_uart_write on the same port is the caller's problem - the
 * two are different masters of the same transmitter.
 */
/*
 * Wait until everything handed to this port has actually left it - the
 * driver's own buffer and the peripheral's FIFO both empty.
 *
 * Not ag_port_uart_flush, which is a different thing wearing a similar name:
 * that one throws away what has arrived, and using it to make room for a DMA
 * transfer would silently eat the far end's replies.
 *
 * It exists because DMA and the ordinary write are two masters of one
 * transmitter.  Whoever mixes them has to hand over cleanly, and this is the
 * handover.
 */
ag_err_t ag_port_uart_tx_wait(int port, uint32_t timeout_ms);

bool     ag_port_uart_dma_ready(int port);
int32_t  ag_port_uart_dma_write(int port, const void *buf, size_t len);

#include <argon/port/impl/uart.h>

#endif /* ARGON_PORT_UART_H */
