/*
 * ArgonOS - power mode tests.
 *
 * What is being tested is a promise with two halves that pull against each
 * other, and both halves matter.  An application must be told before the
 * machine changes under it and must be able to say that it cannot run like
 * that - otherwise a realtime path breaks silently.  And an application must
 * not be able to keep the machine at full speed by saying nothing - otherwise
 * one wedged process is a battery that goes flat.
 *
 * So: silence settles the transition at the deadline and counts as consent, a
 * hold refuses a command and is overridden by force, a process that stopped
 * polling is not waited for, raising the clock is never refused, and everything
 * a process asked for goes when the process does.
 *
 * The time is an argument to every call in power.c, which is why all of this
 * runs in microseconds here rather than in half seconds on a board.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/power.h>

#include "test.h"

#define APP_A ((ag_pid_t)1)
#define APP_B ((ag_pid_t)2)

#define MAX_MHZ 240u
#define ECO_MHZ 80u

static ag_power_target_t eco_target(uint32_t mhz, bool screen_on)
{
    ag_power_target_t t;
    t.mode = (mhz < MAX_MHZ) ? AG_POWER_ECO : AG_POWER_FULL;
    t.cpu_min_mhz = mhz;
    t.cpu_max_mhz = mhz;
    t.screen_on = screen_on;
    return t;
}

/* A process that polls at `at_ms` is one the system will wait for. */
static void poll_at(ag_pid_t pid, uint32_t at_ms)
{
    ag_power_status_t st;
    AG_CHECK_INT(ag_power_poll(pid, at_ms, &st, MAX_MHZ), AG_OK);
}

static void setup(void)
{
    ag_power_init(MAX_MHZ);
}

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
}

static void test_no_listeners_settles_at_once(void)
{
    setup();

    const ag_power_target_t want = eco_target(ECO_MHZ, true);
    AG_CHECK_INT(ag_power_begin(&want, 1000, AG_POWER_GRACE_MS), AG_OK);
    AG_CHECK(ag_power_settled(1000));

    ag_power_target_t got;
    AG_CHECK_INT(ag_power_commit(false, &got), AG_OK);
    AG_CHECK_INT(got.cpu_max_mhz, ECO_MHZ);
    AG_CHECK_INT(ag_power_mode(), AG_POWER_ECO);
}

/* An application that polls is waited for, and its answer ends the wait. */
static void test_listener_answers(void)
{
    setup();
    poll_at(APP_A, 900);
    AG_CHECK_INT(ag_power_watcher_count(), 1);

    const ag_power_target_t want = eco_target(ECO_MHZ, true);
    AG_CHECK_INT(ag_power_begin(&want, 1000, AG_POWER_GRACE_MS), AG_OK);
    AG_CHECK(!ag_power_settled(1000));

    /* What it sees when it polls during the transition: the numbers, not just
     * the name of the mode. */
    ag_power_status_t st;
    AG_CHECK_INT(ag_power_poll(APP_A, 1010, &st, 240), AG_OK);
    AG_CHECK_INT(st.mode, AG_POWER_FULL);
    AG_CHECK_INT(st.pending, AG_POWER_ECO);
    AG_CHECK_INT(st.cpu_max_mhz, MAX_MHZ);
    AG_CHECK_INT(st.pending_cpu_max_mhz, ECO_MHZ);
    AG_CHECK_INT(st.cpu_mhz, 240);
    AG_CHECK_INT(st.grace_ms, AG_POWER_GRACE_MS - 10);

    AG_CHECK_INT(ag_power_reply(APP_A, AG_POWER_PARKED, NULL), AG_OK);
    AG_CHECK(ag_power_settled(1010));

    ag_power_target_t got;
    AG_CHECK_INT(ag_power_commit(false, &got), AG_OK);
    AG_CHECK_INT(got.cpu_max_mhz, ECO_MHZ);

    /* And the row the `power` command prints afterwards. */
    ag_power_watcher_t w;
    AG_CHECK(ag_power_watcher_at(0, 1010, &w));
    AG_CHECK_INT(w.pid, APP_A);
    AG_CHECK_INT(w.answer, AG_POWER_PARKED);
    AG_CHECK(!w.hold);
    AG_CHECK(w.listening);
}

