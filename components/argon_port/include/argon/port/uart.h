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
 *   ag_err_t ag_port_uart_close(int port)
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
 *   both want it UP, and neither knows about the other.
 * - Sharing that a port is up is not sharing its STREAM.  A port's bytes have
 *   exactly one owner: two readers steal from each other, and each sees the
 *   other's traffic as noise and its own as truncated.  This is not
 *   hypothetical - HostFS probed UART1 for three seconds and never let go of
 *   it, so the node link that opened the same port afterwards saw HostFS
 *   handshakes as garbage and its own frames arrive in pieces.
 * - close() is therefore not decoration: a subsystem that has decided it does
 *   not want a port must say so, or nothing else can have it.  Closing what is
 *   not open is not an error; closing the console is -AG_EBUSY, because the
 *   console is how the failure would have to be reported.
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
ag_err_t ag_port_uart_close(int port);
ag_err_t ag_port_uart_config(int port, const ag_port_uart_cfg_t *cfg);
ag_err_t ag_port_uart_pins(int port, int tx, int rx);
bool     ag_port_uart_is_open(int port);
ag_err_t ag_port_uart_flush(int port);
int32_t  ag_port_uart_write(int port, const void *buf, size_t len);
int32_t  ag_port_uart_read(int port, void *buf, size_t len,
                           uint32_t timeout_ms);
int32_t  ag_port_uart_pending(int port);

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

/*
 * The same, in two halves: hand a buffer over and come back for it later.
 *
 * Waiting for each transfer before starting the next leaves the transmitter
 * idle between them - the interrupt has to be taken, the task scheduled, the
 * next transfer started, and the wire has nothing to send meanwhile.  Submit
 * keeps a small queue full instead, and drain waits for what is still out.
 * The buffers belong to the hardware until drain returns.
 */
int32_t  ag_port_uart_dma_submit(int port, const void *buf, size_t len);
ag_err_t ag_port_uart_dma_drain(int port);
/*
 * Wait until at most `max_out` submitted transfers are still out.  They
 * finish in the order they were submitted, so a caller that rotates through
 * N buffers gets the oldest one back by settling to N - 1 - without waiting
 * for the rest, which drain would.
 */
ag_err_t ag_port_uart_dma_settle(int port, uint32_t max_out);

/*
 * Receive by DMA into the caller's buffer: what has arrived, up to len,
 * waiting up to timeout_ms for the first byte.  The receive side (two rings,
 * kept for good) is set up on the first read - or by listen, which does only
 * that: a caller that allocates for itself afterwards does not leave the
 * rings in the middle of the hole its own memory makes when it is freed.
 */
int32_t  ag_port_uart_dma_read(int port, void *buf, size_t len,
                               uint32_t timeout_ms);
ag_err_t ag_port_uart_dma_listen(int port);
/*
 * The same, receiving WITHOUT STOPPING: one ring of descriptors, taken up
 * once, that the DMA goes round for as long as the port is open.  An idle
 * line closes the descriptor it is in and the DMA goes on into the next -
 * where IDF's driver stops the channel at every pause and the receive has to
 * be mounted again, and at 16 Mbaud the FIFO holds 80 us of what arrives
 * meanwhile.  -AG_ENOTSUP where this build cannot reach the driver's channel.
 */
ag_err_t ag_port_uart_dma_listen_ring(int port);
/* Pieces of what arrived that were thrown away: overwritten in the ring
 * before anybody read them, or pushed out of a full queue of them. */
uint32_t ag_port_uart_dma_overruns(int port);

#include <argon/port/impl/uart.h>

#endif /* ARGON_PORT_UART_H */
