/*
 * ArgonOS - threads of one program across the nodes.  See argon/clu.h and
 * ag_clu_api_t in argon/abi.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/clu.h>

#include <stdio.h>
#include <string.h>

#include <argon/kernel.h>
#include <argon/log.h>
#include <argon/nodedrv.h>
#include <argon/nodename.h>
#include <argon/proc.h>

#include <argon/port/mem.h>
#include <argon/port/sync.h>
#include <argon/port/task.h>
#include <argon/port/time.h>
#include <argon/port/place.h>

#include "core/place_bss.h"

#include "proc_internal.h"

/*
 * How long a spawn on another node may take before it is called failed.
 *
 * Generous, and the reason is not caution: the far node has to LOAD the image,
 * and on the first spawn of a job that image is read a few hundred bytes at a
 * time off this node's card, over the same wire the answer will come back on.
 * A timeout sized for a message is a timeout that fails every first spawn.
 */
#define CLU_SPAWN_MS 20000u

/* How long a worker process is given to declare its entry points. */
#define CLU_DECLARE_MS 20000u

/* Bytes of undelivered messages this node will hold for the whole job. */
#define CLU_TOTAL_BYTES (64u * 1024u)

typedef struct clu_msg {
    struct clu_msg *next;
    ag_clu_tid_t    from;
    uint32_t        len;
    uint8_t        *data;
} clu_msg_t;

typedef struct {
    bool    used;
    bool    local;
    bool    finished;
    int32_t node; /* 0 is this board */

    char      entry[AG_CLU_ENTRY_MAX];
    ag_clu_fn fn; /* local only */

    uint8_t  arg[AG_CLU_ARG_MAX];
    uint32_t arglen;

    ag_port_task_t task; /* local only: what self() matches on */

    clu_msg_t    *head;
    clu_msg_t    *tail;
    uint32_t      count;
    uint32_t      bytes;
    ag_port_sem_t msgs;

    /* Remote only: how many more messages we may put in its mailbox. */
    uint32_t credits;
} clu_thread_t;

enum { CLU_NONE = 0, CLU_MASTER, CLU_WORKER };

/*
 * The thread table, and it is a POINTER rather than an array on purpose.
 *
 * As an array it was three kilobytes of internal SRAM on every board, whether
 * or not that board would ever run a cluster job - and internal SRAM is the one
 * memory this system is always short of.  It cost a gate: with six kilobytes
 * less of it, `nettest`'s TLS fetch failed with "no mem for receive buffer",
 * because a TLS handshake wants tens of kilobytes and the Ethernet driver could
 * no longer get its receive buffers.  The parent commit passed the same test
 * with 96 KB free; this one had 90.
 *
 * A job is not a moment when asking for memory is unreasonable - it already
 * loads an image and starts a process - so the table is taken when a job starts
 * and given back when it ends.  A board that never runs one pays nothing.
 */
static clu_thread_t   *s_threads;
static ag_port_mutex_t s_lock;
static uint8_t         s_role;
static ag_pid_t        s_job_pid;    /* whose job this is, or who serves it */
static uint32_t        s_held_bytes; /* over every mailbox here */
static volatile bool   s_ending;
static volatile bool   s_pending_end;

/* The entry points the program declared, and which process declared them. */
static const ag_clu_entry_t *s_tab;
static uint32_t              s_tab_n;
static ag_pid_t              s_tab_pid;

/* Worker side: the process started to serve another node's job. */
static ag_pid_t s_worker_pid;
static char     s_worker_image[AG_CLU_IMAGE_MAX] AG_KERNEL_STATIC;

static const ag_clu_transport_t *s_tr;

/* ---------------------------------------------------------------------- */

static void lock(void)
{
    if (s_lock != NULL) {
        ag_port_mutex_take_recursive(s_lock, AG_PORT_FOREVER);
    }
}

static void unlock(void)
{
    if (s_lock != NULL) {
        ag_port_mutex_give_recursive(s_lock);
    }
}

static uint32_t now_ms(void) { return (uint32_t)(ag_port_us() / 1000); }

/* PSRAM first, SRAM only if there is no PSRAM: a message may be four kilobytes
 * and internal memory is what everything else on this board is short of. */
static void *msg_alloc(uint32_t bytes)
{
    void *p = ag_port_alloc(bytes, AG_MEM_SLOW | AG_MEM_BYTE);
    if (p == NULL) {
        p = ag_port_alloc(bytes, AG_MEM_FAST | AG_MEM_BYTE);
    }
    return p;
}

/*
 * Takes the table if this node has not got one.  Called under the lock, from
 * the two places a job can begin: a master's first spawn, and a worker being
 * asked for its first thread.
 */
