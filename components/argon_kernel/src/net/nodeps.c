/*
 * ArgonOS - what the other node is running.  See argon/nodeps.h.
 *
 * Channel 7.  A listing and a kill, and nothing else: everything harder about
 * a process on another board - focusing it, moving it - needs its console to
 * come with it, which is a different mechanism and belongs elsewhere.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/nodeps.h>

#include <string.h>

#include <argon/libc.h>
#include <argon/linkprov.h>
#include <argon/log.h>
#include <argon/proc.h>

#include <argon/port/task.h>
#include <argon/port/time.h>

#define NP_CHAN 7u
#define NP_HDR 4u
#define NP_MAX_MSG 240u

enum {
    NP_LIST = 1,
    NP_SOME = 2,
    NP_END = 3,
    NP_KILL = 4,
    NP_KILLED = 5,
};

/*
 * One process on the wire: a fixed record, NOT the ABI struct.
 *
 * ag_procinfo_t has an enum and a size_t in it, and both are whatever width the
 * compiler on that side chose - so sending the struct would work perfectly
 * between two identical builds and mean something else the first time the
 * cluster is an S3 beside a C6.  Which is the normal case here.
 *
 *   pid(4) name(32) state(1) fg(1) prio(1) pad(1) mem_kb(4) cpu(2)
 */
#define NP_NAME 32u
#define NP_REC (4u + NP_NAME + 4u + 4u + 2u)

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
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

static void put_rec(uint8_t *p, const ag_procinfo_t *info)
{
    memset(p, 0, NP_REC);
    put32(p, (uint32_t)info->pid);
    memcpy(p + 4, info->name, NP_NAME - 1u);
    p[4 + NP_NAME] = (uint8_t)info->state;
    p[4 + NP_NAME + 1u] = info->foreground ? 1u : 0u;
    p[4 + NP_NAME + 2u] = info->priority;
    put32(p + 4 + NP_NAME + 4u, (uint32_t)(info->mem_used / 1024u));
    put16(p + 4 + NP_NAME + 8u, (uint16_t)info->cpu_permille);
}

static void get_rec(ag_procinfo_t *info, const uint8_t *p)
{
    memset(info, 0, sizeof(*info));
    info->pid = (ag_pid_t)get32(p);
    memcpy(info->name, p + 4, NP_NAME - 1u);
    info->state = (ag_proc_state_t)p[4 + NP_NAME];
    info->foreground = (p[4 + NP_NAME + 1u] != 0u);
    info->priority = p[4 + NP_NAME + 2u];
    /* Kilobytes on the wire, bytes in the struct: a process big enough to
     * overflow four bytes of kilobytes is not a case this hardware has. */
    info->mem_used = (size_t)get32(p + 4 + NP_NAME + 4u) * 1024u;
    info->cpu_permille = get16(p + 4 + NP_NAME + 8u);
}

static bool s_listening;

static ag_procinfo_t   *s_out;
static uint32_t         s_out_max;
static volatile uint32_t s_out_n;
static volatile bool     s_done;
static volatile bool     s_wanted;

static volatile bool    s_kill_wanted;
static volatile bool    s_kill_have;
static volatile int32_t s_kill_status;

