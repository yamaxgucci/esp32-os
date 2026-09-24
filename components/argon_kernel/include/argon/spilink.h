/*
 * ArgonOS - the SPI slave link service (kernel private).
 *
 * The ESP32 as the SLAVE end of a bus the peer clocks, presented as a plain
 * byte stream: bytes written here go out in the next blocks the master clocks,
 * and bytes the master clocks in come back from a read.  Nothing above this
 * line has to know that a block is not a frame or that the queue has to be kept
 * armed - see src/net/spilink.c for why that is the whole job.
 *
 * The four calls are what api->io carries (ABI 0.57).  They live here as well
 * so the kernel can drive the service without going through the ABI table.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_SPILINK_H
#define ARGON_SPILINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h>

/*
 * The counters themselves are the ABI's (ag_spilink_stats_t, ABI 0.57), because
 * a driver has to be able to read them: see the note on that type for how to
 * tell a working link from a wedged one.  They are reported at close and
 * readable at any time - the close line alone was useless in the one failure
 * that mattered, where the board never got as far as closing anything.
 */

/*
 * Brings the service up on `bus` (the chip's number: 2 or 3) and starts its
 * task.  The bus itself must already be up as a slave with `cs` as its select
 * input - the io layer does that, so that the pins, the clock and the drive
 * strength come from BOARD.CFG's spi<bus> section exactly as the master path
 * gets them.  `block` is the unit of every transaction in both directions.
 *
 * `block` has to be a whole number of words and at most AG_PORT_SPI_MAX_XFER,
 * and the blocks are armed from DMA-capable memory: the port hands its engine
 * the caller's own buffers rather than copying, and refuses anything it cannot
 * reach or that is not word-aligned.  A block that cannot be armed is a link
 * that opens and then carries nothing, so this says -AG_EINVAL rather than
 * finding out later.
 *
 * -AG_EBUSY when the bus is already open as a link, -AG_ENOMEM when the rings
 * and the armed blocks do not fit (nothing is allocated after open).
 *
 * The three pin numbers are the bus's, as BOARD.CFG gave them to the io layer,
 * and they are here for one reason: when the peer goes quiet for a whole stall
 * window the service gives up on the blocks it armed and rebuilds the slave
 * interface - and if the port could not build it again it leaves the bus
 * CLOSED, where only opening it as a slave brings it back.  Reading the pins a
 * second time from the configuration is what this avoids; they are the numbers
 * the bring-up already used.
 */
ag_err_t ag_spilink_open(int bus, int cs, uint32_t khz, uint32_t block, int sck,
                         int mosi, int miso);
/* The same with AG_SPILINK_MASTER / AG_SPILINK_FRAMED (ABI 0.60). */
ag_err_t ag_spilink_open2(int bus, int cs, uint32_t khz, uint32_t block,
                          uint32_t flags, int sck, int mosi, int miso);

/*
 * Stops the task, waits for it to leave, and gives back everything open
 * allocated.  `out` may be NULL.  -AG_ENODEV when no link is open on the bus,
 * -AG_ETIMEDOUT when the task would not stop - in which case nothing is freed
 * and the link stays open, because buffers a running task is pointing into are
 * worse than a leak.
 */
ag_err_t ag_spilink_close(int bus, ag_spilink_stats_t *out);

/*
 * The counters while the link is alive, and the place a fault is said out loud.
 *
 * Runs on the CALLER's task, never on the service task, and that is not a
 * detail: the service task must not log at all (a device revoke stops it while
 * holding the device registry lock, and the console lock sits under that one),
 * so this is where the one line about a stalled or rebuilt link is printed -
 * at most once every few seconds, and only while something is wrong.  A caller
 * that polls this therefore gets both a number and a line.
 */
ag_err_t ag_spilink_stats(int bus, ag_spilink_stats_t *out);

/*
 * Bytes taken, which may be fewer than `len` when the TX ring runs out of room
 * first, or -AG_EAGAIN when it is full.  Never blocks and never drops: the
 * caller keeps the remainder and comes back.
 */
int32_t ag_spilink_write(int bus, const void *buf, size_t len);
int32_t ag_spilink_write_wait(int bus, const void *buf, size_t len,
                              uint32_t timeout_ms);
int32_t ag_spilink_send(int bus, const void *buf, size_t len, uint32_t flags,
                        uint32_t timeout_ms);
int32_t ag_spilink_recv(int bus, void *buf, size_t cap,
                        ag_spilink_block_t *info, uint32_t timeout_ms);
/* A block in two parts, head then buf (ABI 0.62). */
int32_t ag_spilink_send2(int bus, const void *head, size_t hlen,
                         const void *buf, size_t len, uint32_t flags,
                         uint32_t timeout_ms);
int32_t ag_spilink_recv2(int bus, void *head, size_t hcap, void *buf,
                         size_t cap, ag_spilink_block_t *info,
                         uint32_t timeout_ms);

/*
 * Bytes taken from the RX ring, or -AG_EAGAIN when nothing arrived inside
 * `timeout_ms` (0 polls once, UINT32_MAX waits as long as it takes).
 */
int32_t ag_spilink_read(int bus, void *buf, size_t len, uint32_t timeout_ms);

#endif /* ARGON_SPILINK_H */
