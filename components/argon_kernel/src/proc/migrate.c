/*
 * ArgonOS - moving a running program to another board.  See argon/migrate.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/migrate.h>

#include <stdio.h>
#include <string.h>

#include <argon/linkprov.h>
#include <argon/log.h>
#include <argon/nodedrv.h>
#include <argon/nodeproc.h>
#include <argon/proc.h>

#include <argon/port/mem.h>
#include <argon/port/task.h>
#include <argon/port/time.h>

#include "proc_internal.h"

#define TAG "migrate"

/*
 * One migration at a time, and one buffer for it.
 *
 * Not a limitation worth removing: moving a program is an operator's decision
 * about one program, and two at once would have to agree about which of them a
 * departing process was answering.  A second attempt gets -AG_EBUSY, which is
 * an answer somebody can act on.
 */
static ag_pid_t s_asking;              /* who is being asked to move          */
static uint32_t s_state_len;
static bool     s_saved;               /* it agreed and handed something over */

/* What a process starting here should find - the far end of somebody else's
 * migration.  Taken once, by the first process that asks. */
static uint32_t s_arrived_len;
static bool     s_arriving;

/*
 * BOTH BUFFERS COME FROM THE HEAP, and not for tidiness: as plain arrays they
 * were 2 KB of internal .bss on every board ever built, whether or not anyone
 * ever migrates anything - and this tree has already watched a few kilobytes
 * of exactly that break TLS, because the handshake needs a large contiguous
 * block and internal SRAM is what runs out first.  `nettest` said it again the
 * moment these were added: https, and only https.
 *
 * Kept once taken, rather than freed after each handover: a kilobyte is not
 * worth the risk of failing to allocate halfway through moving somebody's
 * program, and a board that migrated once will migrate again.
 */
static uint8_t *s_state;
static uint8_t *s_arrived;

static uint8_t *buffer_take(uint8_t **slot)
{
    if (*slot == NULL) {
        /* Outside SRAM first, exactly like the thread table next door: this
         * is a kilobyte that is read and written twice in a handover, so
         * where it lives costs nothing and what it displaces costs a lot. */
        *slot = (uint8_t *)ag_port_alloc(AG_MIGRATE_MAX,
                                         AG_MEM_SLOW | AG_MEM_BYTE);
        if (*slot == NULL) {
            *slot = (uint8_t *)ag_port_alloc(AG_MIGRATE_MAX,
                                             AG_MEM_FAST | AG_MEM_BYTE);
        }
    }
    return *slot;
}

void ag_migrate_asking(ag_pid_t pid)
{
    s_asking = pid;
    s_saved = false;
    s_state_len = 0;
}

void ag_migrate_done(void) { s_asking = AG_PID_KERNEL; }

void ag_migrate_arriving(const void *state, uint32_t len)
{
    if (len > AG_MIGRATE_MAX) {
        len = AG_MIGRATE_MAX;
    }
    if (state != NULL && len > 0) {
        if (buffer_take(&s_arrived) == NULL) {
            /* Nothing to hand the program: it will start fresh, which is the
             * honest outcome and the one it is written to survive. */
            s_arrived_len = 0;
            s_arriving = false;
            return;
        }
        memcpy(s_arrived, state, len);
    }
    s_arrived_len = len;
    s_arriving = (len > 0);
}

bool ag_migrate_wanted(void)
{
    const ag_pid_t me = ag_proc_self();
    return me != AG_PID_KERNEL && me == s_asking;
}

ag_err_t ag_migrate_save(const void *state, uint32_t len)
{
    if (!ag_migrate_wanted()) {
        return -AG_EPERM;
    }
    if (len > AG_MIGRATE_MAX) {
        return -AG_ERANGE;
    }
    if (state != NULL && len > 0) {
        if (buffer_take(&s_state) == NULL) {
            /* Refused before the program has given anything up, so it can act
             * on the answer: it is still here, and still holds its work. */
            return -AG_ENOMEM;
        }
        memcpy(s_state, state, len);
    }
    s_state_len = len;
    s_saved = true;
    return AG_OK;
}

