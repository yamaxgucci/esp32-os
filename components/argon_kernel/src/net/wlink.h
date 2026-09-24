/*
 * ArgonOS - the framed link between two boards, whatever the wire.
 *
 * The protocol half of the kernel's board-to-board links: frames with an
 * offset, an acknowledgement, a room and a checksum; only what is lost sent
 * again; blocks that say whether they are worth sending again; sessions; the
 * frame size agreed as the smaller of the two ends'.  It knows nothing of the
 * wire.  spilink.c carries its frames as SPI transactions of a fixed length,
 * one master and one slave; uartlink.c as bytes on a UART, frames of the
 * length they are, both ends equal.  Each keeps a wlink_t and calls wl_build
 * for the next frame to send and wl_rx for one that arrived.
 *
 * Private to the kernel: the ABI is io->spi_link_* and io->uart_link_*.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_NET_WLINK_H
#define ARGON_NET_WLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <argon/abi.h>

#include <argon/port/sync.h>

/*
 * FRAMED: EVERY FRAME SAYS WHAT IN IT IS DATA (ABI 0.60).
 *
 * The first shape of this link moved raw blocks: whatever was in the slot was
 * the stream, so a side with nothing to say clocked zeros and the other side
 * took them in as bytes - 826 blocks for 69 that carried anything, measured -
 * and neither side could know whether the other had room.  A caller that
 * wanted its own framing had to make each of ITS blocks one transaction,
 * which at 64 bytes and 20 MHz is a transaction's setup for 26 us of wire,
 * and 34% of the line was what that came to.
 *
 * So in a framed link a transaction is a FRAME of the size given at open, and
 * its first twenty bytes are a header:
 *
 *   0     0xA5
 *   1     flags: 1 data, 2 "I have a hole at my ack", 4 segments,
 *         8 "... and it is a frame I had no room for: send it as it was",
 *         16 sent by the master
 *   2     0x5A
 *   3     the largest frame the SENDER can take, in 64-byte units
 *   4-7   where this frame's bytes start in the stream (little endian)
 *   8-11  ack: every byte before this offset has arrived
 *   12-13 payload bytes that follow (segments: the stream bytes they
 *         stand for)
 *   14-15 room: free bytes in the SENDER's receive ring when it built this
 *   16-19 the sender's session: a random number drawn when its link opened
 *   20-23 checksum of bytes 0..19 and the payload
 *
 * The stream is the payloads, in order.  A side with nothing to say still
 * sends a header: that is how the other side learns its room and its ack.
 *
 * A NEW SESSION IS A NEW STREAM.  When the other end's session changes - it
 * rebooted, or its link was closed and opened again - everything this end had
 * of the old stream, both ways, is thrown away and both begin again at offset
 * zero: the other end remembers none of it, and a survivor that waited for
 * the old offsets would wait for ever.  A writer or a reader caught inside a
 * block is told (-AG_EIO) rather than left to finish it into the new stream.
 *
 * THE FRAME IS THE SMALLER OF THE TWO ENDS' (24 Sep).  Each end opens with
 * its own largest frame - the caller's, or `frame` under [wire] in its
 * SYSTEM.CFG - and allocates for that alone, so a C3 that says 2048 pays for
 * 2 KB frames whatever its peer can take.  Byte 3 of every header says it.
 * The master clocks WL_HELLO-byte frames until it has read a header of
 * the slave's, and then the smaller of the two; the slave, which learns the
 * master's size from the first hello, puts data in a frame only once the
 * master clocks that size - before, its frames would be cut by the short
 * clock.  Nothing is sent by either end before that, which is how it was
 * already: no data goes until the other end's room is known.
 *
 * NOTHING IS LOST, AND ONLY WHAT IS LOST IS SENT AGAIN (24 Sep).  A payload
 * leaves the transmit ring only when the other side's ack passes it.  A frame
 * that arrives after a hole is put straight into its place in the receive
 * ring - its offset says where - and the reader is given bytes only up to
 * the first hole, so a receiver needs no memory of its own for this.  It
 * says, once per hole, "I have a hole at my ack", and the sender sends the
 * one frame that starts there again; 50 ms with no ack does the same.  Acks
 * ride the frames going the other way, which both sides send anyway.
 *
 * BLOCKS (AG_SPILINK_BLOCKS).  An application may write BLOCKS instead of
 * bytes: each goes into the stream as an 8-byte record header - length,
 * block number, flags - and its bytes, and a block not marked
 * AG_SPILINK_RELIABLE is one that is not worth sending again (a piece of a
 * screen the next update will cover).  A lost frame with anything reliable
 * in it is sent again - but made of SEGMENTS: the pieces of reliable blocks
 * and of blocks cut by the frame's edges as bytes, and each run of whole
 * unreliable blocks as a notice naming them, which the receiver turns into
 * a LOST record of the same size in the same place.  So the reader gets
 * every block in order, or knows exactly which ones it will never get, and
 * the price is two bytes of block number per block.  (Skipping only a frame
 * made wholly of unreliable blocks skipped nothing: blocks are cut at the
 * frame's edges nearly always, and "lost and told" was 0 with every block
 * unreliable, measured.)  A segmented payload is its length (2 bytes) and
 * then segments, each a 2-byte head - bit 15 set for a notice, the stream
 * bytes it stands for below - and then its bytes, or for a notice the first
 * and last block numbers.  The sender keeps no list of its blocks: it reads
 * their record headers out of the transmit ring, where they are anyway.
 */
