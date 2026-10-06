/*
 * ArgonOS - the node link indirection.  See argon/linkprov.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/linkprov.h>

#include <stddef.h>
#include <string.h>

#include <argon/budget.h>
#include <argon/device.h>
#include <argon/libc.h>
#include <argon/log.h>
#include <argon/nodename.h>

#include <argon/port/sync.h>
#include <argon/port/task.h>
#include <argon/port/time.h>

#include "net/wlink.h"
#include "proc/proc_internal.h"

/*
 * SEVERAL WIRES, AND THE ONE THIS CALL MEANS.
 *
 * A board used to have exactly one neighbour, so "the link" was a pointer and
 * every service above simply sent into it.  Three boards need more than that,
 * and the job layer was already written for it - every ag_clu_transport_t call
 * takes a node index - so what was missing was only down here.
 *
 * Which wire a call means is NOT a new argument on nine files' worth of call
 * sites.  It is derived: one reader task takes every wire's messages, and while
 * it is handling one it knows which wire that one came in on (s_reading), so a
 * reply goes back out THAT wire without anybody saying so.  Everything else - a
 * shell command, a migration, a program starting a job - means the selected
 * wire, the one `link use` last named.
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
} wire_t;

static wire_t s_wire[LINK_WIRES];
static int    s_sel = -1; /* what `link use` last bound; -1 is none */

/*
 * ONE READER FOR EVERY WIRE, and the wire it is handling right now.
 *
 * It used to be a task a wire, eight kilobytes of stack each (see
 * reader_start for why eight), and on the C3 in the middle of three boards
 * that was sixteen of its 189.  One task can read them all because nothing it
 * waits on is the wire itself: it asks each wire without waiting, and when none
 * had anything it sleeps on s_wake, which every kernel link gives when its
 * reader has something (wl_rx_notify) - so a message is picked up at once,
 * whichever wire it came on.  A driver that is not on a kernel link is still
 * read, twenty milliseconds late at worst.
 *
 * The price is that the wires now take turns inside a handler: a far node's
 * file read that goes down FAT holds up the other wire's messages for its
 * length, where it used to hold up only its own.  They were already queued
 * behind each other at the far end's reader; what is new is only the middle
 * board's wait.
 */
