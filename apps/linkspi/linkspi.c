/*
 * ArgonOS - LINKSPI.SYS: the wire to another node of the same system, over a
 * four-wire SPI cable.
 *
 * This is NODELINK with a different wire underneath.  Everything above the
 * transport is byte for byte the same driver - the frame assembler, the class
 * vtable, the windows, the scheduler, the service task, the addressing - and
 * that is the point of it: NLINK (apps/common/nlink) was written to know
 * nothing about the physics, so replacing "a UART" with "a clocked bus" is a
 * new .SYS and not a new protocol.  Where this file's reasoning differs from
 * nodelink's, SPI is why, and it says so.
 *
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc \
 *       --include sdk/include --include apps/common --include apps/common/nlink \
 *       -o build\apps\LINKSPI.SYS apps/linkspi/linkspi.c apps/common/nlink/ag_nlink.c
 *   drv load c:\drv\linkspi.sys     (publishes /dev/node1 by default)
 *
 * WHICH END HOLDS THE CLOCK is this driver's business, unlike nodelink's, and
 * it is the one thing the wire cannot hide: SPI has a master and a slave and
 * they are not interchangeable.  So `[linkspi] role` says which this board is.
 * The FRAMES stay symmetric - the same image runs on both ends and both ends
 * send and receive the same NLNK frames - which is the property a cluster of
 * identical boards needs most.
 *
 * WHERE THE SPLIT NOW RUNS, and it is not where it started.  This file began
 * with the driver owning the whole transport on both ends: it filled a block,
 * clocked it and fed what came back, and on a slave that was a losing position
 * by construction.  A slave cannot clock, so it can only receive inside a
 * transaction it has ALREADY armed - and the peer owns the select, so a block
 * that arrives while the driver is between one transfer and the next is not
 * delayed, it is lost.  Measured on this project's own boards: the receiving
 * slave accepted 92 frames while logging 238 gaps and 99 bad, and the master
 * retransmitted 87% of what it sent.  Nothing was wrong with the cable; a
 * driver's loop runs in application time and the wire's rate is not
 * application time.
 *
 * So the two halves are now:
 *
 *   THE KERNEL moves blocks.  On a slave it owns the clock, the select, the gap
 *   between transactions and the arming - several blocks are kept armed and
 *   refilled from an interrupt at a priority the kernel chooses rather than one
 *   a driver is given.  It knows nothing about frames: a block boundary is not
 *   a frame boundary, and it never sees a channel, a class or a sequence
 *   number.
 *
 *   THE DRIVER frames bytes.  It assembles frames into a byte stream and hands
 *   them over (spi_link_write), drains what arrives through the same assembler
 *   nodelink uses (spi_link_read), and decides which frame goes next.  It owns
 *   no clock on that end and has no opinion about transactions, which is why a
 *   slave's readiness is no longer this driver's problem.
 *
 * The MASTER is unchanged and still owns its side completely - it clocks, so
 * `gap_us`, the block loop and the pacing rule below are all still its business.
 * That asymmetry is the ABI's and not a wart: a master chooses when the wire
 * moves and a slave cannot, so only one of the two has a schedule to keep.
 *
 * THE TRANSPORT, and why no new framing was invented for it.  Bytes go out
 * through a 4096-byte ring; a transaction is LSPI_BLOCK bytes in each direction:
 * fill the block from the ring, zero-pad the tail, transfer, and push every
 * received byte through the SAME assembler nodelink uses.  Nothing at the SPI
 * level frames anything.  The assembler already resynchronises on the NLNK
 * magic and checks the header's own CRC, and 0x00 filler cannot be mistaken for
 * a magic, so a block that is lost, late or misaligned costs at most one frame
 * and is covered by go-back-N.  Padding is therefore free to be dumb: it is the
 * one byte value that means nothing.  On a slave the kernel moves those blocks
 * and this file only ever sees the bytes.
 *
 * MEASURED, so far: the UART pair on the same boards moves a 19 KB file at
 * about 46 000 B/s; this wire, with the driver clocking the slave's blocks
 * itself, managed 1 075 B/s in the direction where the SLAVE RECEIVES (86
 * accepted against 110 gaps and 33 bad) while the slave SENDING ran at
 * 11 039 B/s and nearly clean.  That asymmetry is what the split above is for,
 * and the numbers after it are what the next run is for.
 *
 * UNVERIFIED, and said plainly: nothing in this file has run on hardware in its
 * current shape.  The pair it is written for is a soldered four-wire cable
 * between the two ESP32-S3 boards - sck 12, mosi 11, miso 13, cs 10 on both -
 * and the bus pins themselves live in BOARD.CFG as spi<bus>.sck/mosi/miso like
 * every other SPI user's, not in this driver and not in a key invented here.
 *
 * TWO MUTEXES ON THE MASTER, and the naming says which is which.  `lock` is
 * nodelink's single mutex over the wire, the assembler and the queues.  `xfer`
 * covers a block in flight and nothing else: the driver's mutex is released for
 * the transfer so that a 205 us clock does not hold up a caller, and `xfer`
 * keeps one block in flight so two threads cannot clock two blocks out of
 * order.  On a slave neither is held across anything that waits - the kernel's
 * calls are buffer moves - which is the same rule reached from the other side.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

#include "nlink/ag_nlink.h"

AG_DRV("LINKSPI", "0.1", "argon");

/*
 * The two enumerations of the service classes have to agree, and nothing
 * enforces it but this.  The ABI cannot include nlink's header (nlink knows
 * nothing of ArgonOS, which is what lets its codec be tested on a development
 * machine), so the numbers are mirrored by hand - and a mirror nobody checks is
 * a mirror that drifts.  Here it costs one line and fails the build.  Same line
 * as nodelink's, for the same reason, and it has to stay identical: the two
 * drivers publish the same class to the same kernel.
 */
_Static_assert((int)NL_CLS_CONTROL == (int)AG_LINK_CONTROL &&
                   (int)NL_CLS_STREAM == (int)AG_LINK_STREAM &&
                   (int)NL_CLS_BULK == (int)AG_LINK_BULK &&
                   (int)NL_CLASSES == (int)AG_LINK_CLASSES,
               "nlink and the ABI disagree about the service classes");

/* ---------------------------------------------------------------------- */
/* The numbers                                                             */
/* ---------------------------------------------------------------------- */

/*
 * The block: the unit of every transaction, in both directions at once.
 *
 * 512, and NOT the 2592 that would carry a whole window in one transfer, and
 * this is the one place where "faster means a bigger frame" is wrong.  Measured
 * on this pair's UART, and recorded in docs/plans/cluster.md (the SPI section):
 * 24.7% of 516-byte blocks arrive corrupt, and the probability of catching an
 * error rises to 76% at 2592 bytes.  A block is what the line damages, so a
 * smaller one damages less and costs less to send again.
 *
 * WHAT IT COSTS.  A frame is at most NL_QUANTUM + NL_HDR_SIZE + NL_PSUM_SIZE =
 * 284 bytes (LSPI_FRAME_MAX), so a 512-byte block carries at most one frame and
 * roughly half of every block is zero padding.  That is a known cost of this
 * first cut and it is deliberately NOT recovered by growing the block: the
 * paragraph above says bigger blocks are worse, and the plan doc says to
 * measure the line at this size before changing it.
 *
 * It is also comfortably inside the port's largest single transfer (8192
 * bytes), so a block is one SPI transfer and never a loop of them.
 */
#define LSPI_BLOCK 512u

/* The largest a frame can be, and so the denominator of the framing ratio that
 * info() reports: payload over header, payload and payload checksum. */
#define LSPI_FRAME_MAX (NL_QUANTUM + NL_HDR_SIZE + NL_PSUM_SIZE)

/*
 * How many blocks one turn may clock.  ONE, and that is a measurement and not a
 * default.
 *
 * It was four, and the arithmetic behind the four was about the CALLER: a
 * caller-driven send_common does exactly one turn() per message, so one block
 * per turn would cap a bulk send at 512 bytes a call, and the wire would run at
 * the calling rate rather than its own.  The reasoning was right and the
 * conclusion was wrong, because it is not the caller that sets the block rate -
 * the TURN RATE does, and the turn rate is what the loops in pump_task,
 * recv_common and nd_poll decide (wire_backoff, below).
 *
 * WHY A BURST CANNOT WORK HERE, when this driver was the slave's transport.
 * The slave armed ONE transaction per transfer - its task returned from
 * spi_slave_xfer and queued the next one - and that takes time, which is what
 * gap_us is for.  A master clocking blocks back to back was therefore clocking
 * into a chip that was not armed for the second one, and the failure is exactly
 * the one measured: the master reads whatever MISO happened to hold, the slave
 * never sees the bytes, and the two ends disagree about the same cable.  Measured
 * on the 19 KB file with a four-block burst: 47 bad frames on the master, 68% of
 * the slave's frames lost, 18 252 of 19 212 bytes delivered in 120 s while the
 * master clocked 34.7 MB.  The port's queue depth was the other half of that
 * arrangement and could not help while the driver queued one transaction at a
 * time.
 *
 * So one block per turn, and the TURN RATE is the wire's rate.
 *
 * AND THIS IS NOW THE MASTER'S NUMBER ONLY.  The arming above is the kernel's
 * on a slave (spi_link_open, ABI 0.57): several blocks are kept armed, so a
 * burst of turns no longer lands on an unarmed chip, and the driver does not
 * clock at all.  The master's side is untouched and still runs one block per
 * turn, because one master clocking into a correctly armed slave is a
 * different question from one driver trying to be a slave.
 *
 * The FRAME decision is still made exactly once per turn, as nodelink makes it
 * (see turn()): a loop that drained a whole class inside one turn is the 13 ms
 * of pixels that starved the sound on the real board.  It is the block count
 * that changes here, not the decision count.
 */
#define LSPI_BLOCKS_PER_TURN 1u

/* The outbound byte ring: everything emit() produces, waiting for a block.
 * A power of two so the wrap is a mask, and 8 blocks deep - far more than one
 * turn can fill, so the ring is never the thing that drops a frame.
 *
 * IT IS USED BY BOTH ROLES and means slightly different things, which is worth
 * knowing before adding a second one: on a master it holds every byte until the
 * block that carries it is filled, and on a slave it holds only what
 * spi_link_write REFUSED (its transmit ring was full), which wire_write gives
 * straight over whenever the ring is empty. */
#define LSPI_RING      4096u
#define LSPI_RING_MASK (LSPI_RING - 1u)

/*
 * LSPI_SLAVE_TIMEOUT_MS WAS HERE, and its absence is the point of ABI 0.57.
 *
 * It was the slave's wait inside one transaction - one second, argued at length,
 * because a timed-out IDF transfer cannot be cancelled and the next select
 * serves the stale one.  That whole problem is the driver owning a clock it
 * does not have: it armed one transaction, and between that one completing and
 * the next being queued there was a window with nobody listening.  The kernel
 * now keeps several armed and refills them from an interrupt, so there is no
 * wait for this driver to time out and no stale transaction for it to leave
 * behind.  The constant went with the path, deliberately: a second copy of the
 * kernel's arming policy in this file would be a second thing to keep right.
 */

/*
 * The master's pacing, and the two numbers are the whole of it.
 *
 * THE MASTER'S TURN RATE IS THE SLAVE'S UPLINK SPEED, and it is now the only
 * rate there is.  A slave's traffic exists only inside a transaction the master
 * clocks, so a master that turns once per wake-up sets the slave's ceiling:
 * with this kernel's 10 ms tick (nodelink says it twice - "ten milliseconds is
 * a tick; asking for less does not get less") that is 512 bytes per tick, about
 * 50 KB/s, slower than the UART this wire is meant to beat.  So the tick is
 * taken out of the fast path altogether - wire_backoff() below is the one place
 * that decides - and what is left is: while the wire has anything to do, the
 * loops yield and come straight back, and the scheduler tick is paid only when
 * there is nothing to do at all.  The measured 560 blocks/s was a sleep, not a
 * wire; the rate is the turn rate now.
 *
 * LSPI_LIVE_MS is how long "the peer is talking just now" lasts.  Two things
 * about it are worth stating, because the number has been wrong once and the
 * MEANING has been wrong once, in opposite directions:
 *
 *   - what it MEANS.  A valid frame - class, header CRC, payload checksum -
 *     heard on the wire, counted in rx_frames, with exactly one exclusion: a
 *     frame we threw away because our own inbox was full, which is our fault
 *     and not the wire's.  It is NOT a non-zero byte in the block: padding is
 *     zeros, but data is not reliably non-zero, and a measured run kept the
 *     master's clock on for 50 ms at a stretch on the strength of blocks that
 *     delivered nothing.  And it is NOT "a message was delivered" either: an
 *     ACKNOWLEDGEMENT, a reset and a duplicate all count, none of which
 *     delivers anything.  They have to.  The master is the only clock on this
 *     wire, so a master that reads a peer which only acknowledges as silent
 *     stops clocking, the acknowledgements cannot physically move, the
 *     sender's window never opens, and the pair deadlocks into one block per
 *     tick with a retransmission storm on top - the measured 340 B/s, 1.3
 *     frames a second, on a file the UART pair moves at ~46 000 B/s.  The full
 *     rule is written at rx_frames.
 *
 *   - how LONG.  20 ms, two ticks, and the reasoning is that the window has
 *     only one job left: to bridge the master's own scheduling jitter.  It does
 *     not need to estimate the peer's rate, because a peer that is streaming
 *     refreshes it on every block, and a peer that has paused costs exactly one
 *     tick of latency for its next frame - the master's next turn receives that
 *     frame and the fast path is live again.  So a long window buys nothing and
 *     costs wire time, every millisecond of it spent clocking empty blocks: at
 *     50 ms a silence cost about 164 empty blocks, 84 KB of zeros at 305 us
 *     each, and at 20 ms about 65, 33 KB.  It must also be STRICTLY more than
 *     one tick, or a master pre-empted for a tick would see the window expire
 *     between two of its own turns and pay a sleep for nothing: 20 ms is two
 *     ticks, one to be late by and one of margin.
 *
 * LSPI_IDLE_MS is the other end of the same rule: the shortest gap the master
 * leaves between two blocks when it has nothing to send.  A caller that pumps
 * turn() in a tight loop (recv does) must not clock the bus at its own rate.
 *
 * LSPI_LIVE_MS IS THE ONE NUMBER HERE THAT BOTH ROLES USE, and it means the
 * same thing on each even though the work it holds open is different: on a
 * master it is "keep clocking, the peer has more to say", and on a slave it is
 * "keep draining and answering, the peer is saying something".  A slave no
 * longer clocks anything, so it has no use for LSPI_IDLE_MS or the block
 * numbers below - but it still has a reason to come straight back rather than
 * sleep a tick, and rushing a frame's acknowledgement back is that reason.
 */
