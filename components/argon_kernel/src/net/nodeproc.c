/*
 * ArgonOS - running a program on the other node.  See argon/nodeproc.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/nodeproc.h>

#include <string.h>

#include <argon/cmdline.h>
#include <argon/console.h>
#include <argon/linkprov.h>
#include <argon/log.h>
#include <argon/path.h>
#include <argon/nodedrv.h>
#include <argon/migrate.h>
#include <argon/proc.h>

#include <argon/port/mem.h>
#include <argon/port/task.h>
#include <argon/port/time.h>

/*
 * The channel this conversation lives on.  Channels are independent, so a file
 * being read or a screen being sent later does not queue behind a program's
 * output, and vice versa.
 */
#define NP_CHAN 2u

/*
 * A message is a header and whatever follows it, and that is the whole
 * protocol.  No magic and no checksum: the link delivers whole messages that
 * have already been checked, or does not deliver them.
 *
 *   RUN   payload = the command line, as typed.  Split on the far side by the
 *         same code the shell uses, so quoting means there what it means here.
 *   OUT   payload = bytes the program wrote.  Not lines: a program that prints
 *         half a line and waits should have that half arrive.
 *   EXIT  code = what the program returned; no payload.
 *   BUSY  the far node is already running something.
 */
enum {
    NP_OP_RUN = 1,
    NP_OP_OUT = 2,
    NP_OP_EXIT = 3,
    NP_OP_BUSY = 4,
    /*
     * A migrating program's state, sent in pieces just before the RUN that
     * starts it.  Separate from RUN because a command line and a blob of
     * somebody's private state have nothing to do with each other, and
     * because the state can need more than one message while a command line
     * never does.
     */
    NP_OP_STATE = 5,
    /*
     * "It is running here, and it is the one you sent."  Emitted by the far
     * node the moment a program takes the state that arrived with it, which is
     * the earliest instant at which that sentence is true.
     */
    NP_OP_STARTED = 6,
};

#define NP_HDR 8u
/* One message, less the header.  The link's quantum is what caps this. */
#define NP_MAX_PAYLOAD 240u

/* Little-endian by hand, as everywhere else that puts bytes on a wire. */
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

/* What the local client is waiting for.  Only one at a time, and the server
 * task is the only thing that writes these. */
static volatile bool    s_awaiting;
static volatile bool    s_done;
static volatile int32_t s_code;
static volatile bool    s_busy_reply;

/* True while this node is running something for the other one. */
static volatile bool s_serving_run;

