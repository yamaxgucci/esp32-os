/*
 * ArgonOS - power mode tests.
 *
 * Two kinds of transition, and the difference between them is the difference
 * between a nuisance and a lost process, so it is worth testing properly.
 *
 * An automatic transition is advisory: everybody is told, nobody has to answer,
 * nothing is ended, and a process that says it needs the full clock is enough to
 * stop the timer lowering it at all.
 *
 * One a person typed is an order: the answers decide who is fit, and being fit
 * is what keeps a process alive.  So what is checked here is the verdict -
 * ag_power_fit - for every way of arriving at it: answered, declared, silent,
 * gone.  The killing itself is in powerctl.c, which needs a machine; the
 * decision that costs a life is here, where it costs microseconds to check.
 *
 * The time is an argument to every call in power.c, which is why all of this
 * runs in microseconds rather than in half seconds on a board.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/power.h>

#include "test.h"

#define APP_A ((ag_pid_t)1)
#define APP_B ((ag_pid_t)2)

#define MAX_MHZ 240u
#define ECO_MHZ 80u

static ag_power_target_t target(ag_power_mode_t mode, uint32_t mhz,
                                bool screen_on)
{
    ag_power_target_t t;
    t.mode = mode;
    t.cpu_min_mhz = mhz;
    t.cpu_max_mhz = mhz;
    t.screen_on = screen_on;
    return t;
}

static ag_power_target_t eco_target(void)
{
    return target(AG_POWER_ECO, ECO_MHZ, true);
}

/* A process that polls at `at_ms` is one the system will wait for. */
static void poll_at(ag_pid_t pid, uint32_t at_ms)
{
    ag_power_status_t st;
    AG_CHECK_INT(ag_power_poll(pid, at_ms, &st, MAX_MHZ), AG_OK);
}

static void setup(void) { ag_power_init(MAX_MHZ); }

/* ---------------------------------------------------------------------- */

static void test_starts_at_full(void)
{
    setup();

    ag_power_target_t now;
    ag_power_current(&now);
    AG_CHECK_INT(now.mode, AG_POWER_FULL);
    AG_CHECK_INT(now.cpu_min_mhz, MAX_MHZ);
    AG_CHECK_INT(now.cpu_max_mhz, MAX_MHZ);
    AG_CHECK(now.screen_on);
    AG_CHECK(ag_power_screen_on());
    AG_CHECK_INT(ag_power_watcher_count(), 0);
    AG_CHECK(!ag_power_full_only_held());

    /* Nothing pending, so nothing to answer. */
    AG_CHECK(ag_power_settled(0));
    AG_CHECK_INT(ag_power_reply(APP_A, AG_POWER_OK, NULL), -AG_ENOENT);
}

/*
 * The kernel asking for status is not an application waiting to be told.  If it
 * were, the shell would be waiting for an answer from the task doing the
 * waiting, and every mode change would take the whole grace period.
 */
static void test_kernel_is_not_a_watcher(void)
{
    setup();
    poll_at(AG_PID_KERNEL, 100);
    AG_CHECK_INT(ag_power_watcher_count(), 0);
    AG_CHECK_INT(ag_power_declare(AG_PID_KERNEL, AG_POWER_FIT_ANY, "no"),
                 -AG_EPERM);
}

static void test_no_listeners_settles_at_once(void)
{
    setup();

    const ag_power_target_t want = eco_target();
    AG_CHECK_INT(ag_power_begin(&want, AG_POWER_USER, 1000, AG_POWER_GRACE_MS),
                 AG_OK);
    AG_CHECK(ag_power_settled(1000));
    AG_CHECK_INT(ag_power_cause(), AG_POWER_USER);

    ag_power_target_t got;
    AG_CHECK_INT(ag_power_commit(&got), AG_OK);
    AG_CHECK_INT(got.cpu_max_mhz, ECO_MHZ);
    AG_CHECK_INT(ag_power_mode(), AG_POWER_ECO);
}