#define LSPI_LIVE_MS 20u
#define LSPI_IDLE_MS 2u

/*
 * How many blocks the master will keep clocking while it is WAITING FOR AN
 * ANSWER it has not heard yet - frames sent and not acknowledged, with nothing
 * coming back.  64 blocks, which is about 20 ms of wire at 305 us each (205 us
 * of transfer plus gap_us).
 *
 * WHY WAITING FOR AN ANSWER IS WORK AT ALL, because it is the one case where
 * the master clocks for a reply that is not in its own queues: on a wire with
 * one clock, a slave's answer can only leave inside a block the MASTER clocks.
 * A master that sends a request and then decides it has nothing to do stops
 * clocking, so the answer cannot physically move and the exchange costs a
 * scheduler tick (10 ms) at best.  That is structural, not a bug in the peer,
 * and it is exactly what the kernel's `link get` measures: nodedrv.c asks 232
 * bytes at a time and waits for each reply - 682 ms per exchange, 340 B/s,
 * against the UART's 5 ms per exchange.
 *
 * AND WHY IT MUST BE BOUNDED.  A peer that has gone away leaves the window
 * non-empty for ever, so "we have frames outstanding" cannot hold the fast path
 * on its own - that is a core burned with nothing to clock.  20 ms of clocking
 * is long enough to cover the slave's turn-around and the answer's travel
 * (a block or two), and short enough that an absent peer costs 64 empty blocks
 * and then goes back to the tick.  Any frame heard resets the count to zero, so
 * a live exchange - which hears an acknowledgement per round trip - never
 * reaches the bound at all, and neither does a transfer whose acks are arriving.
 */
#define LSPI_FRUITLESS_MAX 64u

/*
 * How long a SLAVE's loop stays awake while it has frames outstanding and
 * nothing has answered them yet.  The same decision as LSPI_FRUITLESS_MAX above,
 * in the unit that works on this end.
 *
 * WHY A SLAVE NEEDS IT AT ALL, since it owes nobody a clock: the reply to a
 * frame it sent arrives in the KERNEL's receive buffer, and the only thing that
 * can move those bytes into the assembler is this driver's next turn.  The
 * kernel cannot wake it - spi_link_read is a poll, not a callback - so a slave
 * that sleeps the tick while it waits answers every round trip one tick late,
 * which is the same 10 ms this whole split exists to remove, just moved to the
 * other board.  On the master the same waiting is what keeps its clock running;
 * on the slave it is what keeps its ear open.
 *
 * AND WHY IT IS MILLISECONDS HERE AND BLOCKS ABOVE.  64 blocks is about 20 ms on
 * a master because a master's unit of waiting is a transaction - 305 us of wire
 * each time.  A slave's turn is a poll of a buffer and costs microseconds, so
 * counting turns would bound almost nothing: 64 of them are gone in a
 * millisecond and the loop would drop back to the tick almost immediately.  The
 * number is the same 20 ms because the question is the same one - has this
 * stopped being an exchange - and only the currency differs.  It runs from the
 * last NEW TRANSMISSION (send_common, and the reset that announces one), so a
 * caller that keeps asking keeps the ear open, and a peer that has gone away
 * costs 20 ms of polling and then the tick takes over.
 */
#define LSPI_SLAVE_WAIT_MS 20u

/*
 * The dead time between two blocks the MASTER clocks, in microseconds; the
 * `gap_us` config key, 0 allowed.
 *
 * A slave can only answer a block if a transaction is already armed when the
 * chip select pulses, and blocks with no gap leave no time to arm the next one -
 * so the block is simply lost, with no error on either end and nothing in any
 * counter.  That is the failure shape that costs an afternoon, and it is the
 * measured reason LSPI_BLOCKS_PER_TURN is 1.
 *
 * ON A SLAVE THAT IS NOW THE KERNEL'S BUSINESS, and this key is not read there:
 * spi_link_open keeps several blocks armed and refills them from an interrupt,
 * which is exactly the margin the gap used to buy.  The MASTER still owns this
 * one, and it is still load-bearing on its side: the fast path makes consecutive
 * TURNS back to back when there is traffic (205 us of wire, then this gap), so
 * the peer still has to be armed between them, and without the gap the next
 * turn's block is the lost one.  The probe that measured this pair used a gap
 * too.
 *
 * 0 is legal, and worth trying: whether the gap is needed at all at this block
 * size is one of the things the link is for measuring.
 */
#define LSPI_GAP_US_DEFAULT 100u

/* The defaults for the config keys, all of them overridable in SYSTEM.CFG
 * under [linkspi].  `dev` is node1 and not node0 because the UART driver
 * already owns node0, and on the Zero both wires are meant to coexist: the SPI
 * pair is the new wire and the UART the one that still answers when it does
 * not. */
#define LSPI_DEV_DEFAULT "node1"
#define LSPI_BUS_DEFAULT 2
#define LSPI_CS_DEFAULT  10
#define LSPI_KHZ_DEFAULT 20000u

/*
 * The windows, in frames, and they are arithmetic rather than taste: a window
 * has to hold a round trip's worth of data or the wire idles waiting for
 * acknowledgements (ag_nlink.h, "SIZING THE WINDOW").
 *
 * NODELINK'S NUMBERS, deliberately unchanged.  They were derived for 5 MB/s and
 * a millisecond, and this wire's rate and round trip are exactly what it has
 * not measured yet - so redoing the arithmetic now would be guessing with extra
 * steps.  `link stats` on this wire gives srtt_ms; window = rate * RTT /
 * NL_QUANTUM is then two multiplications, and that is the moment to change
 * these two lines.
 */
#define LSPI_WIN_CONTROL 4u
#define LSPI_WIN_BULK    24u

/* The stream class is not windowed - it is not retransmitted - so its queue is
 * a plain ring of whole messages waiting for their slot. */
#define LSPI_STREAM_QUEUE 8u

/* Delivered messages waiting to be taken by recv(). */
#define LSPI_RX_QUEUE 8u

/* The reservation, in bytes per period: 44.1 kHz stereo is 176400 B/s and a
 * 10 ms period is 1764.  A number to be set from what is actually being sent,
 * not a constant of the universe - see ag_nlink_sched_init. */
#define LSPI_RESERVE_BYTES 1764u
#define LSPI_PERIOD_MS     10u

/*
 * How long to wait for an acknowledgement before sending the window again.
 * This is where the timeout STARTS, not what it is: NLINK measures the round
 * trip and follows it, because a fixed number is wrong at every speed.  The
 * full reasoning is nodelink's and is not repeated here; the value is the same
 * and for the same reason - the first frames go out before there is anything to
 * measure, and starting too short spends the first exchanges retransmitting.
 */
#define LSPI_RTO_MS 150u

/* frame assembler states */
enum { SM_SYNC = 0, SM_HDR, SM_BODY, SM_PSUM };

typedef struct {
    uint8_t  chan;
    uint8_t  cls;
    /*
     * The addressing the frame carried.  The driver does NOT act on it: it
     * cannot, because it does not know this board's name nor any neighbour's -
     * that is the kernel's (see linkprov.c, which relays).  It is carried here,
     * and out through recv_a, so the kernel can decide whether a message is for
     * this board or is one to move across to the next wire.
     */
    uint16_t dst;
    uint16_t src;
    uint16_t len;
    uint8_t  data[NL_QUANTUM];
} msg_t;

typedef struct {
    /* The wire itself, as SYSTEM.CFG describes it. */
    int      bus;
    int      cs;
    uint32_t khz;    /* what info() computes the rate from                  */
    uint32_t gap_us; /* MASTER only: dead time between two blocks it clocks */
    bool     master; /* true = this end holds the clock                    */
    /*
     * SLAVE only: the kernel's link is open on this bus, so unloading has to
     * close it - spi_link_close stops the service task and gives the bus back.
     * Kept as a flag rather than left to the role because the open can fail
     * after the wire object exists, and a close of something never opened is
     * not a thing to do twice.
     */
    bool     link_open;
    char     dev[AG_DEV_NAME_MAX];

    /*
     * ONE MUTEX over the wire, the assembler and the queues, for the same
     * reason as the radio driver: there is then a single reader, and the frame
     * assembler needs no locking of its own.
     *
     * On a MASTER it is not held across the transfer (see clock_block), which
     * is the one place this driver departs from nodelink's single-lock shape.
     * On a slave it is held across the kernel's calls, because those are buffer
     * moves and not waits - no transaction happens on this end at all.
     */
    ag_mutex_t lock;
    /*
     * MASTER only: one block in flight per wire.  Held across the transfer and
     * the feed that follows it, and never WAITED on: a turn that cannot have it
     * skips its transfer instead of queueing behind the wire's own task, so a
     * caller never waits behind a block on the cable.
     */
    ag_mutex_t xfer;
    bool       up;

    /*
     * The outbound byte stream.  emit() appends here through wire_write; a
     * block carries up to LSPI_BLOCK of it and the rest waits.  `ring_fill`
     * bytes starting at `ring_r` are the stream; nothing is consumed until a
     * block actually goes out, because on a slave waiting for a master most
     * blocks do not.
     */
    uint8_t  ring[LSPI_RING];
    uint32_t ring_r, ring_fill;
    uint32_t tx_dropped;

    /*
     * The bytes in flight, in the direction they are being moved.
     *
     * On a MASTER these are the block being clocked.  Instance state rather than
     * two locals in clock_block(), because the transfer runs with the driver's
     * mutex released and these are what is being read and written while it is.
     *
     * On a SLAVE they are the buffer handed to and from the kernel:
     * spi_link_write takes txblk (the ring's front, made contiguous by
     * ring_peek) and spi_link_read fills rxblk.  Both are only ever touched
     * under the driver's mutex, and only ever by one role, so the same two
     * buffers serve both without a third.
     */
    uint8_t txblk[LSPI_BLOCK];
    uint8_t rxblk[LSPI_BLOCK];

    /*
     * Pacing state: when the last block went out, when a valid frame was last
     * heard on the wire, and how many have been heard in all, which is what
     * clock_block() snapshots around a block to tell whether this one carried
     * anything.  `last_block_ms` is the master's alone; the other two are used
     * by both roles, because both want to know whether the far end is talking.
     *
     * RX_FRAMES MEANS "THE FAR END IS TALKING", and it counts EVERY frame that
     * came out of the assembler valid - class, header CRC and payload checksum -
     * with exactly one exclusion: a frame this end then threw away because its
     * own inbox was full, which is our fault and not the wire's.
     *
     * So an ACKNOWLEDGEMENT counts, and so does a RESET, and so does a
     * duplicate - none of which delivers anything.  That is not tidiness.  On
     * this wire one end is the clock: a master that reads a peer which only
     * acknowledges as SILENT stops clocking, the peer's acknowledgements cannot
     * physically move, the sender's window never opens, and the pair deadlocks
     * into one block per tick with a retransmission storm on top.  Measured
     * before this rule: 340 B/s, 1.3 frames a second, on a file the UART pair
     * moves at ~46 000 B/s.  The byte scan that preceded it had the opposite
     * fault - padding and data look the same to it, so it kept the clock on for
     * 50 ms at a time on blocks that delivered nothing.
     *
     * On a slave the same rule now keeps the driver's loop draining and
     * answering instead of sleeping a tick - it does not hold a clock open, but
     * it does hold an acknowledgement back if it sleeps.
     *
     * `last_rx_ms` is refreshed from this in clock_block(), which is what
     * LSPI_LIVE_MS is measured against.
     */
    uint32_t last_block_ms;
    uint32_t last_rx_ms;
    uint32_t rx_frames;
    /*
     * MASTER ONLY, and the slave's equivalent is by time rather than by block
     * count.  Blocks clocked SINCE anything was heard - reset to zero by every
     * heard frame and by every new transmission from a caller, incremented by a
     * block that went out with nothing on it.  It is the bound on "we have
     * frames outstanding, so an answer is owed to us"
     * (LSPI_FRUITLESS_MAX): the master must keep clocking while it waits,
     * because the answer can only arrive through its clock, and must stop when
     * the waiting has clearly stopped being an exchange.  A slave never
     * increments it - it clocks nothing - and is bounded by
     * LSPI_SLAVE_WAIT_MS instead; see awaiting_answer().
     */
    uint32_t fruitless;
    /*
     * SLAVE ONLY: when the current wait began, in ms, set by a new transmission
     * from a caller.  The other half of the same question as `fruitless` above,
     * measured in the unit a slave's loop actually spends.
     */
    uint32_t wait_started_ms;

    /* Send side: a window per reliable class, a ring for the stream class. */
    ag_nlink_slot_t slot_ctl[LSPI_WIN_CONTROL];
    ag_nlink_slot_t slot_bulk[LSPI_WIN_BULK];
    ag_nlink_tx_t   tx[NL_CLASSES];
    msg_t           stream[LSPI_STREAM_QUEUE];
    uint32_t        stream_r, stream_w, stream_fill;
    uint16_t        stream_seq;

    ag_nlink_rx_t    rx[NL_CLASSES];
    /* The acknowledgement last PUT ON THE WIRE for each class, as against the
     * one rx would give now.  When they differ, the far end is waiting for
     * something only we can say. */
    uint16_t         acked[NL_CLASSES];
    bool             acked_any[NL_CLASSES];
    ag_nlink_sched_t sched;
    uint32_t         period_at; /* ms, when this reservation period began */

    /* Receive side: whole messages, waiting to be taken. */
    msg_t    inbox[LSPI_RX_QUEUE];
    uint32_t in_r, in_w, in_fill;
    /*
     * Two counters, and they are not two names for the same thing.
     *
     * `in_dropped` is the STREAM class only: a frame nobody collected is gone
     * by design, because that class is not acknowledged and a retransmitted
     * audio frame arrives after the moment it was for (see inbox_put).
     *
     * `in_refused` is CONTROL or BULK arriving with no room in the queue.  The
     * frame was NOT taken, the sequence it carries is still owed, and the
     * peer's retransmission brings it back - so non-zero here is NORMAL and
     * harmless under load: it is this board's reader being behind the wire and
     * the window doing its job.  There is deliberately no counter for
     * "accepted and then discarded", because that event is the defect and this
     * file no longer has a path that can produce it; a non-zero value of such
     * a counter is the thing to be afraid of, and its absence is the point.
     *
     * Neither number is in ag_link_stats_t, so neither can be printed by
     * `link`: no field of it means this, and `gaps`, `dups` and `bad` all mean
     * something else.  THIS driver does print both, in the listener-stopped log
     * at the bottom of pump_task - which is more than the previous shape had,
     * and that absence is what let the defect live: `in_dropped` was printed
     * nowhere, no command showed it, and the sender was told nothing.
     */
    uint32_t in_dropped;
    uint32_t in_refused;

    /* frame assembler */
    int            sm;
    uint8_t        win[4];
    uint8_t        hdr[NL_HDR_SIZE];
    uint32_t       hlen;
    ag_nlink_hdr_t cur;
    uint8_t        body[NL_QUANTUM];
    uint32_t       blen;
    uint8_t        psum[NL_PSUM_SIZE];
    uint32_t       plen;

    uint32_t tx_bytes, rx_bytes, bad, acks;
    /*
     * Blocks, not frames: `blocks_ok` went through, `blocks_failed` did not,
     * `blocks_heard` of the ones that went carried a frame from the peer, and
     * `tx_dropped` counts bytes the ring had no room for.  They are NOT in
     * stats, because ag_link_stats_t is the ABI's and nodelink's counters are
     * the shape it has - so they are said out loud where a fault shows up (see
     * clock_block) and once more when the listener stops, which is where the
     * ratio that diagnoses a wire - blocks clocked against blocks that carried
     * anything - finally has a reader.
     */
    uint32_t blocks_ok, blocks_failed, blocks_heard;
    bool     fault_said, drop_said;
} node_t;

