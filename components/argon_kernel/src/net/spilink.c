/*
 * ArgonOS - the SPI slave link service: a byte stream, kept armed.
 *
 * The peer owns the clock and the select on this wire, so a block that arrives
 * while this end has nothing armed is not delayed, it is GONE.  The slave side
 * was losing them exactly there: with the driver arming one transaction per
 * call, the bench run accepted 92 frames while logging 238 gaps and 99 bad, and
 * the master retransmitted 87% of what it sent.  Nothing was wrong with the
 * wire.  Between one transaction completing and the next being queued there was
 * nobody listening, and closing that window is the whole job of this file.
 *
 * WHY A KERNEL TASK AND NOT A .SYS SERVICE TASK
 *   Because the priority has to be chosen, and a driver is not offered the
 *   choice: task->svc_start takes no priority argument on purpose (ABI 0.50 -
 *   "a driver task above applications has cost this project a game").  The
 *   kernel has no such rule, and this is a service of the system's, not of one
 *   driver's.
 *
 * THE PRIORITY IS 9, and the numbers behind it:
 *   Applications are mapped in src/proc/proc.c, prio_for():
 *     AG_PRIO_LOW -> 2, AG_PRIO_NORMAL -> 5, AG_PRIO_HIGH -> 8.
 *   The kernel's own tasks, as they stand in the tree:
 *     1  ag_link (linkprov), rdevpour and rdevsrv (noderdev), nodeclu,
 *        noderun (nodeproc), ag_handover and ag_standby (nodestandby)
 *     4  ag_hwait, 5 ag_hpad (hostfs)
 *     6  ag_telnet, ag_ssh
 *     10 ag_con (console), 15 ag_super (supervisor)
 *   So 9 is above every priority an application can be given and below the
 *   console: a link that misbehaves cannot take the prompt away from whoever is
 *   standing at the board, which is what "below the console" is for.  It is a
 *   deliberate exception to "every kernel service runs at 1": those run at 1
 *   because they must lose to applications, and this one must not lose to an
 *   application that is not clocking anything - it is the listening end of a
 *   wire, and a block missed is a block the far side has to send again.
 *
 * THE INVARIANT THAT MATTERS: THE QUEUE IS NEVER EMPTY
 *   The port's slave pool is four transactions deep, and slot 0 of it belongs
 *   for good to spi_slave_xfer, which the rest of the system still uses.  So
 *   this arms THREE, which is all a link may have, and replaces each one as it
 *   completes, one at a time: a completion is always followed by two blocks
 *   still in the driver.  Nothing here has to be fast for the window to stay
 *   shut - the port's own interrupt pulls the next queued descriptor on the
 *   select pulse - so the task's job is only to keep the queue from running
 *   dry, and a late task costs a block only once all three are spent.  With
 *   nothing to send it arms with `tx = NULL`, which the port clocks as zeros:
 *   on this wire the master clocks whether or not the slave has anything to
 *   say, and "nothing to send" is not a reason to stop listening.
 *
 * IT NEVER SPINS.  THIS IS NOT A STYLE RULE.
 *   A task at priority 9 that busy-waits IS a board that looks hung.  It
 *   outranks every application (2/5/8) and it holds the CPU it is given, so a
 *   loop that retries arm/reap without blocking starves everything the operator
 *   can see: the first hardware round with this service had five sessions in a
 *   row in which EVERY command on the slave timed out at 30 s, including the
 *   first one, with no transfer involved and nothing printed, because the
 *   console never got a turn.  So every path that leaves this task with nothing
 *   to do blocks - and blocks for at least a whole tick, because
 *   ag_port_ms_to_ticks(1) rounds to ZERO on a slow tick and vTaskDelay(0) does
 *   not block at all.  That rounding is how a "5 ms backoff" became a spin.
 *   The priority is only safe while the task is blocked nearly all the time;
 *   when the wire is busy it is blocked inside the port waiting for a block,
 *   and when the wire is quiet it is blocked in a delay.  Both are blocking.
 *
 * WHEN THE PEER GOES QUIET: RECOVERY, BOUNDED AND LOUD
 *   A slot is freed only when its owner reaps it, and a block the peer never
 *   clocks is never reaped and never given back.  So armed-but-never-clockable
 *   blocks are a pool that stays full for ever: arm returns -AG_EAGAIN, the
 *   task has nothing to do, and the link cannot come back without a reflash.
 *   That is not a link, it is a board that has to be power-cycled.
 *
 *   After SPILINK_STALL_MS (one second) with no block clocked, the service
 *   tears the slave interface down and builds it again, which is the only thing
 *   that abandons armed slots.  One second, and the numbers behind it: a block
 *   on this wire is about 305 us at 512 bytes and 20 MHz, an idle master still
 *   clocks a block every 2-10 ms (LSPI_IDLE_MS in the driver), and the link's
 *   own retransmission timeout is 150 ms.  A second is therefore two to three
 *   orders of magnitude past any quiet a live peer shows, and it is short
 *   enough that a board recovers inside the session that noticed - which is the
 *   whole point, since the failure being fixed could only be cleared by
 *   reflashing.
 *
 *   A recovery is real work, so it is counted and said out loud rather than
 *   done quietly: `recoveries` in the counters, and one line from the stats
 *   call the next time anybody asks.  A wire that needs a recovery a second is
 *   a wire problem, and the count is how that is told from a service problem.
 *
 * WHAT THIS KNOWS NOTHING ABOUT
 *   Frames.  A block boundary is not a frame boundary: this moves bytes in both
 *   directions and has never heard of NLINK, a channel or a class.  The driver
 *   that consumes the stream does the framing, and that division is the point -
 *   a service that parsed would have to be changed by every protocol above it.
 *   A short block - a master that clocked fewer bytes than were armed - is
 *   ordinary and simply puts fewer bytes in the RX ring; nothing here looks for
 *   a boundary it could mis-read.
 *
 * THE SERVICE TASK DOES NOT LOG; THE STATS CALL DOES.
 *   The one path that stops this task from outside is a device revoke, which
 *   runs with the device registry lock held, and the console lock sits
 *   underneath that lock in the console task.  A log call from the task could
 *   close that ring and hang the board - so it never makes one, and the same
 *   numbers are instead read through ag_spilink_stats (ABI 0.57, io->
 *   spi_link_stats), which runs on the CALLER's task and prints the one line
 *   about a stall, a rebuild or a refusal, rate-limited.  Keeping the numbers
 *   for close alone was the mistake the first hardware round exposed: the
 *   failure that mattered hung the board before anything could close.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/spilink.h>

#include <string.h>

#include <argon/board.h>
#include <argon/device.h>
#include <argon/kernel.h>
#include <argon/log.h>
#include <argon/module.h>

#include <argon/port/io.h>
#include <argon/port/mem.h>
#include <argon/port/sync.h>
#include <argon/port/task.h>
#include <argon/port/time.h>

#include "core/sysconfig.h"

/*
 * Three, and not four: the port's slave pool is AG_PORT_SPI_SLAVE_QUEUE deep
 * (io_hw.c) and slot 0 of it belongs for good to spi_slave_xfer, which the rest
 * of the system still uses.  Three is therefore every transaction a link may
 * have armed, and asking for a fourth would only be bookkeeping for one the
 * port will refuse.  Three is about a millisecond of slack at this wire's block
 * size, which is far more than a task at this priority needs.
 */
#define SPILINK_SLOTS 3u

/*
 * The raw link's ring sizes.  (Indices wrap by remainder, so a framed link can
 * size its rings by the frame - see ag_spilink_open2.)
 *
 * THE TWO RINGS DO NOT CARRY THE SAME THING, and sizing them alike was the
 * mistake this comment used to make.  The transmit ring holds what this board
 * chose to say: frames, and never more than a round trip's worth, so eight
 * blocks of 512 is generous and 4096 stays.
 *
 * THE RECEIVE RING HOLDS EVERY BYTE THE PEER CLOCKS, AND MOST OF THEM ARE
 * PADDING.  A master with nothing to send still clocks a whole block and fills
 * it with zeros (linkspi.c, clock_block), because a block is the only way the
 * other end gets to speak.  Measured on the real pair, one 19 KB fetch: 826
 * blocks clocked, 69 of them carrying anything - so 423 KB went through this
 * ring to deliver 35 KB, and the ring lost 3584 bytes of it ("bytes with no
 * room", seven whole blocks).  What it loses is not padding, because padding is
 * most of what is there: it is whatever arrived when the ring happened to be
 * full, and an acknowledgement lost that way costs a retransmission window.
 *
 * So the receive side is four times the transmit side.  That is a floor under
 * a burst, not a cure: the cure is for the master to stop sending half a
 * megabyte of zeros, and that is a change to the block loop rather than to a
 * constant.  This one is what makes the measurement of that change readable.
 */
#define SPILINK_TX_RING 4096u
#define SPILINK_RX_RING 16384u

#define SPILINK_TASK_STACK 4096u

/* Above every application, below the console.  See the note at the top. */
#define SPILINK_PRIO 9u

/*
 * How long one reap waits before the loop looks at the world again.  It bounds
 * two things: how long a close has to wait for the task to notice it must stop,
 * and how often an idle link wakes up.  Twenty milliseconds is short enough
 * that a close is not a stall - a device revoke runs holding the registry lock,
 * and that path closes this too - and long enough that an idle wire costs fifty
 * wake-ups a second.
 */
#define SPILINK_REAP_MS 20u

/* Patience for a task that should have stopped within SPILINK_REAP_MS. */
#define SPILINK_STOP_WAIT_MS 200u

/* A reader waits in slices so it can see the link close under it. */
#define SPILINK_WAIT_SLICE_MS 50u

/*
 * The shortest sleep this task will ever take, and it exists because
 * ag_port_ms_to_ticks can round a millisecond DOWN TO ZERO: on a 100 Hz tick
 * every value below 10 ms becomes nothing at all, and vTaskDelay(0) does not
 * block.  A backoff that rounds to zero is not a backoff - it is the spin that
 * starved the console - so every wait goes through sleep_at_least(), which
 * never asks for less than one tick.
 */
#define SPILINK_IDLE_MS 1u

/*
 * How long the wire may be silent before this end gives up on the blocks it has
 * armed and rebuilds the slave interface.
 *
 * A slot is freed only by its owner reaping it, and a block the peer never
 * clocks is never reaped, so armed-forever blocks are a pool that stays full
 * and a link that cannot recover without a reflash.  Justification for the
 * number is in the header comment: a live wire's quiet is 2-10 ms, its
 * retransmission timeout is 150 ms, and a second is orders of magnitude past
 * both while still being short enough to recover inside the session.
 */
#define SPILINK_STALL_MS 1000u

/* One fault line at most this often, and only while something is wrong.  The
 * stats call prints it, so a driver polling its link cannot fill the log. */
#define SPILINK_FAULT_REPORT_MS 5000u

/*
 * FRAMED: EVERY TRANSACTION SAYS WHAT IN IT IS DATA (ABI 0.60).
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
 *   16-19 checksum of bytes 0..15 and the payload
 *
 * The stream is the payloads, in order.  A side with nothing to say still
 * sends a header: that is how the other side learns its room and its ack.
 *
 * THE FRAME IS THE SMALLER OF THE TWO ENDS' (24 Sep).  Each end opens with
 * its own largest frame - the caller's, or `frame` under [wire] in its
 * SYSTEM.CFG - and allocates for that alone, so a C3 that says 2048 pays for
 * 2 KB frames whatever its peer can take.  Byte 3 of every header says it.
 * The master clocks SPILINK_HELLO-byte frames until it has read a header of
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
#define SPILINK_HDR 20u
/* frame sizes are said in these units, and the first frames are this long */
#define SPILINK_UNIT  64u
#define SPILINK_HELLO 64u
/* the frame when neither the caller nor SYSTEM.CFG names one */
#define SPILINK_FRAME_DEFAULT 4096u
#define SPILINK_F_DATA 1u
#define SPILINK_F_NAK  2u
#define SPILINK_F_REFUSED 8u
#define SPILINK_F_MASTER 16u
#define SPILINK_F_SEG  4u
#define SPILINK_SEG_SKIP 0x8000u
/* frames a sender may have out and unacknowledged, and the out-of-order
 * frames a receiver will hold beyond its hole */
