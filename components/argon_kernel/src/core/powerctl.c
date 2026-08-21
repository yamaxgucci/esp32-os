/*
 * ArgonOS - doing what power.c decided.
 *
 * The clock through argon/port/power.h, the screen through whichever driver
 * owns the panel, the waiting through the scheduler, and one line in the
 * journal so that a board found running slowly a week later can be asked why.
 *
 * Nothing here decides anything: the order is announce, wait, commit, apply,
 * and the middle two live next door in power.c where they can be tested
 * without a machine under them.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/power.h>

#include <argon/device.h>
#include <argon/display.h>
#include <argon/log.h>
#include <argon/textpanel.h>

#include <argon/port/power.h>
#include <argon/port/task.h>
#include <argon/port/time.h>

/* How often the wait loop looks again.  Ten milliseconds is well inside a
 * frame, so an application polling once a frame is never the reason the
 * command feels slow. */
#define AG_POWERCTL_STEP_MS 10

static bool s_backlight_ok;

static uint32_t now_ms(void) { return (uint32_t)(ag_port_us() / 1000); }

static const char *mode_name(ag_power_mode_t m)
{
    switch (m) {
    case AG_POWER_FULL: return "full";
    case AG_POWER_ECO:  return "eco";
    case AG_POWER_DOZE: return "doze";
    default:            return "?";
    }
}

/* ---------------------------------------------------------------------- */

ag_err_t ag_powerctl_init(void)
{
    uint16_t steps[AG_PORT_PWR_STEPS_MAX];
    const uint32_t n = ag_port_power_cpu_steps(steps, AG_PORT_PWR_STEPS_MAX);

    /*
     * out[0] is the maximum by contract.  A port that answered nothing at all
     * would leave the system with no idea how fast it is; the live reading is
     * the honest fallback, and it is what this machine is running at right now
     * by definition.
     */
    const uint32_t max = (n > 0u) ? (uint32_t)steps[0] : ag_port_power_cpu_mhz();
    ag_power_init(max);
    s_backlight_ok = false;
    return AG_OK;
}

uint32_t ag_powerctl_cpu_mhz(void) { return ag_port_power_cpu_mhz(); }

uint32_t ag_powerctl_cpu_steps(uint16_t *out, uint32_t max)
{
    return ag_port_power_cpu_steps(out, max);
}

bool ag_powerctl_can_scale(void)
{
    return (ag_port_power_caps() & AG_PORT_PWR_CPU_BAND) != 0u;
}

bool ag_powerctl_backlight_works(void) { return s_backlight_ok; }

/* ---------------------------------------------------------------------- */

/*
 * The panel is asked rather than told about, for the same reason the console
 * asks it to draw a row: the driver is loadable, so the pin the backlight is
 * on belongs to code that may not be there.  Every display in the registry
 * gets the request; the ones that answer -AG_ENOTSUP have no light to switch.
 */
static void backlight_broadcast(uint8_t percent)
{
    bool taken = false;

    for (uint32_t i = 0;; i++) {
        ag_devinfo_t info;
        if (ag_dev_info(i, AG_DEV_DISPLAY, &info) != AG_OK) {
            break;
        }
        ag_device_t *dev = ag_dev_find(info.name);
        if (dev == NULL) {
            continue;
        }
        uint8_t        arg = percent;
        const ag_err_t err =
            ag_dev_ioctl(dev, AG_IOC_DISPLAY_BACKLIGHT, &arg, sizeof(arg));
        if (err == AG_OK) {
            taken = true;
        }
    }

    s_backlight_ok = taken;
}

static void apply_screen(bool on)
{
    /*
     * Order matters on the way down and on the way up, and it is not the same
     * order.  Going dark: stop sending pixels first, then put the light out,
     * so nothing is half drawn on a panel that is about to go black.  Coming
     * back: light first, then repaint, because a panel lit with last week's
     * contents on it for one tick is what a person reads as a glitch.
     */
    if (!on) {
        ag_display_power(false);
        ag_textpanel_enable(false);
        backlight_broadcast(0);
    } else {
        backlight_broadcast(100);
        ag_display_power(true);
        ag_textpanel_enable(true);
    }
}

