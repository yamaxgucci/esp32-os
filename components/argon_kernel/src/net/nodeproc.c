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
#include <argon/loader.h>
#include <argon/log.h>
#include <argon/path.h>
#include <argon/nodedrv.h>
#include <argon/migrate.h>
#include <argon/proc.h>

#include <argon/port/mem.h>

#include "proc/proc_internal.h"
#include <argon/port/task.h>
#include <argon/port/time.h>
#include <argon/port/place.h>
#include <argon/place.h>

#include "core/place_bss.h"

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
    /*
     * "Send me back what you are holding for me."
     *
     * The other half of the standby shape: a board hands its work to a small
     * neighbour and goes quiet, and when work turns up again it has to be
     * able to get it back.  Migration is always started by the process that
     * moves, so this does not carry a process or a state - it asks the far
     * node to start ITS side of an ordinary migration, homewards.
     *
     * No argument, and that is deliberate: the far node knows exactly which
     * process arrived from here, and a pid chosen on this side would be this
     * board's number for something it no longer holds.
     */
    NP_OP_RETURN = 7,
    /*
     * "It is still running here."  Every NP_ALIVE_MS while a guest runs
     * (6 Oct 2026).  The asking node waited a fixed minute for the EXIT, and a
     * program that runs longer - a 200 s tone from the CYD on the C3 - was
     * reported -60 after it, while it went on playing, and the asking shell
     * then tried to run it locally.  It waits now as long as the far node says
     * the program lives; a far node that goes quiet still ends the wait.
     */
    NP_OP_ALIVE = 8,
};

#define NP_ALIVE_MS      5000u
#define NP_ALIVE_LOST_MS (3u * NP_ALIVE_MS)

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

/*
 * WHO ASKED FOR THE PROGRAM BEING SERVED HERE.
 *
 * A run is answered for seconds after it was asked for, on the program's own
 * task, and the frames that answer it - its output, its exit, and the STARTED
 * a migrating program emits when it takes its state - are sent from there.
 * ag_linkprov_from() means nothing on those tasks: it is only valid inside the
 * receive callback.  So the asker's hash is taken in serve_run, on that
 * callback, and remembered for as long as the guest lives.  Without it the
 * answer would say "for whoever is reading", be taken by a board in between,
 * and the node that asked would wait out its whole timeout.
 *
 * One guest at a time, because one run is served at a time - NP_OP_BUSY says so
 * to anybody asking for a second - so one word is all the state this needs.
 */
static volatile uint16_t s_run_from;

/* Whose guest we are holding, for NP_OP_RETURN to send home.  Zero when none;
 * one at a time, because one run is served at a time. */
static volatile ag_pid_t s_guest_pid;

/*
 * How long the homeward state send waits for the far node to say the program
 * is running there.  The same three seconds the return task used before it was
 * removed - see the two halves in NP_OP_RETURN below.
 */
#define NP_HOME_TIMEOUT_MS 3000u

/*
 * One frame out, ON PURPOSE TO SOMEBODY.
 *
 * `dst` is ag_node_hash() of the node the frame is for.  It is a parameter
 * because this file speaks to two different nodes: an answer goes to whoever
 * asked (which can be a board beyond the neighbour), while a new conversation -
 * a run ordered by this board's own prompt - goes to the node `link use` last
 * named.  Those are the same node only when nothing is in between.
 */
