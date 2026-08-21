/*
 * ArgonOS - what the machine has been asked to be, and who asked to be told.
 *
 * The table, not the machine: no clock, no screen, no journal, no scheduler,
 * and nothing here ends a process.  The time arrives as an argument, which is
 * what lets a transition and its deadline be tested on the host in microseconds
 * rather than in half seconds on a board - see host-tests/test_power.c.  The
 * half that touches hardware, and the half that can kill, are both in
 * powerctl.c next door.
 *
 * No lock, and that is a statement about who calls: transitions are serialised
 * one layer up, by the mutex in powerctl.c, because that is where both of the
 * two callers are - the shell when a person types the command, and the
 * supervisor on the idle tick.  Everything else that arrives here is an
 * application reading or writing its own row.  A second lock at this level
 * would only add an order to get wrong.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/power.h>

#include <string.h>

typedef struct {
    bool               used;
    ag_pid_t           pid;
    bool               polled;   /* has ever asked for status               */
    uint32_t           last_poll_ms;
    ag_power_fitness_t fitness;  /* standing, outlives any transition       */
    ag_power_answer_t  answer;   /* to the transition in flight             */
    char               why[AG_POWER_WHY_MAX];
} watcher_t;

static watcher_t s_watch[AG_POWER_WATCHERS];

static ag_power_target_t s_now;
static uint32_t          s_cpu_max_mhz = 1;

static bool              s_pending;
static ag_power_cause_t  s_cause;
static ag_power_target_t s_want;
static uint32_t          s_begin_ms;
static uint32_t          s_deadline_ms;

/* Wrap-safe: the tick is milliseconds in a uint32_t, which comes round every
 * seven weeks, and a board that has been up that long must not stop changing
 * modes because two numbers crossed zero. */
static bool reached(uint32_t now_ms, uint32_t when_ms)
{
    return (int32_t)(now_ms - when_ms) >= 0;
}

static void set_why(watcher_t *w, const char *why)
{
    if (why == NULL) {
        w->why[0] = '\0';
        return;
    }
    strncpy(w->why, why, sizeof(w->why) - 1);
    w->why[sizeof(w->why) - 1] = '\0';
}

static watcher_t *find(ag_pid_t pid)
{
    for (uint32_t i = 0; i < AG_POWER_WATCHERS; i++) {
        if (s_watch[i].used && s_watch[i].pid == pid) {
            return &s_watch[i];
        }
    }
    return NULL;
}

/*
 * The kernel is never a watcher.  The shell asks for status to print it and the
 * supervisor asks on its tick; neither is an application deciding what to do
 * about a slower clock, and counting them would mean waiting for an answer from
 * the task that is doing the waiting.
 */
static watcher_t *find_or_add(ag_pid_t pid)
{
    if (pid == AG_PID_KERNEL) {
        return NULL;
    }
    watcher_t *w = find(pid);
    if (w != NULL) {
        return w;
    }
    for (uint32_t i = 0; i < AG_POWER_WATCHERS; i++) {
        if (!s_watch[i].used) {
            memset(&s_watch[i], 0, sizeof(s_watch[i]));
            s_watch[i].used = true;
            s_watch[i].pid = pid;
            return &s_watch[i];
        }
    }
    return NULL;
}

/* Recently enough to be waited for.  Measured from the moment the transition
 * was announced, so the answer does not depend on how long the wait has run. */
static bool listening(const watcher_t *w)
{
    if (!w->polled) {
        return false;
    }
    return (uint32_t)(s_begin_ms - w->last_poll_ms) <= AG_POWER_LISTEN_MS;
}

static bool fit(const watcher_t *w)
{
    if (w->fitness == AG_POWER_FIT_ANY) {
        return true;
    }
    return w->answer == AG_POWER_OK || w->answer == AG_POWER_PARKED;
}

void ag_power_init(uint32_t cpu_max_mhz)
{
    memset(s_watch, 0, sizeof(s_watch));
    s_pending = false;
    s_cause = AG_POWER_USER;
    s_cpu_max_mhz = (cpu_max_mhz > 0u) ? cpu_max_mhz : 1u;

    s_now.mode = AG_POWER_FULL;
    s_now.cpu_min_mhz = s_cpu_max_mhz;
    s_now.cpu_max_mhz = s_cpu_max_mhz;
    s_now.screen_on = true;
}

void ag_power_current(ag_power_target_t *out)
{
    if (out != NULL) {
        *out = s_now;
    }
}

ag_power_mode_t ag_power_mode(void) { return s_now.mode; }

bool ag_power_screen_on(void) { return s_now.screen_on; }

ag_power_cause_t ag_power_cause(void) { return s_cause; }

ag_err_t ag_power_poll(ag_pid_t pid, uint32_t now_ms, ag_power_status_t *out,
                       uint32_t cpu_mhz)
{
    if (out == NULL) {
        return -AG_EINVAL;
    }

    watcher_t *w = find_or_add(pid);
    if (w != NULL) {
        w->polled = true;
        w->last_poll_ms = now_ms;
    }

    memset(out, 0, sizeof(*out));
    out->mode = (uint8_t)s_now.mode;
    out->cause = (uint8_t)s_cause;
    out->screen_on = s_now.screen_on;
    out->cpu_mhz = (cpu_mhz > 0u) ? cpu_mhz : s_now.cpu_max_mhz;
    out->cpu_min_mhz = s_now.cpu_min_mhz;
    out->cpu_max_mhz = s_now.cpu_max_mhz;

    if (s_pending) {
        out->pending = (uint8_t)s_want.mode;
        out->pending_screen_on = s_want.screen_on;
        out->pending_cpu_min_mhz = s_want.cpu_min_mhz;
        out->pending_cpu_max_mhz = s_want.cpu_max_mhz;
        out->grace_ms =
            reached(now_ms, s_deadline_ms) ? 0u : (s_deadline_ms - now_ms);
    } else {
        out->pending = (uint8_t)s_now.mode;
        out->pending_screen_on = s_now.screen_on;
        out->pending_cpu_min_mhz = s_now.cpu_min_mhz;
        out->pending_cpu_max_mhz = s_now.cpu_max_mhz;
        out->grace_ms = 0u;
    }
    return AG_OK;
}