/* Silence is consent, but only at the deadline: before it, the wait goes on. */
static void test_silence_settles_at_the_deadline(void)
{
    setup();
    poll_at(APP_A, 1000);

    const ag_power_target_t want = eco_target(ECO_MHZ, true);
    AG_CHECK_INT(ag_power_begin(&want, 1000, 500), AG_OK);
    AG_CHECK(!ag_power_settled(1400));
    AG_CHECK(ag_power_settled(1500));

    ag_power_target_t got;
    AG_CHECK_INT(ag_power_commit(false, &got), AG_OK);
    AG_CHECK_INT(got.cpu_max_mhz, ECO_MHZ);

    ag_power_watcher_t w;
    AG_CHECK(ag_power_watcher_at(0, 1500, &w));
    AG_CHECK_INT(w.answer, AG_POWER_ANSWER_NONE);
    AG_CHECK(w.listening); /* it was listening; it just did not answer */
}

/*
 * A process that polled once and stopped is not listening.  Waiting the full
 * grace period for it on every mode change would make the command feel broken
 * while telling nobody anything.
 */
static void test_stale_listener_is_not_waited_for(void)
{
    setup();
    poll_at(APP_A, 1000);

    const ag_power_target_t want = eco_target(ECO_MHZ, true);
    const uint32_t          late = 1000 + AG_POWER_LISTEN_MS + 1;
    AG_CHECK_INT(ag_power_begin(&want, late, AG_POWER_GRACE_MS), AG_OK);
    AG_CHECK(ag_power_settled(late));

    ag_power_watcher_t w;
    AG_CHECK(ag_power_watcher_at(0, late, &w));
    AG_CHECK(!w.listening);
}

/* Two listeners: the wait ends when the last one has answered. */
static void test_two_listeners(void)
{
    setup();
    poll_at(APP_A, 1000);
    poll_at(APP_B, 1000);

    const ag_power_target_t want = eco_target(ECO_MHZ, true);
    AG_CHECK_INT(ag_power_begin(&want, 1000, AG_POWER_GRACE_MS), AG_OK);

    AG_CHECK_INT(ag_power_reply(APP_A, AG_POWER_OK, NULL), AG_OK);
    AG_CHECK(!ag_power_settled(1100));
    AG_CHECK_INT(ag_power_reply(APP_B, AG_POWER_OK, NULL), AG_OK);
    AG_CHECK(ag_power_settled(1100));
}

/* A hold refuses the command, names itself, and is not waited for. */
static void test_hold_refuses_and_force_overrides(void)
{
    setup();
    AG_CHECK_INT(ag_power_set_hold(APP_A, true, "22 kHz tract"), AG_OK);

    const ag_power_target_t want = eco_target(ECO_MHZ, true);
    AG_CHECK_INT(ag_power_begin(&want, 1000, AG_POWER_GRACE_MS), AG_OK);
    /* A standing hold has already answered, so there is nobody to wait for. */
    AG_CHECK(ag_power_settled(1000));
    AG_CHECK_INT(ag_power_commit(false, NULL), -AG_EPERM);

    /* Refused means nothing moved. */
    ag_power_target_t now;
    ag_power_current(&now);
    AG_CHECK_INT(now.cpu_max_mhz, MAX_MHZ);
    AG_CHECK_INT(now.mode, AG_POWER_FULL);

    /* And the hold is still there to be printed, with its reason. */
    ag_power_watcher_t w;
    AG_CHECK(ag_power_watcher_at(0, 1000, &w));
    AG_CHECK_INT(w.pid, APP_A);
    AG_CHECK(w.hold);
    AG_CHECK_STR(w.why, "22 kHz tract");

    /* Forced: the person at the console outranks the application. */
    AG_CHECK_INT(ag_power_begin(&want, 2000, AG_POWER_GRACE_MS), AG_OK);
    AG_CHECK_INT(ag_power_commit(true, NULL), AG_OK);
    ag_power_current(&now);
    AG_CHECK_INT(now.cpu_max_mhz, ECO_MHZ);
}

/* An answer of HOLD to this one transition refuses it exactly like a standing
 * hold - the difference is only how long it lasts. */
