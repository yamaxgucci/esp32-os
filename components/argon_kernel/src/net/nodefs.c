/*
 * ArgonOS - fetching a file from the other node.
 *
 * The first thing on this link that is BULK rather than CONTROL, and that is
 * most of why it exists.  The window, the go-back-N and the retransmit timer
 * were written with care and proved on a development machine against an
 * injected loss model; until something streamed kilobytes through them between
 * two kernels, none of that had met a wire.
 *
 * It is also the smallest useful piece of "one disk for the whole system": a
 * program that should run over there has to get over there, and `link get` is
 * how.  A mounted remote drive is the larger version and is what the manifest
 * half of placement is waiting for - to decide by a program's declaration you
 * must read the program, and a node can only read what it can reach.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/nodefs.h>

#include <string.h>

#include <argon/linkprov.h>
#include <argon/log.h>
#include <argon/vfs.h>

#include <argon/port/task.h>
#include <argon/port/time.h>

#define NF_CHAN 4u

enum {
    NF_OP_GET = 1,  /* payload = the path, as the far node would type it */
    NF_OP_DATA = 2, /* payload = bytes, in order                         */
    NF_OP_END = 3,  /* code = AG_OK, or why it stopped                   */
};

#define NF_HDR 8u
/*
 * The most of a file one frame may carry.
 *
 * It was 240 - the UART's own message size, written down here as if it were a
 * property of file transfer.  It is not: it is a ceiling on THIS file's
 * buffers, and a wire that says it carries less is obeyed (nf_payload_max
 * below).  A wire that says it carries more is used up to here.  255 plus the
 * 8-byte header is 263, inside the 288-byte buffer the link reader receives
 * into, and raising it is one number on the day there is a wire to raise it
 * for.
 */
#define NF_CHUNK_MAX 255u
/*
 * A FILE ON WHAT IS LEFT OVER (27 Sep 2026).  The bytes of a file go on the
 * background class: behind the picture and the sound on every board, windowed
 * from end to end (linkprov.c, BACKGROUND).  A window like that is opened by
 * answers the READER takes in, so the file can no longer be served from the
 * reader, as it was: it would wait for answers its own task was too busy to
 * read.  A request is handed to a task of its own, which may wait as long as
 * background must - NF_BG_WAIT_MS a frame before it gives up.
 */
#define NF_BG_WAIT_MS 60000u
/*
 * 6 KB, measured (29 Sep 2026): a file served from each board - LittleFS on
 * the Zero and the C3, FAT on the CYD's card - took at most 4 324 bytes of the
 * 8 KB it had been given by guess ("as the reader's own stack"), and on the C3
 * playing sound 8 KB was sometimes more than there was.
 */
#define NF_TASK_STACK 6144u
#define NF_TASK_PRIO  2u    /* an application's low class: a file waits */

/* What one frame on the bound wire can hold, header aside.  Asked, not
 * assumed: the wire states its own shape and this is what info() is for.  A
 * driver older than info() is answered with the UART's measured numbers by
 * ag_linkprov_info itself, so there is no "unknown" case here. */
static uint32_t nf_payload_max(void)
{
    ag_link_info_t info;
    uint32_t       n = NF_CHUNK_MAX;

    if (ag_linkprov_info(&info) == AG_OK && info.mtu > NF_HDR) {
        const uint32_t m = info.mtu - NF_HDR;
        if (m < n) {
            n = m;
        }
    }
    return n;
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}
static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool s_listening;

/* The fetch in progress.  One at a time; the reader task is the only writer. */
static ag_handle_t       s_sink = -1;
static volatile uint32_t s_got;
static volatile bool     s_done;
static volatile int32_t  s_status;
static volatile bool     s_wanted;

/*
 * One frame out, ON PURPOSE TO SOMEBODY.
 *
 * `dst` is ag_node_hash() of the node the frame is for, and it is a parameter
 * rather than left to the link because the two callers mean different nodes:
 * a fetch asks the node this board is talking to (the selected target), while
 * every frame of an answer is for the node that ASKED - which need not be the
 * same board, and is not the same board at all when a third one sits in
 * between.  An answer that says "for whoever is reading" is taken by the board
 * in between and never passed home, and the asking node then waits for a file
 * that arrived somewhere else.
 */
