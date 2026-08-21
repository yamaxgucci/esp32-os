/*
 * ArgonOS - `power`.
 *
 * Three things, and they are the three a person actually asks for: run slower,
 * put the screen out, and tell the applications so they can decide what to do
 * about it.  Everything it prints is read back from the machine rather than
 * remembered - the frequency above all, because a command that reports what it
 * asked for instead of what happened is worse than no command.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "shell/cmd_power.h"

#include <stdlib.h>
#include <string.h>

#include <argon/console.h>
#include <argon/path.h>
#include <argon/power.h>
#include <argon/proc.h>

#include <argon/port/power.h>
#include <argon/port/time.h>

/*
 * The default for `power eco` with no number.  Eighty megahertz is a third of
 * full speed and still a PLL setting, so the peripheral bus keeps a frequency
 * every driver in the tree has been run at.  Dropping to the crystal is a
 * bigger saving and a bigger risk, so it is available but not the default.
 */
#define AG_POWER_ECO_DEFAULT_MHZ 80

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

static const char *answer_name(ag_power_answer_t a)
{
    switch (a) {
    case AG_POWER_OK:     return "carrying on";
    case AG_POWER_PARKED: return "parked";
    case AG_POWER_HOLD:   return "held";
    default:              return "no answer";
    }
}