#define SPILINK_WIN 8u
/* no acknowledgement for this long with frames out: send the first again */
#define SPILINK_RTO_US 50000
/* a receiver says "hole" again for the same hole only after this long */
#define SPILINK_NAK_US 20000
/* the record header in front of every block, in block mode */
#define SPILINK_REC 8u
#define SPILINK_REC_LOST 0x80u
/*
 * TWO FRAMES AHEAD, in a framed link, and it is a constant of the protocol
 * rather than of one end: the slave keeps this many frames armed, and the
 * master takes the payloads of its last this-many frames off the room the
 * slave reported, because that room was reckoned when the slave armed the
 * frame - this many frames before it was clocked.
 *
 * Two and not the raw link's three because of memory: a frame of 4 KB costs
 * the slave two buffers per armed frame, and on the C3 three of them plus the
 * rings did not fit in the largest free block once an application was
 * loaded.  Two leaves the slave a whole frame's time to re-arm - 1.6 ms at
 * 20 MHz, 0.8 ms at 40 - which for a task at priority 9 is a great deal.
 */
#define SPILINK_AHEAD 2u

/*
 * How often an idle MASTER clocks a frame anyway.  The slave cannot start a
 * transaction, so a master that clocked only when it had something to say
 * would never hear the slave: this is the slave's latency when both are
 * quiet, and it is a frame of bus time every 2 ms.  The moment either side
 * carries data the master stops waiting and clocks back to back.
 */
#define SPILINK_POLL_MS 2u

/* How long after the last frame that carried data the master keeps clocking
 * back to back before it falls back to the idle poll.  See the master task. */
#define SPILINK_HOT_US 5000

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

static uint32_t ring_space(const ring_t *r) { return r->cap - r->count; }

/*
 * Copy out without removing.  A block is filled from the ring and then armed,
 * and the arm can be refused (the port is full); consuming the bytes for a
 * block that never went out would lose them.  The head bytes cannot move under
 * this peek, because the only writer appends at the tail.
 */
static uint32_t ring_peek(const ring_t *r, uint8_t *dst, uint32_t want)
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
static void ring_commit(ring_t *r, uint32_t n)
{
    r->head = (r->head + n) % r->cap;
    r->count -= n;
}

static uint32_t ring_put(ring_t *r, const uint8_t *src, uint32_t want)
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

static uint32_t ring_take(ring_t *r, uint8_t *dst, uint32_t want)
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
/* One open link                                                          */
/* ---------------------------------------------------------------------- */

typedef struct {
    bool     open;
    int      bus;
    int      cs;
    uint32_t khz;
    uint32_t block;
    /* Which end of the clock, and whether the transactions carry headers. */
    bool     master;
    bool     framed;
    /* Frames a slave keeps armed: SPILINK_SLOTS raw, SPILINK_AHEAD framed. */
    uint32_t nslots;
    /* The two rings' sizes for this open: framed links size them by frame. */
    uint32_t txcap;
    uint32_t rxcap;
    /* A master's frame pair, transmit then receive, DMA-capable. */
    uint8_t *mbuf;
    /*
     * Set when the frames and both rings are one allocation, starting at the
     * frames (slots or mbuf): then that pointer is the only one to free.
     */
    bool     onepiece;
    /*
     * Framed bookkeeping.  peer_room is the last room the other side said it
     * had; reaped_used (slave) is the payload of the frame just reaped, and
     * hist[] (master) the payloads of the last SLOTS frames - both are bytes
     * on their way that the room they are compared with did not yet count.
     */
    bool     peer_known;
    /* the sending half: the stream offset of the ring's head (everything
     * before it acknowledged), bytes past the head sent at least once, the
     * frames out, and a frame to send again */
    uint32_t tx_base;
    uint32_t sent_off;
    struct {
        uint32_t off;
        uint32_t blk; /* block mode: its first record header, >= off */
        uint16_t n;
    } infl[SPILINK_WIN];
    uint32_t infl_n;
    bool     resend;
    bool     resend_whole; /* the receiver refused it: nothing is skipped */
    uint32_t resend_off;
    int64_t  ack_at;
    /* the frame both ends run at, the smaller of their two; until they have
     * heard each other, SPILINK_HELLO and no data */
    uint32_t frame;
    bool     agreed;
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
    } ooo[SPILINK_WIN];
    uint32_t ooo_n;
    /* a frame this end threw away for want of room: a hole it made itself,
     * which it must remember past the filling of the hole before it */
    bool     refused;
    uint32_t refused_off;
    bool     nak_pending;
    bool     nak_sent;
    uint32_t nak_expect;
    int64_t  nak_at;
    /* test only: drop every Nth data frame on arrival (open2 flags >> 8) */
    uint32_t drop_every;
    uint32_t drop_count;
    /* the payload of the last frame put in the RX ring: counted as room,
     * because the reader takes it before the room is next looked at, and a
     * frame that turns out not to fit is simply not acknowledged */
    uint32_t last_rx_len;
    uint32_t rewinds;
    uint32_t naks;
    uint32_t starve_data;
    uint32_t starve_credit;
    uint32_t peer_room;
    uint32_t reaped_used;
    uint32_t hist[SPILINK_SLOTS];
    uint32_t hist_at;
    /* Written by a writer, taken by an idle master so it clocks at once. */
    ag_port_sem_t tx_sem;
    /* Given by the service whenever it frees transmit ring: a writer that
     * found the ring full waits on this instead of guessing with a sleep. */
    ag_port_sem_t space_sem;
    uint32_t frames_bad;
    uint32_t seq_gaps;
    uint32_t wire_us;
    uint32_t loop_us;
    uint32_t reorders;
    uint32_t short_frames;
    /*
     * The bus's pins, as BOARD.CFG gave them to the io layer at bring-up.  Kept
     * only so that a recovery can build the slave interface again if the port's
     * reset could not - the port leaves the bus closed when that fails, and
     * opening it as a slave is the only thing that brings it back.
     */
    int sck;
    int mosi;
    int miso;

    /*
     * The module that opened this, or NULL for the kernel.  Kept so that a
     * driver which unloads without closing cannot leave this task running: the
     * device registry tells every watcher when a module's devices go away, and
     * the watcher below matches on this.
     */
    const void *owner;

    /*
     * The armed blocks: SPILINK_SLOTS transmit blocks first, then the same
     * number of receive blocks.  Allocated once, at open, word-aligned and
     * DMA-capable, and handed to the port for as long as a transaction is in
     * flight.  The port uses them as the DMA engine's buffers and keeps the
     * pointers until the block is reaped, so one pair per transaction is the
     * only arrangement with no reuse window in it.
     */
    uint8_t *slots;

    uint8_t *txmem;
    uint8_t *rxmem;
    ring_t   tx;
    ring_t   rx;

    /*
     * The order the transactions were armed in.  The port brings blocks back
     * in that order, so a completion belongs to the buffers at the head of this
     * queue.  The slot number the port names is its own numbering, not an index
     * into anything here - slot 0 of its pool is spi_slave_xfer's and is never
     * handed to this service - so the arm order is what maps a completion to
     * the caller's buffers, and it is also what keeps every armed slot reaped
     * before its memory is armed again.
     */
    uint8_t  armq[SPILINK_SLOTS];
    uint32_t qhead;
    uint32_t qcount;
    /*
     * Per slot: the real bytes armed into it and whether it is still in
     * flight.  Touched only by the service task, which is why they need no
     * lock - nothing else reads them while the task runs.
     */
    uint32_t used[SPILINK_SLOTS];
    bool     live[SPILINK_SLOTS];
    uint32_t armed;

    volatile bool  stop;
    ag_port_task_t task;

    /*
     * The lock and the two semaphores are created on the first open and kept
     * for the boot.  A reader blocked on the wake semaphore must not have it
     * freed underneath it, and whoever closes the link is not necessarily the
     * task that was reading; four small objects per bus is the price of not
     * having a use-after-free there.
     */
    ag_port_mutex_t lock;
    ag_port_sem_t   rx_sem;
    ag_port_sem_t   exit_sem;

    /* Written by the service task, read by close and by the stats call. */
    uint32_t blocks_clocked;
    uint32_t blocks_data;
    uint32_t blocks_unarmed;
    uint32_t bytes_dropped;
    uint32_t timeouts;
    uint32_t stalls;
    uint32_t recoveries;
    uint32_t errors;
    ag_err_t last_error;
    /*
     * Liveness for the stall detector and for the rate-limited fault line, in
     * the port's own ticks: when the current silence started (with a flag,
     * because tick zero is a real time and "never" has to be told from it), and
     * when a fault was last printed.  Unsigned tick subtraction wraps
     * correctly, so nothing here needs a wall clock.
     */
    ag_port_ticks_t quiet_since;
    bool            quiet_started;
    ag_port_ticks_t last_fault;
    bool            fault_reported;
} spilink_t;

static spilink_t s_link[AG_SPI_BUSES];

static int link_index(int bus)
{
    const int idx = bus - AG_SPI_FIRST;
    return (idx >= 0 && idx < AG_SPI_BUSES) ? idx : -1;
}

static void lock(spilink_t *s)
{
    (void)ag_port_mutex_take(s->lock, AG_PORT_FOREVER);
}

static void unlock(spilink_t *s) { (void)ag_port_mutex_give(s->lock); }

static uint8_t *slot_tx(const spilink_t *s, unsigned i)
{
    return s->slots + (size_t)i * s->block;
}

static uint8_t *slot_rx(const spilink_t *s, unsigned i)
{
    return s->slots + (size_t)(s->nslots + i) * s->block;
}

/*
 * The counters, written under the lock the reader takes.
 *
 * They are no longer "written by the task and read once it has stopped": the
 * stats call reads them from another task while this one runs, which is the
 * whole point - a counter readable only after the link closes is no use on the
 * failure that hangs the board before anything closes.  A mutex pair per
 * counter per block costs a couple of microseconds against a 305 us block, and
 * buys reads that are never torn or half-updated.
 */
static void count(spilink_t *s, uint32_t *field, uint32_t by)
{
    lock(s);
    *field += by;
    unlock(s);
}

/* Counted, never logged from the task: the service task must not log.  See the
 * note at the top of this file. */
static void note_error(spilink_t *s, ag_err_t rc)
{
    lock(s);
    if (s->errors == 0u) {
        s->last_error = rc;
    }
    s->errors++;
    unlock(s);
}

/*
 * The only delay this file uses, and it never rounds to zero.
 *
 * ag_port_ms_to_ticks(1) is 0 ticks on a tick slower than a kilohertz, and
 * vTaskDelay(0) returns without blocking - which is exactly how a "5 ms
 * backoff" turned into a spin at a priority above every application, and into
 * five bench sessions where every command timed out at 30 s.  A sleep that can
 * become a no-op is not a sleep.
 */
/*
 * Ticks for a wait, never zero.
 *
 * ag_port_ms_to_ticks rounds DOWN, so on a tick slower than a kilohertz every
 * value below 10 ms becomes nothing at all, and a wait of no ticks is not a
 * wait - it returns at once.  Every wait in this file goes through here,
 * because a wait that can silently become a poll is how a backoff turned into a
 * spin at a priority above every application, and how a read would have turned
 * into a busy loop on the caller's task.
 */