/*
 * ONE WIRE, and the config section says which.  Nodelink carries up to four
 * from `[link]`, `[link2]`, ... because the middle of a star needs a UART per
 * spoke.  A board has at most two SPI buses (AG_SPI_BUSES) and the bus, the
 * chip select, the clock and the role are one wire's worth of decision, so this
 * driver publishes exactly one device from one `[linkspi]` section.  A second
 * SPI wire, if a board ever wants one, is a second section and a name of its
 * own - not a table built from a template.
 */
static ag_svc_t s_svc;
/* The one wire, for the unload hook.  Kept because a slave's link has to be
 * closed by name at teardown and the device is gone by then; see
 * linkspi_unload(). */
static node_t *s_nd;

static const ag_io_api_t   *IO;
static const ag_task_api_t *TASK;

static uint32_t now_ms(void)
{
    return (uint32_t)(g_ag_api->time->us() / 1000u);
}

/* ---------------------------------------------------------------------- */
/* The wire                                                                */
/* ---------------------------------------------------------------------- */

/*
 * The outbound buffer, which is the MASTER's whole send path and the SLAVE's
 * leftovers.  nodelink handed bytes to uart_write and looped until they were
 * taken; here they are appended, and what happens next depends on which end of
 * the wire this is - see wire_write below, which is the only policy over this
 * and the only caller that matters.
 *
 * A FULL RING DROPS, and counts.  Blocking here would stall whichever caller is
 * trying to send, and it cannot happen while the wire is moving: a master turn
 * carries LSPI_BLOCKS_PER_TURN * LSPI_BLOCK = 512 bytes out and decides at most
 * one frame in - 284 bytes (LSPI_FRAME_MAX) at the very most - and a slave turn
 * hands the kernel everything it will take.  It can happen when nothing is
 * moving at all: a master whose peer is not answering, a slave whose kernel ring
 * has been full for a whole turn, a cable unplugged from both.  Then dropping is
 * right - those bytes are a frame that will not arrive, which is exactly what a
 * call to a wire with nobody on it produces.  The tail is dropped rather than
 * the head, so a truncated frame is on the wire; the far end resynchronises on
 * the next NLNK magic and go-back-N sends it again.
 *
 * Caller holds nd->lock.
 */
static void ring_append(node_t *nd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        const uint32_t room = LSPI_RING - nd->ring_fill;
        if (room == 0u) {
            nd->tx_dropped += (uint32_t)len;
            /* Once, and out loud: a ring that is full means nothing is being
             * moved out at all, which is the one state this wire can be in
             * where every caller's send succeeds and nothing arrives. */
            if (!nd->drop_said) {
                nd->drop_said = true;
                ag_log(AG_LOG_WARN, "linkspi",
                       "the send ring is full (%u bytes dropped so far): "
                       "nothing is being moved out of spi%d cs%d",
                       (unsigned)nd->tx_dropped, nd->bus, nd->cs);
            }
            return;
        }
        uint32_t n = (len < room) ? (uint32_t)len : room;
        const uint32_t w = (nd->ring_r + nd->ring_fill) & LSPI_RING_MASK;
        uint32_t       first = LSPI_RING - w;
        if (first > n) {
            first = n;
        }
        memcpy(nd->ring + w, p, first);
        if (n > first) {
            memcpy(nd->ring, p + first, n - first);
        }
        nd->ring_fill += n;
        p += n;
        len -= n;
    }
}

/*
 * Everything emit() produces leaves through here, and the two roles leave by
 * different doors.
 *
 * THE MASTER BUFFERS: nothing can be handed over until a block is filled from
 * the ring, so the ring IS the send path, and clock_block() drains it.  This is
 * also what makes the three-phase lock rule there possible - queueing never
 * touches the bus.
 *
 * THE SLAVE HANDS OVER: the kernel owns the clock and its transmit ring is
 * filled a call at a time, so the bytes go to spi_link_write and the RING HOLDS
 * ONLY WHAT IT REFUSED.  -AG_EAGAIN means its ring is full, and the remainder is
 * this driver's to offer again on the next turn - which is why the ring is not a
 * second buffer but the same one, used for the leftovers.  A frame reaches here
 * as three calls (header, body, payload checksum) and the kernel may take the
 * first and refuse the rest, so when the ring is NOT empty the new bytes are
 * appended behind it: the ring always holds the OLDEST unsent bytes, and that is
 * what keeps a frame's three pieces in order.
 *
 * Caller holds nd->lock.
 */
static void wire_write(node_t *nd, const void *buf, size_t len)
{
    if (nd->master || nd->ring_fill > 0u) {
        /* A master has nowhere to put it but the ring; a slave with a
         * remainder already waiting must not jump the queue. */
        ring_append(nd, buf, len);
        return;
    }

    const int32_t took = IO->spi_link_write(nd->bus, buf, len);
    if (took < 0) {
        ring_append(nd, buf, len); /* -AG_EAGAIN: the kernel ring is full */
        return;
    }
    if ((size_t)took < len) {
        ring_append(nd, (const uint8_t *)buf + took, len - (size_t)took);
    }
    /*
     * `tx_bytes` counts what this end actually handed to the wire, which on a
     * slave is bytes and not blocks: there are no blocks on this side any more,
     * and a number that pretended otherwise would not add up against the
     * kernel's own.
     */
    nd->tx_bytes += (uint32_t)took;
}

/*
 * The next block's worth of the stream, WITHOUT consuming it, zero-padded to
 * LSPI_BLOCK.
 *
 * Peek and commit rather than take, because a block that does not go out must
 * not cost a frame: a slave waiting for a master times out on most of its
 * turns, and a version of this that consumed would throw away 512 bytes of its
 * outbound stream on every turn that timed out - the ring would drain itself
 * into transactions nobody clocked, faster than any master could carry, so
 * nothing would ever cross.  The commit happens in clock_block when the transfer
 * returns, under the same transfer lock that kept these bytes untouched.
 *
 * Caller holds nd->lock and nd->xfer.
 */
static uint32_t ring_peek(const node_t *nd, uint8_t *tx, uint32_t max)
{
    uint32_t n = (nd->ring_fill < max) ? nd->ring_fill : max;
    uint32_t first = LSPI_RING - nd->ring_r;
    if (first > n) {
        first = n;
    }
    if (n > first) {
        memcpy(tx, nd->ring + nd->ring_r, first);
        memcpy(tx + first, nd->ring, n - first);
    } else if (n > 0u) {
        memcpy(tx, nd->ring + nd->ring_r, n);
    }
    return n;
}

/* Consume what ring_peek handed out and the wire has now carried. */
static void ring_commit(node_t *nd, uint32_t n)
{
    nd->ring_r = (nd->ring_r + n) & LSPI_RING_MASK;
    nd->ring_fill -= n;
}

/* ---------------------------------------------------------------------- */
/* Receiving                                                               */
/* ---------------------------------------------------------------------- */

/*
 * One message into the queue, room assumed.
 *
 * THE CALLER HAS ALREADY CHECKED THERE IS ROOM, and for a sequenced class that
 * check is what keeps the class honest - see frame_arrived, where it is made
 * BEFORE the sequence number is allowed to move.  The check is repeated here
 * anyway because the STREAM class has no sequence to lean on and dropping is
 * the only thing left to do with a frame that will never be sent again.
 *
 * A STREAM message that cannot be queued is genuinely gone; that is the class
 * working as designed.  "Dropped rather than delayed" is its whole contract and
 * a retransmitted audio frame would arrive after the moment it was for.
 *
 * WHAT THIS BRANCH MUST NEVER SEE AGAIN is a CONTROL or BULK frame.  It used
 * to see them, and that was the defect: the frame had already been accepted,
 * the sequence the peer is acknowledged with had already moved, so the far end
 * was told its message had arrived - and its retransmission, which is what its
 * window exists to make, was refused as a duplicate.  Permanent silent loss, at
 * whole-message granularity, reported to the operator as success.
 *
 * THE ORIGINAL AUTHOR CHOSE THE DROP, and the reasoning was defensible enough
 * that it is worth keeping: stopping the acknowledgement closes the far end's
 * window, and then "the sender blocks on a link that is only slow because
 * nobody here is reading it, and a control message behind the jam waits on a
 * picture nobody wants.  A count says the same thing without the deadlock."
 * He weighed a stall against a silent drop and took the drop because he
 * believed the count would tell somebody.  It did not: `in_dropped` was never
 * printed by any command and the sender was never told, so what shipped was
 * not a count instead of a deadlock but permanent loss with no counter, no log
 * and a success at the end of it.  The instrumentation the choice depended on
 * did not exist, and without it the choice was wrong.  Do not revert this for
 * the reason that produced the bug.
 *
 * ON THIS WIRE IT TOOK TWO BOARDS TO FIND, and it cost whole files rather than
 * frames: the receiving board had accepted 83 frames while only 77 messages
 * reached the file, and every short file was short by a whole number of
 * 240-byte messages (18 252 = 76x240 + 12, 14 400 = 60x240, 3 120 = 13x240).
 * The class is only reachable when the wire outruns the file write behind it,
 * and a wire this driver clocks at thousands of blocks a second is the first
 * one in this tree that does.
 */
static void inbox_put(node_t *nd, uint8_t chan, uint8_t cls, uint16_t dst,
                      uint16_t src, const uint8_t *data, uint16_t len)
{
    if (nd->in_fill >= LSPI_RX_QUEUE) {
        nd->in_dropped++;
        return;
    }
    msg_t *m = &nd->inbox[nd->in_w];
    m->chan = chan;
    m->cls = cls;
    m->dst = dst;
    m->src = src;
    m->len = len;
    if (len > 0) {
        memcpy(m->data, data, len);
    }
    nd->in_w = (nd->in_w + 1u) % LSPI_RX_QUEUE;
    nd->in_fill++;
    /*
     * Delivered, so the far end is talking - but this is not the only place
     * rx_frames moves: see its declaration, and the three other paths in
     * frame_arrived() that deliver nothing and still count.
     *
     * Neither queue-full path counts: not the STREAM drop above, and not the
     * refusal in frame_arrived() that keeps a CONTROL or BULK message for the
     * peer to send again.  Both are this end being behind the wire, which is
     * our fault and not the wire's, and there is nothing to be gained by
     * clocking faster at a peer whose frames we cannot take.
     */
    nd->rx_frames++;
}

/* Defined below, beside the bind that sends it.  Declared here because the
 * receive path has to act on a reset before it judges a sequence. */
static void reset_channels(node_t *nd, bool announce);