/* An application that polls is waited for, and its answer ends the wait. */
static void test_listener_answers(void)
{
    setup();
    poll_at(APP_A, 900);
    AG_CHECK_INT(ag_power_watcher_count(), 1);

    const ag_power_target_t want = eco_target();
    AG_CHECK_INT(ag_power_begin(&want, AG_POWER_USER, 1000, AG_POWER_GRACE_MS),
                 AG_OK);
    AG_CHECK(!ag_power_settled(1000));

    /* What it sees when it polls during the transition: who asked, and the
     * numbers - not just the name of the mode. */
    ag_power_status_t st;
    AG_CHECK_INT(ag_power_poll(APP_A, 1010, &st, 240), AG_OK);
    AG_CHECK_INT(st.mode, AG_POWER_FULL);
    AG_CHECK_INT(st.pending, AG_POWER_ECO);
    AG_CHECK_INT(st.cause, AG_POWER_USER);
    AG_CHECK_INT(st.cpu_max_mhz, MAX_MHZ);
    AG_CHECK_INT(st.pending_cpu_max_mhz, ECO_MHZ);
    AG_CHECK_INT(st.cpu_mhz, 240);
    AG_CHECK_INT(st.grace_ms, AG_POWER_GRACE_MS - 10);

    AG_CHECK_INT(ag_power_reply(APP_A, AG_POWER_PARKED, NULL), AG_OK);
    AG_CHECK(ag_power_settled(1010));
    AG_CHECK_INT(ag_power_commit(NULL), AG_OK);

    /* Parked is fit: it did what was asked of it. */
    AG_CHECK(ag_power_fit(APP_A));

    ag_power_watcher_t w;
    AG_CHECK(ag_power_watcher_at(0, 1010, &w));
    AG_CHECK_INT(w.pid, APP_A);
    AG_CHECK_INT(w.answer, AG_POWER_PARKED);
    AG_CHECK_INT(w.fitness, AG_POWER_FIT_ASK);
    AG_CHECK(w.listening);
}

/* Silence settles the transition at the deadline - and is what kills. */
static void test_silence_settles_and_is_not_fit(void)
{
    setup();
    poll_at(APP_A, 1000);

    const ag_power_target_t want = eco_target();
    AG_CHECK_INT(ag_power_begin(&want, AG_POWER_USER, 1000, 500), AG_OK);
    AG_CHECK(!ag_power_settled(1400));
    AG_CHECK(ag_power_settled(1500));
    AG_CHECK_INT(ag_power_commit(NULL), AG_OK);

    /* The whole point: it was there, it was asked, it said nothing. */
    AG_CHECK(!ag_power_fit(APP_A));
    AG_CHECK_STR(ag_power_why(APP_A), "");
}

/* A process nobody has ever heard from is not fit either - there is no row for
 * it at all, and on a commanded transition that is fatal. */
static void test_unknown_process_is_not_fit(void)
{
    setup();
    const ag_power_target_t want = eco_target();
    AG_CHECK_INT(ag_power_begin(&want, AG_POWER_USER, 1000, 0), AG_OK);
    AG_CHECK_INT(ag_power_commit(NULL), AG_OK);
    AG_CHECK(!ag_power_fit(APP_B));
}

/*
 * A process that polled once and stopped is not listening.  Waiting the full
 * grace period for it on every mode change would make the command feel broken
 * while telling nobody anything - but it is still not fit, because being waited
 * for and being spared are different questions.
 */
static void test_stale_listener_is_not_waited_for(void)
{
    setup();
    poll_at(APP_A, 1000);

    const ag_power_target_t want = eco_target();
    const uint32_t          late = 1000 + AG_POWER_LISTEN_MS + 1;
    AG_CHECK_INT(ag_power_begin(&want, AG_POWER_USER, late, AG_POWER_GRACE_MS),
                 AG_OK);
    AG_CHECK(ag_power_settled(late));
    AG_CHECK_INT(ag_power_commit(NULL), AG_OK);

    ag_power_watcher_t w;
    AG_CHECK(ag_power_watcher_at(0, late, &w));
    AG_CHECK(!w.listening);
    AG_CHECK(!ag_power_fit(APP_A));
}