static bool table_take(void)
{
    if (s_threads != NULL) {
        return true;
    }
    const size_t bytes = AG_CLU_THREADS_MAX * sizeof(clu_thread_t);
    s_threads = (clu_thread_t *)ag_port_alloc(bytes, AG_MEM_FAST | AG_MEM_BYTE);
    if (s_threads == NULL) {
        /* PSRAM will do: this is touched per message, not per sample. */
        s_threads = (clu_thread_t *)ag_port_alloc(bytes,
                                                  AG_MEM_SLOW | AG_MEM_BYTE);
    }
    if (s_threads == NULL) {
        return false;
    }
    memset(s_threads, 0, bytes);
    return true;
}

static clu_thread_t *by_tid(ag_clu_tid_t tid)
{
    if (s_threads == NULL || tid < 0 || (uint32_t)tid >= AG_CLU_THREADS_MAX) {
        return NULL;
    }
    return s_threads[tid].used ? &s_threads[tid] : NULL;
}

/* The calling task's thread record, or NULL if it is not one of ours. */
static clu_thread_t *me(void)
{
    if (s_threads == NULL) {
        return NULL;
    }
    const ag_port_task_t self = ag_port_task_self();
    for (uint32_t i = 0; i < AG_CLU_THREADS_MAX; i++) {
        if (s_threads[i].used && s_threads[i].local &&
            s_threads[i].task == self) {
            return &s_threads[i];
        }
    }
    return NULL;
}

static ag_clu_tid_t tid_of(const clu_thread_t *t)
{
    return (t == NULL) ? -1 : (ag_clu_tid_t)(t - s_threads);
}

static void mbox_clear(clu_thread_t *t)
{
    clu_msg_t *m = t->head;
    while (m != NULL) {
        clu_msg_t *next = m->next;
        if (s_held_bytes >= m->len) {
            s_held_bytes -= m->len;
        }
        ag_port_free(m->data);
        ag_port_free(m);
        m = next;
    }
    t->head = NULL;
    t->tail = NULL;
    t->count = 0;
    t->bytes = 0;
}

static void slot_release(clu_thread_t *t)
{
    mbox_clear(t);
    /*
     * The semaphore is NOT freed while a thread of ours might still be inside
     * it.  A job can end while a thread is blocked in recv() - that is the
     * normal shape of being cut off, not an edge case - and freeing what it is
     * waiting on is a use-after-free in the scheduler, which does not report a
     * fault, it makes the machine stop making sense.
     *
     * So it is leaked, deliberately and loudly.  The thread belongs to a
     * process; the process ending deletes it, and a few dozen bytes lost at
     * that point is enormously better than the alternative.  This tree has made
     * the opposite trade before and paid for it.
     */
    if (t->msgs != NULL) {
        if (t->local && !t->finished && t->task != NULL) {
            ag_log(AG_LOG_WARN, "clu",
                   "thread %d is still in the kernel; its mailbox is left "
                   "behind rather than freed under it",
                   (int)(t - s_threads));
        } else {
            ag_port_sem_free(t->msgs);
        }
    }
    memset(t, 0, sizeof(*t));
}

/*
 * Ends the job here: every mailbox, every record, the role.  Local threads are
 * NOT killed - they belong to a process, and the process ending is what takes
 * them with it.  Reaching in to delete a task that is running inside the
 * application's own code is how this project has hung boards before.
 */
static void job_clear(void)
{
    if (s_threads != NULL) {
        for (uint32_t i = 0; i < AG_CLU_THREADS_MAX; i++) {
            if (s_threads[i].used) {
                slot_release(&s_threads[i]);
            }
        }
        ag_port_free(s_threads);
        s_threads = NULL;
    }
    s_held_bytes = 0;
    s_role = CLU_NONE;
    s_job_pid = AG_PID_KERNEL;
    s_worker_pid = AG_PID_KERNEL;
    s_worker_image[0] = '\0';
}

/* ---------------------------------------------------------------------- */
/* Nodes                                                                   */
/* ---------------------------------------------------------------------- */

