/*
 * ArgonOS - NLINK, the link between two boards of one system.
 *
 * RLINK (apps/common/rlink) carries one conversation with a coprocessor: one
 * request outstanding, a reply, and pushes.  That is right for a radio and
 * wrong for a cluster, where a wire has to carry a screen, a sound card, a file
 * being read, and the control traffic that starts a process - all at once, all
 * with different deadlines, and none of them allowed to stop the others.
 *
 * This header is the whole contract and the .c beside it is the whole logic:
 * bytes in, frames out; frames in, bytes out; and the scheduler that decides
 * which queued frame goes next.  No UART, no SPI, no allocation, no clock of
 * its own - so every mistake worth catching is caught on a development machine
 * (host-tests/test_nlink.c), which is the division that keeps rlink and netmsg
 * honest.
 *
 * WHY CLASSES, AND WHY THIS IS THE FIRST THING BUILT
 *
 *   Both times a wire has starved something on this project, the cause was the
 *   same: traffic with a deadline queued behind traffic without one.  The
 *   driver's task sat above applications and never blocked, and a game went
 *   from 40 fps to unplayable; one band of pixels was up to 5 KB and 13 ms of
 *   blocking write, and a sound frame with a 0.7 ms period waited behind it.
 *   Neither was a property of UART.  Both would happen again at 40 MHz on SPI,
 *   just at a different size.
 *
 *   So the link is a scheduler before it is a transport, and the physics
 *   underneath it is a detail that can change later without any of this moving.
 *
 *   NL_CLS_CONTROL   Small, prompt, reliable.  Starting a process, a device
 *                    call, a status answer.  Always goes first; it is bounded
 *                    in size, so "always first" cannot starve anything.
 *
 *   NL_CLS_STREAM    Reserved rate, NOT retransmitted, dropped rather than
 *                    delayed.  Sound and picture.  A retransmitted audio frame
 *                    arrives after the moment it was for; a retransmitted band
 *                    of pixels is a rectangle that the next frame overwrites.
 *                    What this class needs is not reliability but a guaranteed
 *                    slot, which is what the reservation below is.
 *
 *   NL_CLS_BULK      Reliable, windowed, may be delayed as long as necessary.
 *                    A file, an application image, a checkpoint.  It gets what
 *                    is left, and it is the only class that may be made to wait
 *                    for a long time, because it is the only one where waiting
 *                    costs nothing but time.
 *
 * THE TWO NUMBERS THAT MAKE IT WORK
 *
 *   The reservation (bytes per period) is what STREAM may send before BULK is
 *   looked at.  Set it from the stream's real rate: 44.1 kHz stereo is 176 KB/s
 *   and a 10 ms period is 1764 bytes.
 *
 *   The quantum (NL_QUANTUM) is the largest payload any single frame may carry.
 *   It is the whole answer to "how long can a sound frame wait": once a frame
 *   has started going out it cannot be interrupted, so the worst wait is one
 *   quantum at line rate.  256 bytes at 4 Mbaud is 0.64 ms, inside one 44.1 kHz
 *   frame period of 0.73 ms; at 400 KB/s a 5 KB band would have been 13 ms,
 *   which is the measured number this constant exists to prevent.
 *
 * A CHANNEL is one conversation: a remote device, one process's console, one
 * file being read.  Channels are independent - a stalled one does not hold up
 * the others - and the class is a property of the frame, not of the channel, so
 * a device that carries both sound and control uses both.
 *
 * WHAT IS CHECKSUMMED, AND WHY NOT EVERYTHING
 *
 *   The header always: a corrupt header desyncs the link, and sixteen bytes
 *   cost nothing to check.  The payload only when NL_F_PSUM says so, which the
 *   sender sets for reliable classes.  A CRC over the payload costs about five
 *   cycles a byte, which at 5 MB/s is a tenth of a core - real money on this
 *   chip - and for STREAM it buys nothing: the answer to a corrupt audio frame
 *   is to drop it, which is also the answer to a lost one.
 *
 *   They are two separate checksums, and that is not tidiness.  One CRC over
 *   header-and-payload cannot be checked until the payload has been read, and
 *   the length saying how much to read is in the header - so a corrupt header
 *   would be believed exactly long enough to do damage with it.  The header
 *   carries its own CRC and is checked on its own; the payload's, when there is
 *   one, is four bytes AFTER the payload and outside `len`.
 *
 *   So a frame on the wire is:  header (20) + len + (NL_F_PSUM ? 4 : 0).
 *
 * Little-endian on the wire, because both ends that matter are little-endian
 * and saying so once is cheaper than byte-swapping twice.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef AG_NLINK_H
#define AG_NLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NL_MAGIC   0x4B4E4C4Eu /* 'NLNK' little-endian on the wire */
#define NL_VERSION 1u