/* Two listeners: the wait ends when the last one has answered. */
static void test_two_listeners(void)
{
    setup();
    poll_at(APP_A, 1000);
    poll_at(APP_B, 1000);

    const ag_power_target_t want = eco_target();
    AG_CHECK_INT(ag_power_begin(&want, AG_POWER_USER, 1000, AG_POWER_GRACE_MS),
                 AG_OK);

    AG_CHECK_INT(ag_power_reply(APP_A, AG_POWER_OK, NULL), AG_OK);
    AG_CHECK(!ag_power_settled(1100));
    AG_CHECK_INT(ag_power_reply(APP_B, AG_POWER_OK, NULL), AG_OK);
    AG_CHECK(ag_power_settled(1100));

    AG_CHECK_INT(ag_power_commit(NULL), AG_OK);
    AG_CHECK(ag_power_fit(APP_A));
    AG_CHECK(ag_power_fit(APP_B));
}

/*
 * A standing declaration is a standing answer.  The one that says any mode
 * suits it is never waited for and never ended; the one that says it needs the
 * full clock is never waited for either, and is ended by a commanded
 * transition with its own words as the reason.
 */
static void test_declarations(void)
{
    setup();
    AG_CHECK_INT(ag_power_declare(APP_A, AG_POWER_FIT_ANY, "nothing to decide"),
                 AG_OK);
    AG_CHECK_INT(ag_power_declare(APP_B, AG_POWER_FIT_FULL_ONLY,
                                  "22 kHz tract"),
                 AG_OK);
    poll_at(APP_A, 1000); /* even a polling one: the declaration stands */
    poll_at(APP_B, 1000);

    const ag_power_target_t want = eco_target();
    AG_CHECK_INT(ag_power_begin(&want, AG_POWER_USER, 1000, AG_POWER_GRACE_MS),
                 AG_OK);
    AG_CHECK(ag_power_settled(1000)); /* nobody to wait for */
    AG_CHECK_INT(ag_power_commit(NULL), AG_OK);

    AG_CHECK(ag_power_fit(APP_A));
    AG_CHECK(!ag_power_fit(APP_B));
    AG_CHECK_STR(ag_power_why(APP_B), "22 kHz tract");
    AG_CHECK(ag_power_full_only_held());

    /* Dropped again, and it stops counting. */
    AG_CHECK_INT(ag_power_declare(APP_B, AG_POWER_FIT_ASK, NULL), AG_OK);
    AG_CHECK(!ag_power_full_only_held());
    AG_CHECK_STR(ag_power_why(APP_B), "");
}

/* Answering UNFIT is the same verdict as saying nothing, said honestly - and it
 * carries a reason, which is the difference that matters to a person. */
static void test_answered_unfit(void)
{
    setup();
    poll_at(APP_A, 1000);

    const ag_power_target_t want = eco_target();
    AG_CHECK_INT(ag_power_begin(&want, AG_POWER_USER, 1000, AG_POWER_GRACE_MS),
                 AG_OK);
    AG_CHECK_INT(ag_power_reply(APP_A, AG_POWER_UNFIT, "mid-render"), AG_OK);
    AG_CHECK(ag_power_settled(1000));
    AG_CHECK_INT(ag_power_commit(NULL), AG_OK);

    AG_CHECK(!ag_power_fit(APP_A));
    AG_CHECK_STR(ag_power_why(APP_A), "mid-render");

    /* And the mode did change: an application saying it cannot cope is not a
     * veto.  That is what the killing replaced. */
    ag_power_target_t now;
    ag_power_current(&now);
    AG_CHECK_INT(now.mode, AG_POWER_ECO);
    AG_CHECK_INT(now.cpu_max_mhz, ECO_MHZ);
}

/* An automatic transition is advisory: the same silence, no consequence, and a
 * short wait.  What the caller does about it is powerctl's business, but the
 * cause has to survive the commit for it to be able to. */
static void test_auto_is_advisory(void)
{
    setup();
    poll_at(APP_A, 1000);

    ag_power_target_t want = target(AG_POWER_FULL, MAX_MHZ, false);
    AG_CHECK_INT(
        ag_power_begin(&want, AG_POWER_AUTO, 1000, AG_POWER_AUTO_GRACE_MS),
        AG_OK);
    AG_CHECK_INT(ag_power_cause(), AG_POWER_AUTO);
    AG_CHECK(!ag_power_settled(1100));
    AG_CHECK(ag_power_settled(1000 + AG_POWER_AUTO_GRACE_MS));
    AG_CHECK_INT(ag_power_commit(NULL), AG_OK);
    AG_CHECK_INT(ag_power_cause(), AG_POWER_AUTO);
    AG_CHECK(!ag_power_screen_on());
    AG_CHECK(!ag_power_fit(APP_A)); /* the verdict stands; nobody acts on it */
}

