/*
 * ArgonOS - the node link indirection.  See argon/linkprov.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/linkprov.h>

#include <stddef.h>
#include <string.h>

#include <argon/device.h>
#include <argon/libc.h>
#include <argon/log.h>
#include <argon/nodename.h>

#include <argon/port/task.h>

/*
 * SEVERAL WIRES, AND THE ONE THIS CALL MEANS.
 *
 * A board used to have exactly one neighbour, so "the link" was a pointer and
 * every service above simply sent into it.  Three boards need more than that,
 * and the job layer was already written for it - every ag_clu_transport_t call
 * takes a node index - so what was missing was only down here.
 *
 * Which wire a call means is NOT a new argument on nine files' worth of call
 * sites.  It is derived: each wire has its own reader task, a handler runs on
 * the task of the wire its message arrived on, so a reply goes back out THAT
 * wire without anybody saying so.  Everything else - a shell command, a
 * migration, a program starting a job - means the selected wire, the one
 * `link use` last named.
 *
 * That rule is worth stating the other way round, because it is the whole
 * design: an answer travels home the way its question came, and only a new
 * conversation has to choose.
 */
#define LINK_WIRES AG_LINK_WIRES

typedef struct {
    const ag_link_ops_t *ops;
    ag_device_t         *dev;
    char                 name[AG_DEV_NAME_MAX];
    ag_port_task_t       reader;
    volatile bool        reader_stop;
} wire_t;

static wire_t s_wire[LINK_WIRES];
static int    s_sel = -1; /* what `link use` last bound; -1 is none */

static const char s_none[] = "none";

/*
 * A wire's driver, held for one call into it: an unload waits until it is let
 * go (ag_dev_call_begin).  on_revoke below only clears the pointers, and a task
 * that read them a moment earlier - the reader, inside recv nearly always - was
 * still in the image when it was given back.  The pointers are read once and
 * checked again once held, so a wire rebound in between is not called with the
 * other driver's ops.
 */
typedef struct {
    const ag_link_ops_t *ops;
    ag_device_t         *dev;
} held_t;

static bool hold(const wire_t *w, held_t *h)
{
    h->ops = w->ops;
    h->dev = w->dev;
    if (h->ops == NULL || h->dev == NULL || !ag_dev_call_begin(h->dev)) {
        return false;
    }
    if (w->ops != h->ops || w->dev != h->dev) {
        ag_dev_call_end(h->dev);
        return false;
    }
    return true;
}

static void let_go(held_t *h) { ag_dev_call_end(h->dev); }

/* The wire a call on this task means: the one it is reading, else the
 * selected one.  Both can be absent, and then there is no wire at all. */