static void frame_arrived(node_t *nd)
{
    const ag_nlink_hdr_t *h = &nd->cur;
    const uint8_t         cls = h->cls;

    if (cls >= NL_CLASSES) {
        nd->bad++;
        return;
    }
    if (!ag_nlink_check_psum(h, nd->body, nd->psum)) {
        /*
         * A payload that does not check out is treated exactly as a payload
         * that never came: dropped, not acknowledged, and left to the far end's
         * timer.  There is no separate "corrupt" path because there is nothing
         * different to do - and pretending otherwise would mean acknowledging
         * bytes we know are wrong.
         */
        nd->bad++;
        return;
    }
    nd->up = true;

    /* The peer's acknowledgement rides on every frame it sends. */
    if (h->ack != NL_ACK_NONE) {
        ag_nlink_tx_ack(&nd->tx[cls], h->ack, now_ms());
    }

    if ((h->flags & NL_F_RST) != 0u) {
        /*
         * The peer has started over - booted, or been bound again - and has
         * forgotten both what it sent us and what it had of ours.  Acted on
         * BEFORE the sequence check below, because a receiver waiting for
         * sequence zero rejects everything, this frame included, and a reset
         * that has to arrive in order is not a reset.
         *
         * Not announced back: both ends are now at the beginning, and a reply
         * would only give the two of them something to answer for ever.
         */
        reset_channels(nd, false);
        nd->rx_frames++; /* a reset is the far end talking: see rx_frames */
        ag_log(AG_LOG_INFO, "linkspi", "the other end started over");
        return;
    }

    if ((h->flags & NL_F_ACKONLY) != 0u) {
        /* Nothing but the acknowledgement above.  No sequence, so nothing to
         * accept; no delivery, so nothing to queue; and not acknowledged
         * itself, so the two ends cannot answer each other for ever. */
        /*
         * AND IT IS THE FAR END TALKING.  This is the one that mattered: an
         * acknowledgement delivers nothing, so a counter that only moves when a
         * message is queued reads a peer whose whole contribution is
         * acknowledgements as DEAD - and on this wire that is not a missed
         * measurement.  The master is the only clock, so a master that decides
         * the peer is silent stops clocking, the peer's acknowledgements cannot
         * physically move, the sender's window never opens, and the pair
         * deadlocks into one block per tick with a retransmission storm on top.
         * Measured: 340 B/s, 1.3 frames a second, on a 19 KB file.
         */
        nd->rx_frames++;
        return;
    }

    if (cls == NL_CLS_STREAM) {
        /* Not sequenced and not acknowledged: whatever arrives is delivered,
         * and what did not arrive is not worth knowing about by the time it
         * could be asked for.  Left exactly as it was, and deliberately: for
         * THIS class a full queue is the design (see inbox_put), not a defect,
         * because there is no sequence to hold back and nothing will bring the
         * frame again. */
        inbox_put(nd, h->chan, cls, h->dst, h->src, nd->body, h->len);
        return;
    }

    /*
     * ROOM FIRST, SEQUENCE SECOND - the order of these two statements is the
     * whole of this fix.
     *
     * It was the other way round.  ag_nlink_rx_accept() moved the sequence this
     * receiver will acknowledge, and only afterwards did inbox_put() look and
     * find no room, discard the message and count it.  The far end had already
     * been told that message had arrived, so when it sent the message again -
     * which is precisely what its window is for - rx_accept refused the copy as
     * a duplicate (ag_nlink.c, rx->dups) and the bytes were gone for good.
     * Nothing said so: not a log line, not a counter the shell prints, and not
     * the sender, whose window had slid past it.  What that looked like from
     * outside was `link get` reporting success over a file short by a whole
     * number of messages, because whole messages are what was lost.
     *
     * A REFUSAL IS NOT A LOSS.  Leaving the expected sequence where it is says
     * "I still want this one": the peer's next retransmission brings the same
     * message, and it is taken as soon as the queue has drained.  That is
     * ordinary back-pressure, and it is what the window is for - the peer
     * waits, it does not lose.  It costs retransmissions while this board's
     * reader is behind the wire, and that is the honest price of not lying.
     *
     * WHAT THE PEER IS TOLD stays correct by construction rather than by a
     * second rule: rx->last only moves for a message that is actually queued,
     * so the acknowledgement - riding on the next frame, or sent alone by
     * turn() - still says "everything up to here" and stops short of the frame
     * that was refused.  Neither a reset nor an acknowledgement-only frame
     * reaches this point (both return above, and neither carries a sequence),
     * so neither is affected by it; and the peer's acknowledgement of OUR
     * frames was taken even earlier, so refusing an inbound frame never stops
     * this end's own window from sliding.
     *
     * It is also why the liveness counter does not move here: a refused frame is
     * this end's queue being full, not the far end having gone quiet, and the
     * two must not be confused - see rx_frames.
     */
    if (nd->in_fill >= LSPI_RX_QUEUE) {
        /*
         * Counted, and non-zero here is normal and harmless under load - it
         * means the reader is behind the wire and the window is doing its job.
         * There is no counter for "accepted and then discarded" because this
         * branch is what makes that impossible; see in_refused in node_t.
         *
         * AND IT THROTTLES THE MASTER, which is worth knowing because it is a
         * consequence of two rules meeting rather than a rule of its own.  A
         * refusal does not count as liveness (rx_frames, above): the far end is
         * not quiet, but this end is not listening either, and only one of those
         * is the wire's fault.  So a sustained refusal period lets LSPI_LIVE_MS
         * expire and the master falls back to a block per tick - which is the
         * influx slowing to something the reader can actually take, and the fast
         * path comes back the moment one frame is accepted again.  Without that
         * the master would keep clocking at full rate into a full queue and the
         * peer would retransmit into it.
         */
        nd->in_refused++;
        return;
    }
    if (ag_nlink_rx_accept(&nd->rx[cls], h)) {
        /*
         * Accepted on THIS hop, whether or not it is for this board, and that
         * is deliberate: a relayed frame travels two hops with two independent
         * sequence spaces, so the middle board owes the origin an
         * acknowledgement for its hop and owes the far end its own sequence
         * numbers on the other wire.  Whether it is delivered here or moved
         * across is the kernel's decision, and this driver is not told it -
         * it has no names to decide with.
         *
         * The queue has room - it was checked immediately above and nothing
         * runs in between - so this put cannot drop what was just accepted.
         */
        inbox_put(nd, h->chan, cls, h->dst, h->src, nd->body, h->len);
    } else {
        /* A duplicate or out-of-order frame is answered by re-acking, and the
         * re-ack is worth doing only because the peer is still there: it is the
         * far end talking like any other valid frame. */
        nd->rx_frames++;
    }
}

/*
 * The frame assembler, byte for byte nodelink's, and it is the reason the
 * block transport needed no framing of its own: it looks for a magic and
 * checks a CRC, so it neither knows nor cares where a block began.
 */
static void feed(node_t *nd, uint8_t b)
{
    switch (nd->sm) {
    case SM_SYNC:
        nd->win[0] = nd->win[1];
        nd->win[1] = nd->win[2];
        nd->win[2] = nd->win[3];
        nd->win[3] = b;
        if (nd->win[0] == 'N' && nd->win[1] == 'L' && nd->win[2] == 'N' &&
            nd->win[3] == 'K') {
            memcpy(nd->hdr, nd->win, 4);
            nd->hlen = 4;
            nd->sm = SM_HDR;
        }
        break;

    case SM_HDR:
        nd->hdr[nd->hlen++] = b;
        if (nd->hlen < NL_HDR_SIZE) {
            break;
        }
        if (!ag_nlink_unpack(&nd->cur, nd->hdr)) {
            /* The header failed its own CRC, so `len` is not a number to
             * believe - resync from the next magic rather than skipping
             * however many bytes a corrupt field claims. */
            nd->bad++;
            nd->sm = SM_SYNC;
            break;
        }
        nd->blen = 0;
        nd->plen = 0;
        nd->sm = (nd->cur.len > 0) ? SM_BODY
                  : ((nd->cur.flags & NL_F_PSUM) ? SM_PSUM : SM_SYNC);
        if (nd->sm == SM_SYNC) {
            frame_arrived(nd);
        }
        break;

    case SM_BODY:
        nd->body[nd->blen++] = b;
        if (nd->blen == nd->cur.len) {
            if ((nd->cur.flags & NL_F_PSUM) != 0) {
                nd->sm = SM_PSUM;
            } else {
                frame_arrived(nd);
                nd->sm = SM_SYNC;
            }
        }
        break;

    case SM_PSUM:
        nd->psum[nd->plen++] = b;
        if (nd->plen == NL_PSUM_SIZE) {
            frame_arrived(nd);
            nd->sm = SM_SYNC;
        }
        break;

    default:
        nd->sm = SM_SYNC;
        break;
    }
}

/* ---------------------------------------------------------------------- */
/* Sending                                                                 */
/* ---------------------------------------------------------------------- */

/* The acknowledgement to put on the next frame of this class. */
static uint16_t ack_for(node_t *nd, uint8_t cls)
{
    return (cls < NL_CLASSES) ? ag_nlink_rx_ack(&nd->rx[cls]) : NL_ACK_NONE;
}

/* Build one frame and append it to the ring.  Under the lock, from the
 * scheduler; the wire carries it when the next block is filled. */
static void emit(node_t *nd, uint8_t cls, uint8_t chan, uint16_t seq,
                 uint8_t flags, uint16_t dst, uint16_t src,
                 const uint8_t *body, uint16_t len)
{
    ag_nlink_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.cls = cls;
    h.chan = chan;
    h.seq = seq;
    h.flags = flags;
    h.ack = ack_for(nd, cls);
    /* The credit field is reserved: this driver's back-pressure is the window
     * itself, and a second mechanism that says the same thing differently is a
     * second mechanism to get wrong. */
    h.credit = 0;
    h.len = len;
    /* Stamped, not decided: what the caller asked for, or - for the frames the
     * link generates about ITSELF, a reset or an acknowledgement - zero, which
     * means "whoever is reading it" and is exactly what those mean.  A relayed
     * frame's src came from the far end of a previous hop and is passed through
     * untouched, because it is what that far end answers. */
    h.dst = dst;
    h.src = src;

    /* Whatever we are sending, it carries the current acknowledgement - so
     * record what the far end has now been told. */
    if (cls < NL_CLASSES) {
        nd->acked[cls] = h.ack;
        nd->acked_any[cls] = (h.ack != NL_ACK_NONE);
    }

    uint8_t hdr[NL_HDR_SIZE];
    ag_nlink_pack(hdr, &h);
    wire_write(nd, hdr, NL_HDR_SIZE);
    if (len > 0) {
        wire_write(nd, body, len);
    }
    if ((flags & NL_F_PSUM) != 0) {
        uint8_t ps[NL_PSUM_SIZE];
        ag_nlink_put_psum(ps, body, len);
        wire_write(nd, ps, NL_PSUM_SIZE);
    }
}

/* Tell the scheduler what is queued, so its answer is about now. */
static void offer_all(node_t *nd, uint32_t t)
{
    for (int c = 0; c < NL_CLASSES; c++) {
        if (c == NL_CLS_STREAM) {
            const uint32_t head =
                (nd->stream_fill > 0)
                    ? (uint32_t)nd->stream[nd->stream_r].len + NL_HDR_SIZE
                    : 0u;
            ag_nlink_sched_offer(&nd->sched, c,
                                 nd->stream_fill * (NL_QUANTUM + NL_HDR_SIZE),
                                 head);
        } else {
            ag_nlink_sched_offer(&nd->sched, c,
                                 ag_nlink_tx_waiting_bytes(&nd->tx[c], t),
                                 ag_nlink_tx_head_len(&nd->tx[c], t));
        }
    }
}

/* ---------------------------------------------------------------------- */
/* The block                                                               */
/* ---------------------------------------------------------------------- */

/*
 * The dead time between two blocks the MASTER clocks; see LSPI_GAP_US_DEFAULT.
 * MASTER ONLY: on a slave the kernel owns the transactions and whatever spacing
 * they need, and this is never reached (slave_move does not call it).
 *
 * Bounded on the microsecond clock and spun rather than slept: sleep_ms(1) in
 * this kernel is a 10 ms tick, which would turn a 100 ms transfer into a
 * minute, and the wait must never exceed gap_us.  This is deliberate dead time,
 * not pacing for its own sake.
 *
 * Called with the driver's mutex released (that is the point of the phase it
 * sits in) and the transfer lock held, so no second block of this wire can
 * close the gap from underneath it.
 */
static void block_gap(const node_t *nd)
{
    if (nd->gap_us == 0u) {
        return;
    }
    const uint64_t start = g_ag_api->time->us();
    while ((uint32_t)(g_ag_api->time->us() - start) < nd->gap_us) {
        /* spin: the bound above is what keeps this from being a stall */
    }
}

/*
 * One block across the wire: fill it from the ring, clock it, feed what came
 * back.  True when the transfer actually happened.  MASTER ONLY - the slave
 * never clocks anything and does not come through here (see slave_move).
 *
 * THE LOCK IS NOT HELD IN THE MIDDLE.  205 us of transfer at 20 MHz is short
 * and it is still not something to hold the driver's mutex through: a caller
 * that wants to queue a message would wait the whole clock, and the rule this
 * file lives by is that a caller never waits on the cable.  So:
 *
 *   1. take the lock, copy up to LSPI_BLOCK pending bytes out of the ring into
 *      the block buffer, and let go of it for the transfer;
 *   2. clock the block, with no lock held - this is the part that waits;
 *   3. take the lock, feed the received bytes to the assembler, and let go.
 *
 * The slave's version of this used to be here too, and it was the reason the
 * rule was written at all: spi_slave_xfer blocked for up to a second waiting for
 * a clock that only the peer could supply.  That path is gone (ABI 0.57 - the
 * kernel keeps the blocks armed now), and no wait of that size exists on either
 * end any more; the 205 us above is what remains, and it is enough reason on its
 * own.
 *
 * WHY THE TRANSFER LOCK IS TAKEN FIRST, before the bytes are copied.  Two
 * threads can be in here at once (the wire's own task and whichever caller is
 * awake), and if both could copy before either consumed, both would clock the
 * same 512 bytes and both would consume them - a duplicated block and a ring
 * that lost 512 bytes it never sent.  Taking `xfer` around the copy, the
 * transfer and the commit makes the block atomic, and taking it WITHOUT WAITING
 * is what keeps the clock off every caller's path: a turn that cannot have the
 * bus skips its transfer and its frame goes out with the next block instead.
 *
 * That the two mutexes are taken in opposite orders is safe for the same
 * reason: the driver-mutex-then-transfer-lock acquisition never waits.
 *
 * Caller holds nd->lock; it is still held on return.
 */
static bool clock_block(node_t *nd)
{
    if (!TASK->mutex_lock(nd->xfer, 0)) {
        return false; /* a block is already in flight on this wire */
    }

    /* Phase 1, under the lock: the block's bytes, zero-padded.  Not consumed -
     * see ring_peek. */
    const uint32_t n = ring_peek(nd, nd->txblk, LSPI_BLOCK);
    if (n < LSPI_BLOCK) {
        memset(nd->txblk + n, 0, LSPI_BLOCK - n);
    }

    /* Phase 2: the transfer, with the driver's mutex released.  THE ONE CALL
     * THE MASTER OWNS: it supplies the clock, and nothing here waits for one. */
    TASK->mutex_unlock(nd->lock);
    const ag_err_t xe =
        IO->spi_xfer(nd->bus, nd->cs, nd->txblk, nd->rxblk, LSPI_BLOCK);
    const bool ok = (xe == AG_OK);
    if (ok) {
        /* Only after a block that went: the gap exists to give the peer time to
         * arm the next transaction, and there is no next one on a bus that has
         * just refused. */
        block_gap(nd);
    }
    TASK->mutex_lock(nd->lock, UINT32_MAX);

    /* Phase 3, under the lock: the bytes are gone only if they went, and the
     * received block goes through the assembler that nodelink uses. */
    if (ok) {
        ring_commit(nd, n);
        nd->blocks_ok++;
        nd->tx_bytes += LSPI_BLOCK;
        nd->rx_bytes += LSPI_BLOCK;
        /*
         * LIVENESS IS A VALID FRAME HEARD, not a non-zero byte in the block.
         * Snapshot the counter, feed the block, and find out whether the peer
         * said anything: the received bytes are only bytes until the assembler
         * has found a magic, a header CRC that comes out and a payload that
         * checks, and a block of padding has none of them.  What counts and
         * what does not is written at rx_frames - an acknowledgement counts.
         */
        const uint32_t heard_before = nd->rx_frames;
        for (uint32_t i = 0; i < LSPI_BLOCK; i++) {
            feed(nd, nd->rxblk[i]);
        }
        if (nd->rx_frames != heard_before) {
            nd->last_rx_ms = now_ms();
            nd->blocks_heard++;
            nd->fruitless = 0; /* the wire answered: the wait is an exchange */
        } else {
            /* A block that carried nothing.  Counted, because it is what
             * bounds the master's clocking while it waits for an answer - see
             * LSPI_FRUITLESS_MAX.  Reset by anything heard and by any new
             * transmission from a caller. */
            nd->fruitless++;
        }
        nd->last_block_ms = now_ms();
    } else {
        /*
         * Nothing went out and nothing came in, so nothing is consumed and no
         * counter of the wire moves.
         *
         * spi_xfer is clocked by us and has nothing to wait for, so any failure
         * here is the bus, its pins, or the clock the port was asked for - there
         * is no "the peer was not there" case to be kind about, which is what
         * the slave's -AG_ETIMEDOUT used to be.
         *
         * Said out loud, but only once, and only after the wire has worked at
         * least once: "a link that worked and then stopped"
         * is the shape of fault that otherwise has no symptom anywhere at all.
         */
        nd->blocks_failed++;
        if (nd->blocks_ok > 0u && !nd->fault_said) {
            nd->fault_said = true;
            ag_log(AG_LOG_WARN, "linkspi",
                   "a block did not go through after %u that did (%d, %u failed "
                   "in all): the far end is not armed for it (gap_us is the "
                   "margin), or the bus or its pins are wrong",
                   (unsigned)nd->blocks_ok, (int)xe,
                   (unsigned)nd->blocks_failed);
        }
    }
    TASK->mutex_unlock(nd->xfer);
    return ok;
}