/* Coming back up is a transition like any other, and one that never has a
 * verdict attached: `power full` is what somebody types when something has gone
 * wrong, and it must not be able to end anything. */
static void test_raising_commits(void)
{
    setup();

    const ag_power_target_t eco = eco_target();
    AG_CHECK_INT(ag_power_begin(&eco, AG_POWER_USER, 1000, 0), AG_OK);
    AG_CHECK_INT(ag_power_commit(NULL), AG_OK);

    AG_CHECK_INT(ag_power_declare(APP_A, AG_POWER_FIT_FULL_ONLY, "still here"),
                 AG_OK);

    const ag_power_target_t full = target(AG_POWER_FULL, MAX_MHZ, true);
    AG_CHECK_INT(ag_power_begin(&full, AG_POWER_USER, 2000, 0), AG_OK);
    AG_CHECK_INT(ag_power_commit(NULL), AG_OK);

    ag_power_target_t now;
    ag_power_current(&now);
    AG_CHECK_INT(now.cpu_max_mhz, MAX_MHZ);
    AG_CHECK_INT(now.mode, AG_POWER_FULL);
}

/* Two transitions at once would count two sets of answers against one
 * deadline. */
static void test_one_transition_at_a_time(void)
{
    setup();

    const ag_power_target_t want = eco_target();
    AG_CHECK_INT(ag_power_begin(&want, AG_POWER_USER, 1000, AG_POWER_GRACE_MS),
                 AG_OK);
    AG_CHECK_INT(ag_power_begin(&want, AG_POWER_AUTO, 1000, AG_POWER_GRACE_MS),
                 -AG_EBUSY);

    ag_power_cancel();
    AG_CHECK_INT(ag_power_begin(&want, AG_POWER_USER, 1000, AG_POWER_GRACE_MS),
                 AG_OK);
}

/* A cancelled transition changes nothing, and leaves nothing pending. */
static void test_cancel(void)
{
    setup();

    const ag_power_target_t want = target(AG_POWER_DOZE, ECO_MHZ, false);
    AG_CHECK_INT(ag_power_begin(&want, AG_POWER_USER, 1000, AG_POWER_GRACE_MS),
                 AG_OK);
    ag_power_cancel();
    AG_CHECK_INT(ag_power_commit(NULL), -AG_ENOENT);

    ag_power_target_t now;
    ag_power_current(&now);
    AG_CHECK_INT(now.cpu_max_mhz, MAX_MHZ);
    AG_CHECK(now.screen_on);

    ag_power_status_t st;
    AG_CHECK_INT(ag_power_poll(APP_A, 1100, &st, 240), AG_OK);
    AG_CHECK_INT(st.pending, st.mode);
    AG_CHECK_INT(st.grace_ms, 0);
}

/*
 * A process that ends - or crashes - takes its declaration with it.  Otherwise
 * the first application to say it needs the full clock and then fault would
 * stop the idle timer for ever, with nothing anywhere saying whose word it was.
 */
static void test_forget(void)
{
    setup();
    AG_CHECK_INT(ag_power_declare(APP_A, AG_POWER_FIT_FULL_ONLY, "gone soon"),
                 AG_OK);
    AG_CHECK_INT(ag_power_watcher_count(), 1);
    AG_CHECK(ag_power_full_only_held());

    ag_power_forget(APP_A);
    AG_CHECK_INT(ag_power_watcher_count(), 0);
    AG_CHECK(!ag_power_full_only_held());
    AG_CHECK(!ag_power_fit(APP_A));
}

/* Every answer counts for one transition only: fitness is not inherited by the
 * next mode change, or one refusal would be a permanent licence. */