static ag_port_task_t   s_reader;
static wire_t *volatile s_reading;
static ag_port_sem_t    s_wake;

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
    if (s_reader != NULL && ag_port_task_self() == s_reader) {
        wire_t *const w = s_reading;
        if (w != NULL && w->ops != NULL) {
            return w;
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
 * `s_from` is the source of the frame being handled, kept per wire so that the
 * answer to one neighbour's message is never addressed from another's.  It is written
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

/*
 * A QUEUE PER WIRE OUT, drained by a task of its own.
 *
 * The one reader relayed by sending, and a send into a slow wire waits for
 * room - so while the C3 moved the Zero's picture onto the CYD's 4 Mbaud
 * wire it read nothing at all: the Zero's own 16 Mbaud wire, with 97% of it
 * free, was refused frame after frame (38% of the Zero's frames sent twice in
 * the ten-minute stress), and a request of the C3's own waited behind the
 * relay until its asker gave up (26 Sep 2026).  Now a frame to relay is copied
 * into the outgoing wire's queue and the reader reads on; the relay task
 * waits for room instead.  Order is kept: once anything is queued for a wire,
 * everything after it for that wire is queued too.
 *
 * A FULL QUEUE STOPS THE READING, IT NEVER DROPS.  The first version waited
 * 32 ms for room and then threw the frame away, and the frame was already
 * acknowledged on the wire it came in on - so the Zero believed its picture
 * delivered and the CYD never saw that piece of it.  A picture is sent as
 * changes, so one lost piece is wrong pixels for as long as that part of the
 * screen stays still: a second of the square, then rubbish, then a screen
 * that looked frozen, as soon as sound shared the CYD's wire (26 Sep 2026).
 * Now the reader does not take the next frame off a wire at all while a queue
 * it could be relayed into has no room for one; the frame stays in that
 * wire's receive ring, the ring fills, and the sender is told "not taken" and
 * sends it again later - the back-pressure the wire already has, reaching the
 * origin.  The relay task wakes the reader each time it makes room.
 */
#define RELAY_Q_BYTES (8u * 1024u)
#define RELAY_FRAME   4096u
#define RELAY_REC     (2u + 1u + sizeof(ag_link_addr_t))

/*
 * A QUEUE PER CLASS, NOT ONE (27 Sep 2026).  One queue per wire out, in the
 * order frames came, and a file sent at the lowest priority on the Zero filled
 * the C3's queue for the CYD: the picture waited 55 to 230 ms a pass behind
 * it, 7 KB/s of picture beside 199 KB/s of file, and the C3's own sound went
 * out between them.  The priority of the task that sent the file was nowhere
 * in the frames.  Their class is: prompt (control and sound), bulk (the
 * picture, a device's data), background (a file, on what is left over), and
 * the relay task always takes the first class that has anything.  Order is
 * kept within a class; between classes it never meant anything.
 *
 * The prompt queue is smaller: its frames are small, and a frame too big for
 * its queue is sent straight on as before a queue existed.  The background
 * queue holds a whole BG_WINDOW and a frame, so the one sender the window
 * allows never fills it (see BACKGROUND, below).
 */
#define RQ_CLASSES 3u
static const uint32_t k_rq_cap[RQ_CLASSES] = {4096u, RELAY_Q_BYTES,
                                              RELAY_Q_BYTES};

static unsigned rq_class(uint8_t cls)
{
    return (cls == AG_LINK_BACKGROUND) ? 2u : (cls == AG_LINK_BULK) ? 1u : 0u;
}

typedef struct {
    uint8_t *mem;
    uint32_t cap;
    uint32_t r, w, used;
} relay_q_t;

static void reader_wake(void);
static uint16_t budget_of(uint8_t cls);
static bool credit_hold(int32_t to, uint8_t chan, const ag_link_addr_t *a,
                        const uint8_t *buf, uint16_t len);

static relay_q_t        s_rq[LINK_WIRES][RQ_CLASSES];
static ag_port_mutex_t  s_rq_lock;
static ag_port_sem_t    s_rq_wake;
static ag_port_task_t   s_rq_task;
static volatile bool    s_rq_failed;
/* The link's own tasks above applications, on a budget: see the reader's
 * note, ABOVE APPLICATIONS, ON A BUDGET OF 2 MS IN EVERY 10. */
#define READER_PRIO_HIGH  6u
#define READER_PRIO_LOW   1u
#define READER_WINDOW_US  10000
#define READER_BUDGET_US  2000
/* the relay's budget, as the reader's (READER_PRIO_HIGH): its window, what it
 * spent in it, whether it has been dropped for the rest */
static volatile int64_t s_rl_window;
static volatile int64_t s_rl_used;
static volatile bool    s_rl_low;
static uint32_t         s_rl_drops;
static void relay_charge(int64_t began);
static void relay_lift(void);
static volatile bool    s_rq_stalled; /* the reader stopped a wire for room  */
static uint32_t         s_rq_stops;   /* how many times it did               */
static uint8_t         *s_rq_frame; /* one frame out of a queue, RELAY_FRAME */

/* In at most two copies: a division a byte cost the C3, whose one core was
 * 98% busy moving a file across it (27 Sep 2026). */
static void rq_put(relay_q_t *q, const void *src, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)src;
    const uint32_t first = (n < q->cap - q->w) ? n : q->cap - q->w;
    memcpy(q->mem + q->w, p, first);
    memcpy(q->mem, p + first, n - first);
    q->w = (q->w + n) % q->cap;
    q->used += n;
}

static void rq_get(relay_q_t *q, void *dst, uint32_t n)
{
    uint8_t       *p = (uint8_t *)dst;
    const uint32_t first = (n < q->cap - q->r) ? n : q->cap - q->r;
    memcpy(p, q->mem + q->r, first);
    memcpy(p + first, q->mem, n - first);
    q->r = (q->r + n) % q->cap;
    q->used -= n;
}

/*
 * THE LONGEST SEND OF A RELAYED FRAME, a second at a time: a wire whose
 * writers wait for each other shows here first (26 Sep 2026).
 */
static int64_t  s_rl_max_us, s_rl_said_us;
static uint32_t s_rl_sends;
/* `link report on`: this and the sound's meter write the journal each
 * second - from tasks that must not wait for a console (27 Sep 2026) */
static volatile bool s_link_report;

void ag_linkprov_report(bool on)
{
    s_link_report = on;
}

bool ag_linkprov_reporting(void)
{
    return s_link_report;
}

static void relay_meter(int64_t took)
{
    s_rl_sends++;
    if (took > s_rl_max_us) {
        s_rl_max_us = took;
    }
    const int64_t now = ag_port_us();
    if (now - s_rl_said_us >= 1000000) {
        if (s_rl_said_us != 0 && s_link_report) {
            ag_log(AG_LOG_INFO, "link",
                   "relay: %u frames sent on, the longest send %u us",
                   (unsigned)s_rl_sends, (unsigned)s_rl_max_us);
        }
        s_rl_said_us = now;
        s_rl_sends = 0;
        s_rl_max_us = 0;
    }
}

/*
 * A BACKGROUND FRAME WAITS ASIDE, NOT IN FRONT (27 Sep 2026).  Background
 * goes into a wire only when the wire has nothing waiting (AG_SPILINK_IDLE),
 * so it is refused often - and a relay that waited on it, as it waits on
 * everything else, would have the picture and the sound behind it in their
 * own queues.  So it is taken out of its queue into its wire's own buffer,
 * tried once, and left there while anything else for that wire goes first.
 */
typedef struct {
    uint8_t       *buf; /* RELAY_FRAME, the first time a wire has one */
    bool           on;
    uint8_t        chan;
    uint16_t       len;
    ag_link_addr_t a;
} bg_pend_t;

static bg_pend_t s_bg_pend[LINK_WIRES];

/* The waiting background frame of wire `i`, once.  True if it went (or is
 * gone for good); false while the wire is busy. */
static bool bg_pend_try(int i)
{
    bg_pend_t *const p = &s_bg_pend[i];
    held_t           h;
    if (!hold(&s_wire[i], &h)) {
        relay_drop(2, p->a.dst, "the wire out of here has gone");
        p->on = false;
        return true;
    }
    if (p->a.budget_ms == 0u) {
        /* the far end's budget does not cross the wire; its class does */
        p->a.budget_ms = budget_of(p->a.cls);
    }
    const int32_t r = h.ops->send_a(h.dev, p->chan, p->buf, p->len, &p->a);
    let_go(&h);
    if (r == -AG_EAGAIN) {
        return false;
    }
    if (r >= 0) {
        s_relayed++;
        if (s_rq_stalled) {
            reader_wake();
        }
    } else {
        relay_drop(2, p->a.dst, "the wire out of here refused it");
    }
    p->on = false;
    return true;
}

/* One queued frame of wire `i` into the wire, waiting for room.  True if
 * there was one. */
static bool rq_send_one(int i)
{
    const int64_t    began = ag_port_us();
    uint8_t *const   frame = s_rq_frame;
    (void)ag_port_mutex_take(s_rq_lock, AG_PORT_FOREVER);
    /* the first class with anything in it: prompt, then bulk; background
     * only when neither has anything, and aside (bg_pend_t) */
    relay_q_t *q = NULL;
    for (unsigned c = 0; c + 1u < RQ_CLASSES; c++) {
        if (s_rq[i][c].used >= RELAY_REC) {
            q = &s_rq[i][c];
            break;
        }
    }
    if (q == NULL) {
        bg_pend_t *const p = &s_bg_pend[i];
        relay_q_t *const bq = &s_rq[i][RQ_CLASSES - 1u];
        if (!p->on && bq->used >= RELAY_REC) {
            if (p->buf == NULL) {
                p->buf = (uint8_t *)ag_port_alloc(RELAY_FRAME, AG_MEM_BYTE);
            }
            if (p->buf != NULL) {
                uint8_t hdr[RELAY_REC];
                rq_get(bq, hdr, RELAY_REC);
                const uint16_t len = (uint16_t)(hdr[0] | (hdr[1] << 8));
                p->chan = hdr[2];
                memcpy(&p->a, hdr + 3, sizeof(p->a));
                p->len = (len <= RELAY_FRAME) ? len : (uint16_t)RELAY_FRAME;
                rq_get(bq, p->buf, p->len);
                for (uint16_t k = p->len; k < len; k++) {
                    uint8_t skip;
                    rq_get(bq, &skip, 1u);
                }
                p->on = true;
            }
        }
        (void)ag_port_mutex_give(s_rq_lock);
        return p->on && bg_pend_try(i);
    }
    uint8_t        hdr[RELAY_REC];
    ag_link_addr_t a;
    rq_get(q, hdr, RELAY_REC);
    const uint16_t len = (uint16_t)(hdr[0] | (hdr[1] << 8));
    const uint8_t  chan = hdr[2];
    memcpy(&a, hdr + 3, sizeof(a));
    const uint16_t take = (len <= RELAY_FRAME) ? len : (uint16_t)RELAY_FRAME;
    rq_get(q, frame, take);
    if (take < len) {
        uint8_t skip;
        for (uint16_t k = take; k < len; k++) {
            rq_get(q, &skip, 1u);
        }
    }
    (void)ag_port_mutex_give(s_rq_lock);
    relay_charge(began);
    for (;;) {
        held_t h;
        if (!hold(&s_wire[i], &h)) {
            relay_drop(2, a.dst, "the wire out of here has gone");
            return true;
        }
        const int64_t t0 = ag_port_us();
        const int32_t r = h.ops->send_a(h.dev, chan, frame, take, &a);
        let_go(&h);
        relay_meter(ag_port_us() - t0);
        if (r >= 0) {
            s_relayed++;
            if (s_rq_stalled) {
                reader_wake(); /* room now: a stopped wire can be read */
            }
            return true;
        }
        if (r != -AG_EAGAIN) {
            relay_drop(2, a.dst, "the wire out of here refused it");
            return true;
        }
        ag_port_task_delay(ag_port_ms_to_ticks(1));
    }
}

static void relay_task(void *arg)
{
    (void)arg;
    for (;;) {
        bool any = false;
        bool waiting = false;
        for (int i = 0; i < (int)LINK_WIRES; i++) {
            any = rq_send_one(i) || any;
            waiting = waiting || s_bg_pend[i].on;
        }
        if (!any) {
            /* background aside is tried again at the next tick, not the
             * next frame: the wire it waits for is sending */
            (void)ag_port_sem_take(s_rq_wake,
                                   ag_port_ms_to_ticks(waiting ? 1u : 20u));
        }
    }
}

/*
 * May the reader take the next frame off wire `in`?  Not while any queue it
 * could be relayed into lacks room for the largest frame that wire carries -
 * which is what makes the wait in relay_queue below one that does not happen.
 * Asked before the frame is read, so it cannot know where the frame is going
 * and answers for every other wire: with the one middle board of two wires
 * that is exact, and with more it stops a wire a little sooner than it had to.
 */
static bool relay_room_for(unsigned in, uint32_t frame)
{
    if (s_rq_task == NULL) {
        return true;
    }
    const uint32_t need = RELAY_REC + ((frame < RELAY_FRAME) ? frame
                                                             : RELAY_FRAME);
    bool room = true;
    (void)ag_port_mutex_take(s_rq_lock, AG_PORT_FOREVER);
    for (unsigned j = 0; j < LINK_WIRES; j++) {
        /*
         * Every class, background too: its window keeps it from filling,
         * and when a busy middle board holds it anyway (bg_may_relay) a
         * full queue stopping this wire is better than the reader waiting
         * inside relay_queue for room that only its own relay can make.  A
         * frame bigger than a queue goes around it, so the most a queue is
         * asked to hold is itself.
         */
        for (unsigned c = 0; c < RQ_CLASSES; c++) {
            const relay_q_t *const q = &s_rq[j][c];
            const uint32_t n = (need < q->cap) ? need : q->cap;
            if (j != in && q->mem != NULL && q->cap - q->used < n) {
                room = false;
            }
        }
    }
    (void)ag_port_mutex_give(s_rq_lock);
    if (!room) {
        s_rq_stalled = true;
        s_rq_stops++;
    }
    return room;
}

/* Queue a frame for wire `to`; false when there is no queue to put it in - the
 * caller then does as before. */
static bool relay_queue(int32_t to, uint8_t chan, const ag_link_addr_t *a,
                        const uint8_t *buf, uint16_t len)
{
    if (s_rq_failed) {
        return false;
    }
    if (s_rq_task == NULL) {
        s_rq_lock = ag_port_mutex_new();
        s_rq_wake = ag_port_sem_new_binary();
        s_rq_frame = (uint8_t *)ag_port_alloc(RELAY_FRAME, AG_MEM_BYTE);
        if (s_rq_lock == NULL || s_rq_wake == NULL || s_rq_frame == NULL ||
            !ag_proc_task_create(relay_task, "ag_relay", 3072, NULL,
                                 READER_PRIO_HIGH, 0, &s_rq_task)) {
            s_rq_failed = true;
            return false;
        }
    }
    const unsigned   c = rq_class(a->cls);
    relay_q_t *const q = &s_rq[to][c];
    const uint32_t   need = RELAY_REC + len;
    if (need > k_rq_cap[c]) {
        return false; /* bigger than its queue: straight on, as it was */
    }
    if (q->mem == NULL) {
        q->mem = (uint8_t *)ag_port_alloc(k_rq_cap[c], AG_MEM_BYTE);
        if (q->mem == NULL) {
            return false;
        }
        q->cap = k_rq_cap[c];
    }
    for (unsigned tries = 0;; tries++) {
        (void)ag_port_mutex_take(s_rq_lock, AG_PORT_FOREVER);
        if (q->cap - q->used >= need) {
            uint8_t hdr[RELAY_REC];
            hdr[0] = (uint8_t)len;
            hdr[1] = (uint8_t)(len >> 8);
            hdr[2] = chan;
            memcpy(hdr + 3, a, sizeof(*a));
            rq_put(q, hdr, RELAY_REC);
            rq_put(q, buf, len);
            (void)ag_port_mutex_give(s_rq_lock);
            relay_lift();
            (void)ag_port_sem_give(s_rq_wake);
            return true;
        }
        (void)ag_port_mutex_give(s_rq_lock);
        /* Only when relay_room_for was wrong about it, or the wire out has
         * stopped taking anything at all for two seconds. */
        if (tries >= 2000u) {
            relay_drop(2, a->dst, "the queue for the wire out of here stayed "
                                  "full for two seconds");
            return true; /* dropped, and said: not for the caller to retry */
        }
        (void)ag_port_sem_give(s_rq_wake);
        ag_port_task_delay(ag_port_ms_to_ticks(1));
    }
}

void ag_linkprov_relay_counts(uint32_t *relayed, uint32_t *dropped,
                              uint32_t *held)
{
    *relayed = s_relayed;
    *dropped = s_relay_dropped;
    *held = s_rq_stops;
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
    if (credit_hold(to, chan, a, buf, len)) {
        return; /* a background credit, kept until this board has time */
    }
    if (relay_queue(to, chan, a, buf, len)) {
        return;
    }
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
    return s_reader != NULL;
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
    return s_reader != NULL && ag_port_task_self() == s_reader;
}

/*
 * BACKGROUND: A WINDOW FROM END TO END (27 Sep 2026).
 *
 * The lowest class has to be the lowest everywhere it passes, and a class
 * alone is not enough for that: a file sent as fast as the first wire takes
 * it still arrives at the middle board faster than the second wire can carry
 * it away, and whatever queue is between them fills - and a full queue stops
 * everything behind it.  So a sender keeps at most BG_WINDOW bytes of it
 * unanswered to any one node, and the node it is for answers what it has
 * received (a CREDIT on LINK_CHAN_SELF, the link's own).  Nothing on the way can
 * then hold more than a window of it.
 *
 * And the answer is what makes it the LEFTOVER.  The far board answers from
 * its reader, and only while the reader is inside its budget (2 ms in 10):
 * a reader busy with sound and a picture spends its budget on them, the
 * answer waits for the next window, and the file slows by exactly that much.
 * Every class above it is answered, or not, as before.
 *
 * An answer can be lost with its wire; a sender that has heard nothing for
 * BG_RESYNC_US with bytes outstanding starts its count again from what it
 * has sent.  The cost is one window too many, once.  An answer is a step,
 * not a total (see bg_heard), so a far board that starts again needs nothing.
 */
/* 2 KB, not 6: measured beside the balls, the picture 1.9 -> 3.4 whole frames a
 * second and the CYD's converter never dry, for 181 -> 127 KB/s of file */
#define BG_WINDOW      2048u
#define BG_PEERS       4u
#define BG_RESYNC_US   1000000
#define BG_QUIET_US    5000
/* not 0: that is the hello exchange's, and taking it here left every board
 * with neighbours that never said who they were (27 Sep 2026) */
#define LINK_CHAN_SELF 15u
#define SELF_CREDIT    'C'

typedef struct {
    volatile uint16_t node;
    volatile uint32_t sent;     /* bytes of background sent to it       */
    volatile uint32_t heard;    /* ...and those it has said it received */
    volatile int64_t  heard_us; /* when it last said so                 */
} bg_out_t;

typedef struct {
    uint16_t node;
    int32_t  wire; /* the wire its frames came in on: the answer's way home */
    uint32_t got, said;
    int64_t  last_us; /* when its last frame came */
} bg_in_t;

static bg_out_t s_bg_out[BG_PEERS];
static bg_in_t  s_bg_in[BG_PEERS];
static uint32_t s_bg_waits, s_bg_resyncs, s_bg_credits;
static uint32_t s_bg_relay_held; /* held in the middle for want of time */

static void     put32(uint8_t *p, uint32_t v);
static uint32_t get32(const uint8_t *p);
static int32_t  send_addressed(int32_t wire, uint16_t dst, uint8_t chan,
                               uint8_t cls, const void *buf, size_t len);

/* The sender's count for `node`; a new node takes a free slot, or the one
 * heard from longest ago. */
static bg_out_t *bg_out_for(uint16_t node)
{
    bg_out_t *oldest = &s_bg_out[0];
    for (unsigned i = 0; i < BG_PEERS; i++) {
        bg_out_t *const o = &s_bg_out[i];
        if (o->node == node) {
            return o;
        }
        if (o->node == 0u || o->heard_us < oldest->heard_us) {
            oldest = o;
        }
    }
    oldest->node = node;
    oldest->sent = 0;
    oldest->heard = 0;
    oldest->heard_us = ag_port_us();
    return oldest;
}

/* May `len` more bytes of background go to it now? */
static bool bg_may_send(bg_out_t *o, size_t len)
{
    const int64_t  now = ag_port_us();
    const uint32_t out = o->sent - o->heard;
    if (out == 0u) {
        o->heard_us = now; /* nothing outstanding: the clock starts now */
        return true;
    }
    if (out > 4u * BG_WINDOW || now - o->heard_us > BG_RESYNC_US) {
        /* one frame, not a window: an answer lost is rare, and a middle
         * board holding background for want of time is not - a whole
         * window more each time would fill its queue */
        o->heard = o->sent + (uint32_t)len - BG_WINDOW;
        o->heard_us = now;
        s_bg_resyncs++;
        return true;
    }
    return out + (uint32_t)len <= BG_WINDOW;
}

/*
 * A credit from `node`: `more` bytes of background from here have arrived
 * there since its last credit.  The bytes since, not a total: a total had to
 * be compared with this side's count, and after any start-again here - a
 * lost answer, a far end that rebooted - every total that came after looked
 * like a far end that had started again, 5 588 times in five minutes (27 Sep
 * 2026).  A step is simply added, never past what was sent.
 */
static void bg_heard(uint16_t node, uint32_t more)
{
    for (unsigned i = 0; i < BG_PEERS; i++) {
        bg_out_t *const o = &s_bg_out[i];
        if (o->node != node) {
            continue;
        }
        const uint32_t out = o->sent - o->heard;
        o->heard += (more < out) ? more : out;
        o->heard_us = ag_port_us();
        return;
    }
}

/* On the reader: a frame of background for this board has arrived. */
static void bg_note(uint16_t node, int32_t wire, uint16_t len)
{
    bg_in_t *slot = NULL;
    for (unsigned i = 0; i < BG_PEERS; i++) {
        if (s_bg_in[i].node == node) {
            slot = &s_bg_in[i];
            break;
        }
        if (slot == NULL && s_bg_in[i].node == 0u) {
            slot = &s_bg_in[i];
        }
    }
    if (slot == NULL) {
        slot = &s_bg_in[0];
    }
    if (slot->node != node) {
        slot->node = node;
        slot->got = 0;
        slot->said = 0;
    }
    slot->wire = wire;
    slot->got += len;
    slot->last_us = ag_port_us();
}

/*
 * On the reader, after every round of the wires: the answers owed, unless
 * the reader has spent its budget in this window - then they wait for the
 * next, and so does whoever is sending background at it.
 */
static bool bg_may_relay(void);

static void bg_answer(bool low, int64_t window)
{
    if (low && ag_port_us() - window < READER_WINDOW_US) {
        return;
    }
    /*
     * AND ONLY WITH TIME TO SPARE HERE (27 Sep 2026).  The reader's budget
     * does not see what receiving costs below it: on the CYD, which has no
     * UART DMA, 304 KB/s of a file took 62% of a core in the wire's receive
     * task at priority 9, and the picture's drawing, at 1, had what was left
     * - 19 passes a second where it had 80.  A board that is a file's end
     * answers it only while it has BG_IDLE_MIN idle, as a middle board does.
     */
    if (!bg_may_relay()) {
        return;
    }
    for (unsigned i = 0; i < BG_PEERS; i++) {
        bg_in_t *const in = &s_bg_in[i];
        if (in->node == 0u || in->got == in->said) {
            continue;
        }
        /*
         * Half a window at a time, or what is owed once its frames have
         * stopped for BG_QUIET_US.  An answer a frame was 72 000 messages
         * in five minutes back through the middle board, and "when the
         * reader found nothing" - true between nearly every two frames -
         * made it 102 000, on a C3 already 98% busy (27 Sep 2026).
         */
        if (in->got - in->said < BG_WINDOW / 2u &&
            ag_port_us() - in->last_us < BG_QUIET_US) {
            continue;
        }
        uint8_t m[5];
        m[0] = (uint8_t)SELF_CREDIT;
        put32(m + 1, in->got - in->said);
        if (send_addressed(in->wire, in->node, LINK_CHAN_SELF,
                           AG_LINK_CONTROL, m, sizeof(m)) >= 0) {
            in->said = in->got;
            s_bg_credits++;
        }
    }
}

/*
 * THE MIDDLE BOARD'S SPARE TIME, TOO (27 Sep 2026).  The window's answers
 * come from the far end, which knows nothing of the board between: the C3,
 * one core, moved the Zero's file to the CYD with 1.5% of its processor
 * idle, and what it spent was the wire's send task at priority 9 - above
 * TONE, which then made the sound of both rooms late.  So a board passes
 * background on only while its processor had BG_IDLE_MIN of the last 20 ms
 * to spare; the rest waits in its queue, the window closes behind it, and
 * the sender slows to what this board can give.  (Held by the reader's and
 * the relay's budgets instead, it waited while there was time and went
 * while there was none: they are 2 ms of 10 each, not the processor.)
 */
/*
 * 30, not 10: at 10 the C3 kept 11.6% of its time and the sound of both
 * rooms was right, but a pass of the picture still took 50 ms through it -
 * a board nine-tenths busy queues everything.  (Not charging background to
 * the reader's budget instead made it 75: the reader, kept high, took the
 * processor from TONE itself.)
 */
#define BG_IDLE_MIN 30u

static bool bg_may_relay(void)
{
    return ag_cpu_idle_percent() >= BG_IDLE_MIN;
}

/*
 * THE ANSWERS ARE WHAT A MIDDLE BOARD HOLDS, NOT THE FRAMES (27 Sep 2026).
 * Holding the file's frames filled its queue, a full queue stopped the Zero's
 * wire, and the picture on that wire waited behind the file: 50 ms a pass,
 * the same at 10% and at 30%.  A credit on its way back through this board
 * says "so many more bytes arrived"; held here, and added to the next one
 * for the same pair, it closes the sender's window before any more of the
 * file is sent - and what is on the way goes on at once.  Released as soon
 * as this board has BG_IDLE_MIN to spare (credit_release, on the reader).
 */
#define BG_HOLD_PAIRS 4u

typedef struct {
    bool           used;
    int32_t        to;
    ag_link_addr_t a;
    uint32_t       more;
} bg_hold_t;

static bg_hold_t s_bg_hold[BG_HOLD_PAIRS];

static bool credit_hold(int32_t to, uint8_t chan, const ag_link_addr_t *a,
                        const uint8_t *buf, uint16_t len)
{
    if (chan != LINK_CHAN_SELF || len < 5u || buf[0] != (uint8_t)SELF_CREDIT) {
        return false;
    }
    bg_hold_t *slot = NULL;
    for (unsigned i = 0; i < BG_HOLD_PAIRS; i++) {
        bg_hold_t *const h = &s_bg_hold[i];
        if (h->used && h->a.dst == a->dst && h->a.src == a->src) {
            slot = h;
            break;
        }
        if (!h->used && slot == NULL) {
            slot = h;
        }
    }
    const bool busy = !bg_may_relay();
    if (slot == NULL || (!busy && !slot->used)) {
        return false; /* time to spare, nothing held: on as it came */
    }
    if (!slot->used) {
        slot->used = true;
        slot->to = to;
        slot->a = *a;
        slot->more = 0;
    }
    slot->more += get32(buf + 1);
    s_bg_relay_held++;
    return true; /* kept, and sent with the rest by credit_release */
}

/* On the reader, after each round: what was held, if there is time now. */
static void credit_release(void)
{
    for (unsigned i = 0; i < BG_HOLD_PAIRS; i++) {
        bg_hold_t *const h = &s_bg_hold[i];
        if (!h->used || !bg_may_relay()) {
            continue;
        }
        uint8_t m[5];
        m[0] = (uint8_t)SELF_CREDIT;
        put32(m + 1, h->more);
        if (relay_queue(h->to, LINK_CHAN_SELF, &h->a, m, sizeof(m))) {
            h->used = false;
        }
    }
}

void ag_linkprov_bg_counts(uint32_t *waits, uint32_t *credits,
                           uint32_t *resyncs, uint32_t *relay_held)
{
    *relay_held = s_bg_relay_held;
    *waits = s_bg_waits;
    *credits = s_bg_credits;
    *resyncs = s_bg_resyncs;
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
     * Recorded before the handler runs, per wire.
     */
    s_from[wire_slot(w)] = from->src;
    if (chan == LINK_CHAN_SELF) {
        /* the window's own channel: only credits */
        if (len >= 5u && buf[0] == (uint8_t)SELF_CREDIT) {
            bg_heard(from->src, get32(buf + 1));
        }
        return;
    }
    if (from->cls == AG_LINK_BACKGROUND) {
        bg_note(from->src, wire_slot(w), len);
    }
    /*
     * The handler runs HERE, on the reader, with s_reading naming this wire,
     * and that is what makes a reply go back the way the question came:
     * current() asks the reader which wire it is handling.
     */
    if (chan < LINK_CHANS && s_rx_fn[chan] != NULL) {
        s_reading = w;
        s_rx_fn[chan](chan, buf, len, s_rx_ctx[chan]);
        s_reading = NULL;
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
 * SIZED AGAIN WHEN A WIRE'S DRIVER CHANGES.  The reader outlives the devices
 * it reads: `drv unload NODELINK`, `drv load linkspi.sys` and `link use node1`
 * rebind a wire under a reader that is still running, and a buffer sized once,
 * for NODELINK's 248 bytes, cut every 1 948-byte rectangle LINKSPI carried to
 * 312 - and the screen service threw each one away as short.  The whole picture
 * crossed the wire and none of it arrived.  One buffer, the size of the largest
 * wire bound.
 */
static const ag_device_t *s_sized[LINK_WIRES];
static uint32_t           s_cap_of[LINK_WIRES];

/*
 * ABOVE APPLICATIONS, ON A BUDGET OF 2 MS IN EVERY 10 (26 Sep 2026).
 *
 * At priority 1 the reader waited for every application to pause, and a
 * wire's receive ring is three frames: on the CYD, whose cores were 85% and
 * 67% idle, it still turned the C3 away 709 times in eight seconds, because
 * a frame arrives every 1.25 ms at 8 Mbaud and the reader shared its core
 * with the sound task and waited its turn.  On the Zero drawing a game, a
 * 22 KB file took forty seconds instead of one.
 *
 * So it runs above applications, and counts its own time by the clock -
 * around each message, two readings - rather than on the tick, which cannot
 * tell 2 ms from 3.  Once it has spent READER_BUDGET_US in a window it drops
 * itself to READER_PRIO_LOW, where it was before, and is lifted again by the
 * first frame that arrives after the window has turned: reader_wake runs on
 * the wire's own task, which is above applications, so a reader starved at
 * the bottom is not left to lift itself.  A game loses at most 2 ms in any
 * ten to the wire, never a whole frame at once; Maxim's figure (the default
 * budgets elsewhere are 20 of 100).
 */
/* READER_PRIO_HIGH and the rest: defined with the relay, which is first */

static volatile int64_t s_rd_window;  /* when the current window began   */
static volatile int64_t s_rd_used;    /* spent in it                     */
static volatile bool    s_rd_low;     /* dropped for the rest of it      */
static uint32_t         s_rd_drops;   /* how many windows it overran     */
/* the probe (1 Oct 2026): the longest one message took to route, and on
 * which channel; how many took a millisecond or more, and how long in all */
static uint32_t         s_rd_route_max_us, s_rd_route_slow, s_rd_route_slow_us;
static uint8_t          s_rd_route_max_chan;

static void reader_charge(int64_t began)
{
    const int64_t now = ag_port_us();
    if (now - s_rd_window >= READER_WINDOW_US) {
        s_rd_window = now;
        s_rd_used = 0;
    }
    s_rd_used += now - began;
    if (!s_rd_low && s_rd_used > READER_BUDGET_US) {
        s_rd_low = true;
        s_rd_drops++;
        ag_port_task_prio_set(s_reader, READER_PRIO_LOW);
    }
}

uint32_t ag_linkprov_reader_overruns(void)
{
    return s_rd_drops;
}

void ag_linkprov_reader_slow(uint32_t *max_us, uint8_t *max_chan,
                             uint32_t *slow, uint32_t *slow_us)
{
    *max_us = s_rd_route_max_us;
    *max_chan = s_rd_route_max_chan;
    *slow = s_rd_route_slow;
    *slow_us = s_rd_route_slow_us;
    s_rd_route_max_us = 0;
    s_rd_route_slow = 0;
    s_rd_route_slow_us = 0;
}


/*
 * THE RELAY, ON THE SAME TERMS (26 Sep 2026).  At priority 1 on the C3 it
 * waited for TONE's synthesis at 5 on the same core, and every frame of the
 * Zero's picture it moved to the CYD waited with it: with one pass in flight,
 * that wait was the picture's frame rate.  It is charged what it does - the
 * copy out of its queue - and a fixed cost a frame for the send, because a
 * send that waits for the wire to take it is asleep, not busy.
 */
#define RELAY_SEND_COST_US 50

static void relay_charge(int64_t began)
{
    const int64_t now = ag_port_us();
    if (now - s_rl_window >= READER_WINDOW_US) {
        s_rl_window = now;
        s_rl_used = 0;
    }
    s_rl_used += (now - began) + RELAY_SEND_COST_US;
    if (!s_rl_low && s_rl_used > READER_BUDGET_US) {
        s_rl_low = true;
        s_rl_drops++;
        ag_port_task_prio_set(s_rq_task, READER_PRIO_LOW);
    }
}

/* From the reader, which queues for it: its priority back once its window
 * has turned. */
static void relay_lift(void)
{
    if (s_rl_low && s_rq_task != NULL &&
        ag_port_us() - s_rl_window >= READER_WINDOW_US) {
        s_rl_low = false;
        ag_port_task_prio_set(s_rq_task, READER_PRIO_HIGH);
    }
}

uint32_t ag_linkprov_relay_overruns(void)
{
    return s_rl_drops;
}

/* The kernel links' "your reader has something": wakes this one reader, and
 * gives it its priority back if its window has turned. */
static void reader_wake(void)
{
    if (s_rd_low && s_reader != NULL &&
        ag_port_us() - s_rd_window >= READER_WINDOW_US) {
        s_rd_low = false;
        ag_port_task_prio_set(s_reader, READER_PRIO_HIGH);
    }
    const ag_port_sem_t sem = s_wake;
    if (sem != NULL) {
        (void)ag_port_sem_give(sem);
    }
}

/* How long the reader sleeps with nothing on any wire, for a driver whose link
 * does not wake it; a kernel link wakes it at once. */
#define READER_IDLE_MS 20u

static void reader_task(void *arg)
{
    (void)arg;
    uint32_t cap = 0;
    uint8_t *buf = NULL;

    for (;;) {
        /* the buffer: the largest any bound wire's driver asks for */
        uint32_t want = READER_BUF_MIN;
        for (unsigned i = 0; i < LINK_WIRES; i++) {
            wire_t *const w = &s_wire[i];
            if (w->ops == NULL) {
                s_sized[i] = NULL;
                continue;
            }
            if (s_sized[i] != w->dev) {
                s_cap_of[i] = reader_cap(w);
                s_sized[i] = w->dev;
            }
            if (s_cap_of[i] > want) {
                want = s_cap_of[i];
            }
        }
        if (buf == NULL || want > cap) {
            ag_port_free(buf);
            cap = want;
            buf = (uint8_t *)ag_port_alloc(cap, AG_MEM_BYTE);
            if (buf == NULL) {
                ag_log(AG_LOG_ERROR, "link",
                       "no %u bytes for the reader; falling back to 288",
                       (unsigned)cap);
                cap = READER_BUF_MIN;
                buf = (uint8_t *)ag_port_alloc(cap, AG_MEM_BYTE);
            }
            if (buf == NULL) {
                ag_port_task_delay(ag_port_ms_to_ticks(1000));
                continue;
            }
        }

        /* one message from each wire that has one, none waited for */
        bool got = false;
        s_rq_stalled = false;
        for (unsigned i = 0; i < LINK_WIRES; i++) {
            wire_t *const w = &s_wire[i];
            if (w->ops == NULL) {
                continue;
            }
            uint8_t        chan = 0;
            ag_link_addr_t from;
            memset(&from, 0, sizeof(from));
            if (!relay_room_for(i, s_cap_of[i])) {
                continue; /* left in its ring: see A FULL QUEUE, above */
            }
            held_t h;
            if (!hold(w, &h)) {
                continue; /* gone meanwhile */
            }
            const int32_t n = h.ops->recv_a(h.dev, &chan, buf, cap, 0, &from);
            let_go(&h);
            if (n > 0) {
                got = true;
                const int64_t began = ag_port_us();
                route(w, chan, &from, buf, (uint16_t)n);
                reader_charge(began);
                const int64_t took = ag_port_us() - began;
                if (took > (int64_t)s_rd_route_max_us) {
                    s_rd_route_max_us = (uint32_t)took;
                    s_rd_route_max_chan = chan;
                }
                if (took >= 1000) {
                    s_rd_route_slow++;
                    s_rd_route_slow_us += (uint32_t)took;
                }
            }
        }
        bg_answer(s_rd_low, s_rd_window);
        credit_release();
        if (!got) {
            (void)ag_port_sem_take(s_wake, ag_port_ms_to_ticks(READER_IDLE_MS));
        }
    }
}

/*
 * THE SINK, and the flood that fills it (26 Sep 2026).
 *
 * A question the cluster could not answer: when a file copy under load took
 * forty times as long as alone, was it the wire, or the boards at its ends?
 * Every real service puts its own work between the wire and the number - a
 * file is read off a card, a picture is decoded and painted - so none of them
 * measures the wire.  The flood sends bytes that mean nothing as fast as the
 * wire takes them, from a task at a priority it is given; the sink on the far
 * board counts them in the reader and does nothing else with them.  What
 * arrives is then the wire plus the two readers and nothing more, and run
 * beside a real service it says how much of the wire that service left.
 *
 * Always listening, because the board being measured is the busy one, and a
 * busy board's console is the first thing to stop answering.  The counts
 * cost two words a frame.
 */
#define LINK_CHAN_SINK 9u
#define SINK_DATA      'D' /* bytes to count                               */
#define SINK_RESET     '0' /* start counting again                         */
#define SINK_ASK       '?' /* what has arrived since the reset             */
#define SINK_ANSWER    'R' /* 'R', bytes, frames, ms first to last, frames
                            * out of order, frames with a wrong byte       */
#define SINK_ANSWER_LEN 21u

/*
 * EVERY FRAME NUMBERED AND EVERY BYTE KNOWN.  A count says the wire carried
 * as much as was sent; it does not say it carried THAT.  A picture sent as
 * changes is ruined by one block delivered twice, late or altered while
 * every counter reads clean, so the sink checks each frame's number against
 * the last and each byte against the pattern its number makes.
 */
static uint8_t sink_byte(uint32_t seq, uint32_t k)
{
    return (uint8_t)(seq * 31u + k * 7u + (k >> 8));
}

static volatile uint32_t s_sink_bytes;
static volatile uint32_t s_sink_frames;
static volatile int64_t  s_sink_first_us;
static volatile int64_t  s_sink_last_us;
static volatile bool     s_sink_heard; /* an answer came back to a flood   */
static uint32_t          s_sink_answer[5];
static uint32_t          s_sink_next;    /* the number the next frame should have */
static uint32_t          s_sink_order;   /* frames that did not have it           */
static uint32_t          s_sink_wrong;   /* frames with a byte not the pattern's  */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void sink_rx(uint8_t chan, const void *buf, uint16_t len, void *ctx)
{
    (void)chan;
    (void)ctx;
    const uint8_t *p = (const uint8_t *)buf;
    if (len == 0u) {
        return;
    }
    if (p[0] == (uint8_t)SINK_DATA) {
        const int64_t now = ag_port_us();
        if (s_sink_frames == 0u) {
            s_sink_first_us = now;
        }
        s_sink_last_us = now;
        s_sink_bytes += len;
        s_sink_frames++;
        if (len >= 5u) {
            const uint32_t seq = get32(p + 1);
            if (seq != s_sink_next) {
                s_sink_order++;
            }
            s_sink_next = seq + 1u;
            for (uint32_t k = 5; k < len; k++) {
                if (p[k] != sink_byte(seq, k)) {
                    s_sink_wrong++;
                    break;
                }
            }
        }
    } else if (p[0] == (uint8_t)SINK_RESET) {
        s_sink_frames = 0u;
        s_sink_bytes = 0u;
        s_sink_next = 0u;
        s_sink_order = 0u;
        s_sink_wrong = 0u;
    } else if (p[0] == (uint8_t)SINK_ASK) {
        uint8_t a[SINK_ANSWER_LEN];
        a[0] = (uint8_t)SINK_ANSWER;
        put32(a + 1, s_sink_bytes);
        put32(a + 5, s_sink_frames);
        put32(a + 9, (uint32_t)((s_sink_last_us - s_sink_first_us) / 1000));
        put32(a + 13, s_sink_order);
        put32(a + 17, s_sink_wrong);
        /* On the reader, so it goes back to the asker, out the wire the
         * question came in on; a full window is waited out briefly. */
        for (int t = 0; t < 50; t++) {
            if (ag_linkprov_send(LINK_CHAN_SINK, AG_LINK_CONTROL, a,
                                 sizeof(a)) != -AG_EAGAIN) {
                break;
            }
            ag_port_task_delay(1);
        }
    } else if (p[0] == (uint8_t)SINK_ANSWER && len >= SINK_ANSWER_LEN) {
        s_sink_answer[0] = get32(p + 1);
        s_sink_answer[1] = get32(p + 5);
        s_sink_answer[2] = get32(p + 9);
        s_sink_answer[3] = get32(p + 13);
        s_sink_answer[4] = get32(p + 17);
        s_sink_heard = true;
    }
}

typedef struct {
    uint32_t              ms;
    uint32_t              size;
    uint8_t              *frame;
    uint8_t               cls;
    ag_linkprov_flood_t  *out;
    ag_port_sem_t         done;
} flood_job_t;

/* A control message into the selected wire, waiting out a full window. */
static int32_t flood_say(const void *buf, size_t len)
{
    int32_t r = -AG_EAGAIN;
    for (int t = 0; t < 500 && r == -AG_EAGAIN; t++) {
        r = ag_linkprov_send(LINK_CHAN_SINK, AG_LINK_CONTROL, buf, len);
        if (r == -AG_EAGAIN) {
            ag_port_task_delay(1);
        }
    }
    return r;
}

static void flood_task(void *arg)
{
    flood_job_t *const         j = (flood_job_t *)arg;
    ag_linkprov_flood_t *const o = j->out;
    const uint8_t              reset = (uint8_t)SINK_RESET;
    if (flood_say(&reset, 1u) < 0) {
        o->error = -AG_EIO;
    }
    const int64_t start = ag_port_us();
    const int64_t until = start + (int64_t)j->ms * 1000;
    while (o->error == 0 && ag_port_us() < until) {
        const uint32_t seq = o->sent_frames;
        put32(j->frame + 1, seq);
        for (uint32_t k = 5; k < j->size; k++) {
            j->frame[k] = sink_byte(seq, k);
        }
        const int32_t r =
            ag_linkprov_send(LINK_CHAN_SINK, j->cls, j->frame, j->size);
        if (r >= 0) {
            o->sent_bytes += j->size;
            o->sent_frames++;
        } else if (r == -AG_EAGAIN) {
            /* The wire is full: the one thing a flood is meant to find. */
            o->full++;
            ag_port_task_delay(1);
        } else {
            o->error = r;
        }
    }
    o->sent_ms = (uint32_t)((ag_port_us() - start) / 1000);
    (void)ag_port_sem_give(j->done);
    ag_port_task_delete(NULL);
}

ag_err_t ag_linkprov_flood(uint32_t ms, uint32_t size, unsigned prio,
                           uint8_t cls, ag_linkprov_flood_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }
    ag_link_info_t info;
    if (ag_linkprov_info_on(ag_linkprov_current(), &info) != AG_OK ||
        info.mtu < 16u) {
        return -AG_ENODEV;
    }
    if (size == 0u || size > info.mtu) {
        size = info.mtu;
    }
    flood_job_t j = {ms, size, NULL,
                     (cls == AG_LINK_BULK) ? cls : (uint8_t)AG_LINK_BACKGROUND,
                     out, NULL};
    j.frame = (uint8_t *)ag_port_alloc(size, AG_MEM_BYTE);
    j.done = ag_port_sem_new_binary();
    ag_port_task_t t = NULL;
    if (j.frame == NULL || j.done == NULL) {
        out->error = -AG_ENOMEM;
    } else {
        for (uint32_t i = 0; i < size; i++) {
            j.frame[i] = (uint8_t)(i * 7u + 1u);
        }
        j.frame[0] = (uint8_t)SINK_DATA;
        out->size = size;
        if (!ag_port_task_create(flood_task, "ag_flood", 3072, &j, prio,
                                 AG_PORT_ANY_CORE, 0, &t)) {
            out->error = -AG_ENOMEM;
        } else {
            (void)ag_port_sem_take(j.done, AG_PORT_FOREVER);
        }
    }
    if (j.done != NULL) {
        ag_port_sem_free(j.done);
    }
    if (j.frame != NULL) {
        ag_port_free(j.frame);
    }
    if (out->error != 0) {
        return (ag_err_t)out->error;
    }
    /* What arrived, by the far end's count.  Asked after the last frame, on
     * the same wire, so the answer is behind every one of them. */
    s_sink_heard = false;
    const uint8_t ask = (uint8_t)SINK_ASK;
    if (flood_say(&ask, 1u) >= 0) {
        for (int w = 0; w < 300 && !s_sink_heard; w++) {
            ag_port_task_delay(ag_port_ms_to_ticks(10));
        }
    }
    if (s_sink_heard) {
        out->answered = true;
        out->got_bytes = s_sink_answer[0];
        out->got_frames = s_sink_answer[1];
        out->got_ms = s_sink_answer[2];
        out->got_order = s_sink_answer[3];
        out->got_wrong = s_sink_answer[4];
    }
    return AG_OK;
}

/*
 * Started by the first `link use` and kept: a board that has bound a wire once
 * is a board in a cluster, and the next wire - or the same one after a driver
 * is swapped - is read by the same task.
 */
static void reader_start(void)
{
    if (s_reader != NULL) {
        return;
    }
    (void)ag_linkprov_listen(LINK_CHAN_SINK, sink_rx, NULL);
    if (s_wake == NULL) {
        /* binary: however many wires spoke, one lap reads them all */
        s_wake = ag_port_sem_new_binary();
        if (s_wake == NULL) {
            ag_log(AG_LOG_ERROR, "link", "no wake for the reader: nothing will "
                                         "be heard");
            return;
        }
    }
    wl_rx_notify = reader_wake;
    /* Above applications, but only for 2 ms in every 10: see READER_PRIO_HIGH.
     * A link's task above them WITHOUT a limit took a game from 40 fps to
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
    /*
     * Internal SRAM if there is room, PSRAM if not, as the cluster's workers
     * do: an emulated S3 after the merge of 25 Sep 2026 had not 8 KB left in
     * one piece by the time `link use` ran, the reader was never made, and
     * both nodes sent into a wire nobody read ("no reader task" in the
     * journal, 0 of 24 pings back).
     */
    if (!ag_proc_task_create(reader_task, "ag_link", 8192, NULL,
                             READER_PRIO_HIGH, 0, &s_reader)) {
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
    /* The reader reads ops == NULL on its next lap and passes the wire by. */
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
        s_wire[wire].ops == NULL || s_reader == NULL) {
        return 0;
    }
    return ag_port_task_stack_unused(s_reader);
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
/*
 * HOW LONG A MESSAGE MAY WAIT, BY ITS CLASS, FOR A SENDER THAT SAID NOTHING
 * (2 Oct 2026).  The wire sends the soonest due first (AG_SPILINK_BUDGET) and
 * knows nothing about the bytes; the class is what the sender said about
 * them, so this is where it becomes a number: an answer or a command at once,
 * a stream (sound) within a period, bulk (a picture, a device's data) within
 * a frame, background (a file) within a second.
 */
static uint16_t budget_of(uint8_t cls)
{
    switch (cls) {
    case AG_LINK_STREAM:
        return 10u;
    case AG_LINK_BULK:
        return 50u;
    case AG_LINK_BACKGROUND:
        return 1000u;
    default:
        return 0u;
    }
}

static int32_t send_addressed(int32_t wire, uint16_t dst, uint8_t chan,
                              uint8_t cls, const void *buf, size_t len)
{
    if (wire < 0 || (unsigned)wire >= LINK_WIRES) {
        return -AG_ENODEV;
    }
    bg_out_t *bo = NULL;
    if (cls == AG_LINK_BACKGROUND) {
        uint16_t node = dst;
        if (node == 0u) {
            /* "whoever reads it": the neighbour, who will answer as itself */
            const char *peer = ag_node_peer_name_at(wire);
            node = (peer != NULL) ? ag_node_hash(peer) : 0u;
        }
        if (node != 0u) {
            bo = bg_out_for(node);
            if (!bg_may_send(bo, len)) {
                s_bg_waits++;
                return -AG_EAGAIN; /* its window is out: it waits its turn */
            }
        }
    }
    held_t h;
    if (!hold(&s_wire[wire], &h)) {
        return -AG_ENODEV;
    }
    /*
     * Counted as sent BEFORE it is: a sender at priority 1 is taken off its
     * core the moment the wire has the frame, and the far end's answer came
     * back before the count was made - told of more than it had sent, the
     * sender took every answer for a far end that had started again, 13 559
     * times in five minutes, and there was no window at all (27 Sep 2026).
     */
    if (bo != NULL) {
        bo->sent += (uint32_t)len;
    }
    const ag_link_addr_t to = {dst, ag_linkprov_here(), cls, 0,
                               budget_of(cls)};
    const int32_t r = h.ops->send_a(h.dev, chan, buf, len, &to);
    let_go(&h);
    if (r < 0 && bo != NULL) {
        bo->sent -= (uint32_t)len;
    }
    return r;
}

int32_t ag_linkprov_send_on(int32_t wire, uint8_t chan, uint8_t cls,
                            const void *buf, size_t len)
{
    return send_addressed(wire, ag_linkprov_target(), chan, cls, buf, len);
}

/*
 * THE WIRE THAT REACHES dst, not the one that happens to be selected (6 Oct
 * 2026).  On the reader an answer goes back the way the question came, which
 * current() already knew.  Off it - a remote run's output and its EXIT, sent
 * from the task the program runs on - the answer went down the SELECTED wire,
 * and on the board in the middle that is whichever its shell last used: with
 * the C3 pointed at the Zero, a run the CYD asked of it was never answered and
 * the CYD reported -60 a minute later.  So: the wire this node last wrote to
 * us on, or the wire it is the neighbour on, and the selected one only when
 * neither says.
 */
static int32_t wire_toward(uint16_t dst)
{
    if (s_reader != NULL && ag_port_task_self() == s_reader &&
        s_reading != NULL && s_reading->ops != NULL) {
        return wire_slot(s_reading);
    }
    if (dst != 0u) {
        for (unsigned i = 0; i < LINK_WIRES; i++) {
            if (s_wire[i].dev != NULL && s_from[i] == dst) {
                return (int32_t)i;
            }
        }
        for (unsigned i = 0; i < LINK_WIRES; i++) {
            const char *peer = ag_node_peer_name_at((int32_t)i);
            if (s_wire[i].dev != NULL && peer != NULL &&
                ag_node_hash(peer) == dst) {
                return (int32_t)i;
            }
        }
    }
    const wire_t *w = current();
    return (w != NULL) ? wire_slot(w) : -1;
}

int32_t ag_linkprov_send_to(uint16_t dst, uint8_t chan, uint8_t cls,
                            const void *buf, size_t len)
{
    return send_addressed(wire_toward(dst), dst, chan, cls, buf, len);
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
     * take a second slot for the same device, or the reader would ask it
     * twice a lap and the two slots would take turns eating each other's
     * messages.
     */
    int slot = ag_linkprov_index_of(dev->name);
    /*
     * AND NOTHING ELSE.  It went on to call the driver's start again, which
     * began the wire over: the far end saw a peer restart, every block in
     * flight was thrown away, and - with devices attaching by themselves - the
     * neighbour took all of this board's devices out and put them back.  A
     * script that says `link use zero` every thirty seconds did that every
     * thirty seconds (26 Sep 2026).
     */
    if (slot >= 0 && s_wire[slot].dev == dev && s_wire[slot].ops == ops) {
        s_sel = slot;
        return AG_OK;
    }
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

    reader_start();

    held_t   h;
    ag_err_t e = -AG_ENODEV;
    if (hold(w, &h)) {
        e = h.ops->start(h.dev);
        let_go(&h);
    }
    if (e != AG_OK) {
        ag_log(AG_LOG_ERROR, "link", "%s did not start: %d", w->name, (int)e);
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

ag_err_t ag_linkprov_select(int32_t wire)
{
    if (wire < 0 || (unsigned)wire >= LINK_WIRES || s_wire[wire].ops == NULL) {
        return -AG_ENODEV;
    }
    s_sel = (int)wire;
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
    /*
     * AN ANSWER GOES TO WHOEVER ASKED, not to the node `link use` last named.
     * Sent from the reader while it handles a message, this is an answer to
     * that message - a device listing, a hello, a file - and it used to be
     * addressed to the target: on the C3 between two boards, with `link use
     * cyd` in force, the answer to the Zero's question went out on the Zero's
     * wire addressed to the CYD, and the Zero passed it on or dropped it.
     */
    uint16_t dst = ag_linkprov_target();
    if (w != NULL && ag_linkprov_in_reader() && s_reading == w &&
        s_from[wire_slot(w)] != 0u) {
        dst = s_from[wire_slot(w)];
    }
    return send_addressed((w != NULL) ? wire_slot(w) : -1, dst, chan, cls, buf,
                          len);
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
static ag_err_t info_of(wire_t *w, ag_link_info_t *out);

ag_err_t ag_linkprov_info(ag_link_info_t *out)
{
    return info_of(current(), out);
}

ag_err_t ag_linkprov_info_on(int32_t wire, ag_link_info_t *out)
{
    if (wire < 0 || (unsigned)wire >= LINK_WIRES || s_wire[wire].ops == NULL) {
        return -AG_ENODEV;
    }
    return info_of(&s_wire[wire], out);
}

static ag_err_t info_of(wire_t *w, ag_link_info_t *out)
{
    if (out == NULL) {
        return -AG_EINVAL;
    }
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