static ag_port_ticks_t ticks_at_least(uint32_t ms)
{
    const ag_port_ticks_t ticks = ag_port_ms_to_ticks(ms);
    return (ticks == 0u) ? 1u : ticks;
}

static void sleep_at_least(uint32_t ms) { ag_port_task_delay(ticks_at_least(ms)); }

/*
 * Give up on the armed blocks and start the slave interface over.
 *
 * Called by the service task only, with nothing held.  The rings are NOT
 * touched: bytes already in them are data the upper layer has not read, and
 * nothing that was armed had been clocked, so no byte was sent and none was
 * received - the only thing lost is the blocks themselves.
 *
 * It cannot wedge the task: both calls are local to the port and bounded by its
 * own teardown and re-initialisation - the port says so of reset, and
 * slave_open has never blocked on a peer - and whatever they return, the loop
 * goes on to arm or to sleep.  It cannot run in a tight loop either, since the
 * caller reaches it only after the stall timer and resets that timer on the way
 * out, so a wire that has died for good rebuilds about once a second and says
 * so through `recoveries`, which is the loud part.
 */
static void slave_rebuild(spilink_t *s)
{
    /*
     * The port frees a slot only when its owner reaps it and has no flush by
     * design, so blocks the peer never clocked are held for ever - and three of
     * those are the whole pool this service is allowed.  reset throws the
     * interface away and builds it again from the same pins, select and depth,
     * and guarantees every slot is free when it answers AG_OK.
     *
     * Two things make it admissible at all, and both are properties of the
     * caller rather than of this call.  It must come from the task that reaps -
     * the port refuses it while a reaper is inside the driver's result call,
     * because freeing the queues under a blocked receiver is a use-after-free -
     * and this is the only reaper there is.  And the reap above it must be
     * BOUNDED: SPILINK_REAP_MS is 20 ms and never AG_PORT_FOREVER, since a
     * reaper parked in the port for ever would hold the very state reset has to
     * take, and a recovery that can never be admitted is the same as no
     * recovery.  Neither is an accident; changing either one disables this.
     *
     * It also takes the port's mutexes, so it is not callable from an ISR, and
     * it is called only from this task's loop.
     *
     * The pad drive strength SURVIVES the teardown - gpio_reset_pin writes the
     * pin mux and the pulls and never FUN_DRV, and the SPI driver does not
     * touch it - so a recovery cannot silently undo the `drive` this link
     * depends on.  Had that gone the other way, every recovery would have
     * turned a working link into the zero-frames-in-four-thousand failure mode
     * and it would have looked like the recovery itself was broken.
     */
    ag_err_t err = ag_port_spi_slave_reset(s->bus);

    if (err == -AG_ENODEV) {
        /*
         * Nothing to reset, or a rebuild that failed and left the bus closed -
         * where arm answers -AG_ENODEV and the port says a fresh slave open is
         * the only way back.  The pins are the ones BOARD.CFG gave at bring-up,
         * so this is not a second reading of the configuration.  If THIS fails
         * the bus is gone and the counters (errors, recoveries) and the fault
         * line the stats call prints are what say so.
         */
        err = ag_port_spi_slave_open(s->bus, s->sck, s->mosi, s->miso, s->cs,
                                     s->khz);
    }

    if (err != AG_OK) {
        /*
         * Counted, never spun on.  -AG_EBUSY is somebody in the way - a
         * one-at-a-time transfer on this bus, or a reaper inside the driver's
         * result call - and there the interface is still up and the next stall
         * window tries again.  Anything else is the bus refusing.  This is why
         * the loop's sleep is at the bottom and not inside this function.
         */
        note_error(s, err);
        return;
    }

    /*
     * Resynchronise EVERYTHING this service believed about the wire.  The port
     * guarantees the pool and not the wire: every block that was armed is gone,
     * nothing that completes from here answers anything armed before here, and
     * so the arm queue must be emptied rather than left holding slots that no
     * longer exist.  Reading it as an empty queue and a free pool is what makes
     * the next top-up arm three fresh blocks.
     *
     * The rings are NOT touched: bytes already in them are the upper layer's
     * and were not affected by what was in flight.  Nothing that was armed had
     * been clocked, so no byte was sent and none was lost beyond the protocol's
     * own need to resynchronise.
     */
    memset(s->live, 0, sizeof(s->live));
    memset(s->used, 0, sizeof(s->used));
    s->qhead = 0;
    s->qcount = 0;
    s->armed = 0;
    count(s, &s->recoveries, 1u);
}

/* ---------------------------------------------------------------------- */
/* The service task                                                       */
/* ---------------------------------------------------------------------- */

static int find_free(const spilink_t *s)
{
    for (unsigned i = 0; i < s->nslots; i++) {
        if (!s->live[i]) {
            return (int)i;
        }
    }
    return -1;
}

static void armq_push(spilink_t *s, uint8_t slot)
{
    s->armq[(s->qhead + s->qcount) % s->nslots] = slot;
    s->qcount++;
}

/* Take `slot` out of the arm queue wherever it is, keeping the rest in order. */
static void armq_remove(spilink_t *s, uint8_t slot)
{
    uint8_t  keep[SPILINK_SLOTS];
    uint32_t n = 0;
    for (uint32_t k = 0; k < s->qcount; k++) {
        const uint8_t v = s->armq[(s->qhead + k) % s->nslots];
        if (v != slot) {
            keep[n++] = v;
        }
    }
    s->qhead = 0;
    s->qcount = n;
    for (uint32_t k = 0; k < n; k++) {
        s->armq[k] = keep[k];
    }
}

/*
 * Fill one slot from the TX ring, zero-pad the tail, and arm it.  The bytes are
 * taken out of the ring only once a transaction is carrying them: committing
 * first would lose them every time the port refused the arm.
 *
 * With nothing to send the block is armed with `tx = NULL`, which the port
 * clocks as zeros without this layer holding or clearing a zero block - the
 * cheap side of the never-empty invariant, which matters because on this wire
 * most blocks of a quiet link are exactly that.
 */
static uint32_t frame_sum(const uint8_t *f, uint32_t len)
{
    /* two running sums over words: cheap enough for 4 KB a frame on a C3,
     * and a flipped bit, a dropped byte or a swapped word all change it */
    uint32_t a = 0x6A09E667u;
    uint32_t b = 0;
    const uint32_t *w = (const uint32_t *)f;
    for (unsigned k = 0; k < 4u; k++) {
        a += w[k];
        b += a;
    }
    const uint8_t  *p = f + SPILINK_HDR;
    const uint32_t *pw = (const uint32_t *)p;
    const uint32_t  words = len / 4u;
    for (uint32_t k = 0; k < words; k++) {
        a += pw[k];
        b += a;
    }
    for (uint32_t k = words * 4u; k < len; k++) {
        a += p[k];
        b += a;
    }
    return a ^ ((b << 13) | (b >> 19));
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

static uint32_t get16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
    return get16(p) | (get16(p + 2) << 16);
}

/* Bytes the peer may be sent now: its room, less what is already on the way. */
static uint32_t peer_allowance(const spilink_t *s, uint32_t in_flight)
{
    if (!s->agreed || !s->peer_known || s->peer_room <= in_flight) {
        return 0u;
    }
    return s->peer_room - in_flight;
}

/* Copy out without removing, starting `off` bytes past the head. */
static uint32_t ring_peek_at(const ring_t *r, uint32_t off, uint8_t *dst,
                             uint32_t want)
{
    if (off >= r->count) {
        return 0u;
    }
    const uint32_t avail = r->count - off;
    const uint32_t n = (want < avail) ? want : avail;
    const uint32_t start = (r->head + off) % r->cap;
    const uint32_t first = r->cap - start;
    const uint32_t a0 = (n < first) ? n : first;
    memcpy(dst, r->mem + start, a0);
    if (n > a0) {
        memcpy(dst + a0, r->mem, n - a0);
    }
    return n;
}

/* Write `n` bytes `delta` past the tail, not yet counted as in the ring;
 * src NULL writes zeros.  The caller has checked delta + n fits. */
static void ring_write_at(ring_t *r, uint32_t delta, const uint8_t *src,
                          uint32_t n)
{
    const uint32_t start = (r->tail + delta) % r->cap;
    const uint32_t first = r->cap - start;
    const uint32_t a0 = (n < first) ? n : first;
    if (src != NULL) {
        memcpy(r->mem + start, src, a0);
        if (n > a0) {
            memcpy(r->mem, src + a0, n - a0);
        }
    } else {
        memset(r->mem + start, 0, a0);
        if (n > a0) {
            memset(r->mem, 0, n - a0);
        }
    }
}

static void ring_advance(ring_t *r, uint32_t n)
{
    r->tail = (r->tail + n) % r->cap;
    r->count += n;
}

/* Receive ring bytes beyond the tail already taken by frames after a hole. */
static uint32_t ooo_reserved(const spilink_t *s)
{
    if (s->ooo_n == 0u) {
        return 0u;
    }
    return s->ooo[s->ooo_n - 1u].off + s->ooo[s->ooo_n - 1u].n - s->rx_next;
}

/* What frame_build did, so a slave whose arm is refused can take it back. */
static struct {
    uint8_t  kind; /* 0 nothing, 1 new frame, 2 a frame sent again */
    uint32_t n;
    uint32_t resend_off;
    uint32_t blk_next;
} s_undo[AG_SPI_BUSES];

/*
 * A lost frame of blocks, as segments: stream bytes [off, off + n), whose
 * first record header is at `blk`, into `p`.  Returns the payload length,
 * or 0 if no block in it may be skipped or the segments would not fit
 * `pay` - the frame then goes again as it was.
 */
static uint32_t seg_build(const spilink_t *s, uint32_t off, uint32_t n,
                          uint32_t blk, uint8_t *p, uint32_t pay,
                          uint32_t *data)
{
    const uint32_t end = off + n;
    uint32_t       used = 2;
    uint32_t       open = 0; /* the data segment being added to, or 0 */
    uint32_t       pos = off;
    uint32_t       b = blk;
    bool           skipped = false;
    *data = 0;
    while ((int32_t)(pos - end) < 0) {
        uint32_t upto;
        if ((int32_t)(pos - b) < 0) {
            upto = ((int32_t)(b - end) < 0) ? b : end;
        } else {
            uint8_t h[SPILINK_REC];
            (void)ring_peek_at(&s->tx, b - s->tx_base, h, SPILINK_REC);
            uint32_t bend = b + SPILINK_REC + get16(h);
            if (!(h[4] & AG_SPILINK_RELIABLE) && (int32_t)(bend - end) <= 0) {
                /* a run of whole unreliable blocks: one notice */
                const uint32_t first = get16(h + 2);
                uint32_t       last = first;
                while ((int32_t)(bend - end) < 0) {
                    (void)ring_peek_at(&s->tx, bend - s->tx_base, h,
                                       SPILINK_REC);
                    const uint32_t nb = bend + SPILINK_REC + get16(h);
                    if ((h[4] & AG_SPILINK_RELIABLE) ||
                        (int32_t)(nb - end) > 0) {
                        break;
                    }
                    last = get16(h + 2);
                    bend = nb;
                }
                if (used + 6u > pay) {
                    return 0u;
                }
                put16(p + used, SPILINK_SEG_SKIP | (bend - pos));
                put16(p + used + 2, first);
                put16(p + used + 4, last);
                used += 6u;
                open = 0;
                skipped = true;
                pos = b = bend;
                continue;
            }
            upto = ((int32_t)(bend - end) < 0) ? bend : end;
            b = bend;
        }
        /* bytes [pos, upto) as they are */
        const uint32_t len = upto - pos;
        if (open == 0u) {
            if (used + 2u + len > pay) {
                return 0u;
            }
            open = used;
            put16(p + used, 0);
            used += 2u;
        } else if (used + len > pay) {
            return 0u;
        }
        (void)ring_peek_at(&s->tx, pos - s->tx_base, p + used, len);
        put16(p + open, get16(p + open) + len);
        used += len;
        *data += len;
        pos = upto;
    }
    if (!skipped) {
        return 0u;
    }
    put16(p, used - 2u);
    return used;
}