void ag_power_forget(ag_pid_t pid)
{
    watcher_t *w = find(pid);
    if (w != NULL) {
        memset(w, 0, sizeof(*w));
    }
}

ag_err_t ag_power_declare(ag_pid_t pid, ag_power_fitness_t fitness,
                          const char *why)
{
    if (pid == AG_PID_KERNEL) {
        return -AG_EPERM;
    }
    if (fitness != AG_POWER_FIT_ASK && fitness != AG_POWER_FIT_ANY &&
        fitness != AG_POWER_FIT_FULL_ONLY) {
        return -AG_EINVAL;
    }
    watcher_t *w = find_or_add(pid);
    if (w == NULL) {
        return -AG_ENFILE;
    }
    w->fitness = fitness;
    set_why(w, (fitness == AG_POWER_FIT_ASK) ? NULL : why);
    return AG_OK;
}

ag_err_t ag_power_begin(const ag_power_target_t *want, ag_power_cause_t cause,
                        uint32_t now_ms, uint32_t grace_ms)
{
    if (want == NULL || want->cpu_min_mhz == 0u ||
        want->cpu_max_mhz < want->cpu_min_mhz) {
        return -AG_EINVAL;
    }
    if (cause != AG_POWER_AUTO && cause != AG_POWER_USER) {
        return -AG_EINVAL;
    }
    if (s_pending) {
        return -AG_EBUSY;
    }

    s_want = *want;
    s_cause = cause;
    s_pending = true;
    s_begin_ms = now_ms;
    s_deadline_ms = now_ms + grace_ms;

    for (uint32_t i = 0; i < AG_POWER_WATCHERS; i++) {
        s_watch[i].answer = AG_POWER_ANSWER_NONE;
    }
    return AG_OK;
}

ag_err_t ag_power_reply(ag_pid_t pid, ag_power_answer_t a, const char *why)
{
    if (!s_pending) {
        return -AG_ENOENT;
    }
    if (a != AG_POWER_OK && a != AG_POWER_PARKED && a != AG_POWER_UNFIT) {
        return -AG_EINVAL;
    }
    watcher_t *w = find_or_add(pid);
    if (w == NULL) {
        return -AG_ENFILE;
    }
    w->answer = a;
    if (a == AG_POWER_UNFIT) {
        set_why(w, why);
    }
    return AG_OK;
}

bool ag_power_settled(uint32_t now_ms)
{
    if (!s_pending) {
        return true;
    }
    for (uint32_t i = 0; i < AG_POWER_WATCHERS; i++) {
        const watcher_t *w = &s_watch[i];
        if (!w->used || w->fitness != AG_POWER_FIT_ASK) {
            continue; /* a standing declaration has already answered */
        }
        if (listening(w) && w->answer == AG_POWER_ANSWER_NONE) {
            return reached(now_ms, s_deadline_ms);
        }
    }
    return true;
}

ag_err_t ag_power_commit(ag_power_target_t *out)
{
    if (!s_pending) {
        return -AG_ENOENT;
    }

    s_now = s_want;
    s_pending = false;
    if (out != NULL) {
        *out = s_now;
    }
    return AG_OK;
}

void ag_power_cancel(void) { s_pending = false; }

bool ag_power_fit(ag_pid_t pid)
{
    const watcher_t *w = find(pid);
    return (w != NULL) && fit(w);
}

const char *ag_power_why(ag_pid_t pid)
{
    const watcher_t *w = find(pid);
    return (w != NULL) ? w->why : "";
}

bool ag_power_full_only_held(void)
{
    for (uint32_t i = 0; i < AG_POWER_WATCHERS; i++) {
        if (s_watch[i].used &&
            s_watch[i].fitness == AG_POWER_FIT_FULL_ONLY) {
            return true;
        }
    }
    return false;
}

uint32_t ag_power_watcher_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < AG_POWER_WATCHERS; i++) {
        if (s_watch[i].used) {
            n++;
        }
    }
    return n;
}

bool ag_power_watcher_at(uint32_t index, uint32_t now_ms,
                         ag_power_watcher_t *out)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < AG_POWER_WATCHERS; i++) {
        const watcher_t *w = &s_watch[i];
        if (!w->used) {
            continue;
        }
        if (n++ != index) {
            continue;
        }
        if (out != NULL) {
            memset(out, 0, sizeof(*out));
            out->pid = w->pid;
            out->fitness = w->fitness;
            out->answer = w->answer;
            /*
             * Outside a transition "listening" is measured from now; inside one
             * it is measured from the announcement, so that a row printed after
             * the wait says what was true when the question was asked.
             */
            out->listening =
                w->polled && ((s_pending ? (s_begin_ms - w->last_poll_ms)
                                         : (now_ms - w->last_poll_ms)) <=
                              AG_POWER_LISTEN_MS);
            memcpy(out->why, w->why, sizeof(out->why));
        }
        return true;
    }
    return false;
}