static ag_err_t send_msg_on(int32_t wire, uint16_t dst, uint8_t op,
                            uint8_t cls, int32_t code, const void *payload,
                            uint16_t len)
{
    uint8_t buf[NF_HDR + NF_CHUNK_MAX];
    if (len > NF_CHUNK_MAX) {
        len = NF_CHUNK_MAX;
    }
    buf[0] = op;
    buf[1] = 0;
    put_u16(buf + 2, len);
    put_u32(buf + 4, (uint32_t)code);
    if (len > 0 && payload != NULL) {
        memcpy(buf + NF_HDR, payload, len);
    }
    /*
     * A full window is back-pressure and the normal state of a transfer that
     * is going as fast as the wire allows - so this waits rather than failing,
     * and pumps while it waits.  Bounded, because a node that has gone away
     * must not hold a file open here for ever.
     */
    const bool    bg = (cls == AG_LINK_BACKGROUND);
    const int64_t until =
        ag_port_us() + (int64_t)(bg ? NF_BG_WAIT_MS : 4000u) * 1000;
    for (;;) {
        const int32_t r =
            (wire >= 0)
                ? ag_linkprov_send_to_on(wire, dst, NF_CHAN, cls, buf,
                                         NF_HDR + len)
                : ag_linkprov_send_to(dst, NF_CHAN, cls, buf, NF_HDR + len);
        if (r >= 0) {
            return AG_OK;
        }
        if (r != -AG_EAGAIN) {
            return (ag_err_t)r;
        }
        if (ag_port_us() >= until) {
            return -AG_ETIMEDOUT;
        }
        if (bg) {
            ag_port_task_delay(1); /* its own task: the reader reads on */
        } else {
            ag_linkprov_poll(2);
        }
    }
}

static ag_err_t send_msg(uint16_t dst, uint8_t op, uint8_t cls, int32_t code,
                         const void *payload, uint16_t len)
{
    return send_msg_on(-1, dst, op, cls, code, payload, len);
}

/* The request the server task is serving: one at a time. */
static struct {
    volatile bool busy;
    uint16_t      from;
    int32_t       wire;
    uint16_t      len;
    uint8_t       path[AG_PATH_MAX];
} s_req;

/* Read the file and push it, in order, on the background class. */
static void serve_get(uint16_t from, int32_t wire, uint8_t cls,
                      const uint8_t *payload, uint16_t len)
{
    /*
     * WHOSE FILE THIS IS, taken once before the first byte moves.
     *
     * ag_linkprov_from() answers only while the receive callback runs, and this
     * callback is a long one: it reads the whole file and pumps the wire
     * whenever the window is full, so further frames are handled by this same
     * task before it returns.  The requester is remembered here and every frame
     * of the transfer is addressed to it - the last one included, which leaves
     * seconds later, on the bulk class, behind every chunk in front of it.  A
     * frame that said "for whoever is reading" instead would be taken by the
     * board in between and the asking node would wait for ever.
     */
    char           path[AG_PATH_MAX];
    /* One frame's worth, from the wire rather than from a number here.
     * nf_payload_max() never answers more than sizeof(chunk). */
    uint8_t  chunk[NF_CHUNK_MAX];
    const uint32_t cap = nf_payload_max();

    if (len == 0 || len >= sizeof(path)) {
        (void)send_msg_on(wire, from, NF_OP_END, AG_LINK_CONTROL, -AG_EINVAL,
                          NULL, 0);
        return;
    }
    memcpy(path, payload, len);
    path[len] = '\0';

    const ag_handle_t h = ag_vfs_open(path, "/", AG_O_RDONLY);
    if (h < 0) {
        (void)send_msg_on(wire, from, NF_OP_END, AG_LINK_CONTROL, (int32_t)h,
                          NULL, 0);
        return;
    }

    ag_err_t err = AG_OK;
    /*
     * WHAT THIS SIDE BELIEVES IT SENT, kept because the receiver's only other
     * source for that number is nothing at all.  It is the count of bytes
     * handed to the link's reliable class - not bytes read, because a chunk the
     * link refused never left - and it travels with the END below.
     */
    uint32_t sent = 0;
    for (;;) {
        const int32_t n = ag_vfs_read(h, chunk, cap);
        if (n < 0) {
            err = (ag_err_t)n;
            break;
        }
        if (n == 0) {
            break;
        }
        /*
         * BACKGROUND, not CONTROL and no longer BULK: it may wait behind
         * anything with a deadline - and behind a picture, which is BULK.
         */
        err = send_msg_on(wire, from, NF_OP_DATA, cls, 0, chunk,
                          (uint16_t)n);
        if (err != AG_OK) {
            break;
        }
        sent += (uint32_t)n;
    }
    (void)ag_vfs_close(h);

    /*
     * END on BULK - the same class as the bytes it ends - and the whole
     * transfer's correctness rests on that word.
     *
     * It was CONTROL, on the belief that "the link keeps a conversation in
     * order, so END cannot overtake the bytes it ends".  A channel is one
     * conversation; a CLASS is one sequence space.  The receiver keeps a
     * separate expected number per class, and the scheduler sends CONTROL
     * first, always, on purpose - a control frame is a device call or a spawn
     * and never a payload.  So END on CONTROL did not queue behind the data,
     * it overtook it.
     *
     * Measured, C3 to S3-Zero over one wire: 80 chunks of a 19 024-byte file
     * went out, 15 of them were still unacknowledged when END left, the
     * receiver took END, closed the file and stopped listening, and the copy
     * was reported as 15 600 bytes with no error said anywhere.  The count
     * moved with the window, not with the wire: 15 600 and 15 840 on two runs,
     * both whole chunks, both short by less than ND_WIN_BULK.
     *
     * On BULK the sequence number puts END behind every chunk it ends.  A tail
     * stuck behind a retransmission is then waited for rather than dropped,
     * and a transfer that cannot finish reports a timeout instead of a file
     * that is quietly the wrong length.
     *
     * AND THE COUNT GOES WITH THE VERDICT (see on_msg's NF_OP_END).  Order was
     * never the whole of the answer: a frame can be in the right place and
     * still not arrive at the file, and then the last frame's arrival says
     * nothing about the file's length.  Four bytes in the payload the header
     * already reserves costs nothing and is the only place the receiver can
     * learn what the length was meant to be.
     */
    uint8_t tail[4];
    put_u32(tail, sent);
    (void)send_msg_on(wire, from, NF_OP_END, cls, (int32_t)err,
                      tail, (uint16_t)sizeof(tail));
}