int32_t ag_migrate_restore(void *out, uint32_t cap)
{
    if (!s_arriving) {
        return 0; /* a fresh start, which is not an error */
    }
    if (s_arrived_len > cap) {
        /*
         * Loud rather than truncated.  Two builds of the same program
         * disagreeing about the size of their own state is exactly the fault
         * that a half-copied struct would hide until it produced nonsense.
         */
        return -AG_ERANGE;
    }
    if (out != NULL && s_arrived_len > 0) {
        memcpy(out, s_arrived, s_arrived_len);
    }
    const int32_t n = (int32_t)s_arrived_len;
    s_arriving = false; /* taken once: the next program starts fresh */
    s_arrived_len = 0;

    /*
     * The node it came from is still waiting to hear whether this worked, and
     * THIS is the moment that answers it: the state arrived and a running
     * program has taken it.  Nothing weaker will do - the image being read, or
     * a pid existing, prove only that a load was attempted, and the asking node
     * has already let its copy of the program go.
     *
     * The alternative it replaces was waiting for the program to EXIT, which
     * for anything worth migrating is never.
     */
    ag_nodeproc_migrated();
    return n;
}

/* ---------------------------------------------------------------------- */

ag_err_t ag_migrate_away(ag_pid_t pid, uint32_t timeout_ms)
{
    if (s_asking != AG_PID_KERNEL) {
        return -AG_EBUSY;
    }
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }

    /*
     * Everything that can be refused is refused BEFORE the program is
     * disturbed.  Asking a program to end and only then discovering there is
     * nowhere to send it would have cost it its work for nothing.
     */
    ag_proc_table_lock();
    proc_t *p = ag_proc_by_pid(pid);
    if (p == NULL) {
        ag_proc_table_unlock();
        return -AG_ENOENT;
    }
    if ((p->app.header.flags & AG_AXE_CAN_MIGRATE) == 0) {
        ag_proc_table_unlock();
        return -AG_ENOTSUP;
    }
    char path[AG_PATH_MAX];
    snprintf(path, sizeof(path), "%s", p->path);
    ag_proc_table_unlock();

    if (path[0] != '/') {
        return -AG_EINVAL;
    }

    /*
     * Stages, in the journal.  A handover crosses four subsystems and the task
     * doing it is the shell's own, so when it stops nothing can be typed to ask
     * where - the console is exactly as dead as the thing being diagnosed.
     * These lines are the only witness that keeps working, and a run that says
     * "asked" and never says "ended" has already named its half.
     */
    ag_log(AG_LOG_INFO, TAG, "pid %u: asking", (unsigned)pid);
    ag_migrate_asking(pid);
    const ag_err_t sig = ag_proc_signal(pid);
    if (sig != AG_OK) {
        ag_migrate_done();
        return sig;
    }

    /* Wait for it to agree by ending.  A program that ignores the request is a
     * program that did not keep its promise, and is left alone. */
    const uint32_t deadline = (uint32_t)(ag_port_us() / 1000) + timeout_ms;
    for (;;) {
        ag_proc_state_t st;
        if (ag_proc_state_of(pid, &st) != AG_OK) {
            break; /* gone: it ended, which is the agreement */
        }
        if ((uint32_t)(ag_port_us() / 1000) >= deadline) {
            ag_migrate_done();
            return -AG_ETIMEDOUT;
        }
        ag_port_task_delay(ag_port_ms_to_ticks(10));
    }

    ag_log(AG_LOG_INFO, TAG, "pid %u: ended, %s", (unsigned)pid,
           s_saved ? "state in hand" : "nothing saved");
    if (!s_saved) {
        ag_migrate_done();
        return -AG_EAGAIN; /* it declined; see the header */
    }

    /*
     * The far node opens this image through its own view of us, the same
     * recipe placement uses: `/node` there is this node's root.
     */
    char line[AG_PATH_MAX + 16];
    snprintf(line, sizeof(line), "%s%s", AG_NODEDRV_MOUNT, path);

    ag_log(AG_LOG_INFO, TAG, "sending %u bytes and '%s'",
           (unsigned)s_state_len, line);
    int32_t        code = 0;
    const ag_err_t e = ag_nodeproc_run_state(line, s_state, s_state_len,
                                             timeout_ms, &code);
    ag_migrate_done();
    if (e != AG_OK) {
        ag_log(AG_LOG_ERROR, TAG, "%s did not start over there: %d", path,
               (int)e);
        return e;
    }
    ag_log(AG_LOG_INFO, TAG, "%s moved to %s with %u bytes of state", path,
           ag_linkprov_active(), (unsigned)s_state_len);
    return AG_OK;
}