static ag_err_t send_msg(uint16_t dst, uint8_t op, int32_t code,
                         const void *payload, uint16_t len)
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
            ag_linkprov_send_to(dst, NP_CHAN, AG_LINK_CONTROL, buf,
                                NP_HDR + len);
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
        if (send_msg(s_run_from, NP_OP_OUT, 0, data, chunk) != AG_OK) {
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
static char           s_run_line[AG_PATH_MAX + 64] AG_KERNEL_STATIC;
static ag_port_task_t s_run_task;

/* Set by the far node's STARTED; only a handover ever waits on it. */
static volatile bool s_started;
/* when the far node last said its guest is running (NP_OP_ALIVE), 0 never */
static volatile int64_t s_alive_us;

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

/*
 * SENDING A GUEST HOME, ON THE TASK THAT WAS ALREADY WAITING FOR IT.
 *
 * There used to be a task of this file's own here - start_return_task, 8192
 * bytes, created the moment NP_OP_RETURN arrived.  It was created at exactly
 * the one moment the board cannot afford it: the guest it has to send home is
 * still running, its 8 KB stack has taken the largest free block, and an 8 KB
 * task stack must be contiguous.  Measured on the C3 (COM8) in the harness run:
 *
 *   I (79879) proc: MIGRANRV.AXE: loading with 16800 free, largest 8704
 *   E (82544) node: no task to send the guest home
 *
 * and the guest went on counting on the C3 for the rest of the run, holding
 * the memory the next scenario needed.  The request WAS answered - that is what
 * three runs of "it is still on the neighbour" got wrong - there was simply no
 * room to answer it with.
 *
 * The task it needed was already there.  run_task below sits in ag_proc_wait
 * for this very pid for as long as the guest lives, and wakes the instant it
 * ends - which is the instant the program has handed its state over.  So the
 * ask is done where the request arrives, on the link's reader, which must not
 * wait (the state travels down the link that task reads) and must not allocate
 * (that is the whole point); the carrying is done below, on the waiting task.
 */
static ag_err_t serve_guest(ag_pid_t pid, int32_t *code)
{
    s_guest_pid = pid;
    ag_err_t e;
    while ((e = ag_proc_wait(pid, code, NP_ALIVE_MS)) == -AG_ETIMEDOUT) {
        (void)send_msg(s_run_from, NP_OP_ALIVE, 0, NULL, 0);
    }
    /*
     * Cleared BEFORE the check, so that a request that arrived while the guest
     * was ending is not lost: an ask that lands after this line finds no guest
     * here and is ignored, and an ask that landed before it is still
     * outstanding for this pid - the program was signalled, it saved its state
     * and returned, and the state is in hand waiting for exactly this send.
     */
    s_guest_pid = 0;
    if (ag_migrate_asked(pid)) {
        (void)ag_migrate_send_home(NP_HOME_TIMEOUT_MS);
    }
    return e;
}

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
            (void)send_msg(s_run_from, NP_OP_EXIT, (int32_t)m, NULL, 0);
            s_serving_run = false;
            ag_port_task_delete(NULL);
            return;
        }
    }

    const int argc = ag_cmdline_split(s_run_line, argv,
                                      (int)(sizeof(argv) / sizeof(argv[0])));
    if (argc <= 0) {
        (void)send_msg(s_run_from, NP_OP_EXIT, -AG_EINVAL, NULL, 0);
        s_serving_run = false;
        ag_port_task_delete(NULL);
        return;
    }

    /*
     * Whatever arrived for it, before it runs: ag_migrate_restore is called by
     * the program in its first moments, so this must be in place by then.
     */
    ag_migrate_arriving(s_in_state, s_in_state_len);
    s_in_state_len = 0;

    /*
     * The sink is handed to the PROGRAM, not hung on the console.
     *
     * Hung on the console it caught every writer on this board for as long as
     * the guest lived - so this node's own shell answered nobody, and for a
     * migrated program, which never ends, that was permanent.  `log -n 60`
     * typed here returned nothing at all while the guest counted happily on
     * the other end of the wire.
     *
     * Given at spawn rather than after it, because the loader logs from the
     * new task before spawn returns: installed afterwards it would already
     * have missed the lines that say why an image did not load - which is
     * exactly what the asking node needs to be told.
     */
    /*
     * BACKGROUND AND NO SESSION, and both words are load-bearing.
     *
     * A guest has no user on this board - its user is on the other one - so it
     * must not become the foreground process here and must not be bound into a
     * session slot.  Started in the foreground (which is what it was) it took
     * this board's keyboard, and the board's own shell then heard nothing for
     * as long as the guest lived: for a migrated program, for ever.
     *
     * This is the same defect as `run /b` binding into the shell's own slot,
     * found twice in two places on the same day.  A slot holds one program and
     * the keyboard belongs to its holder; anything started on somebody else's
     * behalf must stay out of that.
     */
    int32_t  code = 0;
    ag_pid_t pid = 0;
    ag_err_t err = ag_proc_spawn_sink(
        argv[0], argc, argv,
        (uint32_t)AG_SPAWN_BACKGROUND | (uint32_t)AG_SPAWN_NO_SESSION |
            AG_SPAWN_KERNEL_WAITS,
        out_sink, NULL, &pid);
    if (err == AG_OK) {
        /*
         * Remember whose guest this is, so NP_OP_RETURN has something to send
         * home.  One, because one run is served at a time (NP_OP_BUSY says so
         * to anybody asking for a second), and cleared whatever happens to it -
         * a stale pid here would answer a request to come home by migrating a
         * process that is no longer the one that arrived.
         */
        err = serve_guest(pid, &code);
    }

    /*
     * WRONG MACHINE?  THEN ASK FOR THE PROGRAM, NOT FOR THE FILE.
     *
     * The first step of the cascade in docs/plans/cluster.md 3b, and it needs
     * nothing new on the wire.  The asking node handed us a path; we can READ
     * what is there - its card is mounted - we simply cannot EXECUTE it.  So
     * we read its header, which has carried the program's name, version and
     * instruction set all along and which nothing has ever used for this, and
     * we look for OUR OWN build of that program.
     *
     * That is identity, not a filename guess: the answer does not depend on
     * what the neighbour called the file, nor on which drive letter it is on.
     *
     * Said out loud, because a program that quietly ran a different copy of
     * itself would be a mystery the first time the two builds disagreed.
     */
    if (err == -AG_ENOTSUP || code == (int32_t)-AG_ENOTSUP) {
        ag_axe_header_t theirs;
        char            mine[AG_PATH_MAX];
        /*
         * -AG_ENOTSUP FROM THE PEEK IS THE ANSWER, NOT A FAILURE.
         *
         * ag_loader_peek fills the header and only then validates it, and the
         * validation it fails here is the arch check - which is the very fact
         * that sent us looking.  Magic and ABI passed, so the name in that
         * header is as trustworthy as any other image's.
         *
         * The first draft treated every non-OK peek as "cannot read it" and
         * so refused every single time, saying so in the journal while the
         * asking board saw only -45.  Reading a file and being able to run it
         * are different questions, and this asks the first one.
         */
        ag_err_t        peeked = ag_loader_peek(argv[0], "/", &theirs);
        if (peeked == -AG_ENOTSUP) {
            peeked = AG_OK;
        }
        ag_err_t        found  = (peeked == AG_OK)
                                     ? ag_loader_find_by_name(theirs.name, mine,
                                                              sizeof(mine))
                                     : peeked;
        /*
         * SAY WHY, when the substitution does not happen.  The first draft
         * fell through in silence, so a board that could not read the header
         * and a board that simply had no build of that program looked exactly
         * alike from the other end - both just "-45" - and the only way to
         * tell them apart was to guess.
         */
        if (peeked != AG_OK) {
            ag_log(AG_LOG_WARN, "node",
                   "%s is for another processor and I cannot read its header "
                   "to learn which program it is: %s",
                   argv[0], ag_loader_why(peeked));
        } else if (found != AG_OK) {
            ag_log(AG_LOG_WARN, "node",
                   "%s is for another processor and I have no build of %s "
                   "that I could run instead",
                   argv[0], theirs.name);
        }
        if (found == AG_OK) {
            ag_log(AG_LOG_INFO, "node",
                   "%s is for another processor; running my own %s (%s %s)",
                   argv[0], mine, theirs.name, theirs.version);
            argv[0] = mine;
            err = ag_proc_spawn_sink(
                argv[0], argc, argv,
                (uint32_t)AG_SPAWN_BACKGROUND | (uint32_t)AG_SPAWN_NO_SESSION |
                    AG_SPAWN_KERNEL_WAITS,
                out_sink, NULL, &pid);
            if (err == AG_OK) {
                err = serve_guest(pid, &code);
            }
        }
    }

    /*
     * A program that never started and a program that returned a number are
     * different answers, and the far end has to be able to tell: the loader's
     * error goes back as the code, negative, exactly as ag_proc_exec reports it
     * here.  The alternative - reporting 0 for "it did not run" - is the sort
     * of lie a script would act on.
     */
    (void)send_msg(s_run_from, NP_OP_EXIT, (err != AG_OK) ? (int32_t)err : code,
                   NULL, 0);
    s_serving_run = false;
    ag_port_task_delete(NULL);
}