static void server_task(void *arg)
{
    (void)arg;
    serve_get(s_req.from, s_req.wire, AG_LINK_BACKGROUND, s_req.path,
              s_req.len);
    s_req.busy = false;
    /* what of NF_TASK_STACK a file took: the C3 under sound has not got 8 KB
     * to give, and the size was a guess (29 Sep 2026) */
    ag_log(AG_LOG_INFO, "nodefs", "a file served; %u of %u stack bytes unused",
           (unsigned)ag_port_task_stack_unused(NULL), (unsigned)NF_TASK_STACK);
    ag_port_task_delete(NULL);
}

/* On the reader: the request, to the server task; nothing waited for here. */
static void take_get(const uint8_t *payload, uint16_t len)
{
    const uint16_t from = ag_linkprov_from();
    const int32_t  wire = ag_linkprov_current();
    if (s_req.busy) {
        (void)send_msg_on(wire, from, NF_OP_END, AG_LINK_CONTROL, -AG_EBUSY,
                          NULL, 0);
        return;
    }
    if (len > sizeof(s_req.path)) {
        len = (uint16_t)sizeof(s_req.path); /* serve_get refuses it whole */
    }
    s_req.from = from;
    s_req.wire = wire;
    s_req.len = len;
    memcpy(s_req.path, payload, len);
    s_req.busy = true;
    ag_port_task_t t = NULL;
    if (!ag_port_task_create(server_task, "nodefs", NF_TASK_STACK, NULL,
                             NF_TASK_PRIO, AG_PORT_ANY_CORE, 0u, &t)) {
        /*
         * No memory for the task - an emulated node has 5.5 KB free - so
         * served here, on the reader, as before there was one: on BULK,
         * because background waits for answers only this task can read.
         */
        serve_get(from, wire, AG_LINK_BULK, s_req.path, s_req.len);
        s_req.busy = false;
    }
}

