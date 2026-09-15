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
} nc_req_t;

static bool            s_listening;
static ag_port_task_t  s_task;
static nc_req_t        s_req[NC_REQS];
static ag_port_mutex_t s_reqlock;
static ag_cluf_rx_t    s_rx;

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

static ag_err_t send_bytes(const uint8_t *p, uint32_t total)
{
    const uint32_t deadline =
        now_ms() + (ag_linkprov_in_reader() ? 100u : NC_SEND_MS);
    for (;;) {
        const int32_t r = ag_linkprov_send(NC_CHAN, AG_LINK_CONTROL, p, total);
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

static ag_err_t send_frame(const ag_cluf_hdr_t *h, const void *payload)
{
    uint8_t        buf[AG_CLUF_HDR + 64];
    const uint32_t total = AG_CLUF_HDR + h->len;

    if (total > sizeof(buf)) {
        return -AG_ERANGE; /* the caller must use the shared buffer instead */
    }
    (void)ag_cluf_put(buf, h, payload);
    return send_bytes(buf, total);
}

/* The same, for a frame already built in s_txbuf (a data fragment). */
static ag_err_t send_txbuf(uint32_t total) { return send_bytes(s_txbuf, total); }

/* ---------------------------------------------------------------------- */
/* The transport the cluster layer uses                                    */
/* ---------------------------------------------------------------------- */

static ag_err_t tr_spawn(int32_t node, ag_clu_tid_t tid, const char *entry,
                         const void *arg, uint32_t len, const char *image,
                         uint32_t timeout_ms)
{
    (void)node; /* one wire, one neighbour - see ag_clu_transport_t */

    if (ag_linkprov_in_reader()) {
        /* Waiting here would wait for the task that delivers the answer. */
        return -AG_EBUSY;
    }
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }

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
        e = send_txbuf(total);
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
    (void)node;

    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }
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
        e = send_txbuf(total);
        off += take;
    } while (e == AG_OK && off < len);

    ag_port_mutex_give(s_tx);
    return e;
}

static void tr_credit(int32_t node, ag_clu_tid_t tid, uint32_t n)
{
    (void)node;
    ag_cluf_hdr_t h = { .op = AG_CLUF_CREDIT, .flags = 0, .len = 0,
                        .a = tid, .b = (int32_t)n, .c = 0 };
    (void)send_frame(&h, NULL);
}

static void tr_exited(int32_t node, ag_clu_tid_t tid)
{
    (void)node;
    ag_cluf_hdr_t h = { .op = AG_CLUF_EXIT, .flags = 0, .len = 0,
                        .a = tid, .b = 0, .c = 0 };
    (void)send_frame(&h, NULL);
}

static void tr_end(int32_t node)
{
    (void)node;
    ag_cluf_hdr_t h = { .op = AG_CLUF_END, .flags = 0, .len = 0,
                        .a = 0, .b = 0, .c = 0 };
    (void)send_frame(&h, NULL);
}

static int32_t tr_nodes(ag_clu_node_t *out, uint32_t max)
{
    if (!ag_linkprov_bound()) {
        return 0;
    }
    if (out != NULL && max >= 1) {
        const char   *name = ag_node_peer_name();
        const uint8_t cores = ag_node_peer_cores();
        memset(&out[0], 0, sizeof(out[0]));
        snprintf(out[0].name, sizeof(out[0].name), "%s",
                 (name != NULL && name[0] != '\0') ? name
                                                   : ag_linkprov_active());
        /* It has not said, so one - never none.  A node counted as having no
         * cores would never be given a thread, and the cluster would quietly
         * be half its size. */
        out[0].cores = (cores > 0) ? cores : 1u;
        out[0].cpu_hz = ag_node_peer_cpu_hz();
        out[0].here = 0;
    }
    return 1;
}

static const ag_clu_transport_t k_transport = {
    .spawn = tr_spawn,
    .send = tr_send,
    .credit = tr_credit,
    .exited = tr_exited,
    .end = tr_end,
    .nodes = tr_nodes,
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
            ag_cluf_hdr_t h = { .op = AG_CLUF_SPAWN_OK, .flags = 0, .len = 0,
                                .a = req.tid, .b = (int32_t)e, .c = 0 };
            (void)send_frame(&h, NULL);

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
            r->taken = false;
            r->used = true;
            ag_port_mutex_give(s_reqlock);
            return;
        }
    }
    ag_port_mutex_give(s_reqlock);

    /*
     * Answered here, on the reader task, and it is the one send from this
     * handler - short, and the alternative is a spawn that gets no answer at
     * all until the far end's twenty-second timeout.
     */
    ag_cluf_hdr_t no = { .op = AG_CLUF_SPAWN_OK, .flags = 0, .len = 0,
                         .a = h->a, .b = (int32_t)why, .c = 0 };
    (void)send_frame(&no, NULL);
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