static ag_err_t send_msg(uint8_t op, int32_t code, const void *payload,
                         uint16_t len)
{
    uint8_t buf[NP_HDR + NP_MAX_PAYLOAD];
    if (len > NP_MAX_PAYLOAD) {
        len = NP_MAX_PAYLOAD;
    }
    buf[0] = op;
    buf[1] = 0;
    put_u16(buf + 2, len);
    put_u32(buf + 4, (uint32_t)code);
    if (len > 0 && payload != NULL) {
        memcpy(buf + NP_HDR, payload, len);
    }

    /*
     * A full window is back-pressure, not a failure.  Bounded, because a far
     * node that has gone away would otherwise hold a program's output here for
     * ever and look like a hang in the program.
     */
    for (int t = 0; t < 400; t++) {
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

/*
 * Where the far program's output goes while it runs.  Called on the program's
 * own task, which is what makes half a line arrive as half a line.
 */
static int32_t out_sink(void *ctx, const char *data, size_t len)
{
    (void)ctx;
    size_t left = len;
    while (left > 0) {
        const uint16_t chunk =
            (uint16_t)((left > NP_MAX_PAYLOAD) ? NP_MAX_PAYLOAD : left);
        if (send_msg(NP_OP_OUT, 0, data, chunk) != AG_OK) {
            return (int32_t)(len - left); /* the link went: stop quietly */
        }
        data += chunk;
        left -= chunk;
    }
    return (int32_t)len;
}

/*
 * The command line being served, and the task serving it.
 *
 * A task of its own, and this is the one thing in this file that MUST NOT be
 * simplified back: running the program on the link's reader task works only for
 * as long as the program needs nothing from the link.  The moment it reads its
 * own image off the asking node's card - which is what placement by manifest
 * asks for, since the program moves and the file does not - the reply carrying
 * those bytes is delivered by the very task that is blocked waiting for the
 * program to finish.  Nothing times out and nothing crashes; both nodes simply
 * stop, which is the worst shape a bug can have.
 */
static char           s_run_line[AG_PATH_MAX + 64];
static ag_port_task_t s_run_task;

/* Set by the far node's STARTED; only a handover ever waits on it. */
static volatile bool s_started;

/*
 * State arriving with a migrating program, collected before its RUN.
 *
 * From the heap, taken on the first STATE message that ever reaches this
 * board.  As an array it was a kilobyte of internal .bss on every board built,
 * and internal SRAM is what a TLS handshake runs out of first: `nettest`
 * failed on https, and only https, the moment this and its two neighbours
 * were added as plain arrays.
 */
static uint8_t *s_in_state;
static uint32_t s_in_state_len;

static void run_task(void *arg)
{
    (void)arg;
    char *argv[16];

    /*
     * A path under the remote-drive mountpoint means "it is on YOUR card", so
     * mount the asker before looking for it.  Doing it here rather than
     * requiring somebody to have typed `link mount` first is the difference
     * between placement that works and placement that works if you remembered.
     * Already mounted is not an error.
     */
    if (strncmp(s_run_line, AG_NODEDRV_MOUNT "/",
                sizeof(AG_NODEDRV_MOUNT)) == 0) {
        const ag_err_t m = ag_nodedrv_mount(AG_NODEDRV_MOUNT);
        if (m != AG_OK && m != -AG_EEXIST) {
            (void)send_msg(NP_OP_EXIT, (int32_t)m, NULL, 0);
            s_serving_run = false;
            ag_port_task_delete(NULL);
            return;
        }
    }

    const int argc = ag_cmdline_split(s_run_line, argv,
                                      (int)(sizeof(argv) / sizeof(argv[0])));
    if (argc <= 0) {
        (void)send_msg(NP_OP_EXIT, -AG_EINVAL, NULL, 0);
        s_serving_run = false;
        ag_port_task_delete(NULL);
        return;
    }

    /*
     * The same redirection the shell uses for `dir > file.txt`, pointed at the
     * wire instead of a file.  The console does not know what the sink is,
     * which is exactly why this needs nothing new from it.
     */
    /*
     * Whatever arrived for it, before it runs: ag_migrate_restore is called by
     * the program in its first moments, so this must be in place by then.
     */
    ag_migrate_arriving(s_in_state, s_in_state_len);
    s_in_state_len = 0;

    ag_console_redirect(out_sink, NULL);
    int32_t        code = 0;
    const ag_err_t err = ag_proc_exec(argv[0], argc, argv, 0, &code);
    ag_console_redirect(NULL, NULL);

    /*
     * A program that never started and a program that returned a number are
     * different answers, and the far end has to be able to tell: the loader's
     * error goes back as the code, negative, exactly as ag_proc_exec reports it
     * here.  The alternative - reporting 0 for "it did not run" - is the sort
     * of lie a script would act on.
     */
    (void)send_msg(NP_OP_EXIT, (err != AG_OK) ? (int32_t)err : code, NULL, 0);
    s_serving_run = false;
    ag_port_task_delete(NULL);
}

static void serve_run(const uint8_t *payload, uint16_t len)
{
    if (s_serving_run) {
        (void)send_msg(NP_OP_BUSY, 0, NULL, 0);
        return;
    }
    if (len == 0 || len >= sizeof(s_run_line)) {
        (void)send_msg(NP_OP_EXIT, -AG_EINVAL, NULL, 0);
        return;
    }
    memcpy(s_run_line, payload, len);
    s_run_line[len] = '\0';

    s_serving_run = true;
    /*
     * Below applications, deliberately.  A node running a guest program must
     * not be a node that stops answering its own user - and the wire's own
     * driver task is below applications for the same reason.
     */
    if (!ag_port_task_create(run_task, "noderun", 6144, NULL, 1,
                             AG_PORT_ANY_CORE, 0, &s_run_task)) {
        s_serving_run = false;
        (void)send_msg(NP_OP_EXIT, -AG_ENOMEM, NULL, 0);
    }
}

/*
 * One message on this node's process channel.  Runs on the link's reader task,
 * which is the only thing that reads - see argon/linkprov.h for why there is
 * exactly one.
 *
 * serve_run must NOT block here, and the reason is worth keeping: a guest
 * program may read its own image off the ASKING node's card, over this same
 * link, and the reply carrying those bytes is delivered by this task.  Blocking
 * here while that program runs deadlocks both nodes silently.  So serve_run
 * hands the command line to a task of its own and returns at once.
 */
static void on_msg(uint8_t chan, const void *data, uint16_t n, void *ctx)
{
    (void)chan;
    (void)ctx;
    const uint8_t *buf = (const uint8_t *)data;
    if (n >= NP_HDR) {
        const uint8_t  op = buf[0];
        const uint16_t len = get_u16(buf + 2);
        const int32_t  code = (int32_t)get_u32(buf + 4);
        const uint16_t have =
            (uint16_t)(((uint32_t)n - NP_HDR) < len ? ((uint32_t)n - NP_HDR)
                                                    : len);

        switch (op) {
        case NP_OP_STATE:
            /* Appended, so a state larger than one message arrives in order on
             * the reliable class and is put back together by arithmetic. */
            if (code == 0) {
                s_in_state_len = 0;
            }
            if (s_in_state == NULL) {
                s_in_state = (uint8_t *)ag_port_alloc(
                    AG_MIGRATE_MAX, AG_MEM_SLOW | AG_MEM_BYTE);
                if (s_in_state == NULL) {
                    s_in_state = (uint8_t *)ag_port_alloc(
                        AG_MIGRATE_MAX, AG_MEM_FAST | AG_MEM_BYTE);
                }
            }
            /* No buffer means no state, and a program that arrives without its
             * state starts fresh - which it is written to survive. */
            if (s_in_state != NULL && have > 0 &&
                s_in_state_len + have <= AG_MIGRATE_MAX) {
                memcpy(s_in_state + s_in_state_len, buf + NP_HDR, have);
                s_in_state_len += have;
            }
            break;
        case NP_OP_RUN:
            serve_run(buf + NP_HDR, have);
            break;
        case NP_OP_STARTED:
            /* We are the client, and we asked for a handover: this ends the
             * wait.  A program that migrated is not going to exit for us. */
            s_started = true;
            break;
        case NP_OP_OUT:
            /* We are the client: this is the far program talking. */
            if (have > 0) {
                ag_console_write((const char *)(buf + NP_HDR), have);
            }
            break;
        case NP_OP_EXIT:
            s_code = code;
            s_done = true;
            break;
        case NP_OP_BUSY:
            s_busy_reply = true;
            s_done = true;
            break;
        default:
            break;
        }
    }
}

ag_err_t ag_nodeproc_start(void)
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
    ag_log(AG_LOG_INFO, "node", "serving remote runs on %s",
           ag_linkprov_active());
    return AG_OK;
}

bool ag_nodeproc_serving(void) { return s_listening; }

ag_err_t ag_nodeproc_run(const char *cmdline, uint32_t timeout_ms,
                         int32_t *exit_code)
{
    return ag_nodeproc_run_state(cmdline, NULL, 0, timeout_ms, exit_code);
}

void ag_nodeproc_migrated(void)
{
    /*
     * Best effort on purpose.  If this message is lost the asking node reports
     * a timeout and says the program is still there, which is wrong but safe:
     * the program IS running here, and a second copy is not created by anyone
     * believing otherwise.  The opposite mistake - claiming a move that did not
     * happen - is the one that loses somebody's work.
     */
    (void)send_msg(NP_OP_STARTED, 0, NULL, 0);
}

ag_err_t ag_nodeproc_run_state(const char *cmdline, const void *state,
                               uint32_t state_len, uint32_t timeout_ms,
                               int32_t *exit_code)
{
    if (cmdline == NULL || cmdline[0] == '\0') {
        return -AG_EINVAL;
    }
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }
    if (!s_listening) {
        const ag_err_t e = ag_nodeproc_start();
        if (e != AG_OK) {
            return e;
        }
    }

    s_done = false;
    s_busy_reply = false;
    s_started = false;
    s_code = 0;
    s_awaiting = true;
    /* A handover is finished when the program is RUNNING there, not when it
     * ends: a program worth moving does not end.  Anything else still waits
     * for the exit code, which is what its caller asked for. */
    const bool want_started = (state != NULL && state_len > 0);

    /*
     * The state first, then the command line that starts the program - in that
     * order, because the far node puts what arrived in place before it runs
     * anything, and a RUN that overtook its own state would start a migrating
     * program as a fresh one.  The reliable class keeps the order.
     */
    for (uint32_t at = 0; at < state_len;) {
        const uint16_t chunk =
            (uint16_t)(((state_len - at) > NP_MAX_PAYLOAD) ? NP_MAX_PAYLOAD
                                                           : (state_len - at));
        const ag_err_t se = send_msg(NP_OP_STATE, (int32_t)at,
                                     (const uint8_t *)state + at, chunk);
        if (se != AG_OK) {
            s_awaiting = false;
            return se;
        }
        at += chunk;
    }

    const size_t len = strlen(cmdline);
    ag_err_t     err = send_msg(NP_OP_RUN, 0, cmdline,
                                (uint16_t)((len > NP_MAX_PAYLOAD)
                                               ? NP_MAX_PAYLOAD
                                               : len));
    if (err != AG_OK) {
        s_awaiting = false;
        return err;
    }

    /*
     * Waited for on the clock, not by counting sleeps.  A tick is ten
     * milliseconds whatever a one-millisecond sleep is asked for, and a timeout
     * built out of those is wrong by that factor - which this tree learned in
     * the driver next door.
     */
    const int64_t deadline = ag_port_us() + (int64_t)timeout_ms * 1000;
    while (!s_done && !(want_started && s_started)) {
        if (ag_port_us() >= deadline) {
            s_awaiting = false;
            return -AG_ETIMEDOUT;
        }
        ag_port_task_delay(ag_port_ms_to_ticks(5));
    }
    s_awaiting = false;

    if (want_started && s_started) {
        if (exit_code != NULL) {
            *exit_code = 0;
        }
        return AG_OK;
    }
    if (s_busy_reply) {
        return -AG_EBUSY;
    }
    if (exit_code != NULL) {
        *exit_code = s_code;
    }
    return AG_OK;
}