/*
 * Fill a frame.  A frame to send again goes first - the one frame the other
 * side has a hole at, in segments when it held blocks not worth sending
 * again - and otherwise as much NOT YET SENT data as `allow` lets.  Nothing
 * is freed here; the acknowledgement does that.  Returns the data bytes in
 * it.
 */
static uint32_t frame_build(spilink_t *s, uint8_t *f, uint32_t allow,
                            uint32_t room_less, bool *nak_was)
{
    const uint32_t pay = s->frame - SPILINK_HDR;
    const int      ix = link_index(s->bus);
    uint32_t       flags = 0;
    uint32_t       off = 0;
    uint32_t       n = 0;     /* data bytes */
    uint32_t       span = 0;  /* stream bytes this frame stands for */
    uint32_t       body = 0;  /* payload bytes after the header */
    s_undo[ix].kind = 0;

    lock(s);
    if (s->resend) {
        s->resend = false;
        for (uint32_t k = 0; k < s->infl_n; k++) {
            if (s->infl[k].off != s->resend_off) {
                continue;
            }
            off = s->infl[k].off;
            span = s->infl[k].n;
            /* blocks are skipped only for a frame the wire lost, never for
             * one the receiver threw away for want of room: that is not a
             * loss the application was willing to take */
            body = (s->blocks && !s->resend_whole)
                       ? seg_build(s, off, span, s->infl[k].blk,
                                   f + SPILINK_HDR, pay, &n)
                       : 0u;
            if (body > 0u) {
                flags = SPILINK_F_SEG;
            } else {
                flags = SPILINK_F_DATA;
                n = ring_peek_at(&s->tx, off - s->tx_base, f + SPILINK_HDR,
                                 span);
                body = n;
            }
            s_undo[ix].kind = 2;
            s_undo[ix].resend_off = off;
            break;
        }
    }
    if (flags == 0u && s->infl_n < SPILINK_WIN) {
        const uint32_t start = s->tx_base + s->sent_off;
        const uint32_t avail =
            (s->tx.count > s->sent_off) ? s->tx.count - s->sent_off : 0u;
        uint32_t limit = (avail < allow) ? avail : allow;
        if (limit > pay) {
            limit = pay;
        }
        const uint32_t blk = s->blk_next;
        if (s->blocks && limit > 0u) {
            /*
             * Blocks of both kinds share a frame, and any block may be cut
             * by its edges: cutting frames at every change of kind cost a
             * quarter of the wire with one block in eight unreliable, and so
             * did refusing to cut an unreliable block (74%, measured).  What
             * is not sent again is sorted out only if the frame is lost.
             * Here the record headers are only walked past, so the next
             * frame knows where its first one is.
             */
            const uint32_t end = start + limit;
            uint32_t       b = s->blk_next;
            while ((int32_t)(b - end) < 0) {
                uint8_t h[SPILINK_REC];
                (void)ring_peek_at(&s->tx, b - s->tx_base, h, SPILINK_REC);
                b += SPILINK_REC + get16(h);
            }
            s_undo[ix].blk_next = s->blk_next;
            s->blk_next = b;
        }
        if (limit > 0u) {
            n = ring_peek_at(&s->tx, s->sent_off, f + SPILINK_HDR, limit);
            s->infl[s->infl_n].off = start;
            s->infl[s->infl_n].blk = blk;
            s->infl[s->infl_n].n = (uint16_t)n;
            s->infl_n++;
            s->sent_off += n;
            flags = SPILINK_F_DATA;
            off = start;
            span = n;
            body = n;
            s_undo[ix].kind = 1;
            s_undo[ix].n = n;
        }
    }
    /*
     * One frame's payload counted as room beyond what is free: the reader
     * takes what arrived before the room is looked at again - and in block
     * mode the start of a block cut across two frames cannot be taken until
     * the second arrives, which strict counting charged to every frame (74%
     * of the wire, measured).  A frame that does not fit is not
     * acknowledged and comes again, so this can cost a resend, never data.
     *
     * Not while a hole is open: the reader then takes nothing, and the
     * piece of a block before the hole stays in the ring until it is
     * filled, so the frame promised beyond the room did not fit and was
     * thrown away - a new hole behind every hole (block mode under 5%
     * loss: 762 KB thrown away in 5 s, 24% of the wire, measured).
     */
    const bool     hole = s->ooo_n > 0u || s->nak_sent || s->refused;
    uint32_t       space = ring_space(&s->rx) + (hole ? 0u : pay);
    const uint32_t held = ooo_reserved(s);
    space = (space > held) ? space - held : 0u;
    if (space > s->rxcap) {
        space = s->rxcap;
    }
    unlock(s);

    if (flags == 0u) {
        s->starve_data += (s->tx.count > s->sent_off) ? 0u : 1u;
    }
    *nak_was = s->nak_pending;
    if (s->nak_pending) {
        flags |= SPILINK_F_NAK;
        if (s->refused && s->refused_off == s->rx_next) {
            flags |= SPILINK_F_REFUSED;
        }
        s->nak_pending = false;
        s->naks++;
    }
    uint32_t room = (space > room_less) ? space - room_less : 0u;
    if (room > 0xFFFFu) {
        room = 0xFFFFu;
    }
    f[0] = 0xA5u;
    f[1] = (uint8_t)(flags | (s->master ? SPILINK_F_MASTER : 0u));
    f[2] = 0x5Au;
    f[3] = (uint8_t)(s->block / SPILINK_UNIT);
    put32(f + 4, off);
    put32(f + 8, s->rx_next);
    put16(f + 12, span);
    put16(f + 14, room);
    put32(f + 16, frame_sum(f, body));
    return n;
}

/* Take back what frame_build did, for a frame that never went out. */
static void frame_unbuild(spilink_t *s, uint32_t n, bool nak_was)
{
    const int ix = link_index(s->bus);
    (void)n;
    if (s_undo[ix].kind == 1u && s->infl_n > 0u) {
        s->infl_n--;
        s->sent_off -= s_undo[ix].n;
        if (s->blocks) {
            s->blk_next = s_undo[ix].blk_next;
        }
    } else if (s_undo[ix].kind == 2u) {
        s->resend = true;
        s->resend_off = s_undo[ix].resend_off;
    }
    s_undo[ix].kind = 0;
    if (nak_was) {
        s->nak_pending = true;
        s->naks--;
    }
}

static void ask_again(spilink_t *s, int64_t now)
{
    if (!s->nak_sent || s->nak_expect != s->rx_next ||
        now - s->nak_at > SPILINK_NAK_US) {
        s->nak_pending = true;
        s->nak_sent = true;
        s->nak_expect = s->rx_next;
        s->nak_at = now;
        s->seq_gaps++;
    }
}

/* A segmented payload that stands for exactly `span` stream bytes. */
static bool seg_check(const uint8_t *p, uint32_t body, uint32_t span)
{
    uint32_t at = 2u;
    uint32_t total = 0;
    while (at < body) {
        if (at + 2u > body) {
            return false;
        }
        const uint32_t head = get16(p + at);
        const uint32_t len = head & ~SPILINK_SEG_SKIP;
        const uint32_t size = (head & SPILINK_SEG_SKIP) ? 4u : len;
        if (((head & SPILINK_SEG_SKIP) && len <= SPILINK_REC) || len == 0u ||
            at + 2u + size > body) {
            return false;
        }
        total += len;
        at += 2u + size;
    }
    return at == body && total == span;
}

/*
 * One received frame: its ack applied to what this end sent, a hole report
 * honoured, and its bytes put where they belong.  Returns the bytes that
 * reached the reader.
 */
