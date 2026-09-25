/*
 * ArgonOS - cluster threads over the link.  See argon/nodeclu.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/nodeclu.h>

#include <stdio.h>
#include <string.h>

#include <argon/clu.h>
#include <argon/cluframe.h>
#include <argon/linkprov.h>
#include <argon/log.h>
#include <argon/nodename.h>
#include <argon/noderdev.h>

#include <argon/port/mem.h>
#include <argon/port/sync.h>
#include <argon/port/task.h>
#include <argon/port/time.h>

#define NC_CHAN 8u

/* How long a frame may sit waiting for room in the link's window. */
#define NC_SEND_MS 5000u

/*
 * Spawn requests from the far node waiting to be served.
 *
 * Two, not four, and that is not a saving for its own sake: the master's spawn
 * WAITS for its answer, so one wire can have exactly one request in flight.
 * The second slot is there only so a request arriving while one is being served
 * is queued rather than refused.
 */
#define NC_REQS 2

/*
 * A request is filled in on the link's reader task and served on this file's
 * own, so the handover is guarded by a mutex rather than by a flag.
 *
 * A volatile flag set last would very probably work and is not good enough:
 * the fields beside it are not volatile, and nothing in C stops those stores
 * from being sunk past it - on a board with two cores, that is a serving task
 * reading an entry name that has not been written yet, which would show up as
 * "no such entry" once in a long while and never reproduce.  Nothing else in
 * this tree uses atomics, so it uses the lock the tree already uses.
 */
typedef struct {
    bool         used;
    bool         taken;
    ag_clu_tid_t tid;
    char         entry[AG_CLU_ENTRY_MAX];
    uint8_t      arg[AG_CLU_ARG_MAX];
    uint32_t     arglen;
    /*
     * WHO ASKED, AND ON WHICH WIRE.
     *
     * The answer to a spawn is sent from the serving task, not from the reader
     * that took the request, so neither ag_linkprov_from() nor the wire in hand
     * means anything there.  Both are remembered here, per request, at the
     * moment the request arrives; without them the answer says "for whoever is
     * reading" and leaves by whichever wire was last selected, and a board with
     * more than one neighbour answers the wrong one.
     */
    uint16_t from;
    int32_t  wire;
} nc_req_t;

static bool            s_listening;
static ag_port_task_t  s_task;
static nc_req_t        s_req[NC_REQS];
static ag_port_mutex_t s_reqlock;
static ag_cluf_rx_t    s_rx;

/*
 * THE MASTER OF THE JOB BEING SERVED HERE, and the wire its SPAWN arrived on.
 *
 * A worker's own frames - a credit for room in a mailbox, a thread's exit, the
 * end of the job - are sent from a program's task long after the spawn, where
 * neither ag_linkprov_from() nor the wire in hand means anything.  They are
 * answers to the board that started the job, so that board's hash and the wire
 * it reached us on are remembered when the request arrives.  Set on the reader
 * task; a later job replaces it, and a board that starts its OWN job clears it
 * at its first spawn (the cluster layer refuses to be master and worker at
 * once, so the two are never both true).
 */
static volatile uint16_t s_job_from;
static volatile int32_t  s_job_wire = -1;

/*
 * WHAT IS ON THE FAR SIDE OF A NEIGHBOUR.
 *
 * A leaf has one wire and therefore one neighbour, so the list tr_nodes builds
 * has exactly one entry in it - and with three boards wired in a row that
 * answers "who is here?" with "only me and the one next door" while a third
 * board is running two hops away.  The middle knows; this is how it says so,
 * and how a leaf keeps what it was told.
 *
 * NAMES TRAVEL, NOT WIRE INDICES.  A wire index is a position in the
 * RECEIVER's list and would name a different board on every reader; a name is
 * the only identity a board has that means the same thing at both ends.
 *
 * HOPS BOUND IT.  A board tells its neighbours about its own neighbours (one
 * hop) and about what it was told (one hop further), and never more than
 * NC_PEERS_MAX_HOPS - which is enough for a star of any size and cannot grow a
 * loop, because a record that would come back to its author is dropped both by
 * the name check here and by the direct-neighbour check on arrival.
 *
 * THIS IS NOT ROUTING, and the table is kept out of `nodes` on purpose: a frame
 * carries no destination yet, so work placed on a node two hops away would go
 * to whichever neighbour is in hand.  See argon/clu.h for that argument in the
 * place that makes the decision.
 */
#define NC_PEERS 4u            /* far nodes remembered at once */
#define NC_PEERS_MS 2000u      /* how often the list is sent */
#define NC_PEERS_TTL_MS 10000u /* silence for this long forgets one */
#define NC_PEERS_MAX_HOPS 2u   /* told about, then told on - and no further */
#define NC_PEER_REC_MAX 16u    /* name_len + name + cores + hops + hz  */
/*
 * Room for one list: this board, its other neighbours, and what it was told.
 * Sized from the worst case rather than the bench, or the last records would
 * fall off the end of the buffer on a board with all four wires bound.
 */
#define NC_PEERS_BUF (AG_CLUF_HDR + (1u + AG_LINK_WIRES + NC_PEERS) * NC_PEER_REC_MAX)

