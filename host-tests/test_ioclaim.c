/*
 * ArgonOS - pin ownership tests.
 *
 * The rule being tested is small enough to state in a sentence and easy enough
 * to get almost right: a reserved pin refuses configuration and writing, a pin
 * held by somebody else refuses the same, anybody may read any pin, and a
 * process that ends gives back everything it held.  The last one is the reason
 * this is tested at all - a pin left driving, or an interrupt handler left
 * installed on code that has been freed, is a board that misbehaves later and
 * somewhere else.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/ioclaim.h>
#include <argon/proc.h>

#include <string.h>

#include "test.h"

#define APP_A ((ag_pid_t)1)
#define APP_B ((ag_pid_t)2)

/* What the release callback saw, so the order and the contents can be checked. */
static int  g_released[8];
static bool g_released_isr[8];
static int  g_release_count;

static void note_release(int pin, bool had_isr, void *ctx)
{
    (void)ctx;
    if (g_release_count < (int)(sizeof(g_released) / sizeof(g_released[0]))) {
        g_released[g_release_count] = pin;
        g_released_isr[g_release_count] = had_isr;
    }
    g_release_count++;
}

static void setup(void)
{
    AG_CHECK_INT(ag_io_claims_init(48), AG_OK);
    g_release_count = 0;
    memset(g_released, 0, sizeof(g_released));
    memset(g_released_isr, 0, sizeof(g_released_isr));
}

/* ---------------------------------------------------------------------- */

static void test_empty_table(void)
{
    setup();

    AG_CHECK_INT(ag_io_pin_count(), 48);
    AG_CHECK_INT(ag_io_claimed_count(), 0);

    ag_pin_info_t info;
    AG_CHECK_INT(ag_io_pin_info(0, &info), AG_OK);
    AG_CHECK_INT(info.state, AG_PIN_FREE);
    AG_CHECK_STR(info.why, "");

    /* A pin the chip does not have is out of range, not free. */
    AG_CHECK_INT(ag_io_pin_info(48, &info), -AG_ERANGE);
    AG_CHECK_INT(ag_io_claim(48, APP_A, "x"), -AG_ERANGE);
    AG_CHECK_INT(ag_io_claim(-1, APP_A, "x"), -AG_ERANGE);
}

static void test_claim_and_release(void)
{
    setup();

    AG_CHECK_INT(ag_io_claim(5, APP_A, "gpio out"), AG_OK);
    AG_CHECK_INT(ag_io_claimed_count(), 1);
    AG_CHECK(ag_io_held_by(5, APP_A));
    AG_CHECK(!ag_io_held_by(5, APP_B));

    ag_pin_info_t info;
    AG_CHECK_INT(ag_io_pin_info(5, &info), AG_OK);
    AG_CHECK_INT(info.state, AG_PIN_HELD);
    AG_CHECK_INT(info.owner, APP_A);
    AG_CHECK_STR(info.why, "gpio out");

    /* Claiming your own pin again is reconfiguring it, which is ordinary. */
    AG_CHECK_INT(ag_io_claim(5, APP_A, "pwm"), AG_OK);
    AG_CHECK_INT(ag_io_pin_info(5, &info), AG_OK);
    AG_CHECK_STR(info.why, "pwm");

    AG_CHECK_INT(ag_io_release(5, APP_A), AG_OK);
    AG_CHECK_INT(ag_io_claimed_count(), 0);
    /* And releasing it twice is a mistake worth being told about. */
    AG_CHECK_INT(ag_io_release(5, APP_A), -AG_ENOENT);
}

static void test_two_owners(void)
{
    setup();

    AG_CHECK_INT(ag_io_claim(7, APP_A, "gpio out"), AG_OK);
    /* The whole reason the table exists. */
    AG_CHECK_INT(ag_io_claim(7, APP_B, "gpio out"), -AG_EBUSY);
    /* And taking it by releasing it is not a way round that. */
    AG_CHECK_INT(ag_io_release(7, APP_B), -AG_EPERM);
    AG_CHECK(ag_io_held_by(7, APP_A));

    /* Writable means "free, or already mine". */
    AG_CHECK(ag_io_writable_by(7, APP_A));
    AG_CHECK(!ag_io_writable_by(7, APP_B));
    AG_CHECK(ag_io_writable_by(8, APP_B));

    /* Once the holder is done, the other one gets it. */
    AG_CHECK_INT(ag_io_release(7, APP_A), AG_OK);
    AG_CHECK_INT(ag_io_claim(7, APP_B, "gpio in"), AG_OK);
}

static void test_reserved(void)
{
    setup();

    AG_CHECK_INT(ag_io_reserve(43, "console tx"), AG_OK);

    /*
     * -AG_EACCES rather than -AG_EBUSY, and the difference matters: one says
     * "wait", the other says "never".  Only one of them is worth retrying.
     */
    AG_CHECK_INT(ag_io_claim(43, APP_A, "gpio out"), -AG_EACCES);
    AG_CHECK(!ag_io_writable_by(43, APP_A));
    AG_CHECK(!ag_io_writable_by(43, AG_PID_KERNEL));
    AG_CHECK_INT(ag_io_release(43, AG_PID_KERNEL), -AG_EACCES);

    ag_pin_info_t info;
    AG_CHECK_INT(ag_io_pin_info(43, &info), AG_OK);
    AG_CHECK_INT(info.state, AG_PIN_RESERVED);
    AG_CHECK_STR(info.why, "console tx");

    /*
     * The system does not take a pin from a running application, and must not
     * be able to: a bus and the shell are both the kernel, so a reservation
     * that overrode a claim would let the shell pull SDA out from under a
     * driver mid-transaction and call it ownership.
     */
    AG_CHECK_INT(ag_io_claim(14, APP_A, "gpio out"), AG_OK);
    AG_CHECK(!ag_io_reservable(14));
    AG_CHECK_INT(ag_io_reserve(14, "i2c0"), -AG_EBUSY);
    AG_CHECK(ag_io_held_by(14, APP_A));

    /* Once the application lets go, the bus can have it. */
    AG_CHECK_INT(ag_io_release(14, APP_A), AG_OK);
    AG_CHECK(ag_io_reservable(14));
    AG_CHECK_INT(ag_io_reserve(14, "i2c0"), AG_OK);
    AG_CHECK_INT(ag_io_pin_info(14, &info), AG_OK);
    AG_CHECK_INT(info.state, AG_PIN_RESERVED);
    AG_CHECK_STR(info.why, "i2c0");

    /* Reserving what is already reserved is what a remount does; it is fine. */
    AG_CHECK(ag_io_reservable(14));
    AG_CHECK_INT(ag_io_reserve(14, "i2c0"), AG_OK);
}