/*
 * WE ARE WAITING FOR AN ANSWER.  Frames are outstanding on a windowed class and
 * the wait is still fresh - fewer than LSPI_FRUITLESS_MAX blocks have gone out
 * with nothing heard since the last thing the peer said.
 *
 * ONE COPY, and it is load-bearing that there is only one: block_due() asks it
 * to decide whether to CLOCK a block at all, and wire_has_work_now() asks it to
 * decide whether the pumping loop comes straight back instead of sleeping the
 * tick.  On a master those two questions have to give the same answer, because a
 * loop that comes back at once and then finds block_due() false is a spin that
 * clocks nothing - and a loop that clocks nothing never hears the answer it is
 * waiting for.
 *
 * WHY KEEPING AWAKE MATTERS, and it is a different reason on each end:
 *
 *   MASTER  the answer is a frame from the slave, and a slave's frame can only
 *           leave inside a block the master clocks.  The master has nothing of
 *           its own to send after a request, so if it stops clocking when its
 *           own queues empty the answer waits for the next turn - a scheduler
 *           tick, 10 ms - and every round trip costs one.  That is precisely
 *           what nodedrv.c's `link get` does 232 bytes at a time, and it is the
 *           measured 340 B/s.
 *
 *   SLAVE   the answer arrives in the KERNEL's receive buffer and only this
 *           driver can move it into the assembler - spi_link_read is a poll, not
 *           a callback, and nothing wakes this task when bytes land.  So a slave
 *           that sleeps the tick while it waits reads the reply one tick late
 *           and answers one tick late, which is the same 10 ms moved to the
 *           other board.
 *
 * AND THE TWO ARE BOUNDED IN DIFFERENT UNITS, which is not a liberality: the
 * master pays for waiting in blocks clocked (fruitless, 64 of them, about 20 ms
 * of wire) and the slave pays in time (LSPI_SLAVE_WAIT_MS, the same 20 ms).
 * Counting a slave's turns would bound nothing - they are buffer polls costing
 * microseconds - so the one question, "has this stopped being an exchange", is
 * asked in the currency each end actually spends.  A peer that has gone away
 * costs 20 ms on either end and then the tick takes over.
 */
static bool awaiting_answer(const node_t *nd)
{
    bool outstanding = false;
    for (int c = 0; c < NL_CLASSES; c++) {
        if (c == NL_CLS_STREAM) {
            continue;
        }
        if (ag_nlink_tx_pending(&nd->tx[c]) > 0u) {
            outstanding = true;
            break;
        }
    }
    if (!outstanding) {
        return false; /* nothing of ours is unanswered: no exchange is open */
    }
    if (nd->master) {
        return nd->fruitless < LSPI_FRUITLESS_MAX;
    }
    return (uint32_t)(now_ms() - nd->wait_started_ms) < LSPI_SLAVE_WAIT_MS;
}

/*
 * Whether the MASTER clocks a block this turn.  See LSPI_LIVE_MS for its rule
 * and why it is not "one block per wake-up".
 *
 * There is no slave case: a slave's turn does not clock anything (slave_move),
 * and the arming that used to make a slave's answer to this question
 * interesting belongs to the kernel now.
 */
static bool block_due(const node_t *nd)
{
    if (nd->ring_fill > 0u) {
        return true; /* there is something to carry */
    }
    if ((uint32_t)(now_ms() - nd->last_rx_ms) < LSPI_LIVE_MS) {
        return true; /* the peer DELIVERED a frame: it may have more */
    }
    if (awaiting_answer(nd)) {
        /*
         * The reply is owed to us and can only travel on a block we clock, so
         * the idle floor below MUST NOT gate it: without this the master would
         * clock one block per LSPI_IDLE_MS while waiting, the exchange would
         * cost that instead of one block (the measured 682 ms per 232-byte
         * round trip), and wire_backoff() would be yielding into a spin that
         * clocked almost nothing.  The counter beside it is what stops this
         * being a wire that clocks at full rate for a peer that has gone away.
         */
        return true;
    }
    /*
     * The idle block, and the floor under it is what bounds the idle rate: it
     * stops a caller pumping turn() in a tight loop from clocking the bus at its
     * own rate, and because it is on the clock it also limits an idle master to
     * ONE block per LSPI_IDLE_MS however high LSPI_BLOCKS_PER_TURN is set - the
     * timestamps move between two iterations of the loop above.  Whether the
     * block is clocked at all still depends on how often turn() is called, and
     * with this kernel's 10 ms tick an idle master's real cadence is the tick's;
     * the floor is only the shortest gap it will leave.
     */
    return (uint32_t)(now_ms() - nd->last_block_ms) >= LSPI_IDLE_MS;
}

/*
 * The first sequenced class whose acknowledgement has NOT gone out yet, or -1
 * when the far end has been told everything we know.
 *
 * ONE COPY, because it is asked in two places and the two must agree: turn()
 * asks it to decide whether to put an ACKONLY frame on the wire, and
 * wire_backoff() asks it to decide whether the wire has anything to do at all.
 * `acked[c]` is what went out last and `ag_nlink_rx_ack()` is what rx would say
 * now; when they differ, the far end is waiting for something only we can say.
 */
static int ack_owed_class(const node_t *nd)
{
    for (int c = 0; c < NL_CLASSES; c++) {
        if (c == NL_CLS_STREAM) {
            continue; /* not sequenced, so nothing to acknowledge */
        }
        const uint16_t now_ack = ag_nlink_rx_ack(&nd->rx[c]);
        if (now_ack == NL_ACK_NONE) {
            continue;
        }
        if (nd->acked_any[c] && nd->acked[c] == now_ack) {
            continue; /* already told them */
        }
        return c;
    }
    return -1;
}

/*
 * Is there anything for the wire to do RIGHT NOW?
 *
 * Read under the lock.  Six things count, and none of them is "the link has
 * traffic" in general:
 *
 *   - the ring has bytes.  A frame was emitted on the last turn and the round
 *     after this one will carry it;
 *   - a frame is READY to go on a windowed class.  ag_nlink_tx_waiting_bytes()
 *     is the call the scheduler itself is fed from, and it counts what is
 *     unsent plus what is DUE to go again - so a frame waiting on its
 *     retransmission timer does not hold the fast path open.  Waiting on a
 *     timer is what the tick is for;
 *   - the stream queue holds a whole message.  That class has no window, so
 *     waiting_bytes() cannot see it;
 *   - WE OWE AN ACKNOWLEDGEMENT.  This is the state turn() already tracks for
 *     the ack-only emit, asked once through ack_owed_class() - the same test,
 *     not a new one - and it is what makes a master that is only RECEIVING have
 *     work: it owes acks, it is the only clock, and without this it would sleep
 *     the tick and starve its own peer of the clock that peer needs to answer;
 *   - WE ARE WAITING FOR AN ANSWER: frames sent and not yet acknowledged, with
 *     nothing heard back.  The answer to those frames can only leave inside a
 *     block WE clock, so a master that stops clocking because its own queues are
 *     empty makes every round trip cost a scheduler tick - which is the measured
 *     `link get`: nodedrv.c asks 232 bytes and waits, 682 ms an exchange, 340
 *     B/s, against the UART's 5 ms.  That is latency, not bandwidth, and nothing
 *     about it was lost or corrupt;
 *   - the peer said something within LSPI_LIVE_MS (rx_frames, which counts an
 *     acknowledgement as talking).  It usually has more.
 *
 * THE GUARD, and it is why the fifth test above is not simply "unacknowledged
 * frames".  A peer that has gone away leaves its window non-empty FOR EVER, so
 * that condition alone would hold the fast path open for ever: a core burned
 * clocking empty blocks at a wire that is not there.  So it holds only while
 * nd->fruitless < LSPI_FRUITLESS_MAX - a fruitless counter, reset to zero by any
 * frame heard and by any new transmission from a caller.  A live exchange hears
 * an acknowledgement per round trip, so it never reaches the bound and never
 * falls out of the fast path; an absent peer costs about 20 ms of clocking and
 * then goes back to the tick.  Everything else here is bounded by construction:
 * an owed ack is cleared by the very next turn, which emits it.
 *
 * AND THE PIECES MEAN SLIGHTLY DIFFERENT THINGS ON EACH END, which is worth
 * reading before changing one:
 *
 *   - the ring, the stream queue, ready frames and an owed acknowledgement are
 *     work on EITHER end - a slave still has to emit a frame and hand it over,
 *     and it still has to answer what arrived, so its loop must not sleep while
 *     any of those is true.  On a slave "ready to go" means the frame is about
 *     to be written to the kernel, which is promptness rather than clocking;
 *   - liveness is the same idea on both - on a master "keep clocking", on a
 *     slave "keep draining and answering";
 *   - WAITING FOR AN ANSWER IS ALSO BOTH, but for opposite reasons: it holds the
 *     master's clock open and it holds the slave's EAR open, because on a slave
 *     the reply lands in the kernel's buffer and only this driver can read it
 *     out.  The two are bounded differently - blocks clocked against wall time -
 *     and awaiting_answer() has the whole of that.
 */
static bool wire_has_work_now(const node_t *nd)
{
    if (nd->ring_fill > 0u) {
        return true; /* the master's block, or the slave's leftovers */
    }
    if (nd->stream_fill > 0u) {
        return true;
    }
    if (ack_owed_class(nd) >= 0) {
        return true;
    }
    const uint32_t t = now_ms();
    for (int c = 0; c < NL_CLASSES; c++) {
        if (c == NL_CLS_STREAM) {
            continue;
        }
        if (ag_nlink_tx_waiting_bytes(&nd->tx[c], t) > 0u) {
            return true; /* ready now */
        }
    }
    if (awaiting_answer(nd)) {
        /* Asked through the same helper block_due() uses, so on a master the
         * loop cannot come straight back to a turn that then declines to clock.
         * Both roles reach it: see the helper for why each has to stay awake. */
        return true;
    }
    return (uint32_t)(t - nd->last_rx_ms) < LSPI_LIVE_MS;
}

/*
 * What a pumping loop does between two turns - THE one place that decides, and
 * it decides because the alternative has already cost this driver two measured
 * rounds.
 *
 * THE SCHEDULER TICK IS NOT A RATE.  sleep_ms(1) in this kernel is a 10 ms tick
 * ("ten milliseconds is a tick; asking for less does not get less"), so any loop
 * that sleeps between turns pins the wire at a hundred turns a second whatever
 * the bus can do.  Measured: four blocks per 10 ms, about 560 blocks/s, against
 * a cable that carries 1 755 781 B/s.  It bit first as pump_task sleeping on
 * the ROLE, and then again as recv_common and nd_poll sleeping after every
 * turn - so the rule lives here now and all three callers use it, because three
 * copies of a pacing rule is three chances to get the rate wrong:
 *
 *   - the turn moved bytes, or the wire has something to do now -> yield and
 *     come straight back.  Work in progress is never paced by a sleep whose unit
 *     is the scheduler tick, on either end;
 *   - otherwise -> sleep the tick.  Nothing is happening: an idle link, a peer
 *     that has gone quiet, or a window whose only work is waiting on its
 *     retransmission timer.  This is what keeps a board with no cable at the
 *     other end - and a slave whose peer is off - from burning a core.
 *
 * THE SLAVE NO LONGER HAS A SPECIAL CASE HERE, and that is the split showing
 * through: it used to have to return immediately after every turn because
 * returning from a transfer and re-arming WAS its readiness.  The kernel keeps
 * the blocks armed now, so the driver's promptness is only about how quickly it
 * hands over a frame and answers one - which is what the clauses above measure
 * on both ends.
 *
 * The lock is taken to read that state and RELEASED before yielding or
 * sleeping: sleeping while holding the driver's mutex would stop the shell from
 * sending, which is the rule clock_block() exists for.  Call with the lock not
 * held.
 */
static void wire_backoff(node_t *nd, bool moved)
{
    TASK->mutex_lock(nd->lock, UINT32_MAX);
    const bool busy = moved || wire_has_work_now(nd);
    TASK->mutex_unlock(nd->lock);

    if (busy) {
        TASK->yield();
    } else {
        TASK->sleep_ms(1);
    }
}

/*
 * THE SLAVE'S TRANSPORT, which is all that is left of it on this side: bytes in
 * and bytes out, and no clock, no block and no schedule.  Called from turn()
 * before the frame decision, so that everything the peer has said is already in
 * the assembler when the decision is made - and so that an acknowledgement this
 * turn is for a frame that arrived this turn.
 *
 *   (1) HAND OVER what the kernel would not take last time.  The ring holds the
 *       oldest unsent bytes and wire_write only hands new bytes over when it is
 *       empty, so this is what keeps a frame's three pieces (header, body,
 *       payload checksum) in order when the transmit ring filled in the middle
 *       of one.  -AG_EAGAIN is not an error and not a loss: the kernel never
 *       drops, so the remainder is simply offered again on the next turn.
 *
 *   (2) DRAIN everything that has arrived, through the same assembler nodelink
 *       uses, until the kernel has nothing left.  spi_link_read with a timeout
 *       of 0 polls by definition, and -AG_EAGAIN there means "nothing more
 *       buffered", which is the ordinary way out of the loop rather than a
 *       fault.  The loop has no bound because the buffer has one: each call
 *       takes bytes out of it and the next call finds fewer, so the exit is the
 *       emptiness and not a counter.
 *
 *       THE ZERO IS A DELIBERATE FIRST CUT WITH A KNOWN BETTER FORM, and the
 *       better form is small, so it is written here rather than left to be
 *       rediscovered: spi_link_read with a NON-ZERO timeout sleeps inside the
 *       kernel's own call and returns the moment bytes land, which costs no CPU
 *       at all, while 0 makes every turn a poll - a turn that finds nothing must
 *       come back and ask again, and the reply to our own last frame is exactly
 *       what it is asking for.  The intended shape is a LSPI_SLAVE_READ_MS of
 *       about 1 ms, on the wire's own task and not on a caller's path, so that a
 *       caller never blocks in a poll it did not ask for; there is no such
 *       constant in this file yet and adding it IS that change.  It is not a
 *       one-character edit either, because a call that waits must not be made
 *       with the driver's mutex held - the rule clock_block() already lives by -
 *       and two turns must not be inside it at once, which is what the `xfer`
 *       lock is for on the master.  So it is a small patch with a lock rule in
 *       it, not a timeout alone.
 *
 *       WHY IT IS NOT HERE NOW: the measurement round is running against a
 *       FROZEN revision of this file, and a driver edit landing while another
 *       agent builds and flashes puts a moving target under the one run that can
 *       attribute a result to a revision.  What the zero costs is CPU on this
 *       end - a poll loop where the kernel would have slept for us - and not
 *       correctness, so it is worth less than that attribution.  This note is
 *       what keeps the knowledge in the tree instead of in a message.
 *
 * WHERE THE BLOCKS WENT.  The kernel owns the transactions, the arming, the gap
 * and the rate on this end (ABI 0.57, spi_link_open keeps several armed and
 * refills them from an interrupt), so nothing here pads, paces or waits.  A
 * block boundary is not a frame boundary and this function never sees one - it
 * sees a byte stream that happens to arrive in blocks, which is exactly what the
 * assembler was written for.
 *
 * Returns true when the turn moved bytes in or out, which is what the pumping
 * loop's back-off needs to know.
 *
 * Caller holds nd->lock.
 */