#define WL_HDR 24u
/* frame sizes are said in these units, and the first frames are this long */
#define WL_UNIT  64u
#define WL_HELLO 64u
/* the frame when neither the caller nor SYSTEM.CFG names one */
#define WL_FRAME_DEFAULT 4096u
#define WL_F_DATA 1u
#define WL_F_NAK  2u
#define WL_F_REFUSED 8u
#define WL_F_MASTER 16u
#define WL_F_SEG  4u
#define WL_SEG_SKIP 0x8000u
/* frames a sender may have out and unacknowledged, and the out-of-order
 * frames a receiver will hold beyond its hole */
#define WL_WIN 8u
/* no acknowledgement for this long with frames out: send the first again */
#define WL_RTO_US 50000
/* a receiver says "hole" again for the same hole only after this long */
#define WL_NAK_US 20000
/* the record header in front of every block, in block mode */
#define WL_REC 8u
#define WL_REC_LOST 0x80u
/* a reader waits in slices so it can see the link close under it */
#define WL_WAIT_SLICE_MS 50u
/* the largest frame any wire carries: 8 KB, the longest SPI transfer */
#define WL_FRAME_MAX 8192u

/* ---------------------------------------------------------------------- */
/* Fixed-size rings, no allocation after open                             */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint8_t *mem;
    uint32_t cap; /* any size; indices wrap by remainder */
    uint32_t head; /* next byte to read */
    uint32_t tail; /* next byte to write */
    uint32_t count;
} ring_t;

static inline uint32_t ring_space(const ring_t *r) { return r->cap - r->count; }

/*
 * Copy out without removing.  A block is filled from the ring and then armed,
 * and the arm can be refused (the port is full); consuming the bytes for a
 * block that never went out would lose them.  The head bytes cannot move under
 * this peek, because the only writer appends at the tail.
 */
static inline uint32_t ring_peek(const ring_t *r, uint8_t *dst, uint32_t want)
{
    const uint32_t n = (want < r->count) ? want : r->count;
    const uint32_t first = r->cap - r->head;
    const uint32_t a = (n < first) ? n : first;

    memcpy(dst, r->mem + r->head, a);
    if (n > a) {
        memcpy(dst + a, r->mem, n - a);
    }
    return n;
}

/* Caller holds the lock.  Pairs with ring_peek: the bytes are on the wire now. */
static inline void ring_commit(ring_t *r, uint32_t n)
{
    r->head = (r->head + n) % r->cap;
    r->count -= n;
}

static inline uint32_t ring_put(ring_t *r, const uint8_t *src, uint32_t want)
{
    const uint32_t space = ring_space(r);
    const uint32_t n = (want < space) ? want : space;
    const uint32_t first = r->cap - r->tail;
    const uint32_t a = (n < first) ? n : first;

    memcpy(r->mem + r->tail, src, a);
    if (n > a) {
        memcpy(r->mem, src + a, n - a);
    }
    r->tail = (r->tail + n) % r->cap;
    r->count += n;
    return n;
}

static inline uint32_t ring_take(ring_t *r, uint8_t *dst, uint32_t want)
{
    const uint32_t n = (want < r->count) ? want : r->count;
    const uint32_t first = r->cap - r->head;
    const uint32_t a = (n < first) ? n : first;

    memcpy(dst, r->mem + r->head, a);
    if (n > a) {
        memcpy(dst + a, r->mem, n - a);
    }
    r->head = (r->head + n) % r->cap;
    r->count -= n;
    return n;
}

/* ---------------------------------------------------------------------- */
/* One link's protocol state                                              */
/* ---------------------------------------------------------------------- */