static void test_answered_hold_refuses(void)
{
    setup();
    poll_at(APP_A, 1000);

    const ag_power_target_t want = eco_target(ECO_MHZ, true);
    AG_CHECK_INT(ag_power_begin(&want, 1000, AG_POWER_GRACE_MS), AG_OK);
    AG_CHECK_INT(ag_power_reply(APP_A, AG_POWER_HOLD, "mid-render"), AG_OK);
    AG_CHECK(ag_power_settled(1000));
    AG_CHECK_INT(ag_power_commit(false, NULL), -AG_EPERM);

    ag_power_target_t now;
    ag_power_current(&now);
    AG_CHECK_INT(now.cpu_max_mhz, MAX_MHZ);
}

/*
 * Coming back up is never refused.  If it were, a wedged application would
 * make a reboot the only way out of eco - and `power full` is exactly what
 * somebody types when something has gone wrong.
 */
static void test_raising_is_never_refused(void)
{
    setup();

    const ag_power_target_t eco = eco_target(ECO_MHZ, true);
    AG_CHECK_INT(ag_power_begin(&eco, 1000, 0), AG_OK);
    AG_CHECK_INT(ag_power_commit(false, NULL), AG_OK);

    AG_CHECK_INT(ag_power_set_hold(APP_A, true, "still here"), AG_OK);

    const ag_power_target_t full = eco_target(MAX_MHZ, true);
    AG_CHECK_INT(ag_power_begin(&full, 2000, 0), AG_OK);
    AG_CHECK_INT(ag_power_commit(false, NULL), AG_OK);

    ag_power_target_t now;
    ag_power_current(&now);
    AG_CHECK_INT(now.cpu_max_mhz, MAX_MHZ);
    AG_CHECK_INT(now.mode, AG_POWER_FULL);
}

/*
 * A hold is about the clock and about nothing else.  An application that draws
 * has somewhere to put the decision to stop drawing; one whose arithmetic does
 * not fit in a slower processor has nowhere to put it at all.
 */
static void test_hold_does_not_block_the_screen(void)
{
    setup();
    AG_CHECK_INT(ag_power_set_hold(APP_A, true, "audio"), AG_OK);

    ag_power_target_t want;
    ag_power_current(&want);
    want.screen_on = false;

    AG_CHECK_INT(ag_power_begin(&want, 1000, 0), AG_OK);
    AG_CHECK_INT(ag_power_commit(false, NULL), AG_OK);
    AG_CHECK(!ag_power_screen_on());
    AG_CHECK_INT(ag_power_mode(), AG_POWER_FULL);
}

/* Two commands at once would count two sets of answers against one deadline. */
static void test_one_transition_at_a_time(void)
{
    setup();

    const ag_power_target_t want = eco_target(ECO_MHZ, true);
    AG_CHECK_INT(ag_power_begin(&want, 1000, AG_POWER_GRACE_MS), AG_OK);
    AG_CHECK_INT(ag_power_begin(&want, 1000, AG_POWER_GRACE_MS), -AG_EBUSY);

    ag_power_cancel();
    AG_CHECK_INT(ag_power_begin(&want, 1000, AG_POWER_GRACE_MS), AG_OK);
}

/* A cancelled transition changes nothing, and leaves nothing pending. */
static void test_cancel(void)
{
    setup();

    const ag_power_target_t want = eco_target(ECO_MHZ, false);
    AG_CHECK_INT(ag_power_begin(&want, 1000, AG_POWER_GRACE_MS), AG_OK);
    ag_power_cancel();
    AG_CHECK_INT(ag_power_commit(false, NULL), -AG_ENOENT);

    ag_power_target_t now;
    ag_power_current(&now);
    AG_CHECK_INT(now.cpu_max_mhz, MAX_MHZ);
    AG_CHECK(now.screen_on);

    /* And a status poll says nothing is coming. */
    ag_power_status_t st;
    AG_CHECK_INT(ag_power_poll(APP_A, 1100, &st, 240), AG_OK);
    AG_CHECK_INT(st.pending, st.mode);
    AG_CHECK_INT(st.grace_ms, 0);
}

/*
 * The one that matters most for a machine left running: a process that ends -
 * or crashes - takes its hold with it.  Otherwise the first application to
 * hold the clock and then fault pins the board at full speed until a reboot,
 * with nothing anywhere saying whose hold it was.
 */