static int32_t node_list(ag_clu_node_t *out, uint32_t max)
{
    const ag_sysinfo_t *si = ag_sysinfo();
    ag_clu_node_t       here;

    memset(&here, 0, sizeof(here));
    snprintf(here.name, sizeof(here.name), "%s", ag_node_name());
    here.cores = (si != NULL && si->cpu_cores > 0) ? si->cpu_cores : 1;
    here.here = 1;
    here.cpu_hz = (si != NULL) ? si->cpu_hz : 0;

    lock();
    for (uint32_t i = 0; s_threads != NULL && i < AG_CLU_THREADS_MAX; i++) {
        if (s_threads[i].used && s_threads[i].node == 0) {
            here.threads++;
        }
    }
    unlock();

    int32_t n = 1;
    if (out != NULL && max >= 1) {
        out[0] = here;
    }

    if (s_tr != NULL && s_tr->nodes != NULL) {
        ag_clu_node_t far_nodes[AG_CLU_THREADS_MAX];
        const int32_t got =
            s_tr->nodes(far_nodes, (uint32_t)(sizeof(far_nodes) /
                                              sizeof(far_nodes[0])));
        for (int32_t i = 0; i < got; i++) {
            const int32_t idx = n++;
            if (out == NULL || (uint32_t)idx >= max) {
                continue;
            }
            out[idx] = far_nodes[i];
            out[idx].here = 0;
            out[idx].threads = 0;
            lock();
            for (uint32_t j = 0; s_threads != NULL && j < AG_CLU_THREADS_MAX;
                 j++) {
                if (s_threads[j].used && s_threads[j].node == idx) {
                    out[idx].threads++;
                }
            }
            unlock();
        }
    }
    return n;
}

static int32_t api_nodes(ag_clu_node_t *out, uint32_t max)
{
    return node_list(out, (out == NULL) ? 0 : max);
}

/* Where AG_CLU_ANY puts the next thread: the board with the most core left
 * over.  Local wins a tie, because a thread that stays here costs no wire. */
static int32_t pick_node(void)
{
    ag_clu_node_t list[8];
    const int32_t n =
        node_list(list, (uint32_t)(sizeof(list) / sizeof(list[0])));
    int32_t best = 0;
    int32_t best_free = -0x7fffffff;

    for (int32_t i = 0; i < n && i < (int32_t)(sizeof(list) / sizeof(list[0]));
         i++) {
        const int32_t cores = (list[i].cores > 0) ? list[i].cores : 1;
        const int32_t left = cores - (int32_t)list[i].threads;
        if (left > best_free) {
            best_free = left;
            best = i;
        }
    }
    return best;
}

/* ---------------------------------------------------------------------- */
/* Threads                                                                 */
/* ---------------------------------------------------------------------- */

static ag_clu_fn entry_by_name(const char *name)
{
    if (name == NULL || s_tab == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < s_tab_n; i++) {
        if (s_tab[i].name != NULL && strcmp(s_tab[i].name, name) == 0) {
            return s_tab[i].fn;
        }
    }
    return NULL;
}

/* A free slot, or -1.  tid 0 is the master's and is never handed out here. */
static ag_clu_tid_t free_tid(void)
{
    for (uint32_t i = 1; s_threads != NULL && i < AG_CLU_THREADS_MAX; i++) {
        if (!s_threads[i].used) {
            return (ag_clu_tid_t)i;
        }
    }
    return -1;
}

static bool slot_init(clu_thread_t *t)
{
    t->msgs = ag_port_sem_new_counting(AG_CLU_MBOX_MSGS + 2u, 0);
    if (t->msgs == NULL) {
        return false;
    }
    t->used = true;
    t->credits = AG_CLU_MBOX_MSGS;
    return true;
}

static void thread_body(void *arg)
{
    const ag_clu_tid_t tid = (ag_clu_tid_t)(intptr_t)arg;

    lock();
    clu_thread_t *t = by_tid(tid);
    if (t == NULL) {
        unlock();
        return;
    }
    t->task = ag_port_task_self();
    const ag_clu_fn fn = t->fn;
    const int32_t   owner = t->node;
    unlock();

    if (fn != NULL) {
        fn(tid, t->arg, t->arglen);
    }

    lock();
    t = by_tid(tid);
    if (t != NULL) {
        t->finished = true;
    }
    const bool worker = (s_role == CLU_WORKER);
    unlock();

    /*
     * A worker's thread tells the node that asked for it.  A master's local
     * thread tells nobody: join() reads the flag next door.
     */
    if (worker && s_tr != NULL && s_tr->exited != NULL) {
        s_tr->exited((owner == 0) ? 1 : owner, tid);
    }
}

/*
 * Starts `tid` as a thread of process `pid`.  Called on the master's own task
 * for a local spawn, and on the transport's task for one asked for from
 * outside - which is why the process is named rather than assumed to be the
 * caller's.
 */
static ag_err_t start_local(ag_pid_t pid, ag_clu_tid_t tid)
{
    if (ag_thread_create_in(pid, thread_body, (void *)(intptr_t)tid, "clu",
                            0, 0, AG_THREAD_ANY_CORE) == NULL) {
        return -AG_ENOMEM;
    }
    return AG_OK;
}

/* ---------------------------------------------------------------------- */
/* The application's calls                                                 */
/* ---------------------------------------------------------------------- */