static void on_msg(uint8_t chan, const void *data, uint16_t n, void *ctx)
{
    (void)chan;
    (void)ctx;
    if (n < NF_HDR) {
        return;
    }
    const uint8_t *buf = (const uint8_t *)data;
    const uint8_t  op = buf[0];
    uint16_t       len = get_u16(buf + 2);
    if (len > (uint16_t)(n - NF_HDR)) {
        len = (uint16_t)(n - NF_HDR);
    }

    switch (op) {
    case NF_OP_GET:
        take_get(buf + NF_HDR, len);
        break;
    case NF_OP_DATA:
        if (s_wanted && s_sink >= 0 && len > 0) {
            const int32_t w = ag_vfs_write(s_sink, buf + NF_HDR, len);
            /*
             * A SHORT WRITE IS NOT A PARTIAL SUCCESS.  The bytes a sink did
             * not take are gone, and everything written after them lands where
             * they should have been - a file that is both short and shifted.
             * The tree already reads this call this way where the bytes matter
             * (`copy`, cmd_fs.c) and here it was `w < 0` alone, so a sink that
             * took half a chunk was counted as half a chunk and the transfer
             * carried on as if nothing had happened.
             */
            if (w != (int32_t)len) {
                s_status = (w < 0) ? w : -AG_EIO;
                s_done = true;
            } else {
                s_got += (uint32_t)w;
            }
        }
        break;
    case NF_OP_END:
        s_status = (int32_t)get_u32(buf + 4);
        /*
         * THE COUNT, AND WHY NOTHING ELSE HERE COULD DO THIS JOB.
         *
         * The end of a transfer used to be "an END arrived and its code was
         * zero", which is a statement about the LAST FRAME and not about the
         * file: every byte before it could be missing and this still said
         * success.  That is not hypothetical.  The link accepts a frame as
         * delivered - advancing the sequence number it will acknowledge -
         * BEFORE the message is queued for this service, and a full queue
         * there drops it (nodelink.c, frame_arrived then inbox_put).  A message
         * dropped that way is never sent again, because the far end was
         * already told it arrived; neither end can see it in a byte count and
         * no wire counter names it.  What can see it is a count that travelled
         * with the verdict, compared here against what actually reached the
         * file.
         *
         * Only when the sender said AG_OK: with an error the count is where it
         * stopped, and a receiver that is behind that is the error working as
         * intended.
         *
         * A peer that states no count (len < 4) is one built before this, and
         * is left alone: a check that cannot be made must not invent a
         * failure.
         */
        if (s_status == AG_OK && len >= 4 && get_u32(buf + NF_HDR) != s_got) {
            s_status = -AG_EIO;
        }
        s_done = true;
        break;
    default:
        break;
    }
}

ag_err_t ag_nodefs_start(void)
{
    if (s_listening) {
        return AG_OK;
    }
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }
    const ag_err_t e = ag_linkprov_listen(NF_CHAN, on_msg, NULL);
    if (e != AG_OK) {
        return e;
    }
    s_listening = true;
    return AG_OK;
}

int32_t ag_nodefs_get(const char *remote, const char *local, const char *cwd,
                      uint32_t timeout_ms)
{
    if (remote == NULL || local == NULL) {
        return -AG_EINVAL;
    }
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }
    if (!s_listening) {
        const ag_err_t e = ag_nodefs_start();
        if (e != AG_OK) {
            return (int32_t)e;
        }
    }

    /*
     * A PATH THAT DOES NOT FIT ONE FRAME IS REFUSED, not truncated.
     *
     * It was truncated, to whatever the constant of the day allowed, and the
     * far node then opened whatever the first part named: a long path meaning
     * one file could fetch a DIFFERENT file, with no error said anywhere.  The
     * length is checked against the wire - which is what decides how much of a
     * path travels - and before the sink is created, so a refused fetch does
     * not leave an empty file behind.
     */
    const size_t rl = strlen(remote);
    if (rl == 0 || rl > nf_payload_max()) {
        return -AG_EINVAL;
    }

    const ag_handle_t h =
        ag_vfs_open(local, cwd, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (h < 0) {
        return (int32_t)h;
    }

    s_sink = h;
    s_got = 0;
    s_done = false;
    s_status = 0;
    s_wanted = true;

    /*
     * A NEW conversation, so it goes to the node `link use` last named rather
     * than to whoever answered last: this is the one frame in this file that is
     * not an answer, and the target is precisely what that call is for.  Zero
     * there means the neighbour in hand, which is what it has always meant.
     */
    ag_err_t err = send_msg(ag_linkprov_target(), NF_OP_GET, AG_LINK_CONTROL, 0,
                            remote, (uint16_t)rl);
    if (err == AG_OK) {
        const int64_t deadline = ag_port_us() + (int64_t)timeout_ms * 1000;
        while (!s_done) {
            if (ag_port_us() >= deadline) {
                err = -AG_ETIMEDOUT;
                break;
            }
            ag_port_task_delay(ag_port_ms_to_ticks(5));
        }
    }

    s_wanted = false;
    s_sink = -1;
    (void)ag_vfs_close(h);

    if (err != AG_OK) {
        return (int32_t)err;
    }
    if (s_status != 0) {
        return s_status;
    }
    return (int32_t)s_got;
}
