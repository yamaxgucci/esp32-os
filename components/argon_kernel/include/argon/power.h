/*
 * ArgonOS - how hard the machine is being driven.
 *
 * Two files behind this header, and the split is the same one the pin table
 * and the io layer use.
 *
 *   src/core/power.c     the table: what has been asked for, who is listening,
 *                        what they answered, and what the machine should
 *                        therefore be set to.  No clock, no screen, no journal
 *                        and no scheduler - it takes the time as an argument -
 *                        so it builds and is tested on the host.
 *
 *   src/core/powerctl.c  the machine: the clock through argon/port/power.h, the
 *                        backlight through the display driver that owns the
 *                        pin, the waiting, and the one line in the journal.
 *
 * What the arrangement is for is in the ABI, beside ag_power_api_t: a change of
 * mode is announced, applications get a moment to answer, silence counts as
 * consent, and a standing hold blocks a command rather than the system.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_POWER_H
#define ARGON_POWER_H

#include <argon/abi.h>
#include <argon/proc.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One row per process, which is the most there can be: a hold and an answer
 * belong to a process, not to a thread, and AG_PROC_MAX of them is the whole
 * system.  No allocation - this table is read while a transition is in flight,
 * and there is no useful way to fail there.
 *
 * The size is not free.  At forty bytes of reason each these rows were two
 * hundred bytes more than the S3 image had left in its data segment, which is
 * the same wall the console's row buffer hit; see AG_POWER_WHY_MAX in the ABI.
 */
#define AG_POWER_WATCHERS AG_PROC_MAX

/* How long an application has to answer, unless a caller says otherwise. */
#define AG_POWER_GRACE_MS 500

/*
 * How recently a process must have asked for status to be waited for.  A
 * process that polled once at startup and never again is not listening, and
 * waiting half a second for it on every mode change would make the command
 * feel broken while telling nobody anything.
 */
#define AG_POWER_LISTEN_MS 2000

/* What the machine should be.  Filled by a caller, applied by powerctl. */
typedef struct {
    ag_power_mode_t mode;
    uint32_t        cpu_min_mhz;
    uint32_t        cpu_max_mhz;
    bool            screen_on;
} ag_power_target_t;

/* One row of what the `power` command prints. */
typedef struct {
    ag_pid_t          pid;
    ag_power_answer_t answer;
    bool              hold;      /* a standing hold, not this transition   */
    bool              listening; /* has asked for status recently          */
    char              why[AG_POWER_WHY_MAX];
} ag_power_watcher_t;

/* ---------------------------------------------------------------------- */
/* The table - src/core/power.c                                           */
/* ---------------------------------------------------------------------- */

/*
 * cpu_max_mhz is what the machine can do at its fastest, which is where it
 * starts: this system comes up at full speed and is asked to slow down, never
 * the other way round.
 */
void ag_power_init(uint32_t cpu_max_mhz);

void            ag_power_current(ag_power_target_t *out);
ag_power_mode_t ag_power_mode(void);
bool            ag_power_screen_on(void);

/*
 * A process asks for status; that is what makes it a listener.  There is no
 * subscribe call, because there is nothing an application could usefully do
 * with one that polling does not already do - and because a subscription is a
 * thing to leak, while a poll that stops arriving is self-cleaning.
 */
ag_err_t ag_power_poll(ag_pid_t pid, uint32_t now_ms, ag_power_status_t *out,
                       uint32_t cpu_mhz);

/* Everything one process asked for, gone.  Called when a process is reaped. */
void ag_power_forget(ag_pid_t pid);

ag_err_t ag_power_set_hold(ag_pid_t pid, bool on, const char *why);

/*
 * Announce a transition.  -AG_EBUSY while one is already in flight: two
 * commands overlapping would mean two answers to two different questions
 * counted against one deadline.
 */
ag_err_t ag_power_begin(const ag_power_target_t *want, uint32_t now_ms,
                        uint32_t grace_ms);

ag_err_t ag_power_reply(ag_pid_t pid, ag_power_answer_t a, const char *why);

/* Every listener has answered, or the grace period is over. */
bool ag_power_settled(uint32_t now_ms);

/*
 * Apply the pending transition to the table and say what to set.  -AG_EPERM
 * when a hold stands in the way and `force` is false; the pending transition
 * is then dropped and the holds are still in the watcher list to be printed.
 * A transition that raises the clock is never refused.
 */
ag_err_t ag_power_commit(bool force, ag_power_target_t *out);

/* Drop a pending transition without applying it. */
void ag_power_cancel(void);

uint32_t ag_power_watcher_count(void);
bool     ag_power_watcher_at(uint32_t index, uint32_t now_ms,
                             ag_power_watcher_t *out);

/* ---------------------------------------------------------------------- */
/* The machine - src/core/powerctl.c                                      */
/* ---------------------------------------------------------------------- */

ag_err_t ag_powerctl_init(void);

/*
 * The whole dance: announce, wait for the answers, commit, set the clock, put
 * the light out, write one line in the journal.  Blocks for up to the grace
 * period, which is why it is called from a command and not from a tick.
 *
 * -AG_EPERM when a hold refused it, and then nothing at all is applied: a
 * half-done mode is worse than a refused one, because nothing says which half.
 *
 * A machine whose clock cannot be moved is not a refusal.  The mode is entered,
 * the screen and the applications are dealt with, and the band recorded is the
 * band the machine actually has - so the caller notices by comparing what it
 * asked for with ag_power_current(), and the frequency printed is never one the
 * chip is not running at.
 */
ag_err_t ag_powerctl_apply(const ag_power_target_t *want, bool force);

/* Live reading from the machine, megahertz. */
uint32_t ag_powerctl_cpu_mhz(void);

/* The frequencies this machine accepts, highest first.  Returns the count. */
uint32_t ag_powerctl_cpu_steps(uint16_t *out, uint32_t max);

/* Whether the clock can be moved at all in this build. */
bool ag_powerctl_can_scale(void);

/*
 * True when a display driver took the backlight request.  False means the
 * screen went dark only as far as this machine can make it - which on a board
 * with no controllable light is not at all, and the `power` command says so
 * rather than claiming a saving that is not there.
 */
bool ag_powerctl_backlight_works(void);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_POWER_H */