static const char *name_of(ag_pid_t pid)
{
    static char buf[32];

    for (uint32_t i = 0;; i++) {
        ag_procinfo_t info;
        if (ag_proc_info(i, &info) != AG_OK) {
            break;
        }
        if (info.pid == pid) {
            strncpy(buf, info.name, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            return buf;
        }
    }
    return "(gone)";
}

static void print_watchers(void)
{
    bool any = false;

    for (uint32_t i = 0;; i++) {
        ag_power_watcher_t w;
        if (!ag_power_watcher_at(i, now_ms(), &w)) {
            break;
        }
        if (!any) {
            ag_console_puts("  applications:\n");
            any = true;
        }
        ag_console_printf("    pid %-3u %-12s %s", (unsigned)w.pid,
                          name_of(w.pid),
                          w.hold ? "holds the clock" : answer_name(w.answer));
        if (!w.hold && !w.listening) {
            ag_console_puts(" (not polling)");
        }
        if (w.why[0] != '\0') {
            ag_console_printf(" - %s", w.why);
        }
        ag_console_puts("\n");
    }

    if (!any) {
        ag_console_puts("  applications: none is asking to be told\n");
    }
}

static int status(void)
{
    ag_power_target_t now;
    ag_power_current(&now);

    ag_console_printf("power: %s, cpu %u MHz", mode_name(now.mode),
                      (unsigned)ag_powerctl_cpu_mhz());
    if (now.cpu_min_mhz == now.cpu_max_mhz) {
        ag_console_printf(" (pinned %u)", (unsigned)now.cpu_max_mhz);
    } else {
        ag_console_printf(" (band %u-%u)", (unsigned)now.cpu_min_mhz,
                          (unsigned)now.cpu_max_mhz);
    }
    ag_console_printf(", screen %s\n", now.screen_on ? "on" : "off");

    uint16_t       steps[AG_PORT_PWR_STEPS_MAX];
    const uint32_t n = ag_powerctl_cpu_steps(steps, AG_PORT_PWR_STEPS_MAX);
    ag_console_puts("  steps:");
    for (uint32_t i = 0; i < n && i < AG_PORT_PWR_STEPS_MAX; i++) {
        ag_console_printf(" %u", (unsigned)steps[i]);
    }
    ag_console_puts(" MHz\n");

    if (!ag_powerctl_can_scale()) {
        /*
         * Said plainly, because everything else here works without it and the
         * difference is invisible otherwise: the mode changes, the screen goes
         * out, the applications are told, and the clock does not move.
         */
        ag_console_puts("  scaling: not in this build - the clock is fixed "
                        "(ARGON_ENABLE_PM)\n");
    }
    if (!now.screen_on && !ag_powerctl_backlight_works()) {
        ag_console_puts("  backlight: no driver took it - the glass is dark "
                        "only where this machine can make it\n");
    }

    print_watchers();
    return 0;
}

static void usage(void)
{
    ag_console_puts("usage: power [full | eco [mhz] | doze [mhz] | "
                    "screen on|off] [/force]\n");
    ag_console_puts("  eco   pin the clock lower; default "
                    "80 MHz\n");
    ag_console_puts("  doze  the same, and the screen off\n");
    ag_console_puts("  /force  go ahead even if an application asked to keep "
                    "the clock\n");
}

/* One of the machine's own settings, or zero. */
static uint32_t step_or_zero(const char *word)
{
    const long mhz = strtol(word, NULL, 10);
    if (mhz <= 0) {
        return 0u;
    }

    uint16_t       steps[AG_PORT_PWR_STEPS_MAX];
    const uint32_t n = ag_powerctl_cpu_steps(steps, AG_PORT_PWR_STEPS_MAX);
    for (uint32_t i = 0; i < n && i < AG_PORT_PWR_STEPS_MAX; i++) {
        if ((long)steps[i] == mhz) {
            return (uint32_t)mhz;
        }
    }
    return 0u;
}

static uint32_t eco_default(void)
{
    uint16_t       steps[AG_PORT_PWR_STEPS_MAX];
    const uint32_t n = ag_powerctl_cpu_steps(steps, AG_PORT_PWR_STEPS_MAX);
    if (n == 0u) {
        return 0u;
    }
    for (uint32_t i = 0; i < n && i < AG_PORT_PWR_STEPS_MAX; i++) {
        if (steps[i] == AG_POWER_ECO_DEFAULT_MHZ) {
            return AG_POWER_ECO_DEFAULT_MHZ;
        }
    }
    /* No 80 on this part: the lowest it has, which is the honest reading of
     * "as slow as you can". */
    const uint32_t last = (n < AG_PORT_PWR_STEPS_MAX) ? n : AG_PORT_PWR_STEPS_MAX;
    return (uint32_t)steps[last - 1];
}

static int refused(ag_err_t err, const ag_power_target_t *want)
{
    if (err == -AG_EPERM) {
        ag_console_puts("power: an application is holding the clock:\n");
        for (uint32_t i = 0;; i++) {
            ag_power_watcher_t w;
            if (!ag_power_watcher_at(i, now_ms(), &w)) {
                break;
            }
            if (!w.hold && w.answer != AG_POWER_HOLD) {
                continue;
            }
            ag_console_printf("  pid %u %s%s%s\n", (unsigned)w.pid,
                              name_of(w.pid), w.why[0] != '\0' ? " - " : "",
                              w.why);
        }
        ag_console_puts("  add /force to lower it anyway\n");
        return 1;
    }
    if (err == -AG_ENOTSUP) {
        ag_console_printf("power: this build cannot move the clock, so %u MHz "
                          "is not available (ARGON_ENABLE_PM)\n",
                          (unsigned)want->cpu_max_mhz);
        return 1;
    }
    if (err == -AG_EBUSY) {
        ag_console_puts("power: a change is already in flight; try again\n");
        return 1;
    }
    ag_console_printf("power: failed (%d)\n", (int)err);
    return 1;
}

int ag_cmd_power(int argc, char **argv)
{
    bool force = false;
    int  words = 0;
    char *word[3] = {NULL, NULL, NULL};

    for (int i = 1; i < argc; i++) {
        if (ag_path_icmp(argv[i], "/force") == 0) {
            force = true;
            continue;
        }
        if (words < 3) {
            word[words++] = argv[i];
        }
    }

    if (words == 0) {
        return status();
    }

    ag_power_target_t now;
    ag_power_current(&now);
    ag_power_target_t want = now;

    if (ag_path_icmp(word[0], "full") == 0) {
        want.mode = AG_POWER_FULL;
        want.cpu_min_mhz = 0u; /* filled below from the machine's maximum */
        want.cpu_max_mhz = 0u;
        want.screen_on = true;

        uint16_t       steps[AG_PORT_PWR_STEPS_MAX];
        const uint32_t n = ag_powerctl_cpu_steps(steps, AG_PORT_PWR_STEPS_MAX);
        if (n == 0u) {
            ag_console_puts("power: this machine reports no clock settings\n");
            return 1;
        }
        want.cpu_min_mhz = steps[0];
        want.cpu_max_mhz = steps[0];
    } else if (ag_path_icmp(word[0], "eco") == 0 ||
               ag_path_icmp(word[0], "doze") == 0) {
        const bool doze = (ag_path_icmp(word[0], "doze") == 0);
        uint32_t   mhz = (words > 1) ? step_or_zero(word[1]) : eco_default();
        if (mhz == 0u) {
            if (words > 1) {
                ag_console_printf("power: %s is not one of this machine's "
                                  "settings; `power` lists them\n",
                                  word[1]);
            } else {
                ag_console_puts("power: this machine reports no clock "
                                "settings\n");
            }
            return 1;
        }
        want.mode = doze ? AG_POWER_DOZE : AG_POWER_ECO;
        want.cpu_min_mhz = mhz;
        want.cpu_max_mhz = mhz;
        want.screen_on = !doze;
    } else if (ag_path_icmp(word[0], "screen") == 0) {
        if (words < 2) {
            usage();
            return 1;
        }
        if (ag_path_icmp(word[1], "on") == 0) {
            want.screen_on = true;
        } else if (ag_path_icmp(word[1], "off") == 0) {
            want.screen_on = false;
        } else {
            usage();
            return 1;
        }
        /*
         * The screen on its own does not change the mode: a dark screen at
         * full speed is a legitimate thing to want, and calling it `doze`
         * would be a lie in the one place a person looks to find out.
         */
    } else {
        usage();
        return 1;
    }

    if (want.mode == now.mode && want.screen_on == now.screen_on &&
        want.cpu_min_mhz == now.cpu_min_mhz &&
        want.cpu_max_mhz == now.cpu_max_mhz) {
        ag_console_printf("power: already %s, cpu %u MHz, screen %s\n",
                          mode_name(now.mode), (unsigned)now.cpu_max_mhz,
                          now.screen_on ? "on" : "off");
        return 0;
    }

    const ag_err_t err = ag_powerctl_apply(&want, force);
    if (err != AG_OK) {
        return refused(err, &want);
    }

    /*
     * Read back rather than assumed.  On a build without frequency scaling the
     * mode changes and the clock does not, and saying so here - next to the
     * command that was typed - is the difference between a system that looks
     * broken and one that is understood.
     */
    ag_power_target_t got;
    ag_power_current(&got);
    if (got.cpu_max_mhz != want.cpu_max_mhz) {
        ag_console_printf("power: the clock stayed at %u MHz - this build "
                          "cannot move it (ARGON_ENABLE_PM)\n",
                          (unsigned)got.cpu_max_mhz);
    }

    return status();
}