typedef struct {
    bool     used;
    char     name[AG_NODE_NAME_MAX];
    uint8_t  cores;
    uint8_t  hops;
    uint32_t cpu_hz;
    int32_t  via;     /* MY wire index this was heard through */
    uint32_t seen_ms; /* when a neighbour last said it, for ageing */
} nc_peer_t;

static nc_peer_t       s_peer[NC_PEERS];
static ag_port_mutex_t s_peerlock;
static uint32_t        s_peers_at; /* when the list last went out */

/*
 * The image path, once rather than per request.
 *
 * Every thread of one job comes out of the same image - ag_clu_accept_spawn
 * refuses a second job with a different one - so a copy in every slot was a
 * quarter of a kilobyte each saying the same thing.  Held under s_reqlock like
 * the slots themselves.
 */
static char s_image[AG_CLU_IMAGE_MAX];

/* The one spawn this node is waiting on an answer for.  One, because only the
 * master spawns and it does so from its own task, one call at a time. */
static volatile bool    s_await;
static volatile int32_t s_await_tid;
static volatile bool    s_answered;
static volatile int32_t s_answer;

/*
 * A sender's frame buffer.  One at a time: guarded by s_tx, because two threads
 * of a job fragmenting into the same buffer would interleave their bytes.
 *
 * Sized to today's wire, and NOT to a guess about the next one.  It was 512 so
 * a faster driver could use bigger frames without a rebuild, which cost a
 * quarter of a kilobyte of internal SRAM on every board for a wire that does
 * not exist yet - and this file has just been taught what that costs.
 * payload_max() clamps to whatever is here, so a driver reporting a larger
 * message is slower than it could be rather than unsafe, and raising this is
 * one number on the day there is something to raise it for.
 */
#define NC_TXBUF 256u
static ag_port_mutex_t s_tx;
static uint8_t         s_txbuf[NC_TXBUF + AG_CLUF_HDR];

/* ---------------------------------------------------------------------- */

static uint32_t now_ms(void) { return (uint32_t)(ag_port_us() / 1000); }

/* Payload that fits one frame on the bound wire.  Asked rather than assumed -
 * that is what info() is for, and a quad SPI will answer something else. */
static uint32_t payload_max(void)
{
    ag_link_info_t info;
    uint32_t       mtu = AG_LINK_DEFAULT_MTU;
    if (ag_linkprov_info(&info) == AG_OK && info.mtu > AG_CLUF_HDR) {
        mtu = info.mtu;
    }
    if (mtu > sizeof(s_txbuf)) {
        mtu = (uint32_t)sizeof(s_txbuf);
    }
    return mtu - AG_CLUF_HDR;
}

/*
 * One frame onto the wire, waiting for room but not for ever.
 *
 * A full window is back-pressure and not a failure - the far end has not
 * acknowledged what is already out - so it is waited on.  Bounded, because a
 * node that has gone away would otherwise hold a thread here for good and look
 * like a program that hung.
 */
/*
 * Waiting for room in the window, WITHOUT pumping the wire when we are the task
 * that pumps it.
 *
 * ag_linkprov_poll is how a caller with nothing to say lets the far end be
 * heard, and it is the right thing to do here - except on the reader task,
 * which got here from inside a recv() and would be re-entering the driver it is
 * already inside.  From there this only yields, and is bounded much more
 * tightly, because a reader stopped here is a link that hears nothing at all.
 */
static ag_err_t wait_for_room(uint32_t deadline)
{
    if (now_ms() >= deadline) {
        return -AG_ETIMEDOUT;
    }
    if (ag_linkprov_in_reader()) {
        ag_port_task_delay(ag_port_ms_to_ticks(2));
    } else {
        ag_linkprov_poll(2);
    }
    return AG_OK;
}

static int32_t wire_of_node(int32_t node);

/* The node at the far end of a wire, as the hash a frame must carry to reach
 * it.  0 - "whoever is reading" - while the neighbour has not said its name,
 * which reaches the same board all the same. */
static uint16_t dst_of_wire(int32_t wire)
{
    return (wire < 0) ? 0 : ag_node_hash(ag_node_peer_name_at(wire));
}

/*
 * WHERE A FRAME FOR `node` GOES: which wire it leaves by, and which node it
 * names.  Both, and they are not the same fact - a frame for a board past the
 * neighbour leaves by the wire that reaches it and NAMES the far board, and the
 * board in between passes it on because of the name.
 *
 * A node index is this board's own (see wire_of_node), so the destination is
 * the neighbour at that wire.  NOT ag_linkprov_target(): that names the node a
 * new CONVERSATION is for, and a board with two neighbours can have named a far
 * node through one of them while placing a thread on the other - sending that
 * thread's frames to the far node's hash would have them relayed away from the
 * board they are for.
 *
 * THE WORKER SIDE IS THE EXCEPTION.  The cluster layer gives the master node 1
 * whether or not this board holds a second wire - a leaf's only neighbour is
 * wire 0 - so on a board SERVING a job, index 1 is the board that asked and not
 * a wire of ours.  Its address was remembered when its SPAWN arrived.
 */
