/*
 * ArgonOS - threads of one program, on every core of every node.
 *
 * The application's half of this is ag_clu_api_t in argon/abi.h, and the
 * reasoning for the shape - messages and not shared memory, what it costs, what
 * kind of work fits - is written there rather than repeated here.  This file is
 * the kernel's half: the thread table, the mailboxes, and the seam the wire
 * plugs into.
 *
 * WHY THERE IS A SEAM AT ALL.  A thread of a job either runs here or on another
 * node, and everything the application does to it - send it something, wait for
 * it, ask who it is - has to mean the same either way, or the interface is a
 * lie and the program has to be written twice.  So this file knows about a
 * table of threads and nothing about a wire, net/nodeclu.c knows about the wire
 * and fills in ag_clu_transport_t, and a board with no link has no transport
 * and runs every thread locally.  That is also why a single board can run a
 * cluster program unchanged: with one node, "place it on the best node" has one
 * answer.
 *
 * THE OWNERSHIP RULE for everything that crosses this seam: a message buffer
 * handed to ag_clu_deliver() comes from ag_port_alloc and becomes this file's
 * to free.  One allocator on both sides of a hand-off, or the day one of them
 * changes is the day the heap is corrupted somewhere else entirely.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_CLU_H
#define ARGON_CLU_H

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How much of an argument travels with a spawn.  Small deliberately: a spawn
 * must fit one frame of the smallest wire we support, and an argument is for
 * what a thread needs to KNOW - which slice, which parameters - not for what it
 * is to work on.  Work is a message, which costs nothing extra and does not
 * make start-up wait on a transfer.
 */
#define AG_CLU_ARG_MAX 128u

/*
 * Room for the image path AS THE FAR NODE SEES IT, which is this node's path
 * with the remote-drive mount point in front of it - so longer than a local
 * path may be.  Wider than AG_PATH_MAX on purpose: a path truncated here would
 * name a different file, or none, and the failure arrives as "that node cannot
 * run your program", which sends somebody looking at the wrong board.  Longer
 * than the far VFS will accept is refused there, plainly, which is the right
 * way to fail.
 */
#define AG_CLU_IMAGE_MAX (AG_PATH_MAX + 16u)

/* Messages one mailbox holds before a sender is made to wait.  This is also the
 * credit a far node is given for it, which is what stops that node filling our
 * heap from across the wire without ever being told to stop. */
#define AG_CLU_MBOX_MSGS 4u

ag_err_t ag_clu_init(void);

/* The table api->clu points at. */
extern const ag_clu_api_t ag_clu_api_table;

/* ---------------------------------------------------------------------- */
/* The seam                                                               */
/* ---------------------------------------------------------------------- */

/*
 * What a wire must be able to do for a job.  `node` is an index into the list
 * ag_clu_api_t::nodes reports, where 0 is always this board - so a transport
 * only ever sees indices above 0.
 *
 * None of these may be called from the link's reader task, and the transport is
 * responsible for that: spawn() waits for an answer that the reader delivers.
 */
typedef struct ag_clu_transport {
    /* Start `entry` as `tid` on `node`, from the image at `image` (a path as
     * the FAR node will see it).  Waits for its answer. */
    ag_err_t (*spawn)(int32_t node, ag_clu_tid_t tid, const char *entry,
                      const void *arg, uint32_t len, const char *image,
                      uint32_t timeout_ms);

    /* One whole message; the transport fragments and reassembles it. */
    ag_err_t (*send)(int32_t node, ag_clu_tid_t from, ag_clu_tid_t to,
                     const void *buf, uint32_t len);

    /* `n` messages have been taken out of `tid`'s mailbox here, so `node` may
     * send that many more. */
    void (*credit)(int32_t node, ag_clu_tid_t tid, uint32_t n);

    /* A thread of ours has returned; tell the node that asked for it. */
    void (*exited)(int32_t node, ag_clu_tid_t tid);

    /* The job is over. */
    void (*end)(int32_t node);

    /* The nodes this transport can reach, this board NOT included. */
    int32_t (*nodes)(ag_clu_node_t *out, uint32_t max);

    /*
     * The nodes this transport has only HEARD about: the far side of a
     * neighbour, which no wire of ours reaches.
     *
     * Kept apart from `nodes` deliberately, and the reason is not tidiness.
     * Everything that PLACES work walks `nodes` - pick_node in proc/clu.c picks
     * the node with the most cores over and hands the index to spawn() - and a
     * node that cannot be addressed must never be picked: a frame carries no
     * destination, so a spawn for a node two hops away would go to whichever
     * neighbour is in hand and start the thread on the wrong board, silently.
     * So the seen ones are reported here, for an operator to look at, and stay
     * out of every decision.
     *
     * `via` receives the index of the transport's own wire each one is seen
     * through, or -1 when it is not known.  Returns how many were written.
     */
    int32_t (*peers_seen)(ag_clu_node_t *out, int32_t *via, uint32_t max);
} ag_clu_transport_t;

void ag_clu_set_transport(const ag_clu_transport_t *t);

/* ---------------------------------------------------------------------- */
/* What the transport calls back into                                     */
/* ---------------------------------------------------------------------- */

/*
 * Another node asks this one to run `entry` as `tid`, out of `image`.
 *
 * Blocking and bounded: it starts a worker process for the job if there is not
 * one yet and waits for it to declare its entry points, because until it has
 * there is no way to know whether `entry` exists.  MUST NOT be called on the
 * link's reader task - loading that image may read it back over this same link.
 */
ag_err_t ag_clu_accept_spawn(ag_clu_tid_t tid, const char *entry,
                             const void *arg, uint32_t len, const char *image);

/*
 * One whole message for a thread of ours.  Takes ownership of `msg`, which must
 * have come from ag_port_alloc - freed here whether it is delivered or not.
 */
ag_err_t ag_clu_deliver(ag_clu_tid_t from, ag_clu_tid_t to, uint8_t *msg,
                        uint32_t len);

/* `n` more messages may be sent to `tid`, which lives on another node. */
void ag_clu_add_credit(ag_clu_tid_t tid, uint32_t n);

/* A thread on another node has returned. */
void ag_clu_note_exit(ag_clu_tid_t tid);

/* The job is over, said by the node that owns it. */
void ag_clu_job_end(void);

/*
 * True once, when the job's owning process has ended and the other nodes have
 * not been told yet.
 *
 * A flag rather than a call from the teardown path, and the reason is worth
 * keeping: a process is reaped with the process table held, and putting a frame
 * on a wire from inside that lock is how a system deadlocks on a wire that is
 * busy.  The transport's own task takes this instead.
 */
bool ag_clu_take_pending_end(void);

/* Called from the process teardown path.  Cheap and non-blocking. */
void ag_clu_proc_ended(ag_pid_t pid);

/* Is a job running here, and as what.  For `ps` and for diagnostics. */
bool ag_clu_busy(void);
uint32_t ag_clu_thread_count(void);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_CLU_H */