#define NL_HDR_SIZE 20u

/*
 * The largest payload one frame may carry.  Not a buffer size - a latency
 * budget.  See "THE TWO NUMBERS" above: this is how long anything with a
 * deadline can be made to wait behind a transfer that has already started.
 */
#define NL_QUANTUM 256u

/* Channels.  0 is reserved for the link talking about itself. */
#define NL_CHAN_LINK 0u
#define NL_CHANNELS  16u

enum ag_nlink_class {
    NL_CLS_CONTROL = 0,
    NL_CLS_STREAM = 1,
    NL_CLS_BULK = 2,
    NL_CLASSES = 3,
};

enum ag_nlink_flag {
    NL_F_PSUM = 1u << 0, /* payload carries a CRC; check it            */
    NL_F_EOM = 1u << 1,  /* last frame of a message split by quantum   */
    NL_F_RST = 1u << 2,  /* channel reset: drop what you have of it    */
};

/*
 * On the wire.  `seq` and `ack` count FRAMES per class, not bytes: a byte
 * stream would need the two ends to agree about the size of a frame that was
 * lost, which is exactly the thing they cannot agree about.
 *
 * `credit` is how many more frames this sender can accept on this class - its
 * own receive window, told to the peer on every frame it sends.  Piggybacked
 * because a link that needs a round trip to grant credit is a link whose window
 * is one.
 */
typedef struct ag_nlink_hdr {
    uint32_t magic;
    uint8_t  ver;
    uint8_t  cls;    /* ag_nlink_class */
    uint8_t  flags;  /* ag_nlink_flag bitmask */
    uint8_t  chan;
    uint16_t seq;
    uint16_t ack;
    uint16_t credit;
    uint16_t len;    /* payload bytes following, <= NL_QUANTUM */
    uint32_t crc;    /* over the sixteen header bytes before it, and nothing
                      * else - see "WHAT IS CHECKSUMMED" above */
} ag_nlink_hdr_t;

/* The payload's own checksum, when NL_F_PSUM is set: four bytes after the
 * payload, not counted in `len`. */
#define NL_PSUM_SIZE 4u

/* ------------------------------------------------------------------------ */
/* Codec                                                                     */
/* ------------------------------------------------------------------------ */

/*
 * CRC-32, the one everything else calls IEEE 802.3.  Exposed because the two
 * ends and the tests all have to agree, and because a second implementation
 * somewhere else in this tree is how they would stop agreeing.
 */
uint32_t ag_nlink_crc32(uint32_t seed, const void *data, size_t len);

/*
 * Pack a header into exactly NL_HDR_SIZE bytes.  magic, ver and the CRC are
 * written for the caller, and h->crc is filled in so a caller that keeps the
 * header can compare later.  The payload is not touched: it is checksummed
 * separately, by ag_nlink_put_psum, and only when the sender wants it.
 */
void ag_nlink_pack(uint8_t out[NL_HDR_SIZE], ag_nlink_hdr_t *h);

/*
 * Unpack and check the header, completely, on its own.  False for a wrong
 * magic, a version this build does not speak, a class that does not exist, a
 * length past the quantum, or a CRC that does not come out - all of which mean
 * the same thing on a wire, a desync, and are answered the same way: resync
 * from the next magic, do not trust a byte of this.  Which is why they are one
 * function returning one boolean.
 *
 * A true return means `len` may be believed, which is the point of checking the
 * header before reading what it says to read.
 */
bool ag_nlink_unpack(ag_nlink_hdr_t *h, const uint8_t in[NL_HDR_SIZE]);

/* The payload's four trailing bytes, written and checked.  check_psum answers
 * true for a frame that carries no payload checksum: nothing was claimed, so
 * there is nothing to disagree with. */
void ag_nlink_put_psum(uint8_t out[NL_PSUM_SIZE], const void *payload,
                       uint16_t len);
bool ag_nlink_check_psum(const ag_nlink_hdr_t *h, const void *payload,
                         const uint8_t in[NL_PSUM_SIZE]);