static void route_to(int32_t node, int32_t *wire, uint16_t *dst)
{
    if (node == 1 && s_job_from != 0) {
        *wire = s_job_wire;
        *dst = s_job_from;
        return;
    }
    const int32_t w = wire_of_node(node);
    *wire = w;
    *dst = dst_of_wire(w);
}

/*
 * `wire` is which neighbour, and -1 means THE ONE IN HAND: the wire a reply is
 * being written to, which the link layer already knows because this is running
 * on that wire's reader task.  `dst` is who the frame is for, which for every
 * answer is the node that asked - the board in between takes a frame that says
 * "for whoever is reading" and the asker waits out its whole timeout.
 */
static ag_err_t send_bytes(int32_t wire, uint16_t dst, const uint8_t *p,
                           uint32_t total)
{
    const uint32_t deadline =
        now_ms() + (ag_linkprov_in_reader() ? 100u : NC_SEND_MS);
    for (;;) {
        const int32_t r =
            (wire < 0)
                ? ag_linkprov_send_to(dst, NC_CHAN, AG_LINK_CONTROL, p, total)
                : ag_linkprov_send_to_on(wire, dst, NC_CHAN, AG_LINK_CONTROL,
                                         p, total);
        if (r >= 0) {
            return AG_OK;
        }
        if (r != -AG_EAGAIN) {
            return (ag_err_t)r;
        }
        const ag_err_t e = wait_for_room(deadline);
        if (e != AG_OK) {
            return e;
        }
    }
}

static ag_err_t send_frame(int32_t wire, uint16_t dst, const ag_cluf_hdr_t *h,
                           const void *payload)
{
    uint8_t        buf[AG_CLUF_HDR + 64];
    const uint32_t total = AG_CLUF_HDR + h->len;

    if (total > sizeof(buf)) {
        return -AG_ERANGE; /* the caller must use the shared buffer instead */
    }
    (void)ag_cluf_put(buf, h, payload);
    return send_bytes(wire, dst, buf, total);
}

/* The same, for a frame already built in s_txbuf (a data fragment). */
static ag_err_t send_txbuf(int32_t wire, uint16_t dst, uint32_t total)
{
    return send_bytes(wire, dst, s_txbuf, total);
}

/* ---------------------------------------------------------------------- */
/* The transport the cluster layer uses                                    */
/* ---------------------------------------------------------------------- */

static ag_err_t tr_spawn(int32_t node, ag_clu_tid_t tid, const char *entry,
                         const void *arg, uint32_t len, const char *image,
                         uint32_t timeout_ms)
{
    if (ag_linkprov_in_reader()) {
        /* Waiting here would wait for the task that delivers the answer. */
        return -AG_EBUSY;
    }
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }

    /*
     * A job of this board's OWN, so any memory of a job it SERVED is stale and
     * has to go: the cluster layer refuses to be master and worker at the same
     * time, so from here on index 1 means a wire of ours and not the board that
     * once employed us.  Left behind, it would address this job's frames to
     * that board.
     */
    s_job_from = 0;
    s_job_wire = -1;

    int32_t  wire;
    uint16_t dst;
    route_to(node, &wire, &dst);

    /* Entry, image path (which carries the far node's mount prefix), argument. */
    uint8_t body[AG_CLU_ENTRY_MAX + AG_PATH_MAX + 16u + AG_CLU_ARG_MAX];
    uint32_t at = 0;
    const size_t elen = strlen(entry);
    const size_t ilen = strlen(image);
    if (elen + 1u + ilen + 1u + len > sizeof(body)) {
        return -AG_ERANGE;
    }
    memcpy(body + at, entry, elen + 1u);
    at += (uint32_t)elen + 1u;
    memcpy(body + at, image, ilen + 1u);
    at += (uint32_t)ilen + 1u;
    if (arg != NULL && len > 0) {
        memcpy(body + at, arg, len);
        at += len;
    }
    if (at > payload_max()) {
        /*
         * A spawn has to fit one frame - see argon/cluframe.h - and on a wire
         * with a small message that is a real limit rather than a theoretical
         * one: a long path plus a long entry name can reach it.  Said plainly,
         * because the alternative is a spawn that fails with something that
         * sounds like a fault in the program.
         */
        return -AG_ERANGE;
    }

    s_answered = false;
    s_answer = 0;
    s_await_tid = tid;
    s_await = true;

    ag_cluf_hdr_t h = {
        .op = AG_CLUF_SPAWN,
        .flags = 0,
        .len = (uint16_t)at,
        .a = tid,
        .b = (int32_t)len,
        .c = 0,
    };

    ag_err_t e;
    if (ag_port_mutex_take(s_tx, ag_port_ms_to_ticks(NC_SEND_MS))) {
        const uint32_t total = (uint32_t)ag_cluf_put(s_txbuf, &h, body);
        e = send_txbuf(wire, dst, total);
        ag_port_mutex_give(s_tx);
    } else {
        e = -AG_EBUSY;
    }
    if (e != AG_OK) {
        s_await = false;
        return e;
    }

    const uint32_t deadline = now_ms() + timeout_ms;
    while (!s_answered) {
        if (!ag_linkprov_bound()) {
            s_await = false;
            return -AG_ENODEV;
        }
        if (now_ms() >= deadline) {
            s_await = false;
            return -AG_ETIMEDOUT;
        }
        ag_port_task_delay(ag_port_ms_to_ticks(5));
    }
    s_await = false;
    return (ag_err_t)s_answer;
}