static void serve_run(const uint8_t *payload, uint16_t len)
{
    /*
     * THE ASKER, taken here because this is the receive callback and the last
     * moment ag_linkprov_from() means anything - the run itself is served on a
     * task of its own and answers from there.
     *
     * The three refusals below answer THIS asker, so they use `from` directly
     * rather than s_run_from: with a guest already running (NP_OP_BUSY), what
     * is in s_run_from belongs to that guest and its own asker, and answering
     * the wrong node "busy" would leave the right one waiting.
     */
    const uint16_t from = ag_linkprov_from();

    if (s_serving_run) {
        (void)send_msg(from, NP_OP_BUSY, 0, NULL, 0);
        return;
    }
    if (len == 0 || len >= sizeof(s_run_line)) {
        (void)send_msg(from, NP_OP_EXIT, -AG_EINVAL, NULL, 0);
        return;
    }
    memcpy(s_run_line, payload, len);
    s_run_line[len] = '\0';

    s_run_from = from;
    s_serving_run = true;
    /*
     * Below applications, deliberately.  A node running a guest program must
     * not be a node that stops answering its own user - and the wire's own
     * driver task is below applications for the same reason.
     */
    /* Internal stack if it can be had, PSRAM if not: after the merge of
     * 25 Sep 2026 an S3 with a 192 KB code arena had 5 KB internal left and
     * refused every remote run with -12. */
    if (!ag_place_task("cluster.tasks", run_task, "noderun", 6144, NULL, 1,
                       AG_PORT_ANY_CORE, (void **)&s_run_task, NULL)) {
        s_serving_run = false;
        (void)send_msg(from, NP_OP_EXIT, -AG_ENOMEM, NULL, 0);
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
        case NP_OP_ALIVE:
            s_alive_us = ag_port_us();
            break;
        case NP_OP_EXIT:
            s_code = code;
            s_done = true;
            break;
        case NP_OP_BUSY:
            s_busy_reply = true;
            s_done = true;
            break;
        case NP_OP_RETURN:
            /*
             * The board that sent us this work wants it back.
             *
             * ASKED HERE, CARRIED BY THE TASK THAT IS ALREADY WAITING - see the
             * note over serve_guest for the measurement that made this the
             * shape.  This task is the link's reader: it must not wait for the
             * program (the state would travel down the link this task reads,
             * so waiting here is waiting for itself) and it must not create a
             * task to wait on its behalf (a resident guest leaves no room for
             * one, which is exactly how this failed).
             *
             * ag_migrate_ask only signals; the guest agrees at its next
             * checkpoint, ends, and serve_guest above carries the state home
             * the moment it does.  A request that finds no guest here is
             * ignored, which is the right answer to "send back what you have
             * of mine" when it has nothing.
             */
            if (s_guest_pid != 0) {
                const ag_err_t e = ag_migrate_ask(s_guest_pid);
                if (e != AG_OK) {
                    ag_log(AG_LOG_WARN, "node",
                           "asked for pid %u back: %d - it stays here",
                           (unsigned)s_guest_pid, (int)e);
                }
            }
            break;
        default:
            break;
        }
    }
}