/* ------------------------------------------------------------------------ */
/* The scheduler                                                             */
/* ------------------------------------------------------------------------ */
/*
 * Not a queue of bytes - a decision about which queued frame goes next.  The
 * transport hands it what is waiting and asks; it answers with a class, and the
 * transport sends one frame of that class.  Keeping it this way round is what
 * makes it testable without a wire: the test is a sequence of asks and answers,
 * and the property under test ("a stream frame goes out within one quantum of
 * being offered, whatever bulk is doing") is a statement about that sequence.
 *
 * `reserve` is the STREAM allowance in BYTES per period.  Spent as stream
 * frames go out, refilled by ag_nlink_sched_tick.  A reservation of 0 means
 * STREAM is served strictly after CONTROL and before BULK with no cap, which is
 * the right behaviour for a link carrying no bulk at all and the wrong one for
 * a link carrying a file - so a cluster sets it.
 */
typedef struct ag_nlink_sched {
    uint32_t reserve;    /* stream bytes per period; 0 = uncapped */
    uint32_t stream_left;/* of this period's reserve, unspent */
    /* Bytes waiting, per class.  The transport keeps the frames; this only
     * needs to know whether there are any and how big the next one is. */
    uint32_t waiting[NL_CLASSES];
    uint32_t next_len[NL_CLASSES];
    /* Counters, for the operator and for the tests. */
    uint32_t sent[NL_CLASSES];
    uint32_t stream_starved; /* periods where stream had frames and no reserve */
} ag_nlink_sched_t;

void ag_nlink_sched_init(ag_nlink_sched_t *s, uint32_t reserve_bytes);

/* Tell the scheduler what is queued: `bytes` waiting on `cls`, of which the
 * frame at the head is `head_len` long (0 for both when the class is empty). */
void ag_nlink_sched_offer(ag_nlink_sched_t *s, int cls, uint32_t bytes,
                          uint32_t head_len);

/*
 * Which class sends now, or -1 when nothing may.  CONTROL first and always;
 * then STREAM while its reservation holds; then BULK; then STREAM again if
 * BULK has nothing, because an unspent reservation is not a reason to idle a
 * wire.
 */
int ag_nlink_sched_pick(const ag_nlink_sched_t *s);

/* Account for a frame of `len` bytes just sent on `cls`. */
void ag_nlink_sched_sent(ag_nlink_sched_t *s, int cls, uint32_t len);

/* A new reservation period: refill.  Call it every period the reserve was
 * computed for - 10 ms, normally. */
void ag_nlink_sched_tick(ag_nlink_sched_t *s);

/*
 * The question the whole class scheme exists to answer, in bytes: how long can
 * a frame offered to `cls` right now be made to wait before it goes out?
 * Divide by the line rate for a time.  It is an upper bound, and it is the
 * number to put in a document rather than a hope.
 */
uint32_t ag_nlink_sched_worst_wait(const ag_nlink_sched_t *s, int cls);

/* ------------------------------------------------------------------------ */
/* Reliability: the window, for the classes that have one                    */
/* ------------------------------------------------------------------------ */
/*
 * CONTROL and BULK are delivered or the link is broken; STREAM is neither, on
 * purpose, and does not come through here at all.
 *
 * GO-BACK-N, and the choice is the wire's rather than fashion.  Between two
 * boards on a cable there is no reordering - only loss and corruption - so a
 * receiver that accepts out-of-order frames would carry a reassembly buffer to
 * handle something that cannot happen.  The receiver here keeps one number.
 * The cost is that a lost frame re-sends the ones behind it, which on a window
 * of a few kilobytes is a few kilobytes.
 *
 * NO CLOCK AND NO ALLOCATION.  `now_ms` is passed in, and the slots are the
 * caller's array - so a class that sends small messages rarely (CONTROL) and
 * one that streams a file (BULK) are the same code with different amounts of
 * memory, and a test can drive time faster than it passes.
 *
 * SIZING THE WINDOW is arithmetic, not taste: it has to hold a round trip's
 * worth of data, or the wire idles waiting for acknowledgements.  window =
 * rate * RTT / NL_QUANTUM.  At 5 MB/s and a millisecond that is 5 KB, twenty
 * frames; at 400 KB/s it is two.  Undersize it and the link runs at
 * window * NL_QUANTUM / RTT no matter what the physics can do - which is the
 * 260 KB/s that a one-frame window (send, wait, send) produces on any wire at
 * all, and the reason this exists.
 *
 * SEQUENCE NUMBERS WRAP, and comparing them with < is the oldest bug in this
 * family of protocol.  Use ag_nlink_seq_after.
 */