static wire_t *current(void)
{
    const ag_port_task_t self = ag_port_task_self();
    for (unsigned i = 0; i < LINK_WIRES; i++) {
        if (s_wire[i].ops != NULL && s_wire[i].reader != NULL &&
            s_wire[i].reader == self) {
            return &s_wire[i];
        }
    }
    if (s_sel >= 0 && s_wire[s_sel].ops != NULL) {
        return &s_wire[s_sel];
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Beyond the neighbour: who a frame is for, and who is sending it           */
/* ------------------------------------------------------------------------ */
/*
 * THREE PIECES OF STATE, and each is here rather than in a service above for
 * the same reason: they are properties of the WIRE, and something that serves
 * three neighbours at once has one answer per wire, not one answer.
 *
 * `s_from` is the source of the frame being handled, kept per wire because two
 * reader tasks can be inside a handler at the same time and a single variable
 * would attribute one neighbour's message to the other.  It is written
 * immediately before the handler is called and read by ag_linkprov_from(),
 * which is the only caller that means anything by it.
 *
 * `s_here` is this board's own name as a number, cached with the name it was
 * computed from so that a `node name` (which can happen at the prompt at any
 * moment) cannot leave a stale hash deciding what is "for me" - a board that
 * believed the wrong number would relay its own messages onward for ever.
 *
 * `s_target` is what `link use` last named as the far node, and is what a NEW
 * conversation is addressed to.  Everything that ANSWERS uses
 * ag_linkprov_from() instead; this is for the callers that start one.
 */
static uint16_t s_from[LINK_WIRES];
static uint16_t s_target;
static uint16_t s_here;
static char     s_here_name[AG_NODE_NAME_MAX];

static int wire_slot(const wire_t *w)
{
    return (int)(w - &s_wire[0]);
}

uint16_t ag_linkprov_here(void)
{
    const char *n = ag_node_name();
    if (strcmp(s_here_name, n) != 0) {
        ag_strlcpy(s_here_name, n, sizeof(s_here_name));
        s_here = ag_node_hash(n);
    }
    return s_here;
}

uint16_t ag_linkprov_from(void)
{
    const wire_t *w = current();
    return (w != NULL) ? s_from[wire_slot(w)] : 0u;
}

uint16_t ag_linkprov_target(void)
{
    return s_target;
}

void ag_linkprov_set_target(uint16_t hash)
{
    s_target = hash;
}

/*
 * The wire whose neighbour is `dst`, or -1.  A wire whose peer has not named
 * itself yet CANNOT BE ROUTED TO and is passed over rather than guessed at: the
 * first frame of a hello exchange has no name in it to match, and a board that
 * picked a wire for an unnamed neighbour would send a stranger's traffic out of
 * whichever wire happened to be free.
 *
 * The comparison is over hashes, which is what the frame carries; the names
 * behind them are the ones the hello exchange learned, per wire, and are what
 * makes a far leaf reachable at all.
 */
static int32_t wire_towards(uint16_t dst)
{
    for (unsigned i = 0; i < LINK_WIRES; i++) {
        if (s_wire[i].ops == NULL) {
            continue;
        }
        const char *peer = ag_node_peer_name_at((int32_t)i);
        if (peer != NULL && ag_node_hash(peer) == dst) {
            return (int32_t)i;
        }
    }
    return -1;
}

/*
 * A frame the middle board has to move across, and how long it will wait for
 * room on the outgoing wire before giving up on it.
 *
 * The wait is the back-pressure and it is the reason there is no queue task
 * here.  The outgoing class has a window; when it is full the honest answer is
 * to stop reading the incoming wire for a moment, because that lets the
 * origin's window fill too and the origin slow down - which is a wire doing its
 * job.  A queue between the two hops would absorb the difference for a while
 * and then drop, and a drop here is invisible to BOTH ends: the middle has
 * already acknowledged the frame on the incoming hop, so the origin believes it
 * was delivered, and the far end never saw it.  Waiting is the only answer that
 * does not lie.
 *
 * It is bounded because the reader task is the only thing that reads this
 * wire - while it waits, the driver's own service task can still fill the
 * driver's eight-message inbox, and a wait long enough to overflow that would
 * cost frames somewhere the counter cannot explain.  Eight tries at a tick is
 * the compromise; a real one-way congestion lasts longer than that and is
 * logged rather than hidden.
 */
#define LINK_RELAY_TRIES 8u

/* One drop, said once.  Three reasons, so at most three lines in a run: a
 * frame per line at audio or file rates is the flood this tree has already
 * been bitten by. */
static bool s_relay_said[3];
static uint32_t s_relay_dropped;
static uint32_t s_relayed;

static void relay_drop(unsigned why, uint16_t dst, const char *what)
{
    s_relay_dropped++;
    if (!s_relay_said[why]) {
        s_relay_said[why] = true;
        ag_log(AG_LOG_WARN, "link",
               "relay: a frame for %04x was dropped (%s) - %u relayed and %u "
               "dropped since the link came up; this is said once, not per "
               "frame",
               (unsigned)dst, what, (unsigned)s_relayed,
               (unsigned)s_relay_dropped);
    }
}

static void relay(wire_t *arrived, uint8_t chan, const ag_link_addr_t *a,
                  const uint8_t *buf, uint16_t len)
{
    const int32_t to = wire_towards(a->dst);
    if (to < 0) {
        relay_drop(0, a->dst,
                   "no bound wire has a neighbour by that name yet");
        return;
    }
    if (&s_wire[to] == arrived) {
        /* The only wire that leads there is the one it came in on, so
         * forwarding it would put it back where it started. */
        relay_drop(1, a->dst, "the only wire to there is the one it arrived on");
        return;
    }
    /*
     * `a` goes out untouched, `src` included.  That is the whole of what makes
     * a relayed conversation work: the far end answers the node it is talking
     * to, and if this board put its own name there the answer would come back
     * here instead of going home.
     */
    for (unsigned tries = 0;; tries++) {
        held_t h;
        if (!hold(&s_wire[to], &h)) {
            relay_drop(2, a->dst, "the wire out of here has gone");
            return;
        }
        const int32_t r = h.ops->send_a(h.dev, chan, buf, len, a);
        let_go(&h);
        if (r >= 0) {
            s_relayed++;
            return;
        }
        if (r != -AG_EAGAIN || tries >= LINK_RELAY_TRIES) {
            relay_drop(2, a->dst,
                       "the wire out of here had no room for it");
            return;
        }
        ag_port_task_delay(ag_port_ms_to_ticks(1));
    }
}

/*
 * Channels, and who is listening on each.  Sixteen is the link's own limit; the
 * table is static because registering happens while a driver is being bound,
 * which is not a moment to be asking for memory.
 */
#define LINK_CHANS 16u
static ag_link_rx_fn s_rx_fn[LINK_CHANS];
static void         *s_rx_ctx[LINK_CHANS];

ag_err_t ag_linkprov_listen(uint8_t chan, ag_link_rx_fn fn, void *ctx)
{
    if (chan >= LINK_CHANS || fn == NULL) {
        return -AG_EINVAL;
    }
    if (s_rx_fn[chan] != NULL && s_rx_fn[chan] != fn) {
        /* Two consumers of one channel is the bug this design exists to
         * prevent, so it is refused rather than silently chained. */
        return -AG_EBUSY;
    }
    s_rx_fn[chan] = fn;
    s_rx_ctx[chan] = ctx;
    return AG_OK;
}

void ag_linkprov_unlisten(uint8_t chan)
{
    if (chan < LINK_CHANS) {
        s_rx_fn[chan] = NULL;
        s_rx_ctx[chan] = NULL;
    }
}

bool ag_linkprov_reading(void)
{
    for (unsigned i = 0; i < LINK_WIRES; i++) {
        if (s_wire[i].reader != NULL) {
            return true;
        }
    }
    return false;
}

/*
 * Am I the reader?  A different question from the one above, and the two are
 * easy to mistake for each other: anything that sends a request and then waits
 * for the answer must refuse to run on this task, because the answer arrives
 * on it.  Asking "is there a reader" instead refuses ALWAYS - which is what the
 * remote drive did, and it failed as -EBUSY, a plausible-looking error.
 */
bool ag_linkprov_in_reader(void)
{
    const ag_port_task_t self = ag_port_task_self();
    for (unsigned i = 0; i < LINK_WIRES; i++) {
        if (s_wire[i].reader != NULL && s_wire[i].reader == self) {
            return true;
        }
    }
    return false;
}

/*
 * One message off a wire, and the decision this whole file now exists for.
 *
 * A frame with no destination, or with this board's own, is delivered upward
 * exactly as it always was.  Anything else is going somewhere this board is
 * merely in the way of, and is moved across and NOT delivered here - delivering
 * it as well would have the middle board answer its neighbours' traffic, and
 * both ends would then be talking to the middle instead of to each other.
 */
static void route(wire_t *w, uint8_t chan, const ag_link_addr_t *from,
                  const uint8_t *buf, uint16_t len)
{
    const uint16_t me = ag_linkprov_here();
    if (from->dst != 0u && from->dst != me) {
        relay(w, chan, from, buf, len);
        return;
    }
    /*
     * For this board.  Whoever is about to be handed it may answer, and the
     * answer has to be addressed to the node that WROTE the message - which,
     * for a frame that crossed a middle board, is not the wire's neighbour.
     * Recorded before the handler runs and per wire, because two neighbours can
     * be inside a handler at the same moment.
     */
    s_from[wire_slot(w)] = from->src;
    /*
     * The handler runs HERE, on this wire's own task, and that is what makes a
     * reply go back the way the question came: current() finds this wire by
     * asking which task is running.
     */
    if (chan < LINK_CHANS && s_rx_fn[chan] != NULL) {
        s_rx_fn[chan](chan, buf, len, s_rx_ctx[chan]);
    }
}

/*
 * The one reader.  Everything that arrives goes to the handler for its channel
 * and nowhere else; a message on a channel nobody holds is dropped, which is
 * the honest thing to do with a conversation nobody is having.
 *
 * A message addressed past this board is the one exception, and it is decided
 * before the handler table is consulted - see route().
 */
/*
 * THE READER RECEIVES INTO A BUFFER THE SIZE OF THE WIRE, and 288 bytes on its
 * stack was the ceiling on EVERY service on EVERY wire, whatever the driver
 * said it could carry.  The SPI link reports 2 040 bytes a message; a screen's
 * rectangles cut to that size crossed the wire intact and were cut to 288 here,
 * one message at a time, and the picture stayed exactly as slow as before.
 *
 * On the heap and not the stack, because the stack is the one that serves the
 * far node's file reads down through FAT and the SD driver - it has been
 * overflowed once already (see reader_start), and two more kilobytes on it
 * would be the next time.  Sized once per reader from the driver's own mtu,
 * never smaller than the 288 every service was written against.
 */
#define READER_BUF_MIN 288u
#define READER_BUF_MAX 4096u

/* The reader's buffer for the driver on this wire now: its mtu plus room for
 * whatever header the driver keeps on top, never below the 288 every service
 * was written against. */
static uint32_t reader_cap(const wire_t *w)
{
    uint32_t cap = READER_BUF_MIN;
    held_t   h;
    if (hold(w, &h)) {
        ag_link_info_t li;
        memset(&li, 0, sizeof(li));
        if (h.ops->info != NULL && h.ops->info(h.dev, &li) == AG_OK &&
            li.mtu > 0u) {
            cap = li.mtu + 64u;
        }
        let_go(&h);
    }
    if (cap < READER_BUF_MIN) {
        cap = READER_BUF_MIN;
    }
    if (cap > READER_BUF_MAX) {
        cap = READER_BUF_MAX;
    }
    return cap;
}

/*
 * SIZED AGAIN WHEN THE WIRE'S DRIVER CHANGES.  The reader outlives the device
 * it was started for: `drv unload NODELINK`, `drv load linkspi.sys` and
 * `link use node1` rebind the wire under a reader that is still running, and
 * a buffer sized once, for NODELINK's 248 bytes, cut every 1 948-byte
 * rectangle LINKSPI carried to 312 - and the screen service threw each one
 * away as short.  The whole picture crossed the wire and none of it arrived.
 */
static void reader_task(void *arg)
{
    wire_t            *w = (wire_t *)arg;
    const ag_device_t *sized = NULL;
    uint32_t           cap = 0;
    uint8_t           *buf = NULL;

    while (!w->reader_stop) {
        if (w->ops == NULL) {
            ag_port_task_delay(ag_port_ms_to_ticks(20));
            continue;
        }
        if (buf == NULL || w->dev != sized) {
            const uint32_t want = reader_cap(w);
            sized = w->dev;
            if (buf == NULL || want != cap) {
                ag_port_free(buf);
                cap = want;
                buf = (uint8_t *)ag_port_alloc(cap, AG_MEM_BYTE);
                if (buf == NULL) {
                    ag_log(AG_LOG_ERROR, "link",
                           "%s: no %u bytes for the reader; falling back to "
                           "288",
                           w->name, (unsigned)cap);
                    cap = READER_BUF_MIN;
                    buf = (uint8_t *)ag_port_alloc(cap, AG_MEM_BYTE);
                }
                if (buf == NULL) {
                    break;
                }
            }
        }
        uint8_t        chan = 0;
        ag_link_addr_t from;
        memset(&from, 0, sizeof(from));
        held_t h;
        if (!hold(w, &h)) {
            continue; /* gone meanwhile: the next lap waits for a new one */
        }
        const int32_t n = h.ops->recv_a(h.dev, &chan, buf, cap, 50, &from);
        let_go(&h);
        if (n <= 0) {
            continue;
        }
        route(w, chan, &from, buf, (uint16_t)n);
    }
    ag_port_free(buf);
    w->reader = NULL;
    ag_port_task_delete(NULL);
}

static void reader_start(wire_t *w)
{
    if (w->reader != NULL) {
        return;
    }
    w->reader_stop = false;
    /* Priority 1: below every application, for the reason this tree keeps
     * repeating - a link's task above them took a game from 40 fps to
     * unplayable. */
    /*
     * EIGHT KILOBYTES, AND IT IS NOT A ROUND NUMBER PICKED FOR COMFORT.
     *
     * This task serves the far node's file reads, and a read goes down
     * through the VFS into FAT and the SD driver, which carry sector buffers
     * of their own.  At 4096 that overflowed and took the WHOLE BOARD with
     * it: `type n:\sys\autoexec.bat` on the neighbour rebooted the board
     * being read from, every time, in fifteen seconds of silence followed by
     * "error" at the far end.  Nothing said "stack" - a board that reboots
     * loses the journal entry that would have.
     *
     * It survived for as long as it did because no check ever read a FILE
     * across the wire on real hardware: directory listings stay shallow, and
     * the emulator's flash driver is nothing like as deep as FAT on SD.
     * Migration never worked on the boards for this reason and nobody knew,
     * because the thing that failed was three layers above the fault.
     *
     * `link` prints what is left of this, so the next change that eats into
     * it is visible before it is fatal rather than after.
     */
    if (!ag_port_task_create(reader_task, "ag_link", 8192, w, 1, 0, 0,
                             &w->reader)) {
        ag_log(AG_LOG_ERROR, "link", "no reader task: nothing will be heard");
    }
}

/*
 * Every entry the kernel dials must be present.  The size test is against the
 * whole table because there is nothing optional in it yet - but it is written
 * as an offset rather than a sizeof on purpose, so that the first time this
 * vtable grows, a driver built before the growth still binds.  The net provider
 * next door learned that the expensive way.
 */
#define LINK_OPS_MIN                                                          \
    (offsetof(ag_link_ops_t, stats) + sizeof(((ag_link_ops_t *)0)->stats))

static bool ops_complete(const ag_link_ops_t *o)
{
    return o != NULL && o->size >= LINK_OPS_MIN && o->start && o->up &&
           o->send && o->recv && o->poll && o->stats;
}

/*
 * A driver that can carry an address, which since ABI 0.54 is not optional.
 *
 * The entries are past `info` and are tested the way this tree tests every
 * appended entry - by offset rather than by sizeof, so a driver built against a
 * LATER table still answers this question about the pair it does have.
 *
 * MISSING IS REFUSED, and that is the point rather than a strictness.  A
 * driver without them would bind, answer, and carry the direct-neighbour
 * traffic everything is first tested with - while every frame addressed past
 * this board was delivered here instead, or stamped with a zero source the far
 * end cannot answer.  That failure is invisible from both ends: the cluster
 * looks alive and the one thing the addressing exists for does not work.  One
 * line naming the version and a link that does not come up is a bench that can
 * be fixed in thirty seconds.
 */
static bool ops_addressed(const ag_link_ops_t *o)
{
    const size_t need = offsetof(ag_link_ops_t, recv_a) +
                        sizeof(((ag_link_ops_t *)0)->recv_a);
    return o != NULL && o->size >= need && o->send_a != NULL &&
           o->recv_a != NULL;
}

const char *ag_linkprov_active(void)
{
    const wire_t *w = current();
    return (w != NULL) ? w->name : s_none;
}

bool ag_linkprov_bound(void) { return current() != NULL; }

ag_err_t ag_linkprov_drop(const char *name)
{
    const int32_t i = ag_linkprov_index_of(name);
    if (i < 0) {
        return -AG_ENODEV;
    }
    /*
     * The reader is told to stop and the wire is let go now rather than
     * waited for: the task is inside a recv() with its own timeout, and
     * holding the shell until that returns would look like a hang.  It reads
     * ops == NULL on its next lap and ends itself.
     */
    s_wire[i].reader_stop = true;
    s_wire[i].ops = NULL;
    s_wire[i].dev = NULL;
    strcpy(s_wire[i].name, s_none);
    if (s_sel == i) {
        s_sel = -1;
        for (unsigned k = 0; k < LINK_WIRES; k++) {
            if (s_wire[k].ops != NULL) {
                s_sel = (int)k;
                break;
            }
        }
    }
    return AG_OK;
}

uint32_t ag_linkprov_reader_stack_left(int32_t wire)
{
    if (wire < 0 || (unsigned)wire >= LINK_WIRES ||
        s_wire[wire].reader == NULL) {
        return 0;
    }
    return ag_port_task_stack_unused(s_wire[wire].reader);
}

int32_t ag_linkprov_current(void)
{
    const wire_t *w = current();
    if (w == NULL) {
        return -1;
    }
    return (int32_t)(w - &s_wire[0]);
}

int32_t ag_linkprov_count(void)
{
    int32_t n = 0;
    for (unsigned i = 0; i < LINK_WIRES; i++) {
        if (s_wire[i].ops != NULL) {
            n++;
        }
    }
    return n;
}

const char *ag_linkprov_name_at(int32_t wire)
{
    if (wire < 0 || (unsigned)wire >= LINK_WIRES ||
        s_wire[wire].ops == NULL) {
        return NULL;
    }
    return s_wire[wire].name;
}

int32_t ag_linkprov_index_of(const char *name)
{
    if (name == NULL) {
        return -1;
    }
    for (unsigned i = 0; i < LINK_WIRES; i++) {
        if (s_wire[i].ops != NULL && strcmp(s_wire[i].name, name) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

/*
 * One bound wire, one message, addressed.  The one place the addressed send
 * entry is called for a NEW message, so that there is one statement of what a
 * locally-sent frame is stamped with and it is not repeated per caller.
 *
 * `src` is this board, always: a message sent from here was written here.
 * The relay does NOT come through this function - it must keep the source it
 * received, and it calls send_a directly for that reason (see relay()).
 */
static int32_t send_addressed(int32_t wire, uint16_t dst, uint8_t chan,
                              uint8_t cls, const void *buf, size_t len)
{
    held_t h;
    if (wire < 0 || (unsigned)wire >= LINK_WIRES || !hold(&s_wire[wire], &h)) {
        return -AG_ENODEV;
    }
    const ag_link_addr_t to = {dst, ag_linkprov_here(), cls};
    const int32_t r = h.ops->send_a(h.dev, chan, buf, len, &to);
    let_go(&h);
    return r;
}

int32_t ag_linkprov_send_on(int32_t wire, uint8_t chan, uint8_t cls,
                            const void *buf, size_t len)
{
    return send_addressed(wire, ag_linkprov_target(), chan, cls, buf, len);
}

int32_t ag_linkprov_send_to(uint16_t dst, uint8_t chan, uint8_t cls,
                            const void *buf, size_t len)
{
    const wire_t *w = current();
    return send_addressed((w != NULL) ? wire_slot(w) : -1, dst, chan, cls, buf,
                          len);
}

int32_t ag_linkprov_send_to_on(int32_t wire, uint16_t dst, uint8_t chan,
                               uint8_t cls, const void *buf, size_t len)
{
    return send_addressed(wire, dst, chan, cls, buf, len);
}

void ag_linkprov_poll_all(uint32_t ms)
{
    for (unsigned i = 0; i < LINK_WIRES; i++) {
        held_t h;
        if (hold(&s_wire[i], &h)) {
            h.ops->poll(h.dev, ms);
            let_go(&h);
        }
    }
}

/*
 * The device went away - its driver was unloaded, or the hardware did.  Let go
 * while the pointer still means something: a moment later it is an address in
 * an image that has been given back, and a link that answers out of freed
 * memory is worse than one that says it is not there.
 *
 * Called with the registry lock held, so nothing here may reach back into it.
 */
static void on_revoke(ag_device_t *dev)
{
    if (dev == NULL) {
        return;
    }
    for (unsigned i = 0; i < LINK_WIRES; i++) {
        if (s_wire[i].dev == dev) {
            s_wire[i].ops = NULL;
            s_wire[i].dev = NULL;
            strcpy(s_wire[i].name, s_none);
            if (s_sel == (int)i) {
                s_sel = -1;
            }
        }
    }
}

ag_err_t ag_linkprov_use(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return -AG_EINVAL;
    }
    /* Registered on the first bind rather than at boot: a board that never
     * binds a link never needs the watcher, and on_revoke is idempotent. */
    (void)ag_dev_on_revoke(on_revoke);

    /*
     * `link use none` lets go of the SELECTED wire only, not of every wire.
     * On a board with one neighbour that is what it always meant; on a board
     * with three, dropping all of them because one was named would be a
     * surprise, and there is `link drop <name>` for saying it on purpose.
     */
    if (strcmp(name, s_none) == 0) {
        if (s_sel >= 0) {
            s_wire[s_sel].reader_stop = true;
            s_wire[s_sel].ops = NULL;
            s_wire[s_sel].dev = NULL;
            strcpy(s_wire[s_sel].name, s_none);
            s_sel = -1;
        }
        return AG_OK;
    }

    ag_device_t *dev = ag_dev_find(name);
    if (dev == NULL) {
        return -AG_ENODEV;
    }
    if (dev->cls != AG_DEV_LINK) {
        ag_log(AG_LOG_ERROR, "link", "%s is not a link device", name);
        return -AG_EINVAL;
    }
    const ag_link_ops_t *ops = (const ag_link_ops_t *)dev->class_ops;
    if (!ops_complete(ops)) {
        ag_log(AG_LOG_ERROR, "link", "%s: incomplete link ops", name);
        return -AG_ENOTSUP;
    }
    if (!ops_addressed(ops)) {
        ag_log(AG_LOG_ERROR, "link",
               "%s was built before ABI 0.54 and cannot carry an address - "
               "rebuild it; without that pair a frame for a board past this one "
               "would be delivered here instead",
               name);
        return -AG_ENOTSUP;
    }

    /*
     * Already bound is a SELECTION, not a second binding.  Typing `link use
     * node0` twice - which a script does every time it is re-run - must not
     * start a second reader on the same device, and both readers would then
     * take turns eating each other's messages.
     */
    int slot = ag_linkprov_index_of(dev->name);
    if (slot < 0) {
        for (unsigned i = 0; i < LINK_WIRES && slot < 0; i++) {
            if (s_wire[i].ops == NULL) {
                slot = (int)i;
            }
        }
    }
    if (slot < 0) {
        ag_log(AG_LOG_ERROR, "link",
               "%s: this board already holds %u wires; drop one first", name,
               (unsigned)LINK_WIRES);
        return -AG_ENOSPC;
    }

    wire_t *w = &s_wire[slot];
    w->ops = ops;
    w->dev = dev;
    ag_strlcpy(w->name, dev->name, sizeof(w->name));
    s_sel = slot;

    reader_start(w);

    held_t   h;
    ag_err_t e = -AG_ENODEV;
    if (hold(w, &h)) {
        e = h.ops->start(h.dev);
        let_go(&h);
    }
    if (e != AG_OK) {
        ag_log(AG_LOG_ERROR, "link", "%s did not start: %d", w->name, (int)e);
        w->reader_stop = true;
        w->ops = NULL;
        w->dev = NULL;
        strcpy(w->name, s_none);
        s_sel = -1;
        return e;
    }

    /*
     * Say what this wire is, and whether everything above it fits on it.  Once,
     * here, rather than as a surprise inside a driver later - see
     * AG_LINK_MIN_MTU.
     */
    ag_link_info_t info;
    if (ag_linkprov_info(&info) == AG_OK) {
        ag_log(AG_LOG_INFO, "link", "%s: %s, %u B per message, %u B/s", w->name,
               info.wire, (unsigned)info.mtu, (unsigned)info.bytes_per_sec);
        if (info.mtu < AG_LINK_MIN_MTU) {
            ag_log(AG_LOG_WARN, "link",
                   "%s carries only %u bytes a message; the node services want "
                   "%u and will refuse the requests that do not fit",
                   w->name, (unsigned)info.mtu, (unsigned)AG_LINK_MIN_MTU);
        }
    }
    return AG_OK;
}

bool ag_linkprov_up(void)
{
    const wire_t *w = current();
    held_t        h;
    if (w == NULL || !hold(w, &h)) {
        return false;
    }
    const bool up = h.ops->up(h.dev);
    let_go(&h);
    return up;
}

/*
 * A NEW message, from here.  It is stamped `src` = this board and `dst` =
 * whatever `link use` last named - which is why nothing above this file had to
 * learn about addressing: a service that only ever talks to the node it was
 * pointed at keeps calling this, and gets a frame the middle board can route.
 *
 * A service that ANSWERS does not come through here: it addresses the answer to
 * ag_linkprov_from(), because the node that asked is not necessarily the node
 * on the wire.  ag_linkprov_send_to() is that call.
 */
int32_t ag_linkprov_send(uint8_t chan, uint8_t cls, const void *buf, size_t len)
{
    const wire_t *w = current();
    return send_addressed((w != NULL) ? wire_slot(w) : -1, ag_linkprov_target(),
                          chan, cls, buf, len);
}

void ag_linkprov_poll(uint32_t ms)
{
    wire_t *w = current();
    held_t  h;
    if (w != NULL && hold(w, &h)) {
        h.ops->poll(h.dev, ms);
        let_go(&h);
    }
}

ag_err_t ag_linkprov_stats(ag_link_stats_t *out)
{
    wire_t *w = current();
    if (w == NULL) {
        return -AG_ENODEV;
    }
    /*
     * Zeroed before the driver is asked, and it is the driver that makes this
     * necessary rather than tidiness.
     *
     * ag_link_stats_t carries no size and no version marker: a driver fills the
     * fields it was built with and nothing tells it that the struct it was
     * handed is larger.  That is safe in exactly one direction - the caller's
     * struct is the bigger one - and only when the caller has cleared it, since
     * everything past the last field the driver knows keeps whatever the stack
     * held.  A driver image from an older build would then report a
     * retransmission or a dropped-message count assembled out of somebody
     * else's locals, and a number that is wrong is worse than a number that is
     * absent: it gets believed.
     *
     * Here rather than in the caller because every caller needs it and only one
     * of them would remember.
     */
    memset(out, 0, sizeof(*out));
    held_t h;
    if (!hold(w, &h)) {
        return -AG_ENODEV;
    }
    const ag_err_t e = h.ops->stats(h.dev, out);
    let_go(&h);
    return e;
}

/*
 * What the wire IS.  Answered for every bound link, including one whose driver
 * predates info() - that one gets the UART's own measured numbers, because
 * every driver that could exist before this entry was a UART.
 *
 * The caller therefore never has to handle "I do not know", which matters: the
 * numbers decide message sizes and timeouts, and a caller that had to guess
 * when they were missing would be a second set of constants in a second place,
 * which is exactly the thing this exists to remove.
 */
ag_err_t ag_linkprov_info(ag_link_info_t *out)
{
    if (out == NULL) {
        return -AG_EINVAL;
    }
    wire_t *w = current();
    if (w == NULL) {
        return -AG_ENODEV;
    }

    memset(out, 0, sizeof(*out));
    const size_t need =
        offsetof(ag_link_ops_t, info) + sizeof(((ag_link_ops_t *)0)->info);
    held_t     h;
    const bool held = hold(w, &h);
    const bool told = held && h.ops->size >= need && h.ops->info != NULL &&
                      h.ops->info(h.dev, out) == AG_OK;
    if (held) {
        let_go(&h);
    }
    if (told && out->mtu > 0) {
        if (out->bytes_per_sec == 0) {
            out->bytes_per_sec = AG_LINK_DEFAULT_RATE;
        }
        if (out->rtt_us == 0) {
            out->rtt_us = AG_LINK_DEFAULT_RTT_US;
        }
        return AG_OK;
    }

    out->mtu = AG_LINK_DEFAULT_MTU;
    out->bytes_per_sec = AG_LINK_DEFAULT_RATE;
    out->rtt_us = AG_LINK_DEFAULT_RTT_US;
    strcpy(out->wire, "uart");
    return AG_OK;
}