static ag_err_t send_msg(uint8_t op, const void *payload, uint16_t len)
{
    uint8_t buf[NP_HDR + NP_MAX_MSG];
    if (len > NP_MAX_MSG) {
        len = NP_MAX_MSG;
    }
    buf[0] = op;
    buf[1] = 0;
    put16(buf + 2, len);
    if (len > 0 && payload != NULL) {
        memcpy(buf + NP_HDR, payload, len);
    }
    for (int t = 0; t < 200; t++) {
        const int32_t r =
            ag_linkprov_send(NP_CHAN, AG_LINK_CONTROL, buf, NP_HDR + len);
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

static void serve_list(void)
{
    uint8_t  pack[NP_MAX_MSG];
    uint16_t at = 0;

    for (uint32_t i = 0;; i++) {
        ag_procinfo_t info;
        if (ag_proc_info(i, &info) != AG_OK) {
            break;
        }
        if (at + NP_REC > sizeof(pack)) {
            (void)send_msg(NP_SOME, pack, at);
            at = 0;
        }
        put_rec(pack + at, &info);
        at = (uint16_t)(at + NP_REC);
    }
    if (at > 0) {
        (void)send_msg(NP_SOME, pack, at);
    }
    /*
     * END goes out even when there was nothing, and that is why it exists: a
     * node running nothing and a node that is not there look identical without
     * it, and they mean opposite things.
     */
    (void)send_msg(NP_END, NULL, 0);
}

static void on_msg(uint8_t chan, const void *data, uint16_t n, void *ctx)
{
    (void)chan;
    (void)ctx;
    if (n < NP_HDR) {
        return;
    }
    const uint8_t *buf = (const uint8_t *)data;
    const uint8_t  op = buf[0];
    uint16_t       len = get16(buf + 2);
    if (len > (uint16_t)(n - NP_HDR)) {
        len = (uint16_t)(n - NP_HDR);
    }

    switch (op) {
    case NP_LIST:
        serve_list();
        break;
    case NP_SOME:
        if (!s_wanted) {
            break; /* an answer to a question already given up on */
        }
        for (uint16_t at = 0; at + NP_REC <= len; at = (uint16_t)(at + NP_REC)) {
            if (s_out_n >= s_out_max) {
                break;
            }
            get_rec(&s_out[s_out_n], buf + NP_HDR + at);
            s_out_n++;
        }
        break;
    case NP_END:
        s_done = true;
        break;
    case NP_KILL: {
        const ag_pid_t pid =
            (len >= 4u) ? (ag_pid_t)get32(buf + NP_HDR) : (ag_pid_t)0;
        const int32_t r =
            (int32_t)ag_proc_kill(pid, "killed from the other node");
        uint8_t       o[4];
        put32(o, (uint32_t)r);
        (void)send_msg(NP_KILLED, o, sizeof(o));
        break;
    }
    case NP_KILLED:
        if (s_kill_wanted && len >= 4u) {
            s_kill_status = (int32_t)get32(buf + NP_HDR);
            s_kill_have = true;
        }
        break;
    default:
        break;
    }
}

ag_err_t ag_nodeps_serve(void)
{
    if (s_listening) {
        return AG_OK;
    }
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }
    const ag_err_t e = ag_linkprov_listen(NP_CHAN, on_msg, NULL);
    if (e != AG_OK) {
        return e;
    }
    s_listening = true;
    return AG_OK;
}

int32_t ag_nodeps_list(ag_procinfo_t *out, uint32_t max, uint32_t timeout_ms)
{
    if (out == NULL || max == 0u) {
        return -AG_EINVAL;
    }
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }
    if (!s_listening) {
        const ag_err_t e = ag_nodeps_serve();
        if (e != AG_OK) {
            return (int32_t)e;
        }
    }
    if (ag_linkprov_in_reader()) {
        return -AG_EBUSY;
    }

    s_out = out;
    s_out_max = max;
    s_out_n = 0;
    s_done = false;
    s_wanted = true;

    const ag_err_t err = send_msg(NP_LIST, NULL, 0);
    if (err != AG_OK) {
        s_wanted = false;
        return (int32_t)err;
    }

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

ag_err_t ag_nodeps_kill(ag_pid_t pid, uint32_t timeout_ms)
{
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }
    if (!s_listening) {
        const ag_err_t e = ag_nodeps_serve();
        if (e != AG_OK) {
            return e;
        }
    }
    if (ag_linkprov_in_reader()) {
        return -AG_EBUSY;
    }

    uint8_t p[4];
    put32(p, (uint32_t)pid);
    s_kill_have = false;
    s_kill_status = 0;
    s_kill_wanted = true;

    const ag_err_t err = send_msg(NP_KILL, p, sizeof(p));
    if (err != AG_OK) {
        s_kill_wanted = false;
        return err;
    }

    const int64_t deadline = ag_port_us() + (int64_t)timeout_ms * 1000;
    while (!s_kill_have) {
        if (ag_port_us() >= deadline) {
            s_kill_wanted = false;
            return -AG_ETIMEDOUT;
        }
        ag_port_task_delay(ag_port_ms_to_ticks(5));
    }
    s_kill_wanted = false;
    return (ag_err_t)s_kill_status;
}

bool ag_nodeps_split(const char *text, char *node_out, size_t node_len,
                     ag_pid_t *pid_out)
{
    if (text == NULL || pid_out == NULL) {
        return false;
    }
    const char *dot = strchr(text, '.');
    const char *digits = (dot != NULL) ? (dot + 1) : text;

    ag_pid_t v = 0;
    if (*digits == '\0') {
        return false;
    }
    for (const char *s = digits; *s != '\0'; s++) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        v = v * 10 + (ag_pid_t)(*s - '0');
    }
    *pid_out = v;

    if (dot == NULL) {
        return false;
    }
    if (node_out != NULL && node_len > 0) {
        size_t n = (size_t)(dot - text);
        if (n >= node_len) {
            n = node_len - 1u;
        }
        memcpy(node_out, text, n);
        node_out[n] = '\0';
    }
    return true;
}