static void test_forget_releases_the_hold(void)
{
    setup();
    AG_CHECK_INT(ag_power_set_hold(APP_A, true, "gone in a moment"), AG_OK);
    AG_CHECK_INT(ag_power_watcher_count(), 1);

    ag_power_forget(APP_A);
    AG_CHECK_INT(ag_power_watcher_count(), 0);

    const ag_power_target_t want = eco_target(ECO_MHZ, true);
    AG_CHECK_INT(ag_power_begin(&want, 1000, 0), AG_OK);
    AG_CHECK_INT(ag_power_commit(false, NULL), AG_OK);
}

/* A hold that is dropped by the application itself stops refusing. */
static void test_hold_released(void)
{
    setup();
    AG_CHECK_INT(ag_power_set_hold(APP_A, true, "for now"), AG_OK);
    AG_CHECK_INT(ag_power_set_hold(APP_A, false, NULL), AG_OK);

    const ag_power_target_t want = eco_target(ECO_MHZ, true);
    AG_CHECK_INT(ag_power_begin(&want, 1000, 0), AG_OK);
    AG_CHECK_INT(ag_power_commit(false, NULL), AG_OK);

    ag_power_watcher_t w;
    AG_CHECK(ag_power_watcher_at(0, 1000, &w));
    AG_CHECK(!w.hold);
    AG_CHECK_STR(w.why, "");
}

/* Nonsense in, error out: no clock band that means nothing, no answer that is
 * not one of the three. */
static void test_rejects_nonsense(void)
{
    setup();

    ag_power_target_t bad = eco_target(ECO_MHZ, true);
    bad.cpu_min_mhz = 0;
    AG_CHECK_INT(ag_power_begin(&bad, 1000, 0), -AG_EINVAL);

    bad = eco_target(ECO_MHZ, true);
    bad.cpu_min_mhz = 240;
    bad.cpu_max_mhz = 80;
    AG_CHECK_INT(ag_power_begin(&bad, 1000, 0), -AG_EINVAL);

    AG_CHECK_INT(ag_power_begin(NULL, 1000, 0), -AG_EINVAL);

    const ag_power_target_t want = eco_target(ECO_MHZ, true);
    AG_CHECK_INT(ag_power_begin(&want, 1000, AG_POWER_GRACE_MS), AG_OK);
    AG_CHECK_INT(ag_power_reply(APP_A, AG_POWER_ANSWER_NONE, NULL),
                 -AG_EINVAL);
    AG_CHECK_INT(ag_power_poll(APP_A, 1000, NULL, 240), -AG_EINVAL);
    AG_CHECK_INT(ag_power_set_hold(AG_PID_KERNEL, true, "no"), -AG_EPERM);
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

    const ag_power_target_t want = eco_target(ECO_MHZ, true);
    AG_CHECK_INT(ag_power_begin(&want, nearly, 500), AG_OK);
    AG_CHECK(!ag_power_settled(nearly + 100));
    AG_CHECK(ag_power_settled(nearly + 500)); /* wrapped past zero */
    AG_CHECK_INT(ag_power_commit(false, NULL), AG_OK);
}

/* Answering without ever polling still works: an application may decide to
 * park on the strength of something else it noticed. */
static void test_answer_without_polling(void)
{
    setup();

    const ag_power_target_t want = eco_target(ECO_MHZ, true);
    AG_CHECK_INT(ag_power_begin(&want, 1000, AG_POWER_GRACE_MS), AG_OK);
    AG_CHECK_INT(ag_power_reply(APP_B, AG_POWER_PARKED, NULL), AG_OK);
    AG_CHECK_INT(ag_power_watcher_count(), 1);
    AG_CHECK(ag_power_settled(1000));
}

void run_power_tests(void)
{
    test_starts_at_full();
    test_kernel_is_not_a_watcher();
    test_no_listeners_settles_at_once();
    test_listener_answers();
    test_silence_settles_at_the_deadline();
    test_stale_listener_is_not_waited_for();
    test_two_listeners();
    test_hold_refuses_and_force_overrides();
    test_answered_hold_refuses();
    test_raising_is_never_refused();
    test_hold_does_not_block_the_screen();
    test_one_transition_at_a_time();
    test_cancel();
    test_forget_releases_the_hold();
    test_hold_released();
    test_rejects_nonsense();
    test_millisecond_wrap();
    test_answer_without_polling();
}