static bool api_start(const ag_clu_entry_t *tab, uint32_t n)
{
    const ag_pid_t pid = ag_proc_self();

    lock();
    s_tab = tab;
    s_tab_n = (tab != NULL) ? n : 0;
    s_tab_pid = pid;
    const bool worker = (pid != AG_PID_KERNEL && pid == s_worker_pid);
    if (worker) {
        s_role = CLU_WORKER;
        s_job_pid = pid;
    }
    unlock();

    if (!worker) {
        return false;
    }

    /*
     * Serving.  There is nothing to do on this task - the threads are created
     * by the transport as they are asked for, and they run in this process -
     * so it waits, and waiting is the point: the process must stay alive or its
     * image is unmapped underneath the threads running in it.
     */
    ag_log(AG_LOG_INFO, "clu", "serving a job for the other node");
    while (!s_ending) {
        ag_port_task_delay(ag_port_ms_to_ticks(20));
    }
    ag_log(AG_LOG_INFO, "clu", "job over");

    /*
     * Give the threads a moment to come out on their own before the table is
     * taken apart.  They are in recv() with a timeout, so they WILL come out;
     * waiting here is what makes the ordinary end of a job tidy instead of
     * merely safe.
     */
    const uint32_t deadline = now_ms() + 3000u;
    for (;;) {
        lock();
        uint32_t running = 0;
        for (uint32_t i = 0; s_threads != NULL && i < AG_CLU_THREADS_MAX; i++) {
            if (s_threads[i].used && s_threads[i].local &&
                !s_threads[i].finished && s_threads[i].task != NULL) {
                running++;
            }
        }
        unlock();
        if (running == 0 || now_ms() >= deadline) {
            break;
        }
        ag_port_task_delay(ag_port_ms_to_ticks(10));
    }

    lock();
    job_clear();
    s_ending = false;
    unlock();
    return true;
}

static ag_clu_tid_t api_self(void)
{
    lock();
    const clu_thread_t *t = me();
    const ag_clu_tid_t  tid = (t != NULL) ? tid_of(t) : -1;
    unlock();
    return tid;
}

/*
 * Claims the job for the calling process and makes it tid 0.  The first spawn
 * does this rather than start(), so that a program which declares entry points
 * and never spawns costs nothing and never refuses anybody else the job.
 */
static ag_err_t claim(void)
{
    const ag_pid_t pid = ag_proc_self();

    if (s_role == CLU_MASTER && s_job_pid == pid) {
        return AG_OK;
    }
    if (s_role != CLU_NONE) {
        return -AG_EBUSY;
    }
    if (s_tab_pid != pid) {
        return -AG_EPERM; /* start() was never called, so no entry points */
    }
    if (!table_take()) {
        return -AG_ENOMEM;
    }

    memset(&s_threads[0], 0, sizeof(s_threads[0]));
    if (!slot_init(&s_threads[0])) {
        return -AG_ENOMEM;
    }
    s_threads[0].local = true;
    s_threads[0].node = 0;
    s_threads[0].task = ag_port_task_self();
    snprintf(s_threads[0].entry, sizeof(s_threads[0].entry), "%s", "main");

    s_role = CLU_MASTER;
    s_job_pid = pid;
    s_ending = false;
    return AG_OK;
}

/*
 * The master's image, as the FAR node will have to open it.
 *
 * `/node` there is this node's root, so a path of ours gets that prefix - the
 * same recipe the shell uses when it places a program by manifest.  The one
 * exception is a master that is itself running off the neighbour's card: its
 * path already starts with the mount point, and what it names is a file that is
 * local to the far node, so the prefix comes off instead of going on.
 */
static ag_err_t master_image(char *out, size_t len)
{
    const proc_t *p = ag_proc_current();
    if (p == NULL || p->path[0] == '\0') {
        return -AG_ENOENT;
    }
    const size_t mlen = strlen(AG_NODEDRV_MOUNT);
    if (strncmp(p->path, AG_NODEDRV_MOUNT "/", mlen + 1u) == 0) {
        const char *rel = p->path + mlen;
        if (strlen(rel) + 1u > len) {
            return -AG_ERANGE;
        }
        memcpy(out, rel, strlen(rel) + 1u);
        return AG_OK;
    }

    /*
     * Checked rather than truncated.  A path cut short here would name a
     * DIFFERENT file on the far node, or none, and the failure would arrive as
     * "that node cannot run your program" - a sentence that sends somebody
     * looking at the wrong board.
     */
    const size_t plen = strlen(p->path);
    if (mlen + plen + 1u > len) {
        return -AG_ERANGE;
    }
    memcpy(out, AG_NODEDRV_MOUNT, mlen);
    memcpy(out + mlen, p->path, plen + 1u);
    return AG_OK;
}

