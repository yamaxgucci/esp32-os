/*
 * ArgonOS - the framed link over a UART (ABI 0.61).
 *
 * The same frames as the SPI link (net/wlink.h) - acknowledged, checksummed,
 * only what is lost sent again, blocks, sessions, the frame agreed as the
 * smaller of the two ends' - carried as bytes on a UART by DMA, each frame
 * the length it is.  Both ends are equal: there is no clock to hold.
 *
 * The port is brought up by the io layer (pins from BOARD.CFG's uart<N>, the
 * baud given to open); flags are the SPI link's: AG_SPILINK_BLOCKS for block
 * mode, AG_SPILINK_DROP(n) for the loss test.  The stats struct is the SPI
 * link's, with the wire's own counters in four of its fields: blocks_clocked
 * is frames sent, blocks_data frames received, timeouts bytes skipped while
 * looking for the next frame, stalls the port's pieces of input thrown away
 * unread (its DMA rings went round before the link took them).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_UARTLINK_H
#define ARGON_UARTLINK_H

#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h>

/* The port must already be up at its baud (io does that). */
ag_err_t ag_uartlink_open(int port, uint32_t frame, uint32_t flags);
ag_err_t ag_uartlink_close(int port, ag_spilink_stats_t *out);
ag_err_t ag_uartlink_stats(int port, ag_spilink_stats_t *out);
int32_t  ag_uartlink_write(int port, const void *buf, size_t len);
int32_t  ag_uartlink_write_wait(int port, const void *buf, size_t len,
                                uint32_t timeout_ms);
int32_t  ag_uartlink_send(int port, const void *buf, size_t len, uint32_t flags,
                          uint32_t timeout_ms);
int32_t  ag_uartlink_recv(int port, void *buf, size_t cap,
                          ag_spilink_block_t *info, uint32_t timeout_ms);
int32_t  ag_uartlink_read(int port, void *buf, size_t len, uint32_t timeout_ms);

#endif /* ARGON_UARTLINK_H */
