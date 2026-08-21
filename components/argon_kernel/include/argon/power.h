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
 *                        pin, the idle timer, the waiting, ending the processes
 *                        that cannot comply, and one line in the journal.
 *
 * Why a transition carries who asked for it, and what that costs a process that
 * does not answer, is in the ABI beside ag_power_api_t.  The short of it: an
 * automatic transition is advisory, one a person typed is an order.
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
 * One row per process, which is the most there can be: a declaration and an
 * answer belong to a process, not to a thread, and AG_PROC_MAX of them is the
 * whole system.  No allocation - this table is read while a transition is in
 * flight, and there is no useful way to fail there.
 *
 * The size is not free.  At forty bytes of reason each these rows were two
 * hundred bytes more than the S3 image had left in its data segment, which is
 * the same wall the console's row buffer hit; see AG_POWER_WHY_MAX in the ABI.
 */
#define AG_POWER_WATCHERS AG_PROC_MAX

/*
 * How long an application has to answer.  Two numbers, because the two kinds of
 * transition are asking different questions.
 *
 * A person waits at a prompt, and what they are waiting for is every
 * application to have had a fair chance to say it is fit - half a second is
 * that chance, and it is also the last half second of anything that does not
 * take it.
 *
 * The idle timer is asking nothing of anybody: it wants the answers of whoever
 * happens to be looking, and a tick that blocked the supervisor for half a
 * second every minute would be a worse bargain than the power it saves.
 */
#define AG_POWER_GRACE_MS 500
#define AG_POWER_AUTO_GRACE_MS 150

/*
 * How recently a process must have asked for status to be waited for.  A
 * process that polled once at startup and never again is not listening, and
 * waiting half a second for it on every mode change would make the command
 * feel broken while telling nobody anything.
 *
 * Not answering is still fatal on a transition a person asked for - being
 * waited for and being spared are different questions, and this one only
 * decides how long the command takes.
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
    ag_pid_t            pid;
    ag_power_fitness_t  fitness;   /* what it said once, if anything        */
    ag_power_answer_t   answer;    /* what it said about this transition    */
    bool                listening; /* has asked for status recently         */
    char                why[AG_POWER_WHY_MAX];
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

/* A standing answer: any mode suits me, or I need the full clock. */
ag_err_t ag_power_declare(ag_pid_t pid, ag_power_fitness_t fitness,
                          const char *why);

/*
 * Announce a transition.  -AG_EBUSY while one is already in flight: two
 * transitions overlapping would mean two answers to two different questions
 * counted against one deadline.
 */
ag_err_t ag_power_begin(const ag_power_target_t *want, ag_power_cause_t cause,
                        uint32_t now_ms, uint32_t grace_ms);

ag_err_t ag_power_reply(ag_pid_t pid, ag_power_answer_t a, const char *why);

/* Every listener has answered, or the grace period is over. */
bool ag_power_settled(uint32_t now_ms);

/*
 * Apply the pending transition to the table and say what to set.
 *
 * It cannot be refused, and that is the whole design: a person's command wins,
 * an automatic one has already been trimmed to what is safe by its caller, and
 * what happens to an application that cannot live with the result is decided
 * afterwards - by powerctl, which is the half that can end a process.
 */
ag_err_t ag_power_commit(ag_power_target_t *out);

/* Drop a pending transition without applying it. */
void ag_power_cancel(void);

/* Who asked for the transition in flight, or for the last one committed. */
ag_power_cause_t ag_power_cause(void);

/*
 * Did this process establish that it is fit for the transition just decided?
 *
 * True for a standing AG_POWER_FIT_ANY, and for an answer of OK or PARKED.
 * False for everything else - including a process that never said anything at
 * all, which is the case this exists for: on a transition a person asked for,
 * that is what costs it its life.
 */
bool ag_power_fit(ag_pid_t pid);

/* The reason a process gave, for the line that reports what happened to it. */
const char *ag_power_why(ag_pid_t pid);

/*
 * Is any process saying it needs the full clock?  The idle timer asks before
 * it lowers anything: automatic saving is not worth breaking work for, and
 * there is nobody at the console to be told that it did.
 */
bool ag_power_full_only_held(void);

uint32_t ag_power_watcher_count(void);
bool     ag_power_watcher_at(uint32_t index, uint32_t now_ms,
                             ag_power_watcher_t *out);

/* ---------------------------------------------------------------------- */
/* The machine - src/core/powerctl.c                                      */
/* ---------------------------------------------------------------------- */

/*
 * What a transition ended, for the one who asked for it to report.
 *
 * Filled during the sweep and not afterwards: a process's row in the table goes
 * when the process does, so by the time apply() returns there is nothing left
 * to look up - which is exactly the kind of report that ends up saying
 * "something was killed" and nothing more.
 */
#define AG_POWER_ENDED_NAME 16

typedef struct {
    ag_pid_t pid;
    char     name[AG_POWER_ENDED_NAME];
    char     why[AG_POWER_WHY_MAX]; /* its own words, or empty if it said none */
} ag_power_ended_t;

/* How the idle timer is set up.  Zero seconds means "never, by itself". */
typedef struct {
    bool     on;
    uint32_t screen_off_s;
    uint32_t eco_s;
    uint32_t eco_mhz;
} ag_power_auto_t;

ag_err_t ag_powerctl_init(void);

/*
 * The whole dance: announce, wait for the answers, commit, set the clock, put
 * the light out, end what could not comply, write one line in the journal.
 * Blocks for up to the grace period, which is why a person's transition is
 * called from a command and not from a tick.
 *
 * `ended` receives a row per process that was ended, up to `max`, and may be
 * NULL; `n_ended` receives how many there were, and may be NULL.  Only an
 * AG_POWER_USER transition into a lower mode ever ends anything: coming back
 * up, turning the screen off on its own, and anything the idle timer does never
 * cost a process its life.
 */
ag_err_t ag_powerctl_apply(const ag_power_target_t *want,
                           ag_power_cause_t cause, ag_power_ended_t *ended,
                           uint32_t max, uint32_t *n_ended);

/*
 * The idle timer, from the supervisor tick.  Cheap and silent until something
 * has to happen: it compares how long the machine has been left alone with the
 * two timeouts and calls apply() with AG_POWER_AUTO when it is time.
 */
void ag_powerctl_tick(void);

void ag_powerctl_auto_get(ag_power_auto_t *out);
ag_err_t ag_powerctl_auto_set(const ag_power_auto_t *in);

/* Live reading from the machine, megahertz. */
uint32_t ag_powerctl_cpu_mhz(void);

/* The frequencies this machine accepts, highest first.  Returns the count. */
uint32_t ag_powerctl_cpu_steps(uint16_t *out, uint32_t max);

/* What `power eco` with no number means on this part.  Zero if it has no
 * settings at all to choose from. */
uint32_t ag_powerctl_eco_default(void);

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