static bool slave_move(node_t *nd)
{
    bool moved = false;

    /* (1) Outbound: the leftovers, oldest first. */
    while (nd->ring_fill > 0u) {
        const uint32_t n = ring_peek(nd, nd->txblk, LSPI_BLOCK);
        const int32_t took = IO->spi_link_write(nd->bus, nd->txblk, n);
        if (took <= 0) {
            break; /* the kernel's transmit ring is full; keep the rest */
        }
        ring_commit(nd, (uint32_t)took);
        nd->tx_bytes += (uint32_t)took;
        moved = true;
        if ((uint32_t)took < n) {
            break; /* it took part of the block: its ring is full now */
        }
    }

    /* (2) Inbound: everything, into the assembler. */
    for (;;) {
        const int32_t n = IO->spi_link_read(nd->bus, nd->rxblk, LSPI_BLOCK, 0);
        if (n <= 0) {
            break; /* -AG_EAGAIN: nothing buffered, which is the normal exit */
        }
        nd->rx_bytes += (uint32_t)n;
        for (int32_t i = 0; i < n; i++) {
            feed(nd, nd->rxblk[i]);
        }
        moved = true;
    }
    return moved;
}

/*
 * One turn of the wire: at most one acknowledgement and at most one decided
 * frame, then the bytes move.  The decision is the scheduler's and there is
 * still exactly one of it; the acknowledgement is beside it and is not a
 * decision.  See both below.
 *
 * ONE DECISION, exactly as nodelink decides it.  The scheduler's promise is that
 * a frame offered now goes out within a quantum, and that promise is only kept
 * if the decision is remade between frames - a loop that drained a whole class
 * would be exactly the 13 ms of pixels that starved the sound on the real
 * board.  What follows the decision carries it; it makes no further decisions.
 *
 * A TURN MAY ALSO CARRY ONE ACKNOWLEDGEMENT BESIDE THAT DECISION, emitted
 * before it and for its own class - see the block below.  That is not a second
 * decision: nothing about it is sequenced, scheduled or acknowledged, and the
 * section there has the reasoning and the arithmetic.
 *
 * THE SAME TURN, TWO TRANSPORTS, and the difference is who owns the clock:
 *
 *   MASTER  the decision is followed by one block: fill it from the ring, clock
 *           it, feed what came back (clock_block).  Reading is the tail of the
 *           previous turn's block, so a turn starts with the assembler already
 *           up to date, and the frame decided here goes out in this turn's
 *           block.  That pipeline is one block deep and not recoverable by
 *           reordering: the decision must be made before the block is filled,
 *           and the block must come back before there is anything new to
 *           acknowledge.
 *
 *   SLAVE   there is no block to fill and nothing to clock.  The kernel's
 *           buffers are DRAINED FIRST, so the decision below sees what just
 *           arrived, and the bytes the decision produces are handed over as
 *           wire_write runs (slave_move then retries whatever was refused).  The
 *           driver's order is unchanged - drain, decide, send - and only the
 *           party doing the clocking has moved.
 *
 * Returns true when the turn moved bytes: the pumping loops hand that to
 * wire_backoff().
 *
 * Caller holds the lock.
 */
static bool turn(node_t *nd)
{
    const uint32_t t = now_ms();
    if ((uint32_t)(t - nd->period_at) >= LSPI_PERIOD_MS) {
        ag_nlink_sched_tick(&nd->sched);
        nd->period_at = t;
    }

    /*
     * On a slave, what arrived goes into the assembler BEFORE the decision, so
     * an acknowledgement emitted below answers a frame that is already counted
     * and a reset that arrived below has already been acted on.
     */
    bool moved = false;
    if (!nd->master) {
        moved = slave_move(nd);
    }

    /*
     * AN OWED ACKNOWLEDGEMENT GOES OUT FIRST, AND THE TURN THEN CARRIES ON TO
     * THE NORMAL DECISION.  One path, at the top, and the old copy at the
     * bottom of the scheduler branch is gone: a single place decides when an
     * owed acknowledgement reaches the wire.
     *
     * WHY IT CANNOT WAIT FOR THE SCHEDULER TO GO QUIET.  That is what it used
     * to do - the ACKONLY frame was emitted only when ag_nlink_sched_pick()
     * returned nothing - and during a file transfer that moment never comes:
     * BULK has a frame ready on every turn while the file goes the other way,
     * so CONTROL's acknowledgements sat in acked[] until BULK finished, and the
     * peer retransmitted CONTROL against its own timeout until its four-frame
     * window stalled.  An ack cannot ride another class's frame either: emit()
     * takes h.ack from ack_for() of its OWN class, and frame_arrived() feeds it
     * to ag_nlink_tx_ack() of that same class.  So an owed ack is emitted on its
     * own, and the decision is made afterwards, in the same turn.
     *
     * THIS TURN CAN NOW EMIT TWO FRAMES, and it is worth being explicit so that
     * the next reader does not take it for a lost invariant: the invariant is
     * one DECISION per turn, and an acknowledgement is not a decision.  It has
     * no sequence number, the scheduler does not count it, and it is not itself
     * acknowledged - so it cannot delay the frame the scheduler picks, cannot
     * reorder anything, and cannot make the two ends answer each other for
     * ever.  What bounds a stream frame's latency is the decision, and there is
     * still exactly one of those.
     *
     * AND IT IS FREE IN BLOCK TERMS, on the master.  24 bytes of acknowledgement
     * plus at most 284 of the decided frame is 308, so both fit inside one
     * LSPI_BLOCK and go out in the same transaction: the ack is paid for out of
     * padding that was being clocked anyway.  Nothing is delayed by a
     * measurable amount.  On a SLAVE there is no block to do that arithmetic
     * against - the two frames are 308 bytes in the kernel's stream and it packs
     * them into its own transactions - so the same argument holds by not
     * existing: nothing on that end waits for a boundary.
     */
    const int owe = ack_owed_class(nd);
    if (owe >= 0) {
        emit(nd, (uint8_t)owe, 0, 0, NL_F_ACKONLY, 0u, 0u, NULL, 0);
        nd->acks++;
    }

    offer_all(nd, t);
    const int cls = ag_nlink_sched_pick(&nd->sched);
    if (cls == NL_CLS_STREAM) {
        const msg_t *m = &nd->stream[nd->stream_r];
        emit(nd, NL_CLS_STREAM, m->chan, nd->stream_seq++, NL_F_EOM, m->dst,
             m->src, m->data, m->len);
        nd->stream_r = (nd->stream_r + 1u) % LSPI_STREAM_QUEUE;
        nd->stream_fill--;
        ag_nlink_sched_sent(&nd->sched, cls, (uint32_t)m->len + NL_HDR_SIZE);
    } else if (cls >= 0) {
        const ag_nlink_slot_t *s = ag_nlink_tx_next(&nd->tx[cls], t);
        if (s != NULL) {
            emit(nd, (uint8_t)cls, s->chan, s->seq, s->flags, s->dst, s->src,
                 s->data, s->len);
            ag_nlink_sched_sent(&nd->sched, cls,
                                (uint32_t)s->len + NL_HDR_SIZE + NL_PSUM_SIZE);
        }
    }

    /*
     * THE MASTER'S BLOCK, and a slave stops here: it has already drained and
     * answered, and wire_write handed its bytes over as they were built, so
     * there is nothing left for this end of the wire to do.  Everything below is
     * the master's because the master is the one with a clock.
     *
     * One block, now (LSPI_BLOCKS_PER_TURN), because a burst against a
     * one-transaction-at-a-time slave loses everything after the first; on a
     * master the rate comes from how often this function is called, which
     * wire_backoff() is the one place to decide.  A failed or skipped block ends
     * the loop early: a bus that has just refused is not asked again inside the
     * same turn, and a transfer already in flight will carry what is in the ring
     * anyway.
     *
     * THIS LOOP IS NOT A SECOND DECISION POINT, and it has to stay that way:
     * the data frame above was chosen once, from the scheduler, before the first
     * block was filled, and the acknowledgement beside it is not a decision (see
     * the reasoning above) - so a block carrying a full ring decides nothing
     * about what goes in the next one.  Nothing reachable from clock_block()
     * decides a frame either: the feed path can queue what arrived and, on the
     * peer's reset, can only call the NON-announcing reset_channels(), which
     * emits nothing.  A second DECISION in here would be the loop that drains a
     * class inside one turn - exactly the 13 ms of pixels that starved the sound
     * on the real board - and it would be invisible in every counter this wire
     * keeps.  The block count is a pacing knob; the decision count is not.
     */
    if (!nd->master) {
        return moved;
    }

    uint32_t went = 0;
    for (uint32_t done = 0; done < LSPI_BLOCKS_PER_TURN; done++) {
        if (!block_due(nd)) {
            break;
        }
        if (!clock_block(nd)) {
            break;
        }
        went++;
    }
    return went > 0u;
}

/* ---------------------------------------------------------------------- */
/* The class vtable                                                        */
/* ---------------------------------------------------------------------- */

/*
 * Both halves of every sequenced class, back to the beginning.
 *
 * `announce` tells the far end to do the same.  Reset is symmetric on purpose:
 * "forget what you have of this channel" is worthless one-sided, because the
 * peer's next frame would still carry a sequence our receiver has never seen
 * the start of.
 */
static void reset_channels(node_t *nd, bool announce)
{
    ag_nlink_tx_init(&nd->tx[NL_CLS_CONTROL], NL_CLS_CONTROL, nd->slot_ctl,
                     LSPI_WIN_CONTROL, LSPI_RTO_MS);
    ag_nlink_tx_init(&nd->tx[NL_CLS_BULK], NL_CLS_BULK, nd->slot_bulk,
                     LSPI_WIN_BULK, LSPI_RTO_MS);
    for (int c = 0; c < NL_CLASSES; c++) {
        ag_nlink_rx_init(&nd->rx[c]);
    }
    if (announce) {
        /* Sequence zero on the frame itself, and it does not matter: the far
         * end acts on the flag BEFORE it looks at the sequence, which is the
         * only way a reset can reach a receiver that is refusing everything. */
        emit(nd, NL_CLS_CONTROL, 0, 0, NL_F_RST, 0u, 0u, NULL, 0);
        emit(nd, NL_CLS_BULK, 0, 0, NL_F_RST, 0u, 0u, NULL, 0);
        /* A reset is a new transmission, so the wait for an answer starts over -
         * both halves of it, the master's block count and the slave's clock:
         * see LSPI_FRUITLESS_MAX and LSPI_SLAVE_WAIT_MS. */
        nd->fruitless = 0;
        nd->wait_started_ms = now_ms();
    }
}

/*
 * Binding the wire.
 *
 * THE RESET IS THE POINT, and it is nodelink's, unchanged and for the same
 * reason: a receiver accepts sequence zero first and nothing else -
 * deliberately, so that a lost first frame cannot be silently skipped - and two
 * boards on a bench do not start together.  One has been running for a minute,
 * has sent frames 0..N into a wire nobody was listening to, and never sends
 * zero again; the other boots, expects zero, and counts every frame that
 * arrives as a gap.  For ever.
 *
 * On this wire there is a second starting asymmetry that the reset also covers:
 * the slave may be up and armed long before the master clocks anything, and its
 * first frames go into a bus that no one is driving.
 *
 * Whoever binds LAST fixes the pair, which is why this needs no retry: a board
 * that reboots binds again and its reset reaches a peer that never left.
 */
static ag_err_t nd_start(ag_device_t *dev)
{
    node_t *nd = (node_t *)ag_dev_priv(dev);
    if (nd == NULL) {
        return -AG_ENODEV;
    }
    TASK->mutex_lock(nd->lock, UINT32_MAX);
    reset_channels(nd, true);
    turn(nd);
    TASK->mutex_unlock(nd->lock);
    return AG_OK;
}

static bool nd_up(ag_device_t *dev)
{
    node_t *nd = (node_t *)ag_dev_priv(dev);
    if (nd == NULL) {
        return false;
    }
    TASK->mutex_lock(nd->lock, UINT32_MAX);
    turn(nd);
    const bool r = nd->up;
    TASK->mutex_unlock(nd->lock);
    return r;
}

/*
 * One message queued, addressed or not.  Everything that knows about windows,
 * classes and the stream ring lives here once, so that the addressed and the
 * unaddressed entry points cannot drift apart - they differ in the two numbers
 * they stamp and in nothing else.
 */