static uint32_t frame_rx(spilink_t *s, const uint8_t *f, uint32_t got)
{
    const uint32_t pay = s->block - SPILINK_HDR;
    if (got < SPILINK_HDR || f[0] != 0xA5u || f[2] != 0x5Au) {
        count(s, &s->frames_bad, 1u);
        return 0u;
    }
    const uint32_t flags = f[1];
    /*
     * A frame from this end's own side is an ECHO: a slave not yet armed
     * sends back what it last took in, and the master's 64-byte hello is
     * exactly its hardware buffer.  Taken for the slave's, the master
     * agreed on its own frame size (4096 against the C3's 2048, and half
     * the wire, measured) - and an echo of data would be data.
     */
    if (((flags & SPILINK_F_MASTER) != 0u) == s->master) {
        count(s, &s->frames_bad, 1u);
        return 0u;
    }
    const uint32_t off = get32(f + 4);
    const uint32_t ack = get32(f + 8);
    const uint32_t span = get16(f + 12);
    const uint32_t room = get16(f + 14);
    const uint32_t body =
        (flags & SPILINK_F_SEG)
            ? ((got >= SPILINK_HDR + 2u) ? 2u + get16(f + SPILINK_HDR) : ~0u)
        : (flags & SPILINK_F_DATA) ? span
                                   : 0u;
    const int64_t  now = ag_port_us();
    if (body > pay || body > got - SPILINK_HDR || span > pay ||
        frame_sum(f, body) != get32(f + 16)) {
        count(s, &s->frames_bad, 1u);
        ask_again(s, now);
        return 0u;
    }
    if ((flags & SPILINK_F_SEG) && !seg_check(f + SPILINK_HDR, body, span)) {
        count(s, &s->frames_bad, 1u);
        ask_again(s, now);
        return 0u;
    }
    if (!s->agreed) {
        /* a peer that says nothing (0) is taken to be this end's size */
        const uint32_t peer = (f[3] != 0u) ? f[3] * SPILINK_UNIT : s->block;
        s->frame = (peer < s->block) ? peer : s->block;
        s->agreed = s->master || got >= s->frame;
    }
    const bool carries = (flags & (SPILINK_F_DATA | SPILINK_F_SEG)) != 0u;
    if (s->drop_every != 0u && carries &&
        (++s->drop_count % s->drop_every) == 0u) {
        return 0u; /* test: this frame never arrived */
    }

    /* the ack: free what it passes */
    lock(s);
    s->peer_room = room;
    s->peer_known = true;
    bool freed = false;
    const uint32_t d = ack - s->tx_base;
    if ((int32_t)d > 0 && d <= s->sent_off) {
        ring_commit(&s->tx, d);
        s->sent_off -= d;
        s->tx_base = ack;
        while (s->infl_n > 0u &&
               (int32_t)(s->infl[0].off + s->infl[0].n - ack) <= 0) {
            for (uint32_t k = 1; k < s->infl_n; k++) {
                s->infl[k - 1] = s->infl[k];
            }
            s->infl_n--;
        }
        freed = true;
    }
    unlock(s);
    if (freed) {
        s->ack_at = now;
        (void)ag_port_sem_give(s->space_sem);
    }
    if ((flags & SPILINK_F_NAK) && s->infl_n > 0u &&
        (int32_t)(ack - s->tx_base) == 0) {
        s->resend = true;
        s->resend_whole = (flags & SPILINK_F_REFUSED) != 0u;
        s->resend_off = s->tx_base;
        s->rewinds++;
    }
    if (!carries || span == 0u) {
        return 0u;
    }

    /* the bytes: a repeat, the next in order, or after a hole */
    if ((int32_t)(off + span - s->rx_next) <= 0 ||
        (int32_t)(off - s->rx_next) < 0) {
        return 0u;
    }
    const uint32_t delta = off - s->rx_next;
    for (uint32_t k = 0; k < s->ooo_n; k++) {
        if (s->ooo[k].off == off) {
            return 0u; /* already have it */
        }
    }
    lock(s);
    const bool fits = delta + span <= ring_space(&s->rx) &&
                      (delta == 0u || s->ooo_n < SPILINK_WIN);
    unlock(s);
    if (!fits) {
        /* not taken, so not acknowledged: it comes again */
        s->bytes_dropped += span;
        if (!s->refused || (int32_t)(off - s->refused_off) < 0) {
            s->refused = true;
            s->refused_off = off;
        }
        ask_again(s, now);
        return 0u;
    }
    lock(s);
    if (flags & SPILINK_F_DATA) {
        ring_write_at(&s->rx, delta, f + SPILINK_HDR, span);
    } else {
        /* segments: bytes where they go, and for each notice a LOST record
         * of the same size; the reader passes over the rest unread */
        const uint8_t *p = f + SPILINK_HDR + 2u;
        const uint8_t *const pe = f + SPILINK_HDR + body;
        uint32_t       at = delta;
        while (p < pe) {
            const uint32_t head = get16(p);
            const uint32_t len = head & ~SPILINK_SEG_SKIP;
            if (head & SPILINK_SEG_SKIP) {
                uint8_t rec[SPILINK_REC];
                put16(rec, len - SPILINK_REC);
                put16(rec + 2, get16(p + 2));
                rec[4] = SPILINK_REC_LOST;
                rec[5] = 0;
                put16(rec + 6, get16(p + 4));
                ring_write_at(&s->rx, at, rec, SPILINK_REC);
                p += 6;
            } else {
                ring_write_at(&s->rx, at, p + 2, len);
                p += 2u + len;
            }
            at += len;
        }
    }
    uint32_t delivered = 0;
    if (delta == 0u) {
        ring_advance(&s->rx, span);
        s->rx_next += span;
        delivered = span;
        s->last_rx_len = span;
        while (s->ooo_n > 0u && s->ooo[0].off == s->rx_next) {
            ring_advance(&s->rx, s->ooo[0].n);
            s->rx_next += s->ooo[0].n;
            delivered += s->ooo[0].n;
            for (uint32_t k = 1; k < s->ooo_n; k++) {
                s->ooo[k - 1] = s->ooo[k];
            }
            s->ooo_n--;
        }
    } else {
        uint32_t at = s->ooo_n;
        while (at > 0u && (int32_t)(s->ooo[at - 1u].off - off) > 0) {
            s->ooo[at] = s->ooo[at - 1u];
            at--;
        }
        s->ooo[at].off = off;
        s->ooo[at].n = span;
        s->ooo_n++;
    }
    if (s->refused && (int32_t)(s->rx_next - s->refused_off) > 0) {
        s->refused = false; /* the frame thrown away has come again */
    }
    unlock(s);
    if (delivered > 0u) {
        s->nak_sent = false;
        (void)ag_port_sem_give(s->rx_sem);
        /*
         * A further hole behind the one just filled: one between frames
         * held, or the frame this end threw away itself.  Forgetting the
         * second kind promised room again, the frame after it did not fit
         * either, and every window of frames made a new hole (24% of the
         * wire with block mode under 5% loss, measured).
         */
        if (s->ooo_n > 0u || s->refused) {
            ask_again(s, now);
        }
    } else {
        ask_again(s, now);
    }
    return delivered;
}

/* No acknowledgement for too long with frames out: send the first again. */
static void rto_check(spilink_t *s)
{
    const int64_t now = ag_port_us();
    if (s->infl_n > 0u && now - s->ack_at > SPILINK_RTO_US) {
        s->resend = true;
        s->resend_whole = false;
        s->resend_off = s->tx_base;
        s->ack_at = now;
        s->rewinds++;
    }
}

static ag_err_t slot_arm(spilink_t *s, unsigned i, uint32_t *carried)
{
    uint8_t *const tx = slot_tx(s, i);
    uint32_t       n;

    *carried = 0;

    if (s->framed) {
        /*
         * The master's room, as its last frame said, less every payload of
         * ours it had not yet received when it said so: the frame just
         * reaped and every one still armed.
         */
        lock(s);
        uint32_t in_flight = s->reaped_used;
        for (unsigned k = 0; k < s->nslots; k++) {
            if (s->live[k]) {
                in_flight += s->used[k];
            }
        }
        const uint32_t allow = peer_allowance(s, in_flight);
        unlock(s);
        bool nak_was = false;
        n = frame_build(s, tx, allow, 0u, &nak_was);

        const ag_err_t err =
            ag_port_spi_slave_arm(s->bus, tx, slot_rx(s, i), s->block);
        if (err != AG_OK) {
            frame_unbuild(s, n, nak_was);
            return err;
        }
        *carried = n;
        return AG_OK;
    }

    lock(s);
    n = ring_peek(&s->tx, tx, s->block);
    unlock(s);

    if (n > 0u && n < s->block) {
        memset(tx + n, 0, s->block - n);
    }

    const ag_err_t err = ag_port_spi_slave_arm(
        s->bus, (n > 0u) ? (const void *)tx : NULL, slot_rx(s, i), s->block);
    if (err != AG_OK) {
        return err;
    }

    lock(s);
    ring_commit(&s->tx, n);
    unlock(s);

    *carried = n;
    return AG_OK;
}

/*
 * Push what the peer clocked into the RX ring.  Bytes with no room are dropped
 * and counted: a link that lost them silently would look like a wire fault.
 */
static void deliver(spilink_t *s, unsigned slot, uint32_t got)
{
    uint8_t *const rx = slot_rx(s, slot);
    uint32_t       n = got;

    if (s->framed) {
        (void)frame_rx(s, rx, got);
        return;
    }

    if (n > s->block) {
        n = s->block;
    }
    if (n == 0u) {
        return;
    }

    lock(s);
    const uint32_t put = ring_put(&s->rx, rx, n);
    if (put < n) {
        s->bytes_dropped += (n - put);
    }
    unlock(s);

    if (put > 0u) {
        /* A count and not a flag: two blocks can arrive before the reader
         * wakes, and one give per block is what lets it see both. */
        (void)ag_port_sem_give(s->rx_sem);
    }
}

static void spilink_task(void *arg)
{
    spilink_t *s = (spilink_t *)arg;

    for (;;) {
        if (s->stop) {
            break;
        }

        /*
         * Did the WIRE give us anything this pass?  Arming alone does not
         * count: a pass that reaps no block means the peer said nothing, and a
         * pass that says nothing must SLEEP - the one rule that keeps a task at
         * priority 9 from starving the board.
         */
        bool clocked = false;

        if (s->framed) {
            rto_check(s);
        }

        /*
         * Top the queue up.  This is what makes the invariant true: it runs
         * before every reap, so a slot that has just been freed is armed again
         * while the other two are still in the driver.  Bounded by the number
         * of slots, so this inner loop cannot be the spin either.
         */
        for (;;) {
            const int i = find_free(s);
            if (i < 0) {
                break;
            }

            uint32_t       carried = 0;
            const ag_err_t err = slot_arm(s, (unsigned)i, &carried);
            if (err != AG_OK) {
                /*
                 * -AG_EAGAIN is back-pressure: every slot the port has is
                 * armed, so the only way forward is to reap one, which the loop
                 * does next.  Anything else is the bus refusing and is counted.
                 * Either way this pass ends in the wait at the bottom of the
                 * loop, and never in another attempt straight away.
                 */
                if (err != -AG_EAGAIN) {
                    note_error(s, err);
                }
                break;
            }

            s->live[i] = true;
            s->used[i] = carried;
            s->armed++;
            armq_push(s, (uint8_t)i);
        }

        /*
         * `slot` is the port's name for the block that came back, in the port's
         * own numbering - slot 0 of its pool is spi_slave_xfer's - so it is not
         * an index into anything here and is not used as one.  WHICH of ours
         * came back is told by the receive buffer the port names, not by
         * assuming blocks come back in the order they were armed: the arm
         * queue was that assumption, and when it slipped this service re-armed
         * a slot the driver still had queued, and the master was clocked a
         * frame half rewritten.
         */
        int            slot = -1;
        uint32_t       got = 0;
        void          *rxp = NULL;
        const ag_err_t rc = ag_port_spi_slave_reap(s->bus, &slot, &got, &rxp,
                                                   SPILINK_REAP_MS);

        bool    mine_valid = false;
        bool    carried = false;
        uint8_t mine = 0;

        if (rc == AG_OK) {
            count(s, &s->blocks_clocked, 1u);

            int found = -1;
            for (unsigned k = 0; k < s->nslots; k++) {
                if (s->live[k] && (void *)slot_rx(s, k) == rxp) {
                    found = (int)k;
                    break;
                }
            }
            if (s->armed == 0u || s->qcount == 0u || found < 0) {
                /*
                 * A completion with nothing of ours in flight.  It cannot
                 * happen while this loop is the only thing arming, which is why
                 * it is counted: a non-zero count means the port completed a
                 * block this service did not queue, and that is the first thing
                 * to look at when blocks go missing.
                 *
                 * It does NOT count as progress for the sleep at the bottom, so
                 * even a port that answered this way for ever could not make
                 * this task spin - and the quiet clock runs, which after a
                 * second rebuilds the interface.  A completion nobody can
                 * attribute is a reason to start over, not a reason to hurry.
                 */
                count(s, &s->blocks_unarmed, 1u);
            } else {
                if (s->armq[s->qhead] != (uint8_t)found) {
                    count(s, &s->reorders, 1u);
                }
                armq_remove(s, (uint8_t)found);
                mine = (uint8_t)found;
                if (s->framed && s->agreed && got < s->frame) {
                    count(s, &s->short_frames, 1u);
                }
                s->armed--;
                s->live[mine] = false;
                /* on its way until the master's next room counts it */
                s->reaped_used = s->used[mine];
                carried = (s->used[mine] > 0u);
                if (carried) {
                    count(s, &s->blocks_data, 1u);
                }
                mine_valid = true;
                clocked = true;
            }
        } else if (rc == -AG_ETIMEDOUT || rc == -AG_EAGAIN) {
            /*
             * Nothing completed inside the window.  Normal on a quiet wire and
             * NOT an error: which of these two codes a port uses for "nothing
             * arrived" is the port's business, and both mean the same thing
             * here.  The block stays armed and the port still holds it - a
             * timeout does not give it back.
             */
            count(s, &s->timeouts, 1u);
        } else {
            note_error(s, rc);
        }

        /* Read what the peer clocked, outside the lock: deliver takes it
         * itself. */
        if (mine_valid) {
            deliver(s, mine, got);
        }

        if (clocked) {
            lock(s);
            s->quiet_started = false; /* the wire said something */
            unlock(s);
        } else {
            /*
             * Nothing came back.  Start the quiet clock, or act on it: blocks
             * the peer never clocks are held by the port for ever, so after a
             * whole second of silence the only way forward is to build the
             * interface again.  See SPILINK_STALL_MS.
             */
            bool stalled = false;
            lock(s);
            const ag_port_ticks_t now = ag_port_ticks();
            if (!s->quiet_started) {
                s->quiet_started = true;
                s->quiet_since = now;
            } else if ((ag_port_ticks_t)(now - s->quiet_since) >=
                       ticks_at_least(SPILINK_STALL_MS)) {
                stalled = true;
                s->quiet_started = false;
                s->stalls++;
            }
            unlock(s);

            if (stalled && !s->stop) {
                slave_rebuild(s);
            }
        }

        /*
         * THE RULE.  A pass that collected no block sleeps, always, for at least
         * one tick - whether the reap timed out, the pool was full, the port
         * refused something, or a completion arrived that belonged to nobody.
         * Only a pass that took a block off the wire may go round again
         * immediately, and that is bounded by the peer's own block rate.
         * Deleting this is how the board got starved.
         */
        if (!clocked) {
            sleep_at_least(SPILINK_IDLE_MS);
        }
    }

    /*
     * Said goodbye before the task goes, because close waits on exactly this
     * and then frees the buffers this loop was pointing into.  Nothing here
     * touches the link after the give.
     */
    (void)ag_port_sem_give(s->exit_sem);
    ag_port_task_delete(NULL);
    /* Does not return. */
}

