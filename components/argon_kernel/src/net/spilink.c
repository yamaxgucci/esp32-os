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
#include <argon/port/random.h>
#include <argon/port/sync.h>
#include <argon/port/task.h>
#include <argon/port/time.h>

#include "core/sysconfig.h"
#include "net/wlink.h"

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
 * THE FRAMES THEMSELVES - their header, sessions, what is sent again and what
 * is skipped, the frame size both ends agree on - are the protocol's and live
 * in net/wlink.h; this file is how they cross an SPI bus.
 */

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
/* One open link                                                          */
/* ---------------------------------------------------------------------- */

typedef struct {
    /*
     * The protocol: rings, acknowledgements, blocks, sessions, the frame
     * size, the lock and the wakes (net/wlink.h).  A raw link uses its rings
     * and lock and nothing else of it.
     */
    wlink_t  w;
    int      bus;
    int      cs;
    uint32_t khz;
    /* Frames a slave keeps armed: SPILINK_SLOTS raw, SPILINK_AHEAD framed. */
    uint32_t nslots;
    /* A master's frame pairs, transmit then receive, DMA-capable. */
    uint8_t *mbuf;
    /*
     * Set when the frames and both rings are one allocation, starting at the
     * frames (slots or mbuf): then that pointer is the only one to free.
     */
    bool     onepiece;
    /*
     * reaped_used (slave) is the payload of the frame just reaped, and hist[]
     * (master) the payloads of the last frames - both are bytes on their way
     * that the room they are compared with did not yet count.
     */
    uint32_t reaped_used;
    uint32_t hist[SPILINK_SLOTS];
    uint32_t hist_at;
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
     * The armed blocks: transmit blocks first, then the same number of receive
     * blocks.  Allocated once, at open, word-aligned and DMA-capable, and
     * handed to the port for as long as a transaction is in flight.
     */
    uint8_t *slots;

    uint8_t *txmem;
    uint8_t *rxmem;

    /*
     * The order the transactions were armed in, and per slot the real bytes
     * armed into it and whether it is still in flight.  Touched only by the
     * service task, which is why they need no lock.
     */
    uint8_t  armq[SPILINK_SLOTS];
    uint32_t qhead;
    uint32_t qcount;
    uint32_t used[SPILINK_SLOTS];
    bool     live[SPILINK_SLOTS];
    uint32_t armed;

    volatile bool  stop;
    ag_port_task_t task;
    ag_port_sem_t  exit_sem;

    /* Written by the service task, read by close and by the stats call. */
    uint32_t blocks_clocked;
    uint32_t blocks_data;
    uint32_t blocks_unarmed;
    uint32_t timeouts;
    uint32_t stalls;
    uint32_t recoveries;
    uint32_t errors;
    ag_err_t last_error;
    /*
     * Liveness for the stall detector and for the rate-limited fault line, in
     * the port's own ticks.
     */
    ag_port_ticks_t quiet_since;
    bool            quiet_started;
    ag_port_ticks_t last_fault;
    bool            fault_reported;
} spilink_t;

/*
 * On the heap, made by the first open of a bus and kept (the lock and the
 * wakes in it outlive a close), as uartlink's are: 1 200 bytes of statics
 * on every board for a link most never open, and the S3's emulator build
 * was 320 bytes over its DRAM (25 Sep 2026).
 */
static spilink_t *s_link[AG_SPI_BUSES];

static spilink_t *link_make(int idx)
{
    if (s_link[idx] == NULL) {
        spilink_t *s = ag_port_alloc(sizeof(*s), AG_MEM_FAST);
        if (s == NULL) {
            s = ag_port_alloc(sizeof(*s), AG_MEM_BYTE);
        }
        if (s != NULL) {
            memset(s, 0, sizeof(*s));
        }
        s_link[idx] = s;
    }
    return s_link[idx];
}

static int link_index(int bus)
{
    const int idx = bus - AG_SPI_FIRST;
    return (idx >= 0 && idx < AG_SPI_BUSES) ? idx : -1;
}