static ag_clu_tid_t api_spawn(int32_t node, const char *entry, const void *arg,
                              uint32_t len)
{
    if (entry == NULL || entry[0] == '\0' || len > AG_CLU_ARG_MAX) {
        return -AG_EINVAL;
    }

    lock();
    /*
     * Only the master spawns.  Before the first one there is no master, so a
     * caller that has declared entry points is allowed to become it; a thread
     * serving somebody else's job never is.
     */
    if (s_role == CLU_WORKER ||
        (s_role == CLU_MASTER && me() != &s_threads[0])) {
        unlock();
        return -AG_EPERM;
    }
    ag_err_t e = claim();
    if (e != AG_OK) {
        unlock();
        return e;
    }

    if (node == AG_CLU_ANY) {
        node = pick_node();
    } else if (node == AG_CLU_LOCAL) {
        node = 0;
    }
    if (node < 0 || node >= node_list(NULL, 0)) {
        unlock();
        return -AG_ENODEV;
    }

    const ag_clu_tid_t tid = free_tid();
    if (tid < 0) {
        unlock();
        return -AG_ENFILE;
    }
    clu_thread_t *t = &s_threads[tid];
    memset(t, 0, sizeof(*t));
    if (!slot_init(t)) {
        unlock();
        return -AG_ENOMEM;
    }
    t->node = node;
    t->local = (node == 0);
    snprintf(t->entry, sizeof(t->entry), "%s", entry);
    if (arg != NULL && len > 0) {
        memcpy(t->arg, arg, len);
        t->arglen = len;
    }
    if (t->local) {
        t->fn = entry_by_name(entry);
        if (t->fn == NULL) {
            slot_release(t);
            unlock();
            return -AG_ENOENT;
        }
    }
    const ag_pid_t pid = s_job_pid;
    unlock();

    if (node == 0) {
        e = start_local(pid, tid);
    } else if (s_tr == NULL || s_tr->spawn == NULL) {
        e = -AG_ENODEV;
    } else {
        char image[AG_CLU_IMAGE_MAX];
        e = master_image(image, sizeof(image));
        if (e == AG_OK) {
            /* Outside the lock, and it must be: this waits for another board
             * to load an image over the wire, and everything else here would
             * be stopped behind it. */
            e = s_tr->spawn(node, tid, entry, t->arg, t->arglen, image,
                            CLU_SPAWN_MS);
        }
    }

    if (e != AG_OK) {
        lock();
        slot_release(t);
        unlock();
        return e;
    }
    return tid;
}

static ag_err_t mbox_put(clu_thread_t *t, ag_clu_tid_t from, uint8_t *data,
                         uint32_t len)
{
    if (t->count >= AG_CLU_MBOX_MSGS || s_held_bytes + len > CLU_TOTAL_BYTES) {
        return -AG_EAGAIN;
    }
    clu_msg_t *m =
        (clu_msg_t *)ag_port_alloc(sizeof(*m), AG_MEM_FAST | AG_MEM_BYTE);
    if (m == NULL) {
        return -AG_ENOMEM;
    }
    m->next = NULL;
    m->from = from;
    m->len = len;
    m->data = data;
    if (t->tail != NULL) {
        t->tail->next = m;
    } else {
        t->head = m;
    }
    t->tail = m;
    t->count++;
    t->bytes += len;
    s_held_bytes += len;
    ag_port_sem_give(t->msgs);
    return AG_OK;
}

static ag_err_t api_send(ag_clu_tid_t dst, const void *buf, uint32_t len,
                         uint32_t timeout_ms)
{
    if (buf == NULL || len == 0 || len > AG_CLU_MSG_MAX) {
        return -AG_ERANGE;
    }

    lock();
    const clu_thread_t *self = me();
    const ag_clu_tid_t  from = (self != NULL) ? tid_of(self) : AG_CLU_MASTER;
    unlock();

    const uint32_t deadline = now_ms() + timeout_ms;
    for (;;) {
        lock();
        clu_thread_t *t = by_tid(dst);
        if (t == NULL || t->finished) {
            unlock();
            return -AG_ENOENT;
        }

        if (t->local) {
            /*
             * A copy, because the sender's buffer is its own the moment this
             * returns.  Taken before the mailbox is touched so that a failure
             * to allocate is not a half-delivered message.
             */
            if (t->count < AG_CLU_MBOX_MSGS &&
                s_held_bytes + len <= CLU_TOTAL_BYTES) {
                uint8_t *copy = (uint8_t *)msg_alloc(len);
                if (copy == NULL) {
                    unlock();
                    return -AG_ENOMEM;
                }
                memcpy(copy, buf, len);
                const ag_err_t e = mbox_put(t, from, copy, len);
                unlock();
                if (e != AG_OK) {
                    ag_port_free(copy);
                    return e;
                }
                return AG_OK;
            }
            unlock();
        } else {
            if (t->credits > 0) {
                t->credits--;
                const int32_t node = t->node;
                unlock();
                const ag_err_t e =
                    (s_tr != NULL && s_tr->send != NULL)
                        ? s_tr->send(node, from, dst, buf, len)
                        : -AG_ENODEV;
                if (e != AG_OK) {
                    lock();
                    clu_thread_t *back = by_tid(dst);
                    if (back != NULL) {
                        back->credits++;
                    }
                    unlock();
                }
                return e;
            }
            unlock();
        }

        /*
         * Full.  That is back-pressure and not a failure: the far end has not
         * read what it already has, and the cure is to wait, not to drop.
         */
        if (now_ms() >= deadline) {
            return -AG_ETIMEDOUT;
        }
        ag_port_task_delay(ag_port_ms_to_ticks(2));
    }
}