/*
 * THE MASTER END OF A FRAMED LINK.
 *
 * One frame per pass: the header and as much of the TX ring as the slave has
 * room for go out while the slave's frame comes in, and the task sleeps in
 * the port for the length of it (ag_port_spi_xfer_dma - no polling, no
 * staging copy).  Back to back while either side has something to carry.
 * Idle, it waits on the writers' semaphore for SPILINK_POLL_MS and then
 * clocks a frame anyway, because that frame is the slave's only way to speak.
 *
 * THE WORK BETWEEN TWO FRAMES IS NOT WASTE, IT IS THE SLAVE'S TIME.  A version
 * that prepared the next frame while this one crossed and started it within
 * microseconds of the last gained nothing measurable and broke the link: the
 * C3 could no longer take up its next armed frame in time, 1354 of 7134 of
 * its frames came back with no header, the slave counted thousands of the
 * master's frames it never saw, and a frame it picked up late was clocked
 * shifted - damaged blocks, on a wire that had had none.  The pass as it is
 * leaves some 100 us between frames and the C3 has never missed one in it.
 *
 * It never spins, for the reason at the top of this file: every pass blocks,
 * in the transfer or in the wait.
 */
static void spilink_master_task(void *arg)
{
    spilink_t *s = (spilink_t *)arg;
    uint8_t *const txf = s->mbuf;
    uint8_t *const rxf = s->mbuf + s->block;

    int64_t pass0 = ag_port_us();
    int64_t hot_until = 0;
    for (;;) {
        if (s->stop) {
            break;
        }

        /*
         * The slave's room came with its last frame, and it was reckoned when
         * the slave ARMED that frame - SPILINK_AHEAD frames before it was
         * clocked.  Everything this end sent in the last that-many frames may
         * not be in it, so all of that is taken off.
         */
        lock(s);
        uint32_t pending = 0;
        for (unsigned k = 0; k < SPILINK_AHEAD; k++) {
            pending += s->hist[k];
        }
        /* the size of this pass's frame: SPILINK_HELLO until agreed */
        const uint32_t fl = s->frame;
        uint32_t       allow = peer_allowance(s, pending);
        if (allow > fl - SPILINK_HDR) {
            allow = fl - SPILINK_HDR;
        }
        unlock(s);
        rto_check(s);
        bool           nak_was = false;
        const uint32_t n = frame_build(s, txf, allow, 0u, &nak_was);

        const int64_t  w0 = ag_port_us();
        const ag_err_t rc =
            ag_port_spi_xfer_dma(s->bus, s->cs, txf, rxf, fl);
        const int64_t  w1 = ag_port_us();
        if (rc != AG_OK) {
            frame_unbuild(s, n, nak_was);
            note_error(s, rc);
            sleep_at_least(SPILINK_IDLE_MS);
            continue;
        }

        lock(s);
        s->wire_us += (uint32_t)(w1 - w0);
        s->loop_us += (uint32_t)(w1 - pass0);
        pass0 = w1;
        s->hist[s->hist_at] = n;
        s->hist_at = (s->hist_at + 1u) % SPILINK_AHEAD;
        s->blocks_clocked++;
        unlock(s);

        const uint32_t got = frame_rx(s, rxf, fl);
        lock(s);
        /* data not yet sent, or frames out and waiting for their ack */
        const bool more = (s->tx.count > s->sent_off) || s->infl_n > 0u;
        unlock(s);
        if (n > 0u || got > 0u) {
            count(s, &s->blocks_data, 1u);
        }

        /*
         * An empty frame from the slave does not mean it has nothing: its
         * frames are filled when they are ARMED, two ahead, and a writer that
         * was a moment late leaves one empty with data right behind it.
         * Sleeping a whole poll then cost 2 ms per such frame - 583 of 2246
         * frames, a third of the run, on the C3 sending to the Zero.  So the
         * link stays hot for SPILINK_HOT_US after the last frame that carried
         * anything, either way, and only a quiet longer than that is idle.
         */
        const int64_t now = ag_port_us();
        if (n > 0u || got > 0u || more) {
            hot_until = now + SPILINK_HOT_US;
        }
        if (now >= hot_until) {
            (void)ag_port_sem_take(s->tx_sem, ticks_at_least(SPILINK_POLL_MS));
        }
    }

    (void)ag_port_sem_give(s->exit_sem);
    ag_port_task_delete(NULL);
}

/* ---------------------------------------------------------------------- */
/* Teardown                                                               */
/* ---------------------------------------------------------------------- */

static void stats_of(const spilink_t *s, ag_spilink_stats_t *out)
{
    out->blocks_clocked = s->blocks_clocked;
    out->blocks_data = s->blocks_data;
    out->blocks_unarmed = s->blocks_unarmed;
    out->bytes_dropped = s->bytes_dropped;
    out->timeouts = s->timeouts;
    out->stalls = s->stalls;
    out->recoveries = s->recoveries;
    out->errors = s->errors;
    out->last_error = s->last_error;
    out->open = s->open;
    out->task_stuck = false;
    out->frames_bad = s->frames_bad;
    out->seq_gaps = s->seq_gaps;
    out->wire_us = s->wire_us;
    out->loop_us = s->loop_us;
    out->reorders = s->reorders;
    out->short_frames = s->short_frames;
    out->rewinds = s->rewinds;
    out->naks = s->naks;
    out->starve_data = s->starve_data;
    out->starve_credit = s->starve_credit;
    out->frame = s->agreed ? s->frame : 0u;
}

static void free_open_memory(spilink_t *s)
{
    if (s->onepiece) {
        ag_port_free(s->mbuf != NULL ? s->mbuf : s->slots);
        s->onepiece = false;
    } else {
        ag_port_free(s->mbuf);
        ag_port_free(s->slots);
        ag_port_free(s->txmem);
        ag_port_free(s->rxmem);
    }
    s->mbuf = NULL;
    s->slots = NULL;
    s->txmem = NULL;
    s->rxmem = NULL;
    memset(&s->tx, 0, sizeof(s->tx));
    memset(&s->rx, 0, sizeof(s->rx));
}

/*
 * The one teardown path, used by close and by the module-gone watcher.  It logs
 * nothing, for the reason at the top of this file.
 */
static ag_err_t link_teardown(spilink_t *s, ag_spilink_stats_t *out)
{
    lock(s);
    if (!s->open) {
        unlock(s);
        return -AG_ENODEV;
    }
    /* No new traffic, and the task leaves at its next look at `stop`. */
    s->open = false;
    s->stop = true;
    unlock(s);

    /* A reader blocked in read() has to see the link close under it, and so
     * does a writer waiting for room. */
    (void)ag_port_sem_give(s->rx_sem);
    (void)ag_port_sem_give(s->space_sem);

    bool gone = true;
    if (s->task != NULL) {
        gone = ag_port_sem_take(s->exit_sem,
                                ticks_at_least(SPILINK_STOP_WAIT_MS));
    }

    lock(s);
    if (out != NULL) {
        stats_of(s, out);
    }
    unlock(s);

    if (!gone) {
        /*
         * The task is still inside the port and still pointing at these
         * buffers.  Freeing them would be worse than the leak, so nothing is
         * freed and the link stays open, and the caller says so out loud.  It
         * should not happen: the reap it is waiting on is bounded by
         * SPILINK_REAP_MS.
         */
        if (out != NULL) {
            out->task_stuck = true;
        }
        return -AG_ETIMEDOUT;
    }

    /*
     * A SLAVE'S ARMED FRAMES OUTLIVE ITS TASK, and they point into the memory
     * about to be freed.  The driver has no call that cancels a queued
     * transaction, the port has no close for a slave bus, and a master that
     * goes on clocking - it does, at the idle poll - completes them into
     * whatever that memory has become by then.  Measured: every open after
     * the first found the last session's frames still queued, took them for
     * its own, and ran with its arm order one off - every third frame
     * reported out of order, the master clocked stale frames, whole runs of
     * damaged blocks.  The reset throws the interface away and builds it
     * again, and it promises that nothing in the chip can reach a buffer the
     * pool lent out once it returns; only then is the memory given back.
     */
    bool may_free = true;
    if (!s->master && s->armed > 0u) {
        /* if the reset is refused, the memory is left to the engine for good:
         * a leak is a number in `mem`, a DMA write into reused memory is not */
        may_free = (ag_port_spi_slave_reset(s->bus) == AG_OK);
    }

    lock(s);
    if (may_free) {
        free_open_memory(s);
    } else {
        s->onepiece = false;
        s->mbuf = NULL;
        s->slots = NULL;
        s->txmem = NULL;
        s->rxmem = NULL;
    }
    memset(s->used, 0, sizeof(s->used));
    memset(s->live, 0, sizeof(s->live));
    s->qhead = 0;
    s->qcount = 0;
    s->armed = 0;
    s->task = NULL;
    s->stop = false;
    s->owner = NULL;
    s->block = 0;
    s->khz = 0;
    s->cs = AG_PIN_NONE;
    s->blocks_clocked = 0;
    s->blocks_data = 0;
    s->blocks_unarmed = 0;
    s->bytes_dropped = 0;
    s->timeouts = 0;
    s->stalls = 0;
    s->recoveries = 0;
    s->errors = 0;
    s->last_error = AG_OK;
    s->quiet_started = false;
    s->last_fault = 0;
    s->fault_reported = false;
    unlock(s);
    return AG_OK;
}

/*
 * A driver that unloaded without closing.
 *
 * The kernel needs the task stopped, and this is the one hook that says a
 * module's things are going away: the device registry calls every watcher, with
 * the registry lock held, before a revoked device's slot is cleared.  So the
 * link is matched on the module cookie taken at open and torn down here.
 *
 * Registered on the first open made from inside a driver's init rather than at
 * boot: a board that never runs this service never spends a watcher slot (there
 * are four, and the registry is an interface other kernel subsystems use too).
 * A module with no devices of its own never revokes anything, so a driver that
 * both forgets to close and publishes nothing is not caught here - that leak is
 * what the io layer's close log is for noticing.
 */
static void spilink_on_revoke(ag_device_t *dev)
{
    if (dev == NULL || dev->owner == NULL) {
        return;
    }
    for (unsigned i = 0; i < AG_SPI_BUSES; i++) {
        if (s_link[i].open && s_link[i].owner == dev->owner) {
            (void)link_teardown(&s_link[i], NULL);
        }
    }
}

/* ---------------------------------------------------------------------- */
/* The calls the io layer makes                                           */
/* ---------------------------------------------------------------------- */

ag_err_t ag_spilink_open(int bus, int cs, uint32_t khz, uint32_t block, int sck,
                         int mosi, int miso)
{
    return ag_spilink_open2(bus, cs, khz, block, 0u, sck, mosi, miso);
}

