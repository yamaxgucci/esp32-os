/*
 * ArgonOS - handing the work to the neighbour and going quiet.  See
 * argon/nodestandby.h for what this is for and what it refuses to do.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/nodestandby.h>

#include <string.h>

#include <argon/cfg.h>
#include <argon/console.h>
#include <argon/linkprov.h>
#include <argon/log.h>
#include <argon/migrate.h>
#include <argon/nodename.h>
#include <argon/nodeproc.h>
#include <argon/proc.h>

#include <argon/port/task.h>
#include <argon/port/time.h>

#include "core/sysconfig.h"

#define TAG "standby"

/*
 * Twenty per mille - two per cent of a core - and the number matters, so here
 * is where it comes from.  A board with nothing to do is not at zero: the
 * shell's own line editor, the link's reader and the supervisor all wake
 * regularly, and on the CYD that floor measures at a few per mille.  Two per
 * cent is above that floor and far below anything that is actually computing;
 * the smallest real work in this tree, GRIND on one core, sits at over 900.
 */
#define STANDBY_BUSY_DEFAULT 20u

/* How often the watcher looks.  A second: the thing being measured is tens of
 * seconds long, and a task that wakes more often than it can learn anything
 * new is a board that never sleeps for a different reason. */
#define STANDBY_TICK_MS 1000u

/*
 * Somebody at the keyboard IS work, whatever the processes are doing.  A
 * second of it, because a keypress is instant and the measure it feeds is not:
 * anything longer would let a board hand its work away between two keystrokes
 * of somebody typing slowly.
 */
#define STANDBY_TYPING_MS 1000u

static uint32_t       s_after_s;
static uint32_t       s_busy;
static bool           s_handed;
static uint32_t       s_quiet_since;
static ag_port_task_t s_task;

static uint32_t now_ms(void) { return (uint32_t)(ag_port_us() / 1000); }

void ag_standby_settings(uint32_t *after_s, uint32_t *busy_permille)
{
    if (after_s != NULL) {
        *after_s = s_after_s;
    }
    if (busy_permille != NULL) {
        *busy_permille = s_busy;
    }
}

bool ag_standby_handed_over(void) { return s_handed; }

/*
 * Is anything on this board doing work?
 *
 * Every process, not only the foreground one: the whole point of the shape
 * this serves is a board that grinds with nobody watching, and that grind is
 * usually a background job.
 */
static bool anything_busy(void)
{
    if (ag_console_idle_ms() < STANDBY_TYPING_MS) {
        return true;
    }
    for (uint32_t i = 0;; i++) {
        ag_procinfo_t pi;
        if (ag_proc_info(i, &pi) != AG_OK) {
            break;
        }
        if (pi.state == AG_PS_ZOMBIE || pi.state == AG_PS_LOADING) {
            continue;
        }
        if (pi.cpu_permille > s_busy) {
            return true;
        }
    }
    return false;
}