static int32_t api_recv(ag_clu_tid_t *from, void *buf, uint32_t cap,
                        uint32_t timeout_ms)
{
    lock();
    clu_thread_t *t = me();
    unlock();
    if (t == NULL) {
        return -AG_EPERM; /* not a thread of a job */
    }

    const uint32_t deadline = now_ms() + timeout_ms;
    for (;;) {
        const uint32_t now = now_ms();
        const uint32_t wait = (now >= deadline) ? 0u : (deadline - now);
        if (!ag_port_sem_take(t->msgs, ag_port_ms_to_ticks(wait))) {
            return -AG_EAGAIN;
        }

        lock();
        clu_msg_t *m = t->head;
        if (m == NULL) {
            unlock();
            if (now_ms() >= deadline) {
                return -AG_EAGAIN;
            }
            continue;
        }
        if (m->len > cap) {
            /*
             * Left where it is, and the token put back: a message too big for
             * the buffer offered is not a message to throw away.  The caller
             * can ask again with room for it - which it can always find, since
             * AG_CLU_MSG_MAX is the most it can ever be.
             */
            unlock();
            ag_port_sem_give(t->msgs);
            return -AG_ERANGE;
        }
        t->head = m->next;
        if (t->head == NULL) {
            t->tail = NULL;
        }
        t->count--;
        t->bytes -= m->len;
        if (s_held_bytes >= m->len) {
            s_held_bytes -= m->len;
        }
        const clu_thread_t *src = by_tid(m->from);
        const int32_t       src_node = (src != NULL) ? src->node : 0;
        const ag_clu_tid_t  mine = tid_of(t);
        unlock();

        memcpy(buf, m->data, m->len);
        const uint32_t     len = m->len;
        const ag_clu_tid_t who = m->from;
        ag_port_free(m->data);
        ag_port_free(m);

        /* Room again: tell the node that was holding the credit. */
        if (src_node != 0 && s_tr != NULL && s_tr->credit != NULL) {
            s_tr->credit(src_node, mine, 1);
        }
        if (from != NULL) {
            *from = who;
        }
        return (int32_t)len;
    }
}

static ag_err_t api_join(ag_clu_tid_t tid, uint32_t timeout_ms)
{
    const uint32_t deadline = now_ms() + timeout_ms;
    for (;;) {
        lock();
        const clu_thread_t *t = by_tid(tid);
        const bool          gone = (t == NULL);
        const bool          done = (t != NULL && t->finished);
        unlock();

        if (gone) {
            return -AG_ENOENT;
        }
        if (done) {
            return AG_OK;
        }
        if (now_ms() >= deadline) {
            return -AG_ETIMEDOUT;
        }
        ag_port_task_delay(ag_port_ms_to_ticks(5));
    }
}

static void api_done(void)
{
    lock();
    const bool master = (s_role == CLU_MASTER && s_job_pid == ag_proc_self());
    unlock();
    if (!master) {
        return;
    }

    /* Told before the table is forgotten: a node that is never told keeps a
     * process alive for a job that no longer exists. */
    if (s_tr != NULL && s_tr->end != NULL) {
        const int32_t n = node_list(NULL, 0);
        for (int32_t i = 1; i < n; i++) {
            s_tr->end(i);
        }
    }

    lock();
    job_clear();
    unlock();
}

/*
 * The boards beyond our neighbours, as the transport heard them.  A wrapper
 * rather than a pointer straight at the transport's, because a transport is
 * optional - a kernel with no wire has none - and "nobody told me about any"
 * and "there is nobody to tell me" have the same answer.
 */
static int32_t api_peers_seen(ag_clu_node_t *out, int32_t *via, uint32_t max)
{
    if (s_tr == NULL || s_tr->peers_seen == NULL) {
        return 0;
    }
    return s_tr->peers_seen(out, via, max);
}