ag_err_t ag_spilink_open2(int bus, int cs, uint32_t khz, uint32_t block,
                          uint32_t flags, int sck, int mosi, int miso)
{
    const int idx = link_index(bus);
    if (idx < 0 || cs < 0) {
        return -AG_ERANGE;
    }
    const bool master = (flags & AG_SPILINK_MASTER) != 0u;
    const bool framed = (flags & AG_SPILINK_FRAMED) != 0u;
    /* A master has no raw mode: its transactions are how the slave speaks,
     * and without a header neither end could tell data from padding. */
    if (master && !framed) {
        return -AG_EINVAL;
    }
    if (framed && block == 0u) {
        /* the board's own: how much memory a link may cost is its business */
        const int32_t v = ag_cfg_get_int(ag_sysconfig(), "wire.frame",
                                         (int32_t)SPILINK_FRAME_DEFAULT);
        block = (v > 0) ? (uint32_t)v : SPILINK_FRAME_DEFAULT;
        if (block > AG_PORT_SPI_MAX_XFER) {
            block = AG_PORT_SPI_MAX_XFER;
        }
    }
    /* a frame's size travels in 64-byte units, and the first frames are
     * SPILINK_HELLO long, which every frame must be able to take */
    if (framed && (block < SPILINK_HELLO || block % SPILINK_UNIT != 0u)) {
        return -AG_EINVAL;
    }
    /*
     * The port uses the caller's memory as the DMA engine's buffers, so the
     * block has to be a whole number of words - a receive length that is not
     * one is refused by the port, and it is better refused here where the
     * caller can still act on it.  Zero is not a block, and anything past the
     * port's longest transfer is a transfer no port can make.
     */
    if (block == 0u || (block & 3u) != 0u || block > AG_PORT_SPI_MAX_XFER) {
        return -AG_EINVAL;
    }

    spilink_t *const s = &s_link[idx];

    /*
     * Created once and kept.  Each is checked on its own, so an open that
     * failed halfway leaves nothing that a later open cannot finish.
     */
    if (s->lock == NULL) {
        s->lock = ag_port_mutex_new();
    }
    if (s->rx_sem == NULL) {
        s->rx_sem = ag_port_sem_new_counting(255u, 0u);
    }
    if (s->exit_sem == NULL) {
        s->exit_sem = ag_port_sem_new_binary();
    }
    if (s->tx_sem == NULL) {
        s->tx_sem = ag_port_sem_new_binary();
    }
    if (s->space_sem == NULL) {
        s->space_sem = ag_port_sem_new_binary();
    }
    if (s->lock == NULL || s->rx_sem == NULL || s->exit_sem == NULL ||
        s->tx_sem == NULL || s->space_sem == NULL) {
        return -AG_ENOMEM;
    }

    lock(s);
    if (s->open) {
        unlock(s);
        return -AG_EBUSY;
    }

    /*
     * Everything is allocated here: there is no allocation after open.  The
     * armed blocks come from the DMA pool and nowhere else - memory the engine
     * cannot reach is memory the port will refuse to arm, and a link that opens
     * and then cannot put a block on the wire is worse than one that says it
     * will not open.  Aligned so that every receive slot inside them is
     * word-aligned too, which the engine also requires.
     */
    /*
     * A framed link sizes its rings by the frame: the transmit ring has to
     * keep every armed frame full and one more ready, and the receive ring has
     * to hold what is in flight towards it when its room was last reported,
     * or the writer stops and waits every few frames.  So each is one frame's
     * payload more than SPILINK_AHEAD; the raw link keeps its old sizes.
     */
    s->nslots = framed ? SPILINK_AHEAD : SPILINK_SLOTS;
    {
        /*
         * In payloads.  The receive ring is SPILINK_AHEAD + 1 frames.  The
         * room it reports counts the frame last put in as taken, because the
         * reader takes it before the room is next looked at; reckoned
         * strictly - right after a frame went in - a ring of three frames
         * left nothing every other frame (71.6% of the wire, measured), and
         * four did not fit a C3 once the transmit ring had to hold frames for
         * sending again.  Being optimistic is safe because of that: a frame
         * that does not fit is not acknowledged, and it comes again.
         */
        const uint32_t pay = block - SPILINK_HDR;
        /* the transmit ring is also what a frame is sent AGAIN from: it has
         * to hold every frame out and not yet acknowledged, and one more
         * ready.  Four frames: a slave has three out at once (two armed,
         * one clocked and waiting for the ack that rides the master's next
         * frame), and with a ring of three the space an ack freed was armed
         * empty before its writer could run - one frame in four sent
         * nothing, 69.8% of the wire on any block size, measured */
        const uint32_t txeach = (4u * pay + 3u) & ~3u;
        /* the master, usually the bigger part, takes a frame more - its
         * room is the slave's whole allowance, and a slave has none to spare */
        const uint32_t rxeach =
            ((SPILINK_AHEAD + (master ? 2u : 1u)) * pay + 3u) & ~3u;
        s->txcap = framed ? txeach : SPILINK_TX_RING;
        s->rxcap = framed ? rxeach : SPILINK_RX_RING;
    }
    s->slots = NULL;
    s->mbuf = NULL;
    s->txmem = NULL;
    s->rxmem = NULL;
    s->onepiece = false;
    const size_t frames = master ? 2u * (size_t)block
                                 : 2u * (size_t)s->nslots * (size_t)block;
    /*
     * ONE PIECE FIRST.  On the C3 a 4 KB framed slave once needed 24 KB of
     * frames and two 16 KB rings, 56 KB, with 81 KB free and the largest free
     * block 61 KB - and asked for as three, the third was refused: the first
     * two took the big block apart and what was left was all smaller than
     * 16 KB.  Now it is 40 KB (SPILINK_AHEAD), and as one allocation.  The rings are then DMA-capable memory they
     * do not need to be, which on the C3 is all its memory anyway; a part
     * that cannot find the one piece gets the three instead.
     */
    uint8_t *const one = ag_port_alloc_aligned(
        4u, frames + (size_t)s->txcap + (size_t)s->rxcap, AG_MEM_DMA);
    if (one != NULL) {
        s->onepiece = true;
        if (master) {
            s->mbuf = one;
        } else {
            s->slots = one;
        }
        s->txmem = one + frames;
        s->rxmem = s->txmem + s->txcap;
    } else {
        if (master) {
            s->mbuf = ag_port_alloc_aligned(4u, frames, AG_MEM_DMA);
        } else {
            s->slots = ag_port_alloc_aligned(4u, frames, AG_MEM_DMA);
        }
        s->txmem = ag_port_alloc(s->txcap, AG_MEM_BYTE);
        s->rxmem = ag_port_alloc(s->rxcap, AG_MEM_BYTE);
    }
    if ((master ? s->mbuf == NULL : s->slots == NULL) || s->txmem == NULL ||
        s->rxmem == NULL) {
        /* Said here, on the caller's task: which piece did not fit is the
         * whole question when a link will not open on a small part. */
        ag_log(AG_LOG_WARN, "spilink",
               "spi%d: no memory: frames %u B %s, tx ring %u B %s, rx ring "
               "%u B %s",
               bus,
               (unsigned)frames,
               (master ? s->mbuf : s->slots) ? "ok" : "REFUSED",
               (unsigned)s->txcap, s->txmem ? "ok" : "REFUSED",
               (unsigned)s->rxcap, s->rxmem ? "ok" : "REFUSED");
        free_open_memory(s);
        unlock(s);
        return -AG_ENOMEM;
    }

    memset(s->txmem, 0, s->txcap);
    memset(s->rxmem, 0, s->rxcap);
    s->tx = (ring_t){.mem = s->txmem, .cap = s->txcap};
    s->rx = (ring_t){.mem = s->rxmem, .cap = s->rxcap};
    s->master = master;
    s->framed = framed;
    s->frame = framed ? SPILINK_HELLO : block;
    s->agreed = !framed;
    s->tx_base = 0;
    s->sent_off = 0;
    s->infl_n = 0;
    s->resend = false;
    s->resend_whole = false;
    s->resend_off = 0;
    s->ack_at = ag_port_us();
    s->blocks = (flags & AG_SPILINK_BLOCKS) != 0u;
    s->blk_next = 0;
    s->next_block = 0;
    s->rx_next = 0;
    s->ooo_n = 0;
    s->refused = false;
    s->refused_off = 0;
    s->nak_pending = false;
    s->nak_sent = false;
    s->nak_expect = 0;
    s->nak_at = 0;
    s->drop_every = (flags >> 8) & 0xFFu;
    s->drop_count = 0;
    s->last_rx_len = 0;
    s->rewinds = 0;
    s->naks = 0;
    s->starve_data = 0;
    s->starve_credit = 0;
    s->peer_known = false;
    s->peer_room = 0;
    s->reaped_used = 0;
    memset(s->hist, 0, sizeof(s->hist));
    s->hist_at = 0;
    s->frames_bad = 0;
    s->seq_gaps = 0;
    s->wire_us = 0;
    s->loop_us = 0;
    s->reorders = 0;
    s->short_frames = 0;

    memset(s->used, 0, sizeof(s->used));
    memset(s->live, 0, sizeof(s->live));
    s->qhead = 0;
    s->qcount = 0;
    s->armed = 0;
    s->bus = bus;
    s->cs = cs;
    s->khz = khz;
    s->block = block;
    s->sck = sck;
    s->mosi = mosi;
    s->miso = miso;
    s->stop = false;
    s->blocks_clocked = 0;
    s->blocks_data = 0;
    s->blocks_unarmed = 0;
    s->bytes_dropped = 0;
    s->timeouts = 0;
    s->stalls = 0;
    s->recoveries = 0;
    s->errors = 0;
    s->last_error = AG_OK;
    s->quiet_started = false;
    s->last_fault = 0;
    s->fault_reported = false;
    /*
     * Who to blame if this is never closed.  Non-NULL only inside a driver's
     * ag_driver_init, which is where a .SYS opens a link.
     */
    s->owner = ag_module_loading();
    s->open = true;
    unlock(s);

    /*
     * Core 1 on a part with two, which is where the rest of the system's
     * link work sits: the application core, so a block arriving while the
     * foreground is busy is collected on the other one.
     */
    const int core = (int)ag_sysinfo()->app_core;
    if (!ag_port_task_create(master ? spilink_master_task : spilink_task,
                             "spilink", SPILINK_TASK_STACK, s, SPILINK_PRIO,
                             core, 0, &s->task)) {
        lock(s);
        s->open = false;
        s->stop = false;
        free_open_memory(s);
        s->task = NULL;
        unlock(s);
        return -AG_ENOMEM;
    }

    if (s->owner != NULL) {
        (void)ag_dev_on_revoke(spilink_on_revoke);
    }
    return AG_OK;
}

ag_err_t ag_spilink_close(int bus, ag_spilink_stats_t *out)
{
    const int idx = link_index(bus);
    if (idx < 0) {
        return -AG_ERANGE;
    }
    if (s_link[idx].lock == NULL) {
        return -AG_ENODEV;
    }
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->last_error = AG_OK;
    }
    return link_teardown(&s_link[idx], out);
}

/*
 * The counters while the link is alive, and where a fault is said out loud.
 *
 * This runs on the CALLER's task, never on the service task, and that is the
 * whole reason the log line can live here: the service task must not log at all
 * (a device revoke stops it while holding the device registry lock, and the
 * console lock sits underneath that one), while a driver or the shell asking
 * for the numbers holds nothing that matters.  So a caller that polls this gets
 * both the numbers and - at most once every SPILINK_FAULT_REPORT_MS, and only
 * while something is wrong - the line about it.
 */