ag_err_t ag_standby_handover(void)
{
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }

    /*
     * ASK EVERY PROCESS, TAKE WHAT AGREES.
     *
     * ag_migrate_away is a request, not an order: a program that was not
     * written to be moved does not answer and stays exactly where it is.  So
     * this cannot be phrased as "move the work" - it is "offer, and count".
     *
     * The list is walked from the end because a process that agrees LEAVES,
     * and the indices after it shift down; walking forwards would skip the
     * one that took its place.  Found the boring way: two migratable jobs,
     * one moved, and the report said one of two when both had been asked.
     */
    uint32_t total = 0;
    for (uint32_t i = 0;; i++) {
        ag_procinfo_t pi;
        if (ag_proc_info(i, &pi) != AG_OK) {
            break;
        }
        total++;
    }

    uint32_t moved = 0;
    uint32_t stayed = 0;
    for (int32_t i = (int32_t)total - 1; i >= 0; i--) {
        ag_procinfo_t pi;
        if (ag_proc_info((uint32_t)i, &pi) != AG_OK) {
            continue;
        }
        if (pi.state == AG_PS_ZOMBIE || pi.state == AG_PS_LOADING) {
            continue;
        }
        /*
         * THE SAME FIFTEEN SECONDS THE `migrate` COMMAND ALLOWS, and the
         * first draft's 400 ms was not a tuning choice, it was a fault.
         *
         * A program answers at its next CHECKPOINT, and MIGRANT's is every
         * 500 ms - so the short wait expired before the program could
         * possibly have replied.  Worse than slow: by then it had already
         * agreed and saved its state, and a timeout here does not put that
         * back.  The process ended, nothing was sent, and the work was gone.
         * The journal shows it exactly: "handing over at 6", then "nothing
         * here can move", then "returned 0".
         *
         * Nor does a generous wait cost anything on a board with nothing
         * movable, which is what the short one was meant to buy: a program
         * without AG_AXE_CAN_MIGRATE is refused by ag_migrate_away before it
         * is disturbed at all, and that answer is immediate.  Only a program
         * that WILL answer is ever waited for.
         */
        if (ag_migrate_away(pi.pid, 15000u) == AG_OK) {
            moved++;
        } else {
            stayed++;
        }
    }

    if (moved == 0) {
        ag_log(AG_LOG_INFO, TAG,
               "nothing here can move; staying awake (%u process%s asked)",
               (unsigned)stayed, (stayed == 1) ? "" : "es");
        return -AG_ENOENT;
    }

    s_handed = true;
    const char *peer = ag_node_peer_name();
    ag_log(AG_LOG_INFO, TAG, "%u process%s now on %s%s", (unsigned)moved,
           (moved == 1) ? "" : "es",
           (peer != NULL) ? peer : ag_linkprov_active(),
           (stayed > 0) ? "; the rest could not move" : "");
    return AG_OK;
}

ag_err_t ag_standby_fetch(void)
{
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }
    const ag_err_t e = ag_nodeproc_ask_return();
    if (e == AG_OK) {
        s_handed = false;
        ag_log(AG_LOG_INFO, TAG, "asked %s for our work back",
               ag_linkprov_active());
    }
    return e;
}

static void standby_task(void *arg)
{
    (void)arg;
    s_quiet_since = now_ms();
    for (;;) {
        ag_port_task_delay(ag_port_ms_to_ticks(STANDBY_TICK_MS));

        if (s_after_s == 0u) {
            continue;
        }

        if (anything_busy()) {
            s_quiet_since = now_ms();
            /*
             * Work came back, so fetch ours.  Here rather than on the
             * keypress itself, because "busy" is the same question in both
             * directions and answering it in one place is what keeps the two
             * halves from disagreeing about whether this board is working.
             */
            if (s_handed) {
                (void)ag_standby_fetch();
            }
            continue;
        }

        if (s_handed || !ag_linkprov_bound()) {
            continue;
        }
        if ((uint32_t)(now_ms() - s_quiet_since) < s_after_s * 1000u) {
            continue;
        }
        (void)ag_standby_handover();
        /* Whether it worked or not, do not ask again immediately: a board
         * with nothing movable would otherwise sweep its whole process list
         * every second for as long as it stays quiet. */
        s_quiet_since = now_ms();
    }
}

ag_err_t ag_standby_start(void)
{
    const ag_cfg_t *cfg = ag_sysconfig();
    if (cfg != NULL) {
        s_after_s = (uint32_t)ag_cfg_get_int(cfg, "cluster.standby_after", 0);
        s_busy = (uint32_t)ag_cfg_get_int(cfg, "cluster.standby_busy",
                                          (int32_t)STANDBY_BUSY_DEFAULT);
    } else {
        s_busy = STANDBY_BUSY_DEFAULT;
    }
    if (s_busy == 0u) {
        s_busy = STANDBY_BUSY_DEFAULT;
    }
    if (s_after_s == 0u) {
        return AG_OK; /* off, and off is the default */
    }
    if (s_task != NULL) {
        return AG_OK;
    }
    /* Priority 1, below every application, for the reason the link's reader
     * is: a watcher that can interrupt the work it is watching would change
     * the answer by asking the question. */
    if (!ag_port_task_create(standby_task, "ag_standby", 3072, NULL, 1, 0, 0,
                             &s_task)) {
        ag_log(AG_LOG_ERROR, TAG, "no task; standby is off");
        return -AG_ENOMEM;
    }
    ag_log(AG_LOG_INFO, TAG,
           "handing work over after %us with nothing above %u/1000 of a core",
           (unsigned)s_after_s, (unsigned)s_busy);
    return AG_OK;
}
