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
ag_err_t ag_uartlink_open(int port, uint32_t baud, uint32_t frame,
                          uint32_t flags);

/* The largest frame a link opened with `frame` allocates for: 0 is the
 * board's [wire] frame.  io sizes the port's rings by it. */
uint32_t ag_uartlink_frame(uint32_t frame);
ag_err_t ag_uartlink_close(int port, ag_spilink_stats_t *out);
ag_err_t ag_uartlink_stats(int port, ag_spilink_stats_t *out);
/*
 * The probe (`link probe`): where a sender's time goes, since the last look.
 * Kernel-internal, not in the ABI.  Times in microseconds.
 */
typedef struct {
    uint32_t sends, space_waits;      /* writes, and the ones that waited */
    uint32_t send_us, space_us;       /* inside the write, inside its waits */
    uint32_t stop_empty, stop_credit, stop_win; /* the sender stopped: why */
    uint32_t settles, settle_us;      /* waits for a transmit buffer */
    uint32_t frames, wire_bytes;      /* what went onto the wire */
    uint32_t frames_rx;
    uint32_t elapsed_us;
    /* by DMA: transfers, those that found the wire already quiet, and how
     * long it had been quiet in all */
    uint32_t dma_subs, dma_idle, dma_idle_us;
    /* frames acknowledged, their round trip on average and at most (from
     * being handed to the wire to the answer), and the far end's room as it
     * said it on average and at least */
    uint32_t acked, rtt_avg_us, rtt_max_us, room_avg, room_min, rxcap_peer;
    /* this end told under a frame of room: for frames held past a hole, for
     * a hole, for a ring full of what its reader has not taken */
    uint32_t small_ooo, small_hole, small_full;
    /* each lane: records fed, fed late, the latest by (ms), writers that
     * waited for its room; feeds a full stream stopped */
    uint32_t lane_fed[3], lane_late[3], lane_late_ms[3], lane_waits[3];
    uint32_t lane_blocked;
} ag_uartlink_probe_t;
ag_err_t ag_uartlink_probe(int port, ag_uartlink_probe_t *out);
int32_t  ag_uartlink_write(int port, const void *buf, size_t len);
int32_t  ag_uartlink_write_wait(int port, const void *buf, size_t len,
                                uint32_t timeout_ms);
int32_t  ag_uartlink_send(int port, const void *buf, size_t len, uint32_t flags,
                          uint32_t timeout_ms);
int32_t  ag_uartlink_recv(int port, void *buf, size_t cap,
                          ag_spilink_block_t *info, uint32_t timeout_ms);
/* A block in two parts, head then buf (ABI 0.62). */
int32_t  ag_uartlink_send2(int port, const void *head, size_t hlen,
                           const void *buf, size_t len, uint32_t flags,
                           uint32_t timeout_ms);
int32_t  ag_uartlink_recv2(int port, void *head, size_t hcap, void *buf,
                           size_t cap, ag_spilink_block_t *info,
                           uint32_t timeout_ms);
int32_t  ag_uartlink_read(int port, void *buf, size_t len, uint32_t timeout_ms);

#endif /* ARGON_UARTLINK_H */