const ag_clu_api_t ag_clu_api_table = {
    .size = sizeof(ag_clu_api_t),
    .nodes = api_nodes,
    .start = api_start,
    .spawn = api_spawn,
    .send = api_send,
    .recv = api_recv,
    .join = api_join,
    .self = api_self,
    .done = api_done,
    .peers_seen = api_peers_seen,
};

/* ---------------------------------------------------------------------- */
/* What the wire calls                                                     */
/* ---------------------------------------------------------------------- */

void ag_clu_set_transport(const ag_clu_transport_t *t) { s_tr = t; }

ag_err_t ag_clu_accept_spawn(ag_clu_tid_t tid, const char *entry,
                             const void *arg, uint32_t len, const char *image)
{
    if (entry == NULL || image == NULL || tid < 0 ||
        (uint32_t)tid >= AG_CLU_THREADS_MAX || len > AG_CLU_ARG_MAX) {
        return -AG_EINVAL;
    }

    lock();
    if (s_role == CLU_MASTER) {
        /* This board is running a job of its own; it is not also a worker. */
        unlock();
        return -AG_EBUSY;
    }
    const bool have_worker = (s_worker_pid != AG_PID_KERNEL);
    if (have_worker && strcmp(s_worker_image, image) != 0) {
        unlock();
        return -AG_EBUSY; /* a second job, and one at a time is the rule */
    }
    unlock();

    if (!have_worker) {
        /*
         * Start the program in worker mode.  Not exec: this must return so the
         * thread request can be answered, and the process has to stay alive for
         * as long as the job does anyway.
         */
        ag_pid_t       pid = AG_PID_KERNEL;
        char           path[AG_CLU_IMAGE_MAX];
        char          *argv[1];
        snprintf(path, sizeof(path), "%s", image);
        argv[0] = path;

        /*
         * A path under the mount point means "it is on YOUR card", so the
         * asking node's tree has to be here before the loader looks for it.
         * Done here rather than requiring somebody to have typed `link mount`
         * first, which is the difference between a job that works and a job
         * that works if you remembered - the remote-run server next door mounts
         * for exactly the same reason.  Already mounted is not an error.
         */
        if (strncmp(path, AG_NODEDRV_MOUNT "/",
                    strlen(AG_NODEDRV_MOUNT) + 1u) == 0) {
            const ag_err_t m = ag_nodedrv_mount(AG_NODEDRV_MOUNT);
            if (m != AG_OK && m != -AG_EEXIST) {
                return m;
            }
        }

        lock();
        snprintf(s_worker_image, sizeof(s_worker_image), "%s", image);
        s_ending = false;
        unlock();

        const ag_err_t e = ag_proc_spawn(path, 1, argv, 0, &pid);
        if (e != AG_OK) {
            lock();
            s_worker_image[0] = '\0';
            unlock();
            return e;
        }
        lock();
        s_worker_pid = pid;
        unlock();

        /*
         * Wait for it to declare its entry points.  Until it has, "is there an
         * entry called this?" has no answer - and answering the far node "yes"
         * and failing afterwards would leave it waiting on a thread that never
         * existed.
         */
        const uint32_t deadline = now_ms() + CLU_DECLARE_MS;
        for (;;) {
            lock();
            const bool ready = (s_tab_pid == s_worker_pid && s_tab != NULL);
            unlock();
            if (ready) {
                break;
            }
            ag_proc_state_t st;
            if (ag_proc_state_of(pid, &st) != AG_OK) {
                lock();
                s_worker_pid = AG_PID_KERNEL;
                s_worker_image[0] = '\0';
                unlock();
                return -AG_ENOENT;
            }
            if (now_ms() >= deadline) {
                lock();
                s_worker_pid = AG_PID_KERNEL;
                s_worker_image[0] = '\0';
                unlock();
                return -AG_ETIMEDOUT;
            }
            ag_port_task_delay(ag_port_ms_to_ticks(10));
        }
    }

    lock();
    if (!table_take()) {
        unlock();
        return -AG_ENOMEM;
    }
    if (s_threads[tid].used) {
        unlock();
        return -AG_EEXIST;
    }
    clu_thread_t *t = &s_threads[tid];
    memset(t, 0, sizeof(*t));
    if (!slot_init(t)) {
        unlock();
        return -AG_ENOMEM;
    }
    t->local = true;
    t->node = 0;
    snprintf(t->entry, sizeof(t->entry), "%s", entry);
    if (arg != NULL && len > 0) {
        memcpy(t->arg, arg, len);
        t->arglen = len;
    }
    t->fn = entry_by_name(entry);
    if (t->fn == NULL) {
        slot_release(t);
        unlock();
        return -AG_ENOENT;
    }
    /*
     * The master is tid 0 and it is on the other node.  A record for it is what
     * makes send(AG_CLU_MASTER, ...) from a worker thread mean anything, and
     * what holds the credits for its mailbox.
     */
    if (!s_threads[0].used) {
        memset(&s_threads[0], 0, sizeof(s_threads[0]));
        if (!slot_init(&s_threads[0])) {
            slot_release(t);
            unlock();
            return -AG_ENOMEM;
        }
        s_threads[0].local = false;
        s_threads[0].node = 1;
        snprintf(s_threads[0].entry, sizeof(s_threads[0].entry), "%s", "main");
    }
    const ag_pid_t pid = s_worker_pid;
    unlock();

    const ag_err_t e = start_local(pid, tid);
    if (e != AG_OK) {
        lock();
        slot_release(t);
        unlock();
    }
    return e;
}