static void test_reason_is_truncated_not_overrun(void)
{
    setup();

    AG_CHECK_INT(ag_io_claim(3, APP_A, "a reason far longer than the column"),
                 AG_OK);

    ag_pin_info_t info;
    AG_CHECK_INT(ag_io_pin_info(3, &info), AG_OK);
    AG_CHECK_INT((int)strlen(info.why), AG_IO_REASON_MAX - 1);
    AG_CHECK_STR(info.why, "a reason far lo");
}

static void test_isr_is_recorded(void)
{
    setup();

    /* Only the holder may put a handler on a pin. */
    AG_CHECK_INT(ag_io_set_isr(9, APP_A, true), -AG_EPERM);
    AG_CHECK_INT(ag_io_claim(9, APP_A, "gpio in"), AG_OK);
    AG_CHECK_INT(ag_io_set_isr(9, APP_B, true), -AG_EPERM);
    AG_CHECK_INT(ag_io_set_isr(9, APP_A, true), AG_OK);

    ag_pin_info_t info;
    AG_CHECK_INT(ag_io_pin_info(9, &info), AG_OK);
    AG_CHECK(info.isr);

    AG_CHECK_INT(ag_io_set_isr(9, APP_A, false), AG_OK);
    AG_CHECK_INT(ag_io_pin_info(9, &info), AG_OK);
    AG_CHECK(!info.isr);
}

static void test_release_by_owner(void)
{
    setup();

    AG_CHECK_INT(ag_io_claim(4, APP_A, "gpio out"), AG_OK);
    AG_CHECK_INT(ag_io_claim(6, APP_A, "gpio in"), AG_OK);
    AG_CHECK_INT(ag_io_set_isr(6, APP_A, true), AG_OK);
    AG_CHECK_INT(ag_io_claim(8, APP_B, "pwm"), AG_OK);
    AG_CHECK_INT(ag_io_reserve(43, "console tx"), AG_OK);

    /* The process ends: its two pins come back, and nobody else's. */
    AG_CHECK_INT(ag_io_release_owner(APP_A, note_release, NULL), 2);
    AG_CHECK_INT(g_release_count, 2);
    AG_CHECK_INT(g_released[0], 4);
    AG_CHECK(!g_released_isr[0]);
    /* The handler is reported, so the caller can take it off the pin. */
    AG_CHECK_INT(g_released[1], 6);
    AG_CHECK(g_released_isr[1]);

    AG_CHECK(!ag_io_held_by(4, APP_A));
    AG_CHECK(ag_io_held_by(8, APP_B));

    ag_pin_info_t info;
    AG_CHECK_INT(ag_io_pin_info(43, &info), AG_OK);
    AG_CHECK_INT(info.state, AG_PIN_RESERVED);

    /* A second reclaim of the same process finds nothing left to do. */
    g_release_count = 0;
    AG_CHECK_INT(ag_io_release_owner(APP_A, note_release, NULL), 0);
    AG_CHECK_INT(g_release_count, 0);
}

static void test_a_running_bus_owns_its_pins(void)
{
    setup();

    /*
     * A bus reserves rather than claims, and the difference is the whole point.
     * The shell runs as the kernel and so does a driver, so a pin merely
     * "claimed by the kernel" would be a pin the shell could take back - which
     * is exactly the fight this table exists to stop, one level up from the one
     * everybody thinks of.
     */
    AG_CHECK_INT(ag_io_reserve(8, "i2c0"), AG_OK);
    AG_CHECK_INT(ag_io_reserve(9, "i2c0"), AG_OK);

    AG_CHECK_INT(ag_io_claim(8, APP_A, "gpio out"), -AG_EACCES);
    AG_CHECK_INT(ag_io_claim(8, AG_PID_KERNEL, "gpio out"), -AG_EACCES);
    AG_CHECK(!ag_io_writable_by(8, AG_PID_KERNEL));

    /* And reclaiming a process does not take a bus's pins with it. */
    AG_CHECK_INT(ag_io_release_owner(APP_A, note_release, NULL), 0);
    AG_CHECK_INT(ag_io_release_owner(AG_PID_KERNEL, note_release, NULL), 0);
    AG_CHECK_INT(ag_io_claimed_count(), 2);
}

void run_ioclaim_tests(void)
{
    test_empty_table();
    test_claim_and_release();
    test_two_owners();
    test_reserved();
    test_reason_is_truncated_not_overrun();
    test_isr_is_recorded();
    test_release_by_owner();
    test_a_running_bus_owns_its_pins();

    /* Leave the table empty for whatever runs next. */
    (void)ag_io_claims_init(0);
}
