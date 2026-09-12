/*
 * ArgonOS - what the other node has.  See argon/nodedev.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/nodedev.h>

#include <string.h>

#include <argon/device.h>
#include <argon/linkprov.h>
#include <argon/log.h>
#include <argon/proc.h>

#include <argon/port/task.h>
#include <argon/port/time.h>

/* Its own channel, so a listing does not queue behind a program's output. */
#define ND_CHAN 3u

enum {
    ND_OP_LIST = 1, /* "what have you got"                     */
    ND_OP_SOME = 2, /* some of the answer; more may follow     */
    ND_OP_END = 3,  /* that was all of it                      */
    /*
     * "Could you run a program that needs this?"  Payload is a .AXE flags word;
     * the answer is yes or no.
     *
     * A separate question from the listing, and it has to be, because a listing
     * cannot answer it truthfully.  Every node has a sound device - pcmnull, the
     * mute one - so a node with no sound at all still lists an AG_DEV_AUDIO row,
     * and deciding from the list would send a program that needs sound to a
     * board that will refuse it.  Asking lets the far node apply the SAME test
     * the loader there would apply, which is the only test whose answer matches
     * what will actually happen.
     */
    ND_OP_CAN = 4,
    ND_OP_CAN_IS = 5, /* status in the first payload byte: 1 yes, 0 no */
};

/*
 * One device on the wire: a fixed record, NOT the ABI struct.
 *
 * ag_devinfo_t has an enum in the middle of it, and an enum is whatever width
 * the compiler on that side chose - so sending the struct would work perfectly
 * between two identical builds and quietly mean something else the first time
 * the cluster is an S3 beside a C6.  Which is the normal case here.
 */
#define ND_NAME 24u
#define ND_REC  (ND_NAME + ND_NAME + 2u) /* name, driver, class, flags  */

#define ND_HDR 4u
/* Records per message.  Eight is 400 bytes, which is more than a quantum, so
 * the real cap is what fits: the sender stops when the next one would not. */
#define ND_MAX_MSG 240u

static bool s_listening;

/* What a listing in progress is collecting.  One at a time, and the reader task
 * is the only thing that writes it. */
static ag_devinfo_t *s_out;
static uint32_t      s_out_max;
static volatile uint32_t s_out_n;
static volatile bool     s_done;
static volatile bool     s_wanted;

/* The capability question in flight.  One at a time; it is a single yes or no. */
static volatile bool s_can_wanted;
static volatile bool s_can_have;
static volatile bool s_can_yes;

static void put_rec(uint8_t *p, const ag_devinfo_t *info)
{
    memset(p, 0, ND_REC);
    memcpy(p, info->name, ND_NAME - 1u);
    memcpy(p + ND_NAME, info->driver, ND_NAME - 1u);
    p[ND_NAME * 2u] = (uint8_t)info->cls;
    p[ND_NAME * 2u + 1u] = (uint8_t)(info->flags & 0xffu);
}

static void get_rec(ag_devinfo_t *info, const uint8_t *p)
{
    memset(info, 0, sizeof(*info));
    memcpy(info->name, p, ND_NAME - 1u);
    memcpy(info->driver, p + ND_NAME, ND_NAME - 1u);
    info->cls = (ag_dev_class_t)p[ND_NAME * 2u];
    info->flags = p[ND_NAME * 2u + 1u];
}

static ag_err_t send_msg(uint8_t op, const void *payload, uint16_t len)
{
    uint8_t buf[ND_HDR + ND_MAX_MSG];
    if (len > ND_MAX_MSG) {
        len = ND_MAX_MSG;
    }
    buf[0] = op;
    buf[1] = 0;
    buf[2] = (uint8_t)(len & 0xffu);
    buf[3] = (uint8_t)((len >> 8) & 0xffu);
    if (len > 0 && payload != NULL) {
        memcpy(buf + ND_HDR, payload, len);
    }
    for (int t = 0; t < 200; t++) {
        const int32_t r =
            ag_linkprov_send(ND_CHAN, AG_LINK_CONTROL, buf, ND_HDR + len);
        if (r >= 0) {
            return AG_OK;
        }
        if (r != -AG_EAGAIN) {
            return (ag_err_t)r;
        }
        ag_linkprov_poll(5);
    }
    return -AG_ETIMEDOUT;
}

/* Answer "what have you got" with everything in this node's registry. */
static void serve_list(void)
{
    uint8_t  pack[ND_MAX_MSG];
    uint16_t at = 0;

    for (uint32_t i = 0;; i++) {
        ag_devinfo_t info;
        if (ag_dev_info(i, AG_DEV_ANY, &info) != AG_OK) {
            break;
        }
        if (at + ND_REC > sizeof(pack)) {
            (void)send_msg(ND_OP_SOME, pack, at);
            at = 0;
        }
        put_rec(pack + at, &info);
        at = (uint16_t)(at + ND_REC);
    }
    if (at > 0) {
        (void)send_msg(ND_OP_SOME, pack, at);
    }
    /*
     * END goes out even when there was nothing, and that is the point of having
     * it: a node with no devices and a node that is not there look identical
     * without it, and they mean opposite things.
     */
    (void)send_msg(ND_OP_END, NULL, 0);
}