ag_err_t ag_clu_deliver(ag_clu_tid_t from, ag_clu_tid_t to, uint8_t *msg,
                        uint32_t len)
{
    lock();
    clu_thread_t *t = by_tid(to);
    if (t == NULL || !t->local || t->finished) {
        unlock();
        ag_port_free(msg);
        return -AG_ENOENT;
    }
    const ag_err_t e = mbox_put(t, from, msg, len);
    unlock();

    if (e != AG_OK) {
        /*
         * The one place a message can be lost, and it is here rather than
         * hidden: credits mean the far node was entitled to send this, so
         * getting here means the heap gave out.  Said out loud, because a
         * cluster that silently swallows a result is worse than one that stops.
         */
        ag_log(AG_LOG_ERROR, "clu", "dropped %u bytes for thread %d: %d",
               (unsigned)len, (int)to, (int)e);
        ag_port_free(msg);
    }
    return e;
}

void ag_clu_add_credit(ag_clu_tid_t tid, uint32_t n)
{
    lock();
    clu_thread_t *t = by_tid(tid);
    if (t != NULL && !t->local) {
        t->credits += n;
        if (t->credits > AG_CLU_MBOX_MSGS) {
            t->credits = AG_CLU_MBOX_MSGS;
        }
    }
    unlock();
}

void ag_clu_note_exit(ag_clu_tid_t tid)
{
    lock();
    clu_thread_t *t = by_tid(tid);
    if (t != NULL) {
        t->finished = true;
    }
    unlock();
}

/*
 * "The job is over", said from outside: an END frame, or the wire going away
 * under a worker.
 *
 * ONLY a worker acts on it, and the asymmetry is deliberate.  A master owns its
 * job; nothing on the other board may end it.  Two nodes can each be running a
 * job of their own - each refuses to be the other's worker while it is - and an
 * END arriving from the neighbour's job finishing would otherwise tear down
 * ours.  For a master that loses the wire the honest outcome is what already
 * happens: sends to threads over there fail, and the program is told.
 */
void ag_clu_job_end(void)
{
    lock();
    if (s_role == CLU_WORKER) {
        s_ending = true; /* the worker process's start() returns on this */
    }
    unlock();
}

bool ag_clu_take_pending_end(void)
{
    lock();
    const bool p = s_pending_end;
    s_pending_end = false;
    unlock();
    return p;
}

void ag_clu_proc_ended(ag_pid_t pid)
{
    if (s_lock == NULL || pid == AG_PID_KERNEL) {
        return;
    }
    lock();
    if (s_role == CLU_MASTER && pid == s_job_pid) {
        /*
         * The program that owned the job has gone.  The far nodes have to be
         * told, and this is the process table's teardown path - no wire from
         * here.  See ag_clu_take_pending_end.
         */
        s_pending_end = (node_list(NULL, 0) > 1);
        job_clear();
    } else if (s_role == CLU_WORKER && pid == s_job_pid) {
        job_clear();
        s_ending = false;
    } else if (pid == s_worker_pid) {
        s_worker_pid = AG_PID_KERNEL;
        s_worker_image[0] = '\0';
    }
    if (s_tab_pid == pid) {
        s_tab = NULL;
        s_tab_n = 0;
        s_tab_pid = AG_PID_KERNEL;
    }
    unlock();
}

bool ag_clu_busy(void) { return s_role != CLU_NONE; }

uint32_t ag_clu_thread_count(void)
{
    uint32_t n = 0;
    lock();
    for (uint32_t i = 0; s_threads != NULL && i < AG_CLU_THREADS_MAX; i++) {
        if (s_threads[i].used) {
            n++;
        }
    }
    unlock();
    return n;
}

ag_err_t ag_clu_init(void)
{
    if (s_lock == NULL) {
        s_lock = ag_port_mutex_new_recursive();
        if (s_lock == NULL) {
            return -AG_ENOMEM;
        }
    }
    return AG_OK;
}