#define NL_ACK_NONE 0xFFFFu /* "I have accepted nothing yet" */

/* True when `a` is later than `b`, correctly across the 16-bit wrap. */
bool ag_nlink_seq_after(uint16_t a, uint16_t b);

typedef struct ag_nlink_slot {
    uint16_t seq;
    uint16_t len;
    uint8_t  flags;
    uint8_t  chan;
    bool     sent;
    uint16_t tries;  /* > 1 means this one has been round the wire before */
    uint32_t sent_ms;
    uint8_t  data[NL_QUANTUM];
} ag_nlink_slot_t;

typedef struct ag_nlink_tx {
    ag_nlink_slot_t *slot; /* the caller's array, `cap` of them */
    uint16_t         cap;
    uint16_t         count;    /* slots in use, oldest first */
    uint16_t         next_seq; /* the sequence the next queued frame takes */
    uint32_t         rto_ms;
    uint8_t          cls;
    /* Counters.  `retransmits` is the one to watch on a real wire: it is the
     * difference between a link that is working and a link that is working
     * hard, and the two look identical from throughput alone. */
    uint32_t sent;
    uint32_t retransmits;
} ag_nlink_tx_t;

void ag_nlink_tx_init(ag_nlink_tx_t *tx, uint8_t cls, ag_nlink_slot_t *slots,
                      uint16_t cap, uint32_t rto_ms);

/* Room for one more frame in the window. */
bool ag_nlink_tx_room(const ag_nlink_tx_t *tx);

/*
 * Queue one frame's worth.  `len` must be <= NL_QUANTUM - splitting a message
 * into quanta is the caller's job, because only the caller knows where its
 * message boundaries are; set `eom` on the last piece.  False when the window
 * is full, which is back-pressure and not an error: try again after an ack.
 */
bool ag_nlink_tx_queue(ag_nlink_tx_t *tx, uint8_t chan, const void *data,
                       uint16_t len, bool eom);

/*
 * The next frame to put on the wire, or NULL when there is nothing to send at
 * this instant.  It is a fresh frame when one is queued, otherwise the oldest
 * unacknowledged one whose retransmission is due.  The returned slot belongs to
 * the tx and stays valid until the next call.
 */
const ag_nlink_slot_t *ag_nlink_tx_next(ag_nlink_tx_t *tx, uint32_t now_ms);

/* The peer's cumulative acknowledgement: it has everything up to and including
 * `ack`.  NL_ACK_NONE means it has nothing yet. */
void ag_nlink_tx_ack(ag_nlink_tx_t *tx, uint16_t ack);

/* Frames queued or in flight.  Zero means everything has been acknowledged. */
uint16_t ag_nlink_tx_pending(const ag_nlink_tx_t *tx);

/* Bytes the scheduler should be told are waiting on this class, and the length
 * of the frame at the head - the two arguments ag_nlink_sched_offer wants. */
uint32_t ag_nlink_tx_waiting_bytes(const ag_nlink_tx_t *tx);
uint32_t ag_nlink_tx_head_len(const ag_nlink_tx_t *tx);

typedef struct ag_nlink_rx {
    uint16_t last;   /* the highest sequence accepted, or NL_ACK_NONE */
    bool     any;    /* anything accepted at all yet */
    uint32_t taken;
    uint32_t dups;   /* already had it: the ack was lost, not the frame */
    uint32_t gaps;   /* out of order: something ahead of it was lost */
} ag_nlink_rx_t;

void ag_nlink_rx_init(ag_nlink_rx_t *rx);

/*
 * Offer a received frame.  True when it is the next one expected and should be
 * delivered; false when it is a duplicate or arrives out of order, both of
 * which are discarded.  Either way ag_nlink_rx_ack afterwards is what to put in
 * the next frame going the other way - a duplicate is answered by re-acking,
 * because a duplicate means the previous acknowledgement was what got lost.
 */
bool ag_nlink_rx_accept(ag_nlink_rx_t *rx, const ag_nlink_hdr_t *h);

uint16_t ag_nlink_rx_ack(const ag_nlink_rx_t *rx);

#endif /* AG_NLINK_H */