static ag_err_t tr_send(int32_t node, ag_clu_tid_t from, ag_clu_tid_t to,
                        const void *buf, uint32_t len)
{
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }

    int32_t  wire;
    uint16_t dst;
    route_to(node, &wire, &dst);

    if (!ag_port_mutex_take(s_tx, ag_port_ms_to_ticks(NC_SEND_MS))) {
        return -AG_EBUSY;
    }

    const uint32_t chunk = payload_max();
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t       off = 0;
    ag_err_t       e = AG_OK;

    do {
        uint32_t take = len - off;
        if (take > chunk) {
            take = chunk;
        }
        ag_cluf_hdr_t h = {
            .op = AG_CLUF_DATA,
            .flags = (uint8_t)(((off == 0) ? AG_CLUF_F_FIRST : 0u) |
                               ((off + take >= len) ? AG_CLUF_F_LAST : 0u)),
            .len = (uint16_t)take,
            .a = from,
            .b = to,
            .c = len,
        };
        const uint32_t total = (uint32_t)ag_cluf_put(s_txbuf, &h, src + off);
        e = send_txbuf(wire, dst, total);
        off += take;
    } while (e == AG_OK && off < len);

    ag_port_mutex_give(s_tx);
    return e;
}

static void tr_credit(int32_t node, ag_clu_tid_t tid, uint32_t n)
{
    int32_t  wire;
    uint16_t dst;
    route_to(node, &wire, &dst);
    ag_cluf_hdr_t h = { .op = AG_CLUF_CREDIT, .flags = 0, .len = 0,
                        .a = tid, .b = (int32_t)n, .c = 0 };
    (void)send_frame(wire, dst, &h, NULL);
}

static void tr_exited(int32_t node, ag_clu_tid_t tid)
{
    int32_t  wire;
    uint16_t dst;
    route_to(node, &wire, &dst);
    ag_cluf_hdr_t h = { .op = AG_CLUF_EXIT, .flags = 0, .len = 0,
                        .a = tid, .b = 0, .c = 0 };
    (void)send_frame(wire, dst, &h, NULL);
}

static void tr_end(int32_t node)
{
    int32_t  wire;
    uint16_t dst;
    route_to(node, &wire, &dst);
    ag_cluf_hdr_t h = { .op = AG_CLUF_END, .flags = 0, .len = 0,
                        .a = 0, .b = 0, .c = 0 };
    (void)send_frame(wire, dst, &h, NULL);
}

/*
 * NODE INDEX TO WIRE.
 *
 * The cluster layer addresses a far board by its position in the list
 * tr_nodes builds, and that list is the bound wires in order - so node k is
 * the k-th wire this board holds.  One rule, in one place, and the reason
 * tr_nodes and this walk the table the same way.
 *
 * Not a name lookup: names arrive late (a board says its own in its hello, and
 * until then it is "node0"), and a job that started before the hello landed
 * would address a node that has no name yet.  The wire is there from the
 * moment it is bound, which is earlier than anything can ask.
 *
 * A frame that is being SENT goes through route_to rather than through this,
 * because on the board serving a job index 1 is the master and not a wire - see
 * the note there.
 */
static int32_t wire_of_node(int32_t node)
{
    if (node < 0) {
        return -1;
    }
    int32_t seen = 0;
    for (int32_t i = 0; i < (int32_t)AG_LINK_WIRES; i++) {
        if (ag_linkprov_name_at(i) == NULL) {
            continue;
        }
        if (seen == node) {
            return i;
        }
        seen++;
    }
    return -1;
}

/* ---------------------------------------------------------------------- */
/* What the neighbours say about themselves                                */
/* ---------------------------------------------------------------------- */

/* True when a wire of ours reaches a board that calls itself `name`. */
static bool name_is_direct(const char *name)
{
    for (int32_t i = 0; i < (int32_t)AG_LINK_WIRES; i++) {
        if (ag_linkprov_name_at(i) == NULL) {
            continue;
        }
        const char *p = ag_node_peer_name_at(i);
        if (p != NULL && p[0] != '\0' && strcmp(p, name) == 0) {
            return true;
        }
    }
    return false;
}