/*
 * Answer "could you run a program that needs this?" with the SAME test the
 * loader here would apply.  Not a guess from the device list - see ND_OP_CAN.
 */
static void serve_can(const uint8_t *payload, uint16_t len)
{
    uint32_t flags = 0;
    for (uint16_t i = 0; i < 4u && i < len; i++) {
        flags |= (uint32_t)payload[i] << (8u * i);
    }
    const uint8_t yes = (ag_proc_unmet_needs(flags) == 0) ? 1u : 0u;
    (void)send_msg(ND_OP_CAN_IS, &yes, 1u);
}

static void on_msg(uint8_t chan, const void *data, uint16_t n, void *ctx)
{
    (void)chan;
    (void)ctx;
    if (n < ND_HDR) {
        return;
    }
    const uint8_t *buf = (const uint8_t *)data;
    const uint8_t  op = buf[0];
    uint16_t       len = (uint16_t)((uint16_t)buf[2] | ((uint16_t)buf[3] << 8));
    if (len > (uint16_t)(n - ND_HDR)) {
        len = (uint16_t)(n - ND_HDR);
    }

    switch (op) {
    case ND_OP_LIST:
        serve_list();
        break;
    case ND_OP_SOME:
        if (!s_wanted) {
            break; /* nobody asked: an answer to a question already given up on */
        }
        for (uint16_t at = 0; at + ND_REC <= len; at = (uint16_t)(at + ND_REC)) {
            if (s_out_n >= s_out_max) {
                break;
            }
            get_rec(&s_out[s_out_n], buf + ND_HDR + at);
            s_out_n++;
        }
        break;
    case ND_OP_END:
        s_done = true;
        break;
    case ND_OP_CAN:
        serve_can(buf + ND_HDR, len);
        break;
    case ND_OP_CAN_IS:
        if (s_can_wanted && len >= 1u) {
            s_can_yes = (buf[ND_HDR] != 0u);
            s_can_have = true;
        }
        break;
    default:
        break;
    }
}

ag_err_t ag_nodedev_start(void)
{
    if (s_listening) {
        return AG_OK;
    }
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }
    const ag_err_t e = ag_linkprov_listen(ND_CHAN, on_msg, NULL);
    if (e != AG_OK) {
        return e;
    }
    s_listening = true;
    return AG_OK;
}

int32_t ag_nodedev_list(ag_devinfo_t *out, uint32_t max, uint32_t timeout_ms)
{
    if (out == NULL || max == 0u) {
        return -AG_EINVAL;
    }
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }
    if (!s_listening) {
        const ag_err_t e = ag_nodedev_start();
        if (e != AG_OK) {
            return (int32_t)e;
        }
    }

    s_out = out;
    s_out_max = max;
    s_out_n = 0;
    s_done = false;
    s_wanted = true;

    const ag_err_t err = send_msg(ND_OP_LIST, NULL, 0);
    if (err != AG_OK) {
        s_wanted = false;
        return (int32_t)err;
    }

    /* On the clock, not by counting sleeps - a tick is ten milliseconds
     * whatever a shorter one is asked for. */
    const int64_t deadline = ag_port_us() + (int64_t)timeout_ms * 1000;
    while (!s_done) {
        if (ag_port_us() >= deadline) {
            s_wanted = false;
            return -AG_ETIMEDOUT;
        }
        ag_port_task_delay(ag_port_ms_to_ticks(5));
    }
    s_wanted = false;
    return (int32_t)s_out_n;
}

int32_t ag_nodedev_can(uint32_t axe_flags, uint32_t timeout_ms)
{
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }
    if (!s_listening) {
        const ag_err_t e = ag_nodedev_start();
        if (e != AG_OK) {
            return (int32_t)e;
        }
    }
    /* The answer arrives on the reader task; asking from it waits for itself. */
    if (ag_linkprov_in_reader()) {
        return -AG_EBUSY;
    }

    uint8_t pay[4];
    for (uint32_t i = 0; i < 4u; i++) {
        pay[i] = (uint8_t)((axe_flags >> (8u * i)) & 0xffu);
    }

    s_can_have = false;
    s_can_yes = false;
    s_can_wanted = true;

    const ag_err_t err = send_msg(ND_OP_CAN, pay, sizeof(pay));
    if (err != AG_OK) {
        s_can_wanted = false;
        return (int32_t)err;
    }

    const int64_t deadline = ag_port_us() + (int64_t)timeout_ms * 1000;
    while (!s_can_have) {
        if (ag_port_us() >= deadline) {
            s_can_wanted = false;
            /*
             * A node that did not answer is not a node that said no: the
             * difference matters, because "no" means place it elsewhere and
             * this means the wire is not working.
             */
            return -AG_ETIMEDOUT;
        }
        ag_port_task_delay(ag_port_ms_to_ticks(5));
    }
    s_can_wanted = false;
    return s_can_yes ? 1 : 0;
}
