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
/* What fits in one message beside the header.  The link's quantum caps it. */
#define NF_MAX_PAYLOAD 240u

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

static ag_err_t send_msg(uint8_t op, uint8_t cls, int32_t code,
                         const void *payload, uint16_t len)
{
    uint8_t buf[NF_HDR + NF_MAX_PAYLOAD];
    if (len > NF_MAX_PAYLOAD) {
        len = NF_MAX_PAYLOAD;
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
    for (int t = 0; t < 2000; t++) {
        const int32_t r = ag_linkprov_send(NF_CHAN, cls, buf, NF_HDR + len);
        if (r >= 0) {
            return AG_OK;
        }
        if (r != -AG_EAGAIN) {
            return (ag_err_t)r;
        }
        ag_linkprov_poll(2);
    }
    return -AG_ETIMEDOUT;
}

/* Read the file and push it, in order, on the bulk class. */
static void serve_get(const uint8_t *payload, uint16_t len)
{
    char path[AG_PATH_MAX];

    if (len == 0 || len >= sizeof(path)) {
        (void)send_msg(NF_OP_END, AG_LINK_CONTROL, -AG_EINVAL, NULL, 0);
        return;
    }
    memcpy(path, payload, len);
    path[len] = '\0';

    const ag_handle_t h = ag_vfs_open(path, "/", AG_O_RDONLY);
    if (h < 0) {
        (void)send_msg(NF_OP_END, AG_LINK_CONTROL, (int32_t)h, NULL, 0);
        return;
    }

    ag_err_t err = AG_OK;
    for (;;) {
        uint8_t       chunk[NF_MAX_PAYLOAD];
        const int32_t n = ag_vfs_read(h, chunk, sizeof(chunk));
        if (n < 0) {
            err = (ag_err_t)n;
            break;
        }
        if (n == 0) {
            break;
        }
        /*
         * BULK, not CONTROL.  This is the class the window and the
         * retransmissions are for: it may wait behind anything with a deadline,
         * and it is the only one here that could be made to.
         */
        err = send_msg(NF_OP_DATA, AG_LINK_BULK, 0, chunk, (uint16_t)n);
        if (err != AG_OK) {
            break;
        }
    }
    (void)ag_vfs_close(h);

    /*
     * END on CONTROL, and the whole transfer's correctness rests on this being
     * the same channel as the data: a channel is one conversation and the link
     * keeps a conversation in order, so END cannot overtake the bytes it ends.
     * A different channel would let it, and the file would be closed short
     * while the tail was still on the wire.
     */
    (void)send_msg(NF_OP_END, AG_LINK_CONTROL, (int32_t)err, NULL, 0);
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
        serve_get(buf + NF_HDR, len);
        break;
    case NF_OP_DATA:
        if (s_wanted && s_sink >= 0 && len > 0) {
            const int32_t w = ag_vfs_write(s_sink, buf + NF_HDR, len);
            if (w < 0) {
                s_status = w;
                s_done = true;
            } else {
                s_got += (uint32_t)w;
            }
        }
        break;
    case NF_OP_END:
        s_status = (int32_t)get_u32(buf + 4);
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

    const size_t rl = strlen(remote);
    ag_err_t     err = send_msg(NF_OP_GET, AG_LINK_CONTROL, 0, remote,
                                (uint16_t)((rl > NF_MAX_PAYLOAD)
                                               ? NF_MAX_PAYLOAD
                                               : rl));
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