static int32_t send_common(ag_device_t *dev, uint8_t chan, uint8_t cls,
                           const void *buf, size_t len, uint16_t dst,
                           uint16_t src)
{
    node_t *nd = (node_t *)ag_dev_priv(dev);
    if (nd == NULL) {
        return -AG_ENODEV;
    }
    if (buf == NULL || cls >= NL_CLASSES) {
        return -AG_EINVAL;
    }
    if (len > NL_QUANTUM) {
        /* Refused rather than split: only the caller knows where its message
         * boundaries are, and a driver that guessed would deliver two halves
         * that look like two messages. */
        return -AG_ERANGE;
    }

    TASK->mutex_lock(nd->lock, UINT32_MAX);
    int32_t r;
    if (cls == NL_CLS_STREAM) {
        if (nd->stream_fill >= LSPI_STREAM_QUEUE) {
            /*
             * The stream class drops rather than blocks, here as on the wire.
             * A caller told -AG_EAGAIN would retry, and a retried audio frame
             * is the thing this class exists to not do.
             */
            nd->stream_r = (nd->stream_r + 1u) % LSPI_STREAM_QUEUE;
            nd->stream_fill--;
        }
        msg_t *m = &nd->stream[nd->stream_w];
        m->chan = chan;
        m->cls = cls;
        m->dst = dst;
        m->src = src;
        m->len = (uint16_t)len;
        memcpy(m->data, buf, len);
        nd->stream_w = (nd->stream_w + 1u) % LSPI_STREAM_QUEUE;
        nd->stream_fill++;
        r = (int32_t)len;
    } else if (ag_nlink_tx_queue(&nd->tx[cls], chan, dst, src, buf, (uint16_t)len,
                                 true)) {
        r = (int32_t)len;
    } else {
        r = -AG_EAGAIN; /* the window is full: back-pressure, not an error */
    }
    /*
     * A NEW TRANSMISSION STARTS THE WAIT OVER, for both halves of the wait.
     * Whatever the master's fruitless counter had accumulated belongs to an
     * exchange that is over, and the slave's ear is now open for a reply until
     * LSPI_SLAVE_WAIT_MS has passed: from here the caller has something
     * outstanding and the wire must hold the fast path open for the answer.
     */
    if (r >= 0) {
        nd->fruitless = 0;
        nd->wait_started_ms = now_ms();
    }
    /*
     * One turn, one block, one FRAME decision (an acknowledgement may ride
     * beside it - see turn) - and the bandwidth does not come from here.  It
     * comes from the caller itself: this message is in the window now, and every
     * subsequent reach of the pumping loop sends the next frame, because
     * wire_backoff() sees a frame ready to go and refuses to sleep the scheduler
     * tick.  That is where the wire's rate comes from, which is why the pacing
     * rule lives in one place and why a burst of blocks was the wrong place to
     * look for it.
     */
    turn(nd);
    TASK->mutex_unlock(nd->lock);
    return r;
}

static int32_t nd_send(ag_device_t *dev, uint8_t chan, uint8_t cls,
                       const void *buf, size_t len)
{
    /*
     * The entry a kernel from before ABI 0.54 calls, and the honest reading of
     * it is "for whoever is reading": zero in both fields is what every frame
     * meant before a board had a neighbour it could not reach directly.
     */
    return send_common(dev, chan, cls, buf, len, 0u, 0u);
}

static int32_t nd_send_a(ag_device_t *dev, uint8_t chan, const void *buf,
                         size_t len, const ag_link_addr_t *to)
{
    if (to == NULL) {
        return -AG_EINVAL;
    }
    /*
     * The three fields are copied out here and carried with the payload: the
     * frame goes out on a later call than this one, and a relayed message must
     * still leave with the destination and the SOURCE it arrived with.
     */
    return send_common(dev, chan, to->cls, buf, len, to->dst, to->src);
}

static int32_t recv_common(ag_device_t *dev, uint8_t *chan, void *buf,
                           size_t max, uint32_t timeout_ms,
                           ag_link_addr_t *from)
{
    node_t *nd = (node_t *)ag_dev_priv(dev);
    if (nd == NULL) {
        return -AG_ENODEV;
    }
    if (buf == NULL || chan == NULL) {
        return -AG_EINVAL;
    }
    /*
     * The deadline is read off the clock, not counted up by the sleep length.
     * Nodelink was an accumulator once - `waited += 2` beside a `sleep_ms(2)` -
     * and the tick is 100 Hz, so every timeout there was five times what it
     * said.  The same mistake here was worse while a slave's turn could block
     * for a second of its own; nothing on either end blocks for long now, and
     * the deadline is still read off the clock rather than accumulated, because
     * a turn's cost is the port's business and not this loop's.
     */
    const uint32_t deadline = now_ms() + timeout_ms;
    for (;;) {
        TASK->mutex_lock(nd->lock, UINT32_MAX);
        const bool moved = turn(nd);
        if (nd->in_fill > 0) {
            const msg_t   *m = &nd->inbox[nd->in_r];
            const uint16_t n = (m->len < max) ? m->len : (uint16_t)max;
            memcpy(buf, m->data, n);
            *chan = m->chan;
            if (from != NULL) {
                from->dst = m->dst;
                from->src = m->src;
                from->cls = m->cls;
            }
            nd->in_r = (nd->in_r + 1u) % LSPI_RX_QUEUE;
            nd->in_fill--;
            TASK->mutex_unlock(nd->lock);
            return (int32_t)n;
        }
        TASK->mutex_unlock(nd->lock);

        if ((int32_t)(now_ms() - deadline) >= 0) {
            return -AG_EAGAIN;
        }
        /*
         * A caller waiting for a message is still a pump: on a master it is
         * clocking the peer, and on a slave it is draining the kernel's buffer
         * and answering it.  So it paces the same way the listener does instead
         * of sleeping the tick - a recv() with a long timeout used to hold the
         * wire at a hundred blocks a second on its own.
         */
        wire_backoff(nd, moved);
    }
}

static int32_t nd_recv(ag_device_t *dev, uint8_t *chan, void *buf, size_t max,
                       uint32_t timeout_ms)
{
    /* A kernel from before 0.54 asks this, and gets no addressing - which is
     * what it understands. */
    return recv_common(dev, chan, buf, max, timeout_ms, NULL);
}

static int32_t nd_recv_a(ag_device_t *dev, uint8_t *chan, void *buf, size_t max,
                         uint32_t timeout_ms, ag_link_addr_t *from)
{
    return recv_common(dev, chan, buf, max, timeout_ms, from);
}

static void nd_poll(ag_device_t *dev, uint32_t ms)
{
    node_t *nd = (node_t *)ag_dev_priv(dev);
    if (nd == NULL) {
        return;
    }
    const uint32_t deadline = now_ms() + ms;
    for (;;) {
        TASK->mutex_lock(nd->lock, UINT32_MAX);
        const bool moved = turn(nd);
        TASK->mutex_unlock(nd->lock);
        if ((int32_t)(now_ms() - deadline) >= 0) {
            return;
        }
        /* Same rule as everywhere: a polling caller is a pump too. */
        wire_backoff(nd, moved);
    }
}

static ag_err_t nd_stats(ag_device_t *dev, ag_link_stats_t *out)
{
    node_t *nd = (node_t *)ag_dev_priv(dev);
    if (nd == NULL) {
        return -AG_ENODEV;
    }
    if (out == NULL) {
        return -AG_EINVAL;
    }
    TASK->mutex_lock(nd->lock, UINT32_MAX);
    memset(out, 0, sizeof(*out));
    for (int c = 0; c < NL_CLASSES; c++) {
        out->sent += nd->tx[c].sent;
        out->retransmits += nd->tx[c].retransmits;
        out->received += nd->rx[c].taken;
        out->gaps += nd->rx[c].gaps;
        out->dups += nd->rx[c].dups;
    }
    out->sent += nd->sched.sent[NL_CLS_STREAM];
    out->bad = nd->bad;
    out->acks = nd->acks;
    /*
     * THE TWO THAT SAY THE WIRE WAS NOT WHAT LOST IT, and `link` prints them
     * right after the loss line, apart and said out loud, because a reader has
     * to be able to tell them from each other and neither from loss.  Their
     * wording is the shell's: "refused: no room for it, so the peer sends it
     * again - normal under load" and "dropped: stream only, which is never
     * acknowledged - by design".
     *
     * That is what they mean here too, and the mapping is one to one: refused is
     * `in_refused`, the CONTROL and BULK frames this end would not take and left
     * the sequence unmoved for, and dropped is `in_dropped`, the STREAM messages
     * thrown away instead of queued.  Not gaps, not duplicates, not bad frames -
     * and before this pair existed a link that acknowledged a frame and then
     * discarded its message reported nothing at all, which is how a short file
     * came back with a success.  A number nobody prints is the same as no
     * number, so it is printed while the link is RUNNING and not only when it
     * stops.
     */
    out->rx_refused = nd->in_refused;
    out->rx_dropped = nd->in_dropped;
    /*
     * The byte counters are the WIRE's, and the two roles count different
     * things, deliberately, because the two roles have different wires under
     * them: a MASTER counts every successful block as LSPI_BLOCK bytes each way,
     * padding included, so on that end these measure how busy the bus is; a
     * SLAVE counts the bytes it actually handed to and took from the kernel,
     * because there are no blocks on that side any more.  Either way the frame
     * counters above are what the payload did - and `send_common` counts the
     * frame long before the bytes leave, so `sent` runs ahead of `tx_bytes` by
     * whatever is still in the window.
     */
    out->tx_bytes = nd->tx_bytes;
    out->rx_bytes = nd->rx_bytes;
    /*
     * CONTROL's, not a sum or an average: the three classes have separate
     * windows and separate timers, and an average of three timeouts is a
     * number no timer ever used.  Control is the one with a latency budget,
     * so it is the one worth watching.
     */
    out->rto_ms = nd->tx[NL_CLS_CONTROL].rto_ms;
    out->srtt_ms = nd->tx[NL_CLS_CONTROL].srtt_ms;
    TASK->mutex_unlock(nd->lock);
    return AG_OK;
}

/* ---------------------------------------------------------------------- */
/* The task that listens when nobody is asking                             */
/* ---------------------------------------------------------------------- */

static void pump_task(void *arg)
{
    node_t *nd = (node_t *)arg;
    if (nd == NULL) {
        return;
    }
    /*
     * WAIT FOR OUR OWN HANDLE BEFORE ASKING WHETHER WE SHOULD STOP.
     *
     * svc_start returns the handle that svc_stopping is asked about, and this
     * task is running before it has returned - so the first question was put
     * about a handle that was still NULL, and a task that answers "yes, stop"
     * to that never listens at all.  Which is exactly the shape of the fault it
     * caused in nodelink: with no listener, nothing reads the wire between
     * commands, and the class that suffers is the UNRELIABLE one - stream is
     * not acknowledged and not repeated, so a frame nobody collected is gone.
     */
    while (s_svc == NULL) {
        TASK->sleep_ms(1);
    }

    while (!ag_api()->task->svc_stopping(s_svc)) {
        TASK->mutex_lock(nd->lock, UINT32_MAX);
        const bool moved = turn(nd);
        TASK->mutex_unlock(nd->lock);
        /*
         * The pacing rule is NOT repeated here.  It lives in wire_backoff(),
         * where recv_common and nd_poll read the same copy - this loop used to
         * carry its own, and the copy in pump_task was right while the copies in
         * the two callers still slept the tick after every turn, which is how
         * the wire ended up at 560 blocks/s with a correct pump task.  Three
         * copies of a pacing rule is three chances to get the rate wrong.
         *
         * AND THE TWO ROLES NOW PACE THE SAME WAY, which is the last thing the
         * split changed here.  A slave used to have to return immediately after
         * every turn - its turn blocked in a transfer and re-arming WAS its
         * readiness, so sleeping would leave the bus unarmed.  Readiness is the
         * kernel's now, so what is left on this side is promptness: hand a frame
         * over, answer one, and otherwise let the tick pass, which is exactly
         * what the one rule says for both.
         */
        wire_backoff(nd, moved);
    }
    /*
     * Said out loud, with the counters, because this is the one moment they can
     * all be.  `blocks_ok` against `blocks_heard` is the ratio that diagnoses a
     * wire - blocks clocked against blocks that actually carried a frame - and
     * the measured 34.7 MB clocked for 18 KB delivered is exactly what that pair
     * is for.  On a SLAVE those block counters are zero and that is by design,
     * not a fault: this end clocks nothing any more, the kernel moves the blocks
     * and keeps its own account of them.  Read the two receive counters there
     * instead - they are also in ag_link_stats_t now and `link` prints them while
     * the link is running, which is where they belong.
     *
     * AND THIS LINE IS THE INSTRUMENT THE LAST DEFECT DID NOT HAVE.  The
     * receive counters had no reader anywhere - not in ag_link_stats_t, not in
     * any command, not in any log - which is how permanent silent loss shipped
     * as success.  `refused` (CONTROL or BULK, no room, the peer will send it
     * again) being non-zero under load is normal; `dropped` (STREAM only, gone
     * by design) is normal in its own way.  Neither is a fault, and that is
     * exactly why they have to be visible: the fault they replaced was
     * invisible, and a number nobody prints is the same as no number.
     *
     * A listener that ends while the board is still running leaves a wire that
     * works for whoever asks and drops everything nobody asked for, which is a
     * fault three layers from where it is felt.
     */
    ag_log(AG_LOG_WARN, "linkspi",
           "spi%d cs%d %s: the listener has stopped - %u blocks clocked, %u "
           "carried a frame, %u failed, %u bytes dropped, %u frames refused and "
           "%u stream messages dropped",
           nd->bus, nd->cs, nd->master ? "master" : "slave",
           (unsigned)nd->blocks_ok, (unsigned)nd->blocks_heard,
           (unsigned)nd->blocks_failed, (unsigned)nd->tx_dropped,
           (unsigned)nd->in_refused, (unsigned)nd->in_dropped);
}

/*
 * What this wire is, for the layers that have to size themselves to it.
 *
 * The rate is computed from the clock actually configured rather than named as
 * a constant, because the clock is a config line and a constant would be a lie
 * the moment anyone changed it.  Eight bits to a byte - SPI is what it is - and
 * then the framing correction, which is the same correction nodelink makes and
 * for the same reason: a frame carries NL_QUANTUM of payload under NL_HDR_SIZE
 * of header and, for the reliable classes, NL_PSUM_SIZE of payload checksum, so
 * LSPI_FRAME_MAX is the largest one a frame can be and the useful fraction is
 * the ratio.
 *
 * WHAT IS NOT CORRECTED: the block.  A block is LSPI_BLOCK bytes whether it
 * carries one frame or none, and at most one 284-byte frame fits in 512 bytes,
 * so roughly half of every block is zeros.  That overhead is real and it is
 * deliberately not folded into this number: it is not a property of the wire
 * but of this first cut's block size, and hiding it inside a smaller
 * bytes_per_sec would bury the number that has to be measured before the block
 * size changes.  docs/plans/cluster.md (the SPI section) records the
 * measurement that argues against a bigger block: 24.7% of 516-byte blocks
 * arrive corrupt, and at 2592 bytes the chance of catching an error is already
 * 76%.
 *
 * rtt_us is left at zero: the kernel has a measured round trip in the stats
 * above, and a driver's guess would only be used before any traffic has flowed.
 */