typedef struct {
    /* the lock over all of this, and the three wakes: a reader waiting for
     * bytes, a writer waiting for room, the wire's sending side waiting for
     * something to send */
    ag_port_mutex_t lock;
    ag_port_sem_t   rx_sem;
    ag_port_sem_t   space_sem;
    ag_port_sem_t   tx_sem;
    bool            open;
    bool            framed;

    /* This end's role on the wire, and what it means to the protocol.
     * master: headers say "sent by the master" (flag 16).
     * role_check: a frame from this end's own side is an echo, dropped (SPI).
     * agree_by_len: the frame size is agreed only once a frame of that size
     *   has been received (an SPI slave, whose frames the master clocks).
     * kick_writes / kick_reads: give tx_sem when a writer adds bytes / a
     *   reader makes room - to a sending side that waits on it. */
    bool master;
    bool role_check;
    bool agree_by_len;
    bool kick_writes;
    bool kick_reads;

    /* this end's largest frame (what it allocated for), and the one both
     * run at, the smaller of their two; until they have heard each other,
     * WL_HELLO and no data */
    uint32_t block;
    uint32_t frame;
    bool     agreed;

    uint32_t txcap;
    uint32_t rxcap;
    ring_t   tx;
    ring_t   rx;

    /* the other end's room as its last frame said, and its ack in that frame */
    bool     peer_known;
    uint32_t peer_room;
    uint32_t peer_ack;

    /* the sending half: the stream offset of the ring's head (everything
     * before it acknowledged), bytes past the head sent at least once, the
     * frames out, and a frame to send again */
    uint32_t tx_base;
    uint32_t sent_off;
    struct {
        uint32_t off;
        uint32_t blk; /* block mode: its first record header, >= off */
        uint16_t n;
    } infl[WL_WIN];
    uint32_t infl_n;
    bool     resend;
    bool     resend_whole; /* the receiver refused it: nothing is skipped */
    uint32_t resend_off;
    int64_t  ack_at;
    /* what wl_build did, so a frame that never went out can be taken back */
    struct {
        uint8_t  kind; /* 0 nothing, 1 new frame, 2 a frame sent again */
        uint32_t n;
        uint32_t resend_off;
        uint32_t blk_next;
    } undo;

    /* block mode, sending: where the next record header is in the stream,
     * at or after the first byte not yet sent, and the next block number.
     * A list of the blocks out, 32 of them, held 64-byte blocks to 11% of
     * the wire (measured); the headers in the ring are that list already */
    bool     blocks;
    uint32_t blk_next;
    uint16_t next_block;

    /* the receiving half: everything before rx_next is in the ring; ranges
     * after a hole, already written into their places */
    uint32_t rx_next;
    struct {
        uint32_t off;
        uint32_t n;
    } ooo[WL_WIN];
    uint32_t ooo_n;
    /* a frame this end threw away for want of room: a hole it made itself,
     * which it must remember past the filling of the hole before it */
    bool     refused;
    uint32_t refused_off;
    bool     nak_pending;
    bool     nak_sent;
    uint32_t nak_expect;
    int64_t  nak_at;
    /* test only: drop every Nth data frame on arrival (open flags >> 8) */
    uint32_t drop_every;
    uint32_t drop_count;
    /* the payload of the last frame put in the RX ring */
    uint32_t last_rx_len;

    /* this end's session, the other end's as last heard, and a count that
     * moves when the other end starts over (a block caught across it is cut) */
    uint32_t sess;
    uint32_t peer_sess;
    bool     peer_sess_known;
    uint32_t epoch;

    uint32_t rewinds;
    uint32_t naks;
    uint32_t peer_restarts;
    uint32_t starve_data;
    uint32_t starve_credit;
    uint32_t frames_bad;
    uint32_t seq_gaps;
    uint32_t bytes_dropped;
} wlink_t;

/* Everything a fresh open starts from; the rings and caps are the wire's. */
void wl_reset(wlink_t *w, uint32_t block, uint32_t flags);

/* The checksum of a frame whose payload is `body` bytes. */
uint32_t wl_sum(const uint8_t *f, uint32_t body);
/* A frame's length on the wire, from its header: 0 if `have` bytes are not
 * yet a header, UINT32_MAX if they cannot be one. */
uint32_t wl_frame_len(const uint8_t *f, uint32_t have);

/* Bytes the other end may be sent now: its room less what is on the way. */
uint32_t wl_allowance(const wlink_t *w, uint32_t in_flight);
/* The next frame into `f`: a frame to send again first, then new data up to
 * `allow`.  Returns its data bytes; *wire_len (if asked) is the length of
 * the frame, header included. */
uint32_t wl_build(wlink_t *w, uint8_t *f, uint32_t allow, uint32_t room_less,
                  bool *nak_was, uint32_t *wire_len);
/* Take back what wl_build did, for a frame that never went out. */
void wl_unbuild(wlink_t *w, uint32_t n, bool nak_was);
/* One frame in.  Returns the bytes that reached the reader. */
uint32_t wl_rx(wlink_t *w, const uint8_t *f, uint32_t got);
/* No acknowledgement for too long with frames out: send the first again. */
void wl_rto_check(wlink_t *w);

int32_t wl_write(wlink_t *w, const void *buf, size_t len);
int32_t wl_write_wait(wlink_t *w, const void *buf, size_t len,
                      uint32_t timeout_ms);
int32_t wl_send(wlink_t *w, const void *buf, size_t len, uint32_t flags,
                uint32_t timeout_ms);
int32_t wl_recv(wlink_t *w, void *buf, size_t cap, ag_spilink_block_t *info,
                uint32_t timeout_ms);
int32_t wl_read(wlink_t *w, void *buf, size_t len, uint32_t timeout_ms);
/* The protocol's counters into a stats struct. */
void wl_stats(const wlink_t *w, ag_spilink_stats_t *out);

#endif /* ARGON_NET_WLINK_H */