static void test_answers_do_not_carry_over(void)
{
    setup();
    poll_at(APP_A, 1000);

    const ag_power_target_t eco = eco_target();
    AG_CHECK_INT(ag_power_begin(&eco, AG_POWER_USER, 1000, 0), AG_OK);
    AG_CHECK_INT(ag_power_reply(APP_A, AG_POWER_OK, NULL), AG_OK);
    AG_CHECK_INT(ag_power_commit(NULL), AG_OK);
    AG_CHECK(ag_power_fit(APP_A));

    const ag_power_target_t doze = target(AG_POWER_DOZE, ECO_MHZ, false);
    AG_CHECK_INT(ag_power_begin(&doze, AG_POWER_USER, 2000, 0), AG_OK);
    AG_CHECK(!ag_power_fit(APP_A)); /* asked again, has not answered yet */
    AG_CHECK_INT(ag_power_commit(NULL), AG_OK);
    AG_CHECK(!ag_power_fit(APP_A));
}

/* Nonsense in, error out. */
static void test_rejects_nonsense(void)
{
    setup();

    ag_power_target_t bad = eco_target();
    bad.cpu_min_mhz = 0;
    AG_CHECK_INT(ag_power_begin(&bad, AG_POWER_USER, 1000, 0), -AG_EINVAL);

    bad = eco_target();
    bad.cpu_min_mhz = 240;
    bad.cpu_max_mhz = 80;
    AG_CHECK_INT(ag_power_begin(&bad, AG_POWER_USER, 1000, 0), -AG_EINVAL);

    AG_CHECK_INT(ag_power_begin(NULL, AG_POWER_USER, 1000, 0), -AG_EINVAL);

    const ag_power_target_t want = eco_target();
    AG_CHECK_INT(ag_power_begin(&want, (ag_power_cause_t)7, 1000, 0),
                 -AG_EINVAL);
    AG_CHECK_INT(ag_power_begin(&want, AG_POWER_USER, 1000, AG_POWER_GRACE_MS),
                 AG_OK);
    AG_CHECK_INT(ag_power_reply(APP_A, AG_POWER_ANSWER_NONE, NULL), -AG_EINVAL);
    AG_CHECK_INT(ag_power_declare(APP_A, (ag_power_fitness_t)9, NULL),
                 -AG_EINVAL);
    AG_CHECK_INT(ag_power_poll(APP_A, 1000, NULL, 240), -AG_EINVAL);
}

/*
 * Seven weeks of uptime is where a millisecond counter in a uint32_t comes
 * round, and a board that has been up that long must not stop changing modes
 * because two numbers crossed zero.
 */
static void test_millisecond_wrap(void)
{
    setup();

    const uint32_t nearly = 0xffffff00u;
    poll_at(APP_A, nearly);

    const ag_power_target_t want = eco_target();
    AG_CHECK_INT(ag_power_begin(&want, AG_POWER_USER, nearly, 500), AG_OK);
    AG_CHECK(!ag_power_settled(nearly + 100));
    AG_CHECK(ag_power_settled(nearly + 500)); /* wrapped past zero */
    AG_CHECK_INT(ag_power_commit(NULL), AG_OK);
}

/* Answering without ever polling still works: an application may decide to
 * park on the strength of something else it noticed. */
static void test_answer_without_polling(void)
{
    setup();

    const ag_power_target_t want = eco_target();
    AG_CHECK_INT(ag_power_begin(&want, AG_POWER_USER, 1000, AG_POWER_GRACE_MS),
                 AG_OK);
    AG_CHECK_INT(ag_power_reply(APP_B, AG_POWER_PARKED, NULL), AG_OK);
    AG_CHECK_INT(ag_power_watcher_count(), 1);
    AG_CHECK(ag_power_settled(1000));
    AG_CHECK_INT(ag_power_commit(NULL), AG_OK);
    AG_CHECK(ag_power_fit(APP_B));
}

void run_power_tests(void)
{
    test_starts_at_full();
    test_kernel_is_not_a_watcher();
    test_no_listeners_settles_at_once();
    test_listener_answers();
    test_silence_settles_and_is_not_fit();
    test_unknown_process_is_not_fit();
    test_stale_listener_is_not_waited_for();
    test_two_listeners();
    test_declarations();
    test_answered_unfit();
    test_auto_is_advisory();
    test_raising_commits();
    test_one_transition_at_a_time();
    test_cancel();
    test_forget();
    test_answers_do_not_carry_over();
    test_rejects_nonsense();
    test_millisecond_wrap();
    test_answer_without_polling();
}