ag_err_t ag_spilink_stats(int bus, ag_spilink_stats_t *out)
{
    const int idx = link_index(bus);
    if (idx < 0) {
        return -AG_ERANGE;
    }
    if (out == NULL) {
        return -AG_EINVAL;
    }

    spilink_t *const s = &s_link[idx];
    if (s->lock == NULL) {
        return -AG_ENODEV;
    }

    memset(out, 0, sizeof(*out));
    out->last_error = AG_OK;

    bool report = false;
    lock(s);
    stats_of(s, out);
    if (out->open && (out->stalls > 0u || out->recoveries > 0u ||
                      out->errors > 0u || out->blocks_unarmed > 0u)) {
        const ag_port_ticks_t now = ag_port_ticks();
        if (!s->fault_reported ||
            (ag_port_ticks_t)(now - s->last_fault) >=
                ticks_at_least(SPILINK_FAULT_REPORT_MS)) {
            s->fault_reported = true;
            s->last_fault = now;
            report = true;
        }
    }
    unlock(s);

    if (report) {
        ag_log(AG_LOG_WARN, "spilink",
               "spi%d link: %u blocks clocked, %u timeouts, %u stalls, "
               "%u rebuilds, %u unarmed, %u dropped, %u errors (last %d)",
               bus, (unsigned)out->blocks_clocked, (unsigned)out->timeouts,
               (unsigned)out->stalls, (unsigned)out->recoveries,
               (unsigned)out->blocks_unarmed, (unsigned)out->bytes_dropped,
               (unsigned)out->errors, (int)out->last_error);
    }
    return AG_OK;
}

int32_t ag_spilink_write(int bus, const void *buf, size_t len)
{
    const int idx = link_index(bus);
    if (idx < 0 || buf == NULL || len == 0) {
        return -AG_EINVAL;
    }

    spilink_t *const s = &s_link[idx];
    if (s->lock == NULL) {
        return -AG_ENODEV;
    }

    /* A ring this size cannot hold more, so the rest is the caller's to bring
     * back - and saying so is what "-AG_EAGAIN rather than dropping" means. */
    lock(s);
    if (!s->open) {
        unlock(s);
        return -AG_ENODEV;
    }
    const uint32_t want = (len > s->txcap) ? s->txcap : (uint32_t)len;
    const uint32_t n = ring_put(&s->tx, buf, want);
    const bool     wake = s->master && n > 0u;
    unlock(s);

    /* An idle master is waiting out its poll; this is something to clock. */
    if (wake) {
        (void)ag_port_sem_give(s->tx_sem);
    }

    /* Nothing was dropped: what did not fit is still the caller's. */
    return (n > 0u) ? (int32_t)n : -AG_EAGAIN;
}

/*
 * Write all of `buf`, waiting for room as the service makes it, or until
 * `timeout_ms` has passed with nothing more taken.  Returns what was taken.
 *
 * Why it exists: a caller of the plain write that found the ring full could
 * only sleep and try again, and the shortest sleep there is is a tick - a
 * millisecond, in which a 40 MHz link empties 4 KB and then clocks frames
 * with nothing in them.  Measured: 72% of the wire with 8 KB writes, the
 * same with the receiver checking nothing, so it was never the receiver.
 */
int32_t ag_spilink_write_wait(int bus, const void *buf, size_t len,
                              uint32_t timeout_ms)
{
    const int idx = link_index(bus);
    if (idx < 0 || buf == NULL || len == 0) {
        return -AG_EINVAL;
    }
    spilink_t *const s = &s_link[idx];
    if (s->lock == NULL) {
        return -AG_ENODEV;
    }
    const uint8_t *p = (const uint8_t *)buf;
    size_t         done = 0;
    while (done < len) {
        lock(s);
        if (!s->open) {
            unlock(s);
            break;
        }
        const uint32_t n = ring_put(&s->tx, p + done, (uint32_t)(len - done));
        const bool     wake = s->master && n > 0u;
        unlock(s);
        if (wake) {
            (void)ag_port_sem_give(s->tx_sem);
        }
        done += n;
        if (done < len &&
            !ag_port_sem_take(s->space_sem, ticks_at_least(timeout_ms))) {
            break;
        }
    }
    return (done > 0u) ? (int32_t)done : -AG_EAGAIN;
}

/*
 * BLOCK MODE.  One block in: its record header and its bytes go into the
 * transmit ring together, and the block is remembered until it is
 * acknowledged, with its number and whether it is worth sending again.
 */
int32_t ag_spilink_send(int bus, const void *buf, size_t len, uint32_t flags,
                        uint32_t timeout_ms)
{
    const int idx = link_index(bus);
    if (idx < 0 || buf == NULL || len == 0 || len > 0xFFFFu) {
        return -AG_EINVAL;
    }
    spilink_t *const s = &s_link[idx];
    if (s->lock == NULL) {
        return -AG_ENODEV;
    }
    const bool     reliable = (flags & AG_SPILINK_RELIABLE) != 0u;
    const uint32_t need = SPILINK_REC + (uint32_t)len;
    if (!s->blocks || need > s->txcap) {
        return -AG_EINVAL;
    }
    for (;;) {
        lock(s);
        if (!s->open) {
            unlock(s);
            return -AG_ENODEV;
        }
        if (ring_space(&s->tx) >= SPILINK_REC) {
            /*
             * The header now and the bytes as room is made: waiting for room
             * for the WHOLE block left a 2 KB block waiting while a frame
             * went out half empty (81% of the wire, measured).  The frame
             * builder takes only what is in the ring, and a block cut across
             * frames is simply sent again whole if lost.  One writer a link.
             */
            uint8_t hdr[SPILINK_REC];
            const uint16_t no = s->next_block++;
            put16(hdr, (uint32_t)len);
            put16(hdr + 2, no);
            hdr[4] = reliable ? AG_SPILINK_RELIABLE : 0u;
            hdr[5] = 0;
            put16(hdr + 6, no);
            (void)ring_put(&s->tx, hdr, SPILINK_REC);
            const uint8_t *p = (const uint8_t *)buf;
            size_t         done = ring_put(&s->tx, p, (uint32_t)len);
            unlock(s);
            for (;;) {
                if (s->master) {
                    (void)ag_port_sem_give(s->tx_sem);
                }
                if (done >= len) {
                    return (int32_t)len;
                }
                /* the header is in: the rest of the block has to follow,
                 * however long room takes, or the stream would be cut */
                (void)ag_port_sem_take(s->space_sem,
                                       ticks_at_least(timeout_ms));
                lock(s);
                if (!s->open) {
                    unlock(s);
                    return -AG_ENODEV;
                }
                done += ring_put(&s->tx, p + done, (uint32_t)(len - done));
                unlock(s);
            }
        }
        unlock(s);
        if (!ag_port_sem_take(s->space_sem, ticks_at_least(timeout_ms))) {
            return -AG_EAGAIN;
        }
    }
}

/*
 * One block out, in order: its bytes into `buf` (cut at `cap`), and in
 * `*info` its number and flags - or, for blocks that were lost and not sent
 * again, AG_SPILINK_LOST with the first and last of them, and no bytes.
 */
int32_t ag_spilink_recv(int bus, void *buf, size_t cap,
                        ag_spilink_block_t *info, uint32_t timeout_ms)
{
    const int idx = link_index(bus);
    if (idx < 0 || info == NULL) {
        return -AG_EINVAL;
    }
    spilink_t *const s = &s_link[idx];
    if (s->lock == NULL || !s->blocks) {
        return (s->lock == NULL) ? -AG_ENODEV : -AG_EINVAL;
    }
    const ag_port_ticks_t start = ag_port_ticks();
    const ag_port_ticks_t span = ticks_at_least(timeout_ms);
    for (;;) {
        lock(s);
        if (!s->open) {
            unlock(s);
            return -AG_ENODEV;
        }
        uint8_t hdr[SPILINK_REC];
        if (s->rx.count >= SPILINK_REC) {
            (void)ring_peek(&s->rx, hdr, SPILINK_REC);
            ring_commit(&s->rx, SPILINK_REC);
            info->no = (uint16_t)get16(hdr + 2);
            info->last = (uint16_t)get16(hdr + 6);
            info->flags = hdr[4];
            const uint32_t len = get16(hdr);
            const uint32_t n = (buf == NULL) ? 0u
                               : (len < cap) ? len
                                             : (uint32_t)cap;
            if (hdr[4] & SPILINK_REC_LOST) {
                /* a LOST record arrives whole, in one frame */
                ring_commit(&s->rx, len);
                unlock(s);
                return 0;
            }
            /*
             * The block is taken as it arrives, and the call waits for the
             * rest, however long - as a writer does for room once its header
             * is in.  Leaving a block cut across two frames in the ring until
             * the second arrived took its first piece off the room of every
             * frame (Z->C3 81% of the wire against 92% for bytes, measured).
             */
            uint32_t done = 0;
            for (;;) {
                uint32_t have = s->rx.count;
                if (have > len - done) {
                    have = len - done;
                }
                const uint32_t into = (done < n) ? ((n - done < have) ? n - done
                                                                      : have)
                                                 : 0u;
                if (into > 0u) {
                    (void)ring_take(&s->rx, (uint8_t *)buf + done, into);
                }
                ring_commit(&s->rx, have - into);
                done += have;
                unlock(s);
                if (done >= len) {
                    return (int32_t)n;
                }
                (void)ag_port_sem_take(s->rx_sem, ticks_at_least(100));
                lock(s);
                if (!s->open) {
                    unlock(s);
                    return -AG_ENODEV;
                }
            }
        }
        unlock(s);
        const ag_port_ticks_t used = (ag_port_ticks_t)(ag_port_ticks() - start);
        if (used >= span) {
            return -AG_EAGAIN;
        }
        (void)ag_port_sem_take(s->rx_sem, span - used);
    }
}

int32_t ag_spilink_read(int bus, void *buf, size_t len, uint32_t timeout_ms)
{
    const int idx = link_index(bus);
    if (idx < 0 || buf == NULL || len == 0) {
        return -AG_EINVAL;
    }

    spilink_t *const s = &s_link[idx];
    if (s->lock == NULL) {
        return -AG_ENODEV;
    }

    /*
     * A minute is a link reader's idea of forever, and treating it that way
     * keeps ag_port_ms_to_ticks away from a value that would overflow it.
     */
    const bool      forever = (timeout_ms >= 60000u);
    ag_port_ticks_t span = forever ? 0u : ticks_at_least(timeout_ms);
    const ag_port_ticks_t slice = ticks_at_least(SPILINK_WAIT_SLICE_MS);
    const ag_port_ticks_t start = ag_port_ticks();
    const uint32_t        want =
        (len > s->rxcap && s->rxcap != 0u) ? s->rxcap : (uint32_t)len;

    for (;;) {
        lock(s);
        if (!s->open) {
            unlock(s);
            return -AG_ENODEV;
        }
        const uint32_t n = ring_take(&s->rx, buf, want);
        unlock(s);

        if (n > 0u) {
            return (int32_t)n;
        }
        if (timeout_ms == 0u) {
            return -AG_EAGAIN;
        }

        ag_port_ticks_t wait = slice;
        if (!forever) {
            const ag_port_ticks_t used =
                (ag_port_ticks_t)(ag_port_ticks() - start);
            if (used >= span) {
                return -AG_EAGAIN;
            }
            const ag_port_ticks_t left = span - used;
            if (left < wait) {
                wait = left;
            }
            if (wait == 0u) {
                wait = 1u; /* a wait of no ticks is a poll, and this is a wait */
            }
        }

        /*
         * Outside the lock: a read with a timeout is a wait, and holding the
         * ring while waiting would stop the service task from delivering into
         * it.  The wake is a count, so a block that arrived between the empty
         * check and this take is not lost.
         */
        (void)ag_port_sem_take(s->rx_sem, wait);
    }
}