/* The wire whose far end calls itself `name`, or -1. */
static int32_t wire_of_name(const char *name)
{
    for (int32_t i = 0; i < (int32_t)AG_LINK_WIRES; i++) {
        if (ag_linkprov_name_at(i) == NULL) {
            continue;
        }
        const char *p = ag_node_peer_name_at(i);
        if (p != NULL && p[0] != '\0' && strcmp(p, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* One record into `buf`.  False when it does not fit or the name is unusable. */
static bool put_peer_rec(uint8_t *buf, uint32_t cap, uint32_t *at,
                         const char *name, uint8_t cores, uint8_t hops,
                         uint32_t cpu_hz)
{
    const size_t n = strlen(name);
    if (n == 0 || n >= AG_NODE_NAME_MAX) {
        return false;
    }
    if (*at + 1u + n + 1u + 1u + 4u > cap) {
        return false;
    }
    buf[(*at)++] = (uint8_t)n;
    memcpy(buf + *at, name, n);
    *at += (uint32_t)n;
    buf[(*at)++] = cores;
    buf[(*at)++] = hops;
    buf[(*at)++] = (uint8_t)(cpu_hz & 0xffu);
    buf[(*at)++] = (uint8_t)((cpu_hz >> 8) & 0xffu);
    buf[(*at)++] = (uint8_t)((cpu_hz >> 16) & 0xffu);
    buf[(*at)++] = (uint8_t)((cpu_hz >> 24) & 0xffu);
    return true;
}

/*
 * Tell one neighbour who else is here.
 *
 * The first record is THIS board, and that is not decoration: the reader has no
 * way to ask which wire a message arrived on (the link layer answers that
 * question only for a reply), so the sender names itself and the reader learns
 * "through that one" from a name it already knows.
 *
 * `learned` is a snapshot and `buf` belongs to the caller, so no send happens
 * while s_peerlock is held: a send waits for room on a busy wire, and the reader
 * task - which learns on the same lock - must not be made to wait behind it.
 */
static void send_peers(int32_t wire, uint32_t now, const nc_peer_t *learned,
                       uint8_t *buf)
{
    const char *me = ag_node_name();
    const char *towards = ag_node_peer_name_at(wire);
    const uint32_t cap = NC_PEERS_BUF - AG_CLUF_HDR;
    uint32_t       at = 0;

    if (me == NULL || me[0] == '\0') {
        return;
    }
    (void)put_peer_rec(buf + AG_CLUF_HDR, cap, &at, me, 0u, 0u, 0u);

    /* My other neighbours: one hop from whoever is reading this. */
    for (int32_t i = 0; i < (int32_t)AG_LINK_WIRES; i++) {
        if (i == wire || ag_linkprov_name_at(i) == NULL) {
            continue;
        }
        const char *n = ag_node_peer_name_at(i);
        if (n == NULL || n[0] == '\0' || strcmp(n, me) == 0) {
            continue;
        }
        (void)put_peer_rec(buf + AG_CLUF_HDR, cap, &at, n,
                           ag_node_peer_cores_at(i), 1u,
                           ag_node_peer_cpu_hz_at(i));
    }

    /* And what I was told, one hop further out. */
    for (uint32_t k = 0; k < NC_PEERS; k++) {
        const nc_peer_t *p = &learned[k];
        if (!p->used || p->hops >= NC_PEERS_MAX_HOPS) {
            continue;
        }
        if ((uint32_t)(now - p->seen_ms) > NC_PEERS_TTL_MS) {
            continue;
        }
        if (towards != NULL && strcmp(p->name, towards) == 0) {
            continue; /* the one being told; it knows itself */
        }
        if (name_is_direct(p->name)) {
            continue; /* a wire of ours appeared; it is told as a neighbour */
        }
        (void)put_peer_rec(buf + AG_CLUF_HDR, cap, &at, p->name, p->cores,
                           (uint8_t)(p->hops + 1u), p->cpu_hz);
    }

    const ag_cluf_hdr_t h = { .op = AG_CLUF_PEERS, .flags = 0,
                              .len = (uint16_t)at, .a = 0, .b = 0, .c = 0 };
    (void)ag_cluf_put(buf, &h, NULL); /* the records are already there */
    /* The neighbour at the end of this wire, by name: a list poured at whoever
     * is reading would be taken as ITS list by a board in between. */
    (void)send_bytes(wire, dst_of_wire(wire), buf, AG_CLUF_HDR + at);
}

/* The timer, and every bound wire.  Called from the transport's own task. */
static void peers_tick(void)
{
    if (s_peerlock == NULL) {
        return;
    }
    const uint32_t now = now_ms();
    if ((uint32_t)(now - s_peers_at) < NC_PEERS_MS) {
        return;
    }
    s_peers_at = now;

    nc_peer_t snap[NC_PEERS];
    uint8_t   buf[NC_PEERS_BUF];

    ag_port_mutex_take(s_peerlock, AG_PORT_FOREVER);
    memcpy(snap, s_peer, sizeof(snap));
    ag_port_mutex_give(s_peerlock);

    for (int32_t i = 0; i < (int32_t)AG_LINK_WIRES; i++) {
        if (ag_linkprov_name_at(i) != NULL) {
            send_peers(i, now, snap, buf);
        }
    }
}

/* A neighbour's list arrived.  Runs on the link's reader task. */
static void peers_learn(const uint8_t *body, uint32_t len)
{
    const char *me = ag_node_name();
    const uint32_t now = now_ms();
    bool     have_via = false;
    int32_t  via = -1;
    uint32_t at = 0;

    while (at + 2u <= len) {
        const uint32_t n = body[at++];
        if (n == 0 || n >= AG_NODE_NAME_MAX || at + n + 6u > len) {
            return; /* a record that does not fit: whole ones before it stand */
        }
        char name[AG_NODE_NAME_MAX];
        memcpy(name, body + at, n);
        name[n] = '\0';
        at += n;
        const uint8_t cores = body[at++];
        const uint8_t hops = body[at++];
        const uint32_t hz = (uint32_t)body[at] |
                            ((uint32_t)body[at + 1] << 8) |
                            ((uint32_t)body[at + 2] << 16) |
                            ((uint32_t)body[at + 3] << 24);
        at += 4u;

        if (!have_via) {
            /*
             * The first record is the sender.  If it is not a board a wire of
             * ours reaches, this frame did not come from a neighbour and is
             * dropped whole: there would be nowhere to say "through" it, and a
             * node nobody can point at is worse than one not shown.
             */
            via = wire_of_name(name);
            if (via < 0) {
                return;
            }
            have_via = true;
            continue;
        }
        if (me != NULL && strcmp(name, me) == 0) {
            continue; /* my own name coming home */
        }
        if (hops == 0u || hops > NC_PEERS_MAX_HOPS) {
            continue;
        }
        if (name_is_direct(name)) {
            continue; /* a wire of ours beats hearsay */
        }
        if (s_peerlock == NULL) {
            return;
        }

        ag_port_mutex_take(s_peerlock, AG_PORT_FOREVER);
        uint32_t slot = NC_PEERS;
        for (uint32_t k = 0; k < NC_PEERS; k++) {
            if (s_peer[k].used && strcmp(s_peer[k].name, name) == 0) {
                slot = k;
                break;
            }
        }
        /* new to this board, or heard through another wire: its devices */
        const bool fresh = (slot == NC_PEERS) || s_peer[slot].via != via ||
                           (uint32_t)(now - s_peer[slot].seen_ms) >
                               NC_PEERS_TTL_MS;
        for (uint32_t k = 0; slot == NC_PEERS && k < NC_PEERS; k++) {
            if (!s_peer[k].used) {
                slot = k;
            }
        }
        if (slot == NC_PEERS) {
            /* Full.  The oldest goes, so a board that left is the one lost. */
            uint32_t oldest = 0;
            for (uint32_t k = 1; k < NC_PEERS; k++) {
                if ((uint32_t)(now - s_peer[k].seen_ms) >
                    (uint32_t)(now - s_peer[oldest].seen_ms)) {
                    oldest = k;
                }
            }
            slot = oldest;
        }
        nc_peer_t *p = &s_peer[slot];
        memset(p, 0, sizeof(*p));
        snprintf(p->name, sizeof(p->name), "%s", name);
        p->used = true;
        p->cores = cores;
        p->hops = hops;
        p->cpu_hz = hz;
        p->via = via;
        p->seen_ms = now;
        ag_port_mutex_give(s_peerlock);
        /*
         * Every time it is heard of, not only the first: noderdev keeps what
         * is attached and asks again after a failure, and a node that was
         * heard of while it was restarting would otherwise never be asked.
         */
        (void)fresh;
        ag_noderdev_far_named(via, name);
    }
}

/*
 * What an operator is shown: the far side of a neighbour, with the wire it is
 * seen through.  Deliberately NOT part of tr_nodes - see argon/clu.h.
 */
static int32_t tr_peers_seen(ag_clu_node_t *out, int32_t *via, uint32_t max)
{
    if (s_peerlock == NULL) {
        return 0;
    }
    const uint32_t now = now_ms();
    int32_t        n = 0;

    ag_port_mutex_take(s_peerlock, AG_PORT_FOREVER);
    for (uint32_t k = 0; k < NC_PEERS; k++) {
        const nc_peer_t *p = &s_peer[k];
        if (!p->used || (uint32_t)(now - p->seen_ms) > NC_PEERS_TTL_MS) {
            continue;
        }
        if (name_is_direct(p->name)) {
            continue; /* it became a neighbour of ours in the meantime */
        }
        if (out == NULL || (uint32_t)n >= max) {
            n++;
            continue;
        }
        memset(&out[n], 0, sizeof(out[n]));
        snprintf(out[n].name, sizeof(out[n].name), "%s", p->name);
        out[n].cores = (p->cores > 0) ? p->cores : 1u;
        out[n].cpu_hz = p->cpu_hz;
        out[n].here = 0;
        if (via != NULL) {
            via[n] = p->via;
        }
        n++;
    }
    ag_port_mutex_give(s_peerlock);
    return n;
}

static int32_t tr_nodes(ag_clu_node_t *out, uint32_t max)
{
    int32_t n = 0;
    for (int32_t i = 0; i < (int32_t)AG_LINK_WIRES; i++) {
        const char *wire = ag_linkprov_name_at(i);
        if (wire == NULL) {
            continue;
        }
        if (out != NULL && (uint32_t)n < max) {
            const char   *name = ag_node_peer_name_at(i);
            const uint8_t cores = ag_node_peer_cores_at(i);
            memset(&out[n], 0, sizeof(out[n]));
            snprintf(out[n].name, sizeof(out[n].name), "%s",
                     (name != NULL && name[0] != '\0') ? name : wire);
            /* It has not said, so one - never none.  A node counted as having
             * no cores would never be given a thread, and the cluster would
             * quietly be half its size. */
            out[n].cores = (cores > 0) ? cores : 1u;
            out[n].cpu_hz = ag_node_peer_cpu_hz_at(i);
            out[n].here = 0;
        }
        n++;
    }
    return n;
}

static const ag_clu_transport_t k_transport = {
    .spawn = tr_spawn,
    .send = tr_send,
    .credit = tr_credit,
    .exited = tr_exited,
    .end = tr_end,
    .nodes = tr_nodes,
    .peers_seen = tr_peers_seen,
};

/* ---------------------------------------------------------------------- */
/* Serving the other node                                                  */
/* ---------------------------------------------------------------------- */

static void *rx_alloc(uint32_t bytes, void *ctx)
{
    (void)ctx;
    void *p = ag_port_alloc(bytes, AG_MEM_SLOW | AG_MEM_BYTE);
    if (p == NULL) {
        p = ag_port_alloc(bytes, AG_MEM_FAST | AG_MEM_BYTE);
    }
    return p;
}

static void rx_free(void *p, void *ctx)
{
    (void)ctx;
    ag_port_free(p);
}

/*
 * The task that does anything slow.  Everything here either loads an image,
 * waits for a process to start, or puts a frame on a wire, and none of that may
 * happen on the link's reader task.
 */
static void serve_task(void *arg)
{
    (void)arg;
    for (;;) {
        for (uint32_t i = 0; i < NC_REQS; i++) {
            /*
             * Taken as a COPY under the lock, then served without it.  Serving
             * one means loading an image off the other board, which takes
             * seconds; holding the request lock across that would stop the
             * reader task dead the moment another request arrived.
             */
            nc_req_t req;
            char     image[AG_CLU_IMAGE_MAX];
            ag_port_mutex_take(s_reqlock, AG_PORT_FOREVER);
            const bool have = s_req[i].used && !s_req[i].taken;
            if (have) {
                req = s_req[i];
                memcpy(image, s_image, sizeof(image));
                s_req[i].taken = true;
            }
            ag_port_mutex_give(s_reqlock);
            if (!have) {
                continue;
            }

            const ag_err_t e = ag_clu_accept_spawn(req.tid, req.entry, req.arg,
                                                   req.arglen, image);
            if (e != AG_OK) {
                ag_log(AG_LOG_WARN, "clu", "cannot run '%s' here: %d",
                       req.entry, (int)e);
            }
            /*
             * THE ANSWER GOES BACK TO WHOEVER ASKED, out of the wire their
             * request arrived on - both remembered in the slot, because this is
             * not the reader task and neither ag_linkprov_from() nor the wire in
             * hand means anything here.  On the board in the middle, the
             * selected wire is the other neighbour half the time, and a
             * spawn acknowledgement that leaves by it is a job that never
             * starts anywhere.
             */
            ag_cluf_hdr_t h = { .op = AG_CLUF_SPAWN_OK, .flags = 0, .len = 0,
                                .a = req.tid, .b = (int32_t)e, .c = 0 };
            (void)send_frame(req.wire, req.from, &h, NULL);

            ag_port_mutex_take(s_reqlock, AG_PORT_FOREVER);
            s_req[i].used = false;
            s_req[i].taken = false;
            ag_port_mutex_give(s_reqlock);
        }

        /*
         * The job's owner has gone.  Told from here rather than from the
         * process teardown path, which runs with the process table locked and
         * must not touch a wire.
         */
        if (ag_clu_take_pending_end()) {
            tr_end(1);
        }

        /*
         * And the case nothing else covers: the wire itself went away while a
         * job was running on it.  A worker process would otherwise wait for an
         * END that can no longer be sent - a program alive for ever, serving a
         * master that is not there.
         */
        if (!ag_linkprov_bound() && ag_clu_busy()) {
            /* Acted on by a worker only - see ag_clu_job_end.  A master that
             * loses the wire keeps its own job and is told by its sends. */
            ag_clu_job_end();
        }

        /*
         * And what the neighbours should know about each other.  From here
         * rather than from the reader: putting a frame on a wire blocks while
         * the link's window is full, and a reader stopped there hears nothing
         * at all - including the acknowledgement that would empty the window.
         */
        peers_tick();

        ag_port_task_delay(ag_port_ms_to_ticks(20));
    }
}

/* A NUL-terminated field out of the body, or -1 if it does not end inside it. */
static int32_t take_string(const uint8_t *body, uint32_t len, uint32_t at,
                           char *out, uint32_t cap)
{
    uint32_t n = 0;
    while (at + n < len && body[at + n] != '\0') {
        n++;
    }
    if (at + n >= len || n >= cap) {
        return -1;
    }
    memcpy(out, body + at, n);
    out[n] = '\0';
    return (int32_t)(at + n + 1u);
}

static void serve_spawn(const ag_cluf_hdr_t *h, const uint8_t *body)
{
    /*
     * WHO ASKED AND ON WHICH WIRE, taken first because this runs on the reader
     * task and these are the two facts it alone can answer.  They are kept in
     * the slot for the serving task, which answers the spawn, and in s_job_* for
     * every frame of the job afterwards - a worker's own frames are sent from a
     * program's task, where neither is available.
     */
    const uint16_t from = ag_linkprov_from();
    const int32_t  wire = ag_linkprov_current();

    ag_port_mutex_take(s_reqlock, AG_PORT_FOREVER);

    nc_req_t *r = NULL;
    for (uint32_t i = 0; i < NC_REQS && r == NULL; i++) {
        if (!s_req[i].used) {
            r = &s_req[i];
        }
    }

    ag_err_t why = AG_OK;
    if (r == NULL) {
        why = -AG_EBUSY;
    } else {
        /* entry, image, argument - each where the last one ended. */
        int32_t at = take_string(body, h->len, 0, r->entry, sizeof(r->entry));
        if (at >= 0) {
            at = take_string(body, h->len, (uint32_t)at, s_image,
                             sizeof(s_image));
        }
        if (at < 0) {
            why = -AG_EINVAL;
        } else {
            const uint32_t arglen =
                (h->b > 0 && (uint32_t)h->b <= AG_CLU_ARG_MAX) ? (uint32_t)h->b
                                                               : 0;
            r->arglen = 0;
            if (arglen > 0 && (uint32_t)at + arglen <= h->len) {
                memcpy(r->arg, body + at, arglen);
                r->arglen = arglen;
            }
            r->tid = h->a;
            r->from = from;
            r->wire = wire;
            r->taken = false;
            r->used = true;
            s_job_from = from;
            s_job_wire = wire;
            ag_port_mutex_give(s_reqlock);
            return;
        }
    }
    ag_port_mutex_give(s_reqlock);

    /*
     * Answered here, on the reader task, and it is the one send from this
     * handler - short, and the alternative is a spawn that gets no answer at
     * all until the far end's twenty-second timeout.  Addressed to the node
     * that asked, which is not necessarily the one at the end of this wire.
     */
    ag_cluf_hdr_t no = { .op = AG_CLUF_SPAWN_OK, .flags = 0, .len = 0,
                         .a = h->a, .b = (int32_t)why, .c = 0 };
    (void)send_frame(wire, from, &no, NULL);
}

/*
 * One frame on the job channel.  Runs on the link's reader task, so nothing
 * here may block: a spawn is put in a slot for the task above, and everything
 * else is a table change measured in microseconds.
 */
static void on_msg(uint8_t chan, const void *data, uint16_t n, void *ctx)
{
    (void)chan;
    (void)ctx;

    ag_cluf_hdr_t  h;
    const uint8_t *body = NULL;
    if (!ag_cluf_get((const uint8_t *)data, n, &h, &body)) {
        return;
    }

    switch (h.op) {
    case AG_CLUF_SPAWN:
        serve_spawn(&h, body);
        break;

    case AG_CLUF_SPAWN_OK:
        if (s_await && h.a == s_await_tid) {
            s_answer = h.b;
            s_answered = true;
        }
        break;

    case AG_CLUF_DATA: {
        uint8_t *msg = NULL;
        uint32_t len = 0;
        int32_t  from = 0;
        int32_t  to = 0;
        const int r =
            ag_cluf_rx_data(&s_rx, &h, body, &msg, &len, &from, &to);
        if (r == 1) {
            /* Ownership passes: delivered or not, the buffer is freed there. */
            (void)ag_clu_deliver(from, to, msg, len);
        } else if (r < 0) {
            ag_log(AG_LOG_WARN, "clu", "a fragment for thread %d was lost: %d",
                   (int)h.b, r);
        }
        break;
    }

    case AG_CLUF_CREDIT:
        ag_clu_add_credit(h.a, (uint32_t)((h.b > 0) ? h.b : 0));
        break;

    case AG_CLUF_EXIT:
        ag_clu_note_exit(h.a);
        break;

    case AG_CLUF_END:
        ag_cluf_rx_reset(&s_rx);
        ag_clu_job_end();
        break;

    case AG_CLUF_PEERS:
        peers_learn(body, (uint32_t)h.len);
        break;

    default:
        break;
    }
}

bool ag_nodeclu_serving(void) { return s_listening; }

ag_err_t ag_nodeclu_start(void)
{
    if (s_listening) {
        return AG_OK;
    }
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }
    if (s_tx == NULL) {
        s_tx = ag_port_mutex_new();
        if (s_tx == NULL) {
            return -AG_ENOMEM;
        }
    }
    if (s_reqlock == NULL) {
        s_reqlock = ag_port_mutex_new();
        if (s_reqlock == NULL) {
            return -AG_ENOMEM;
        }
    }
    if (s_peerlock == NULL) {
        s_peerlock = ag_port_mutex_new();
        if (s_peerlock == NULL) {
            return -AG_ENOMEM;
        }
    }

    s_rx.alloc_fn = rx_alloc;
    s_rx.free_fn = rx_free;
    s_rx.ctx = NULL;

    const ag_err_t e = ag_linkprov_listen(NC_CHAN, on_msg, NULL);
    if (e != AG_OK) {
        return e;
    }

    if (s_task == NULL) {
        /* Below applications, like every other task this file's neighbours
         * create: a board serving somebody else's job must not stop answering
         * its own user. */
        if (!ag_port_task_create(serve_task, "nodeclu", 4096, NULL, 1,
                                 AG_PORT_ANY_CORE, 0, &s_task)) {
            ag_linkprov_unlisten(NC_CHAN);
            return -AG_ENOMEM;
        }
    }

    ag_clu_set_transport(&k_transport);
    s_listening = true;
    ag_log(AG_LOG_INFO, "clu", "job channel open on %s", ag_linkprov_active());
    return AG_OK;
}