ag_err_t ag_nodeproc_ask_return(void)
{
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }
    /*
     * Asked, not awaited.  The far node answers by starting a migration
     * towards us, and that arrives the ordinary way - as a RUN with a state -
     * so there is nothing here to wait for.  A board that is holding nothing
     * of ours simply does nothing, which is the right answer to "send back
     * what you have of mine" when it has none.
     *
     * A new conversation, so it goes to the node this board is talking to: this
     * is the one frame here that asks rather than answers, and `link use` is
     * what names who is asked.
     */
    return send_msg(ag_linkprov_target(), NP_OP_RETURN, 0, NULL, 0);
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
     *
     * Answered to the node that ordered the run, which is the board the state
     * just came from - not to the selected target, which is a different board
     * whenever this program was sent here by a board beyond the neighbour.
     */
    (void)send_msg(s_run_from, NP_OP_STARTED, 0, NULL, 0);
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
    s_alive_us = 0;
    s_code = 0;
    s_awaiting = true;
    /*
     * A NEW conversation with the node this board is talking to, so every frame
     * below carries that node as its destination - `link use <name>` is what
     * puts a board two hops away in there, and the relay in between is what
     * makes the hop from the neighbour onwards.
     */
    const uint16_t dst = ag_linkprov_target();
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
        const ag_err_t se = send_msg(dst, NP_OP_STATE, (int32_t)at,
                                     (const uint8_t *)state + at, chunk);
        if (se != AG_OK) {
            s_awaiting = false;
            return se;
        }
        at += chunk;
    }

    const size_t len = strlen(cmdline);
    ag_err_t     err = send_msg(dst, NP_OP_RUN, 0, cmdline,
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
        /* the deadline is for starting; a program the far node says is
         * running is waited for as long as it says so (NP_OP_ALIVE) */
        const int64_t now = ag_port_us();
        const bool    alive =
            s_alive_us != 0 &&
            now - s_alive_us < (int64_t)NP_ALIVE_LOST_MS * 1000;
        if (now >= deadline && !alive) {
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
    /*
     * A HANDOVER THAT ENDED INSTEAD OF STARTING IS A FAILED HANDOVER.
     *
     * For a plain remote run, ending IS the answer and its code belongs to the
     * caller - that is the branch below.  For a migration it is the opposite:
     * the program was supposed to be running over there, and what came back
     * instead was an exit.  Reporting AG_OK for that said "moved" about a
     * program that had ceased to exist: it had already ended here, it never
     * started there, and both boards were satisfied.
     *
     * Seen on the bench the day the standby watcher started asking for its
     * work back by itself.  The far board was out of memory - "no memory for a
     * 16384 byte task stack" - and the near one logged "moved to node0 with 12
     * bytes of state" in the same second.
     */
    if (want_started) {
        if (exit_code != NULL) {
            *exit_code = s_code;
        }
        /*
         * The far node's own reason when it gave one - a failed load arrives
         * as its negative error, which is the case worth seeing (-12 when the
         * board is full).  A non-negative code means the program ran and
         * exited without ever taking the state it was sent, so it is not the
         * program we handed over: -AG_EINTR, the handover was cut short.
         */
        return (s_code < 0) ? (ag_err_t)s_code : -AG_EINTR;
    }
    if (exit_code != NULL) {
        *exit_code = s_code;
    }
    return AG_OK;
}
