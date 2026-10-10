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
/* Whether this port could have the chip's DMA now - it holds it, or a
 * controller is free - asked before the port is brought up and without
 * taking anything.  The C3 has one UHCI: the second wire of the board in
 * the middle goes through the driver instead of failing (6 Oct 2026). */
bool     ag_port_uart_dma_available(int port);
/* Whether this chip sends by DMA at all, asked before a port is brought up
 * and without making anything: the ESP32 receives by its UHCI and sends
 * through the driver, which then needs a transmit ring. */
bool     ag_port_uart_dma_can_send(void);
/* Whether this chip RECEIVES by DMA without losing anything - by the receive
 * that is never stopped (ag_port_uart_dma_listen_ring).  Asked the same way.
 * The S3 cannot have that ring, and IDF's own receive there lost whole frames
 * (883 of 123 365 at 8 Mbaud, 27 Sep 2026): it receives through the driver and
 * still sends by DMA.  The chip decides this, not a board's configuration. */
bool     ag_port_uart_dma_can_receive(void);
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
 * meanwhile.  `bytes` is the ring (0: the other receive's two rings' worth):
 * a framed link needs only what its peer may have in flight.  -AG_ENOTSUP
 * where this build cannot reach the driver's channel.
 */
ag_err_t ag_port_uart_dma_listen_ring(int port, uint32_t bytes);
/* Pieces of what arrived that were thrown away: overwritten in the ring
 * before anybody read them, or pushed out of a full queue of them. */
uint32_t ag_port_uart_dma_overruns(int port);
/* A receive by a DMA the port drives itself (the ESP32's UHCI): its
 * interrupts, those that found nothing, descriptors taken as filled whole and
 * as ended by an idle line; of the first, how many had a length written and
 * how many none; of the second, how many had the owner bit given and how
 * many only their end.  Zeros where there is none. */
void     ag_port_uart_dma_diag(int port, uint32_t out[8]);
/* The transmit's gaps (1 Oct 2026, `link probe`): transfers handed over since
 * the start, and since the last call those handed to a DMA that had finished
 * everything before - a wire gone quiet - and for how long in all.  Zero where
 * nothing goes out by DMA. */
void     ag_port_uart_dma_tx_gaps(int port, uint32_t *subs, uint32_t *idle,
                                  uint32_t *idle_us);
/* How long the wire had been quiet before the latest transfer, 0 if it had not
 * (for `link trace`). */
uint32_t ag_port_uart_dma_last_quiet(int port);
/* Transfers since the last call slower than a byte a microsecond, and the
 * slowest of them: its time and its length (for `link probe`). */
void     ag_port_uart_dma_tx_slow(int port, uint32_t *slow, uint32_t *max_us,
                                  uint32_t *max_len);

#include <argon/port/impl/uart.h>

#endif /* ARGON_PORT_UART_H */