static void lock(spilink_t *s)
{
    (void)ag_port_mutex_take(s->w.lock, AG_PORT_FOREVER);
}

static void unlock(spilink_t *s) { (void)ag_port_mutex_give(s->w.lock); }

static uint8_t *slot_tx(const spilink_t *s, unsigned i)
{
    return s->slots + (size_t)i * s->w.block;
}

static uint8_t *slot_rx(const spilink_t *s, unsigned i)
{
    return s->slots + (size_t)(s->nslots + i) * s->w.block;
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
static ag_err_t slot_arm(spilink_t *s, unsigned i, uint32_t *carried)
{
    uint8_t *const tx = slot_tx(s, i);
    uint32_t       n;

    *carried = 0;

    if (s->w.framed) {
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
        const uint32_t allow = wl_allowance(&s->w, in_flight);
        unlock(s);
        bool nak_was = false;
        n = wl_build(&s->w, tx, allow, 0u, &nak_was, NULL);

        const ag_err_t err =
            ag_port_spi_slave_arm(s->bus, tx, slot_rx(s, i), s->w.block);
        if (err != AG_OK) {
            wl_unbuild(&s->w, n, nak_was);
            return err;
        }
        *carried = n;
        return AG_OK;
    }

    lock(s);
    n = ring_peek(&s->w.tx, tx, s->w.block);
    unlock(s);

    if (n > 0u && n < s->w.block) {
        memset(tx + n, 0, s->w.block - n);
    }

    const ag_err_t err = ag_port_spi_slave_arm(
        s->bus, (n > 0u) ? (const void *)tx : NULL, slot_rx(s, i), s->w.block);
    if (err != AG_OK) {
        return err;
    }

    lock(s);
    ring_commit(&s->w.tx, n);
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

    if (s->w.framed) {
        (void)wl_rx(&s->w, rx, got);
        return;
    }

    if (n > s->w.block) {
        n = s->w.block;
    }
    if (n == 0u) {
        return;
    }

    lock(s);
    const uint32_t put = ring_put(&s->w.rx, rx, n);
    if (put < n) {
        s->w.bytes_dropped += (n - put);
    }
    unlock(s);

    if (put > 0u) {
        /* A count and not a flag: two blocks can arrive before the reader
         * wakes, and one give per block is what lets it see both. */
        (void)ag_port_sem_give(s->w.rx_sem);
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

        if (s->w.framed) {
            wl_rto_check(&s->w);
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
                if (s->w.framed && s->w.agreed && got < s->w.frame) {
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
 * TWO FRAMES QUEUED.  The next frame is built while the current one crosses
 * and is queued behind it (ag_port_spi_xfer_start), so it goes on the wire
 * as the current one ends: sequentially, the ~100 us of every pass between
 * two transfers was wire time lost, a sixth of it with 2 KB frames at 20 MHz
 * and a quarter at 40 (86% and 77%, measured).  The task sleeps in the port
 * while a transfer crosses; it never spins.
 *
 * THE SLAVE KEEPS UP WITH NO GAP.  A first version of this (23 Sep) broke
 * the link - 1354 of 7134 frames came back with no header, frames clocked
 * shifted - and that was blamed on the C3 having no time to take up its
 * next armed frame.  More likely it was two faults found after it: the
 * slave matched completions to slots by the order they were armed in, and
 * its armed frames outlived close.  Measured 24 Sep with the port waiting
 * 20, 10, 5 and 0 us in the driver's interrupt before each queued transfer:
 * 88.7, 90.7, 92.0 and 92.9% at 40 MHz, and not one frame missed at 0.  The
 * driver's own
 * interrupt between two queued transfers is the slave's time, and it is
 * enough.  A frame missed anyway would be caught by its checksum and offset
 * and sent again.
 *
 * Idle, it clocks one frame per SPILINK_POLL_MS, because that frame is the
 * slave's only way to speak.  Frames are built from the slave's room one
 * frame staler than before, so one more of this end's frames is counted as
 * on its way (SPILINK_AHEAD + 1).
 */
#define SPILINK_MHIST (SPILINK_AHEAD + 1u)

static void spilink_master_task(void *arg)
{
    spilink_t *s = (spilink_t *)arg;
    uint8_t *const txf[2] = {s->mbuf, s->mbuf + 2u * s->w.block};
    uint8_t *const rxf[2] = {s->mbuf + s->w.block, s->mbuf + 3u * s->w.block};
    uint32_t       qlen[2] = {0, 0}; /* the frame length each was clocked at */
    uint32_t       qdata[2] = {0, 0};
    unsigned       head = 0;         /* the oldest queued */
    unsigned       out = 0;          /* how many are queued */

    int64_t pass0 = ag_port_us();
    int64_t hot_until = 0;
    bool    first = true;
    for (;;) {
        const bool hot = ag_port_us() < hot_until;
        if (out == 0u) {
            if (s->stop) {
                break;
            }
            if (!hot && !first) {
                /* quiet: a writer's wake, or the poll, and then one frame */
                (void)ag_port_sem_take(s->w.tx_sem,
                                       ticks_at_least(SPILINK_POLL_MS));
            }
            first = false;
        }
        if (!s->stop && (out == 0u || (out < 2u && hot))) {
            /*
             * The slave's room came with its last frame, reckoned when the
             * slave ARMED it - SPILINK_AHEAD frames before it was clocked -
             * and this end has queued one more since: all of those are taken
             * off.
             */
            lock(s);
            uint32_t pending = 0;
            for (unsigned k = 0; k < SPILINK_MHIST; k++) {
                pending += s->hist[k];
            }
            /* the size of this frame: WL_HELLO until agreed */
            const uint32_t fl = s->w.frame;
            uint32_t       allow = wl_allowance(&s->w, pending);
            if (allow > fl - WL_HDR) {
                allow = fl - WL_HDR;
            }
            unlock(s);
            wl_rto_check(&s->w);
            const unsigned slot = (head + out) % 2u;
            bool           nak_was = false;
            const uint32_t n =
                wl_build(&s->w, txf[slot], allow, 0u, &nak_was, NULL);
            const ag_err_t rc = ag_port_spi_xfer_start(s->bus, s->cs,
                                                       txf[slot], rxf[slot],
                                                       fl);
            if (rc != AG_OK) {
                wl_unbuild(&s->w, n, nak_was);
                note_error(s, rc);
                if (out == 0u) {
                    sleep_at_least(SPILINK_IDLE_MS);
                    continue;
                }
            } else {
                lock(s);
                s->hist[s->hist_at] = n;
                s->hist_at = (s->hist_at + 1u) % SPILINK_MHIST;
                unlock(s);
                qlen[slot] = fl;
                qdata[slot] = n;
                out++;
                if (out < 2u && hot) {
                    continue; /* fill the pipe before waiting on it */
                }
            }
        }
        if (out == 0u) {
            continue; /* the start was refused */
        }

        /* the oldest transfer: wait for it, and take in what it brought */
        const int64_t  w0 = ag_port_us();
        const ag_err_t rc = ag_port_spi_xfer_finish(s->bus, s->cs, 1000u);
        const int64_t  w1 = ag_port_us();
        const unsigned slot = head;
        head = (head + 1u) % 2u;
        out--;
        if (rc != AG_OK) {
            /* lost, as far as the stream knows: its ack never comes, and
             * the frame is sent again */
            note_error(s, rc);
            continue;
        }
        lock(s);
        s->wire_us += (uint32_t)(w1 - w0);
        s->loop_us += (uint32_t)(w1 - pass0);
        pass0 = w1;
        s->blocks_clocked++;
        unlock(s);

        const uint32_t got = wl_rx(&s->w, rxf[slot], qlen[slot]);
        lock(s);
        /* data not yet sent, or frames out and waiting for their ack */
        const bool more = (s->w.tx.count > s->w.sent_off) || s->w.infl_n > 0u;
        unlock(s);
        if (qdata[slot] > 0u || got > 0u) {
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
        if (qdata[slot] > 0u || got > 0u || more) {
            hot_until = ag_port_us() + SPILINK_HOT_US;
        }
    }

    /* nothing queued is left behind: close frees what the engine writes */
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
    out->bytes_dropped = s->w.bytes_dropped;
    out->timeouts = s->timeouts;
    out->stalls = s->stalls;
    out->recoveries = s->recoveries;
    out->errors = s->errors;
    out->last_error = s->last_error;
    out->open = s->w.open;
    out->task_stuck = false;
    out->frames_bad = s->w.frames_bad;
    out->seq_gaps = s->w.seq_gaps;
    out->wire_us = s->wire_us;
    out->loop_us = s->loop_us;
    out->reorders = s->reorders;
    out->short_frames = s->short_frames;
    out->rewinds = s->w.rewinds;
    out->naks = s->w.naks;
    out->peer_restarts = s->w.peer_restarts;
    out->starve_data = s->w.starve_data;
    out->starve_credit = s->w.starve_credit;
    out->frame = s->w.agreed ? s->w.frame : 0u;
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
    memset(&s->w.tx, 0, sizeof(s->w.tx));
    memset(&s->w.rx, 0, sizeof(s->w.rx));
}

/*
 * The one teardown path, used by close and by the module-gone watcher.  It logs
 * nothing, for the reason at the top of this file.
 */
static ag_err_t link_teardown(spilink_t *s, ag_spilink_stats_t *out)
{
    lock(s);
    if (!s->w.open) {
        unlock(s);
        return -AG_ENODEV;
    }
    /* No new traffic, and the task leaves at its next look at `stop`. */
    s->w.open = false;
    s->stop = true;
    unlock(s);

    /* A reader blocked in read() has to see the link close under it, and so
     * does a writer waiting for room. */
    (void)ag_port_sem_give(s->w.rx_sem);
    (void)ag_port_sem_give(s->w.space_sem);

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
    if (!s->w.master && s->armed > 0u) {
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
    s->w.block = 0;
    s->khz = 0;
    s->cs = AG_PIN_NONE;
    s->blocks_clocked = 0;
    s->blocks_data = 0;
    s->blocks_unarmed = 0;
    s->w.bytes_dropped = 0;
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
        spilink_t *const s = s_link[i];
        if (s != NULL && s->w.open && s->owner == dev->owner) {
            (void)link_teardown(s, NULL);
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
                                         (int32_t)WL_FRAME_DEFAULT);
        block = (v > 0) ? (uint32_t)v : WL_FRAME_DEFAULT;
        if (block > AG_PORT_SPI_MAX_XFER) {
            block = AG_PORT_SPI_MAX_XFER;
        }
    }
    /* a frame's size travels in 64-byte units, and the first frames are
     * WL_HELLO long, which every frame must be able to take */
    if (framed && (block < WL_HELLO || block % WL_UNIT != 0u)) {
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

    spilink_t *const s = link_make(idx);
    if (s == NULL) {
        return -AG_ENOMEM;
    }

    /*
     * Created once and kept.  Each is checked on its own, so an open that
     * failed halfway leaves nothing that a later open cannot finish.
     */
    if (s->w.lock == NULL) {
        s->w.lock = ag_port_mutex_new();
    }
    if (s->w.rx_sem == NULL) {
        s->w.rx_sem = ag_port_sem_new_counting(255u, 0u);
    }
    if (s->exit_sem == NULL) {
        s->exit_sem = ag_port_sem_new_binary();
    }
    if (s->w.tx_sem == NULL) {
        s->w.tx_sem = ag_port_sem_new_binary();
    }
    if (s->w.space_sem == NULL) {
        s->w.space_sem = ag_port_sem_new_binary();
    }
    if (s->w.lock == NULL || s->w.rx_sem == NULL || s->exit_sem == NULL ||
        s->w.tx_sem == NULL || s->w.space_sem == NULL) {
        return -AG_ENOMEM;
    }

    lock(s);
    if (s->w.open) {
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
        const uint32_t pay = block - WL_HDR;
        /*
         * The transmit ring is also what a frame is sent AGAIN from: it has
         * to hold every frame out and not yet acknowledged, and one more
         * ready.  A slave has four out at once - two armed, one clocked, and
         * one the master has but has not answered yet, because the master
         * queues its next frame before it reads the last (see the master
         * task) - so five.  A ring one frame short sends a frame in every
         * few empty: with four and a sequential master it was three, and
         * the ring of three cost 69.8% of the wire, measured; with the
         * master pipelined, four held 2 KB frames to 75%.  A master hears
         * its acks later still, from frames the slave armed two ahead: six.
         *
         * The receive ring, likewise, is one frame deeper on both ends than
         * a sequential master needed: SPILINK_AHEAD + 2 on a slave (C3
         * receiving at 70% with + 1), + 3 on a master, usually the bigger
         * part, whose room is the slave's whole allowance.  With 2 KB frames
         * a C3 slave's link is then 26 KB in all.
         */
        const uint32_t txeach = ((master ? 6u : 5u) * pay + 3u) & ~3u;
        const uint32_t rxeach =
            ((SPILINK_AHEAD + (master ? 3u : 2u)) * pay + 3u) & ~3u;
        s->w.txcap = framed ? txeach : SPILINK_TX_RING;
        s->w.rxcap = framed ? rxeach : SPILINK_RX_RING;
    }
    s->slots = NULL;
    s->mbuf = NULL;
    s->txmem = NULL;
    s->rxmem = NULL;
    s->onepiece = false;
    /* a master queues two transfers, each a frame out and a frame in */
    const size_t frames = master ? 4u * (size_t)block
                                 : 2u * (size_t)s->nslots * (size_t)block;
    /*
     * ONE PIECE FIRST.  On the C3 a 4 KB framed slave once needed 24 KB of
     * frames and two 16 KB rings, 56 KB, with 81 KB free and the largest free
     * block 61 KB - and asked for as three, the third was refused: the first
     * two took the big block apart and what was left was all smaller than
     * 16 KB.  Now it is one allocation - 26 KB at the C3's 2 KB frames, and
     * a 4 KB frame (52 KB) no longer fits it.  The rings are then DMA-capable memory they
     * do not need to be, which on the C3 is all its memory anyway; a part
     * that cannot find the one piece gets the three instead.
     */
    uint8_t *const one = ag_port_alloc_aligned(
        4u, frames + (size_t)s->w.txcap + (size_t)s->w.rxcap, AG_MEM_DMA);
    if (one != NULL) {
        s->onepiece = true;
        if (master) {
            s->mbuf = one;
        } else {
            s->slots = one;
        }
        s->txmem = one + frames;
        s->rxmem = s->txmem + s->w.txcap;
    } else {
        if (master) {
            s->mbuf = ag_port_alloc_aligned(4u, frames, AG_MEM_DMA);
        } else {
            s->slots = ag_port_alloc_aligned(4u, frames, AG_MEM_DMA);
        }
        s->txmem = ag_port_alloc(s->w.txcap, AG_MEM_BYTE);
        s->rxmem = ag_port_alloc(s->w.rxcap, AG_MEM_BYTE);
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
               (unsigned)s->w.txcap, s->txmem ? "ok" : "REFUSED",
               (unsigned)s->w.rxcap, s->rxmem ? "ok" : "REFUSED");
        free_open_memory(s);
        unlock(s);
        return -AG_ENOMEM;
    }

    memset(s->txmem, 0, s->w.txcap);
    memset(s->rxmem, 0, s->w.rxcap);
    s->w.tx = (ring_t){.mem = s->txmem, .cap = s->w.txcap};
    s->w.rx = (ring_t){.mem = s->rxmem, .cap = s->w.rxcap};
    s->w.master = master;
    s->w.framed = framed;
    /* an SPI slave may hear the master's own frames echoed back, agrees the
     * frame only once the master clocks it, and wakes nobody on a write -
     * its frames go when the master clocks them */
    s->w.role_check = true;
    s->w.agree_by_len = !master;
    s->w.kick_writes = master;
    s->w.kick_reads = false;
    wl_reset(&s->w, block, flags);
    s->reaped_used = 0;
    memset(s->hist, 0, sizeof(s->hist));
    s->hist_at = 0;
    s->w.frames_bad = 0;
    s->w.seq_gaps = 0;
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
    s->w.block = block;
    s->sck = sck;
    s->mosi = mosi;
    s->miso = miso;
    s->stop = false;
    s->blocks_clocked = 0;
    s->blocks_data = 0;
    s->blocks_unarmed = 0;
    s->w.bytes_dropped = 0;
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
    s->w.open = true;
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
        s->w.open = false;
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
    spilink_t *const s = s_link[idx];
    if (s == NULL || s->w.lock == NULL) {
        return -AG_ENODEV;
    }
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->last_error = AG_OK;
    }
    return link_teardown(s, out);
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

    spilink_t *const s = s_link[idx];
    if (s == NULL || s->w.lock == NULL) {
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

/* ---------------------------------------------------------------------- */
/* The reader's and the writer's calls: the protocol's (net/wlink.c)       */
/* ---------------------------------------------------------------------- */

static wlink_t *wl_of(int bus)
{
    const int idx = link_index(bus);
    return (idx < 0 || s_link[idx] == NULL) ? NULL : &s_link[idx]->w;
}

int32_t ag_spilink_write(int bus, const void *buf, size_t len)
{
    wlink_t *const w = wl_of(bus);
    return (w == NULL) ? -AG_EINVAL : wl_write(w, buf, len);
}

int32_t ag_spilink_write_wait(int bus, const void *buf, size_t len,
                              uint32_t timeout_ms)
{
    wlink_t *const w = wl_of(bus);
    return (w == NULL) ? -AG_EINVAL : wl_write_wait(w, buf, len, timeout_ms);
}

int32_t ag_spilink_send(int bus, const void *buf, size_t len, uint32_t flags,
                        uint32_t timeout_ms)
{
    wlink_t *const w = wl_of(bus);
    return (w == NULL) ? -AG_EINVAL : wl_send(w, buf, len, flags, timeout_ms);
}

int32_t ag_spilink_recv(int bus, void *buf, size_t cap,
                        ag_spilink_block_t *info, uint32_t timeout_ms)
{
    wlink_t *const w = wl_of(bus);
    return (w == NULL) ? -AG_EINVAL : wl_recv(w, buf, cap, info, timeout_ms);
}

int32_t ag_spilink_send2(int bus, const void *head, size_t hlen,
                       const void *buf, size_t len, uint32_t flags,
                       uint32_t timeout_ms)
{
    wlink_t *const w = wl_of(bus);
    return (w == NULL) ? -AG_EINVAL
                       : wl_send2(w, head, hlen, buf, len, flags, timeout_ms);
}

int32_t ag_spilink_recv2(int bus, void *head, size_t hcap, void *buf,
                       size_t cap, ag_spilink_block_t *info,
                       uint32_t timeout_ms)
{
    wlink_t *const w = wl_of(bus);
    return (w == NULL) ? -AG_EINVAL
                       : wl_recv2(w, head, hcap, buf, cap, info, timeout_ms);
}

int32_t ag_spilink_read(int bus, void *buf, size_t len, uint32_t timeout_ms)
{
    wlink_t *const w = wl_of(bus);
    return (w == NULL) ? -AG_EINVAL : wl_read(w, buf, len, timeout_ms);
}