static ag_err_t apply_clock(const ag_power_target_t *t)
{
    if (!ag_powerctl_can_scale()) {
        return -AG_ENOTSUP;
    }
    return ag_port_power_cpu_band(t->cpu_min_mhz, t->cpu_max_mhz);
}

/* Put the table back where it was, when the machine refused what it said. */
static void table_restore(const ag_power_target_t *prev)
{
    if (ag_power_begin(prev, now_ms(), 0u) == AG_OK) {
        (void)ag_power_commit(true, NULL);
    }
}

ag_err_t ag_powerctl_apply(const ag_power_target_t *want, bool force)
{
    if (want == NULL) {
        return -AG_EINVAL;
    }

    ag_power_target_t prev;
    ag_power_current(&prev);

    ag_power_target_t asked = *want;
    bool              clock_moves = (asked.cpu_min_mhz != prev.cpu_min_mhz) ||
                       (asked.cpu_max_mhz != prev.cpu_max_mhz);

    /*
     * A machine whose clock is fixed still has modes.  Two of the three things
     * a mode does - the screen, and telling the applications - work here
     * exactly as they do anywhere, so the mode is entered and only the clock
     * stays put.
     *
     * What must not happen is the table claiming a frequency the chip is not
     * running at: `power` would then print eighty megahertz on a machine doing
     * two hundred and forty, which is the one thing this command exists to be
     * able to trust.  So the band asked for is replaced with the band there is,
     * before anybody is told about it - the applications are asked about what
     * will actually happen, not about what was typed.
     */
    if (clock_moves && !ag_powerctl_can_scale()) {
        asked.cpu_min_mhz = prev.cpu_min_mhz;
        asked.cpu_max_mhz = prev.cpu_max_mhz;
        clock_moves = false;
    }

    ag_err_t err = ag_power_begin(&asked, now_ms(), AG_POWER_GRACE_MS);
    if (err != AG_OK) {
        return err;
    }

    while (!ag_power_settled(now_ms())) {
        ag_port_task_delay(ag_port_ms_to_ticks(AG_POWERCTL_STEP_MS));
    }

    ag_power_target_t applied;
    err = ag_power_commit(force, &applied);
    if (err != AG_OK) {
        return err; /* -AG_EPERM: a hold refused it, and the holds are still
                     * in the watcher list for the caller to print */
    }

    if (clock_moves) {
        const ag_err_t clk = apply_clock(&applied);
        if (clk != AG_OK) {
            /*
             * The machine said no after the table said yes.  Anything but
             * putting the table back would leave `power` printing a frequency
             * the chip is not running at, which is the one thing this command
             * exists to be able to trust.
             */
            table_restore(&prev);
            ag_log(AG_LOG_ERROR, "power",
                   "%u-%u MHz refused by the machine (%d); staying at %u-%u",
                   (unsigned)applied.cpu_min_mhz, (unsigned)applied.cpu_max_mhz,
                   (int)clk, (unsigned)prev.cpu_min_mhz,
                   (unsigned)prev.cpu_max_mhz);
            return clk;
        }
    }

    if (applied.screen_on != prev.screen_on) {
        apply_screen(applied.screen_on);
    }

    uint32_t parked = 0, silent = 0, held = 0;
    for (uint32_t i = 0;; i++) {
        ag_power_watcher_t w;
        if (!ag_power_watcher_at(i, now_ms(), &w)) {
            break;
        }
        if (w.hold) {
            held++;
        } else if (w.answer == AG_POWER_PARKED) {
            parked++;
        } else if (w.answer == AG_POWER_ANSWER_NONE && w.listening) {
            silent++;
        }
    }

    ag_log(AG_LOG_INFO, "power",
           "%s: cpu %u-%u MHz, screen %s (%u parked, %u silent, %u held%s)",
           mode_name(applied.mode), (unsigned)applied.cpu_min_mhz,
           (unsigned)applied.cpu_max_mhz, applied.screen_on ? "on" : "off",
           (unsigned)parked, (unsigned)silent, (unsigned)held,
           force ? ", forced" : "");

    return AG_OK;
}