static ag_err_t nd_info(ag_device_t *dev, ag_link_info_t *out)
{
    node_t *nd = (node_t *)ag_dev_priv(dev);
    if (nd == NULL) {
        return -AG_ENODEV;
    }
    if (out == NULL) {
        return -AG_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    out->mtu = NL_QUANTUM - 8u; /* room for a caller's own small header */

    uint64_t bytes = (uint64_t)nd->khz * 1000u / 8u;
    bytes = bytes * NL_QUANTUM / LSPI_FRAME_MAX;
    out->bytes_per_sec = (uint32_t)bytes;
    out->rtt_us = 0;
    ag_strlcpy(out->wire, "spi", sizeof(out->wire));
    return AG_OK;
}

static const ag_dev_ops_t k_dev_ops = {0};

static const ag_link_ops_t k_link_ops = {
    .size = sizeof(ag_link_ops_t),
    .start = nd_start,
    .up = nd_up,
    .send = nd_send,
    .recv = nd_recv,
    .poll = nd_poll,
    .stats = nd_stats,
    .info = nd_info,
    /* ABI 0.54.  Both pairs are filled in: this driver is built against the
     * current table and answers the old calls too, so a kernel from before the
     * addressing still binds it and gets the unaddressed behaviour it
     * understands. */
    .send_a = nd_send_a,
    .recv_a = nd_recv_a,
};

/* ---------------------------------------------------------------------- */
/* Lifecycle                                                               */
/* ---------------------------------------------------------------------- */

/*
 * The configuration, all of it from one `[linkspi]` section:
 *
 *   [linkspi]
 *   role   = master        ; or slave - who holds the clock
 *   bus    = 2             ; the chip's own SPI number, 2 or 3
 *   cs     = 10            ; the select: OUR output as master, the peer's
 *                          ;   select as an INPUT as slave (spi_link_open
 *                          ;   takes that pin once, not per transfer)
 *   khz    = 20000         ; the clock: set on the bus as master, and what
 *                          ;   info() computes the rate from on either end
 *   gap_us = 100           ; MASTER only: dead time between two blocks it
 *                          ;   clocks.  A slave ignores it - the kernel spaces
 *                          ;   its transactions and keeps several armed
 *   dev    = node1         ; the /dev name this wire publishes
 *
 * The bus's PINS are not here: sck, mosi and miso come from BOARD.CFG's
 * `spi<bus>.sck/mosi/miso`, which is where every other SPI user on this board
 * gets them, and a driver that invented a key of its own would be a second
 * place to get the wiring wrong.  The pair this driver was written for is
 * sck 12, mosi 11, miso 13 on both boards, with cs 10.
 */
static void cfg_str(const char *key, char *buf, size_t len, const char *dflt)
{
    ag_strlcpy(buf, dflt, len);
    if (ag_api()->cfg != NULL && AG_HAS(ag_api()->cfg, get_str)) {
        (void)ag_api()->cfg->get_str(key, buf, len);
    }
}

static int32_t cfg_int(const char *key, int32_t dflt)
{
    if (ag_api()->cfg != NULL && AG_HAS(ag_api()->cfg, get_int)) {
        return ag_api()->cfg->get_int(key, dflt);
    }
    return dflt;
}

static ag_err_t wire_up(void)
{
    node_t *nd = (node_t *)ag_malloc_caps(sizeof(node_t), AG_MEM_DEFAULT);
    if (nd == NULL) {
        /* Internal SRAM when there is nowhere else, for the reason nodelink
         * gives: the windows and queues are one task's read-and-write working
         * set, which is what the slow bus is for, but the CYD's internal side
         * is measured in thousands of bytes and the S3 pair has PSRAM.  This
         * struct is nodelink's thirteen kilobytes plus the five the ring and
         * the two block buffers take. */
        nd = (node_t *)ag_malloc_caps(sizeof(node_t), AG_MEM_FAST);
    }
    if (nd == NULL) {
        return -AG_ENOMEM;
    }

    memset(nd, 0, sizeof(*nd));
    nd->sm = SM_SYNC;
    nd->bus = cfg_int("linkspi.bus", LSPI_BUS_DEFAULT);
    nd->cs = cfg_int("linkspi.cs", LSPI_CS_DEFAULT);
    nd->khz = (uint32_t)cfg_int("linkspi.khz", (int32_t)LSPI_KHZ_DEFAULT);
    if (nd->khz == 0u) {
        ag_log(AG_LOG_WARN, "linkspi",
               "khz = 0 is not a clock; using %u",
               (unsigned)LSPI_KHZ_DEFAULT);
        nd->khz = LSPI_KHZ_DEFAULT;
    }
    const int32_t gap = cfg_int("linkspi.gap_us", (int32_t)LSPI_GAP_US_DEFAULT);
    nd->gap_us = (gap > 0) ? (uint32_t)gap : 0u;
    cfg_str("linkspi.dev", nd->dev, sizeof(nd->dev), LSPI_DEV_DEFAULT);

    /*
     * Which end holds the clock, and this one refuses rather than guesses: two
     * masters on a four-wire cable are two chips driving sck and mosi into each
     * other, which is a hardware fault and not a link that runs slowly.  A role
     * that is spelled wrong is therefore an error and not a default.
     */
    char role[16];
    cfg_str("linkspi.role", role, sizeof(role), "master");
    if (ag_strcmp(role, "master") == 0) {
        nd->master = true;
    } else if (ag_strcmp(role, "slave") == 0) {
        nd->master = false;
    } else {
        ag_log(AG_LOG_ERROR, "linkspi",
               "role = \"%s\" is neither master nor slave: the clock has to be "
               "one end's and only one",
               role);
        ag_free(nd);
        return -AG_EINVAL;
    }

    nd->lock = TASK->mutex_create();
    if (nd->lock == NULL) {
        ag_free(nd);
        return -AG_ENOMEM;
    }
    nd->xfer = TASK->mutex_create();
    if (nd->xfer == NULL) {
        TASK->mutex_delete(nd->lock);
        ag_free(nd);
        return -AG_ENOMEM;
    }

    /*
     * The bus, and the two roles arrive at it from opposite directions:
     *
     *   - the MASTER owns its side and this is where it says so.  spi_config is
     *     what nodelink's uart_config is, and for the same reason: the bus comes
     *     up on first use at whatever BOARD.CFG's `spi<bus>.khz` says, and that
     *     number belongs to the chips that share the bus, not to this wire.  A
     *     failure is said out loud and carried on with, because the transfer
     *     below will fail too and say so again - there is nothing to be gained by
     *     refusing to publish a device whose bus might come up on the next
     *     transfer;
     *   - the SLAVE HANDS ITS SIDE TO THE KERNEL, and that is not optional: with
     *     no service keeping blocks armed there is no slave to clock, and no
     *     amount of care in this driver can be back in a transaction before the
     *     next select pulse.  A failure here is the end of this wire and the load
     *     fails with the reason.
     *
     * `block` is LSPI_BLOCK, the unit every transaction carries in both
     * directions.  It has to be a whole number of words because the engine
     * writes words - 512 is - and the kernel's own maximum is well above it.
     */
    if (nd->master) {
        if (AG_HAS(IO, spi_config)) {
            const ag_err_t ce = IO->spi_config(nd->bus, nd->cs, nd->khz);
            if (ce != AG_OK) {
                ag_log(AG_LOG_WARN, "linkspi",
                       "spi%d cs%d did not take %u kHz: %d (its clock is then "
                       "BOARD.CFG's spi%d.khz)",
                       nd->bus, nd->cs, (unsigned)nd->khz, (int)ce, nd->bus);
            }
        } else {
            ag_log(AG_LOG_WARN, "linkspi",
                   "this kernel has no io->spi_config (ABI < 0.29): spi%d runs "
                   "at BOARD.CFG's spi%d.khz, not at khz = %u",
                   nd->bus, nd->bus, (unsigned)nd->khz);
        }
    } else {
        const ag_err_t se =
            IO->spi_link_open(nd->bus, nd->cs, nd->khz, LSPI_BLOCK);
        if (se != AG_OK) {
            ag_log(AG_LOG_ERROR, "linkspi",
                   "spi%d cs%d would not open as a link at %u kHz in %u-byte "
                   "blocks: %d",
                   nd->bus, nd->cs, (unsigned)nd->khz, (unsigned)LSPI_BLOCK,
                   (int)se);
            TASK->mutex_delete(nd->xfer);
            TASK->mutex_delete(nd->lock);
            ag_free(nd);
            return se;
        }
        nd->link_open = true;
    }

    ag_nlink_tx_init(&nd->tx[NL_CLS_CONTROL], NL_CLS_CONTROL, nd->slot_ctl,
                     LSPI_WIN_CONTROL, LSPI_RTO_MS);
    ag_nlink_tx_init(&nd->tx[NL_CLS_BULK], NL_CLS_BULK, nd->slot_bulk,
                     LSPI_WIN_BULK, LSPI_RTO_MS);
    /* The stream class has no window and no slots; its tx is left zeroed so
     * that anything reading its counters gets zeros rather than rubbish. */
    for (int c = 0; c < NL_CLASSES; c++) {
        ag_nlink_rx_init(&nd->rx[c]);
    }
    ag_nlink_sched_init(&nd->sched, LSPI_RESERVE_BYTES);
    nd->period_at = now_ms();

    const ag_dev_add_t desc = {
        .name = nd->dev,
        .driver = "LINKSPI",
        .cls = AG_DEV_LINK,
        .flags = 0,
        .ops = &k_dev_ops,        /* the registry requires one; empty = ENOTSUP */
        .class_ops = &k_link_ops, /* the vtable the kernel reads                */
        .priv = nd,
    };
    const ag_err_t err = ag_dev_add(&desc);
    if (err != AG_OK) {
        if (nd->link_open) {
            /* The kernel owns a service task from the moment the link opened,
             * and the one thing this driver must not do is leave it running on
             * a board with no wire published to feed it. */
            (void)IO->spi_link_close(nd->bus);
            nd->link_open = false;
        }
        TASK->mutex_delete(nd->xfer);
        TASK->mutex_delete(nd->lock);
        ag_free(nd);
        return err;
    }
    /*
     * The listener.  Started after the device exists, so that a frame arriving
     * in the same millisecond has somewhere to go.
     *
     * Not fatal if it cannot start: the link still works for anything that
     * calls in, which is every `link` command - it simply cannot hear an
     * unsolicited request.  Said out loud rather than assumed, because "the
     * other node is silent" and "this node was not listening" look identical
     * from the far end.
     */
    if (AG_HAS(ag_api()->task, svc_start)) {
        s_svc = ag_api()->task->svc_start(pump_task, nd, "linkspi", 4096, 0);
    }
    if (s_svc == NULL) {
        ag_log(AG_LOG_WARN, "linkspi",
               "no service task: this node answers only while asked");
    }

    s_nd = nd;
    ag_log(AG_LOG_INFO, "linkspi",
           "/dev/%s on spi%d cs%d as %s at %u kHz%s - bind with `link use %s`",
           nd->dev, nd->bus, nd->cs, nd->master ? "master" : "slave",
           (unsigned)nd->khz, (s_svc != NULL) ? ", listening" : "", nd->dev);
    return AG_OK;
}

/*
 * WHAT UNLOADING HAS TO UNDO, and on this wire it is only ever the slave's half.
 *
 * spi_link_close stops the kernel's service task, waits for it and gives the bus
 * back.  The ABI also tears an unclosed link down when the module is unloaded,
 * so a driver that forgot would not actually leave a task running - but relying
 * on that is relying on somebody else's cleanup for this driver's resource, and
 * the close is also the only place this end can stop cleanly rather than being
 * stopped.  The MASTER has nothing to undo: it opened no link, and the calls it
 * makes leave nothing behind.
 *
 * Called once, during module teardown.  s_nd is read without the lock because
 * by then nothing else can be running - the kernel stops the tasks that could
 * have touched it.
 */
static void linkspi_unload(void)
{
    node_t *nd = s_nd;
    if (nd == NULL || nd->master || !nd->link_open) {
        return;
    }
    const ag_err_t e = IO->spi_link_close(nd->bus);
    nd->link_open = false;
    if (e != AG_OK) {
        /* Said out loud: the bus may still be a slave and the service may still
         * be running, and that is worth knowing at the moment it happens rather
         * than as a mystery on the next load. */
        ag_log(AG_LOG_WARN, "linkspi", "spi%d did not close cleanly: %d",
               nd->bus, (int)e);
    }
}

ag_err_t ag_driver_init(void)
{
    IO = ag_api()->io;
    TASK = ag_api()->task;
    if (IO == NULL || TASK == NULL || !AG_HAS(ag_api()->dev, add) ||
        !AG_HAS(IO, spi_xfer) || !AG_HAS(TASK, mutex_create)) {
        return -AG_ENOSYS;
    }

    /*
     * The slave half of this wire is FOUR ABI entries the kernel did not have
     * before 0.57, and a kernel from before them is not a kernel this can
     * degrade against: a slave that cannot keep a block armed is a board that
     * sits on the cable while the master clocks into a window nobody is
     * listening in.  So it is refused, once, with the line that says which end
     * to make the master - rather than a crash on a NULL entry or a link that
     * looks alive and never carries a byte.
     *
     * AND THERE IS NO FALLBACK TO spi_slave_xfer, deliberately.  ABI 0.56's
     * one-transaction-at-a-time slave is what this round removed, and keeping it
     * here as a second path would keep two transports alive in one file: two
     * pacing rules, two answers to "am I armed", and one of them measured at 92
     * accepted frames against 238 gaps.  A kernel that has only that has the
     * half of the wire that does not work.
     *
     * AG_HAS is the probe and it checks the table's size as well as the pointer,
     * so it also covers a table that predates the fields entirely.
     */
    const bool slave_ops = AG_HAS(IO, spi_link_open) &&
                           AG_HAS(IO, spi_link_close) &&
                           AG_HAS(IO, spi_link_write) &&
                           AG_HAS(IO, spi_link_read);

    char role[16];
    cfg_str("linkspi.role", role, sizeof(role), "master");
    if (ag_strcmp(role, "slave") == 0 && !slave_ops) {
        ag_log(AG_LOG_ERROR, "linkspi",
               "role = slave needs io->spi_link_open/close/write/read, which "
               "this kernel does not have - it predates ABI 0.57, whose whole "
               "point is that the kernel keeps the blocks armed; make this end "
               "the master, or run a kernel that has them");
        return -AG_ENOSYS;
    }

    const ag_err_t e = wire_up();
    if (e != AG_OK) {
        ag_log(AG_LOG_ERROR, "linkspi", "not loaded: %d", (int)e);
        return e;
    }
    /*
     * Asked for AFTER the wire is up, so a failed load registers nothing: a hook
     * that fired for a driver that never published a device would be looking at
     * a link this file may have already closed on its way out.  Nothing to undo
     * on a master, but the call is made for both and the hook decides - the
     * alternative is a role test here and the same test again in the hook.
     */
    ag_module_on_unload(linkspi_unload);
    return AG_OK;
}
