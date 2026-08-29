/*
 * ArgonOS - `power`.
 *
 * Run slower, put the screen out, let the idle timer do both, and tell the
 * applications either way.  Everything it prints is read back from the machine
 * rather than remembered - the frequency above all, because a command that
 * reports what it asked for instead of what happened is worse than no command.
 *
 * The one thing this command does that no other does: a mode a person asks for
 * ends the applications that did not say they can live with it, and then this is
 * where they are named.  It is not softened with a confirmation prompt.  What it
 * replaces - an application left running at a third of its clock, failing in
 * whatever way its arithmetic fails - is worse than a process that is gone and
 * says so.
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

static uint32_t now_ms(void) { return (uint32_t)(ag_port_us() / 1000); }

static const char *mode_name(ag_power_mode_t m)
{
    switch (m) {
    case AG_POWER_FULL:   return "full";
    case AG_POWER_CRUISE: return "cruise";
    case AG_POWER_ECO:    return "eco";
    case AG_POWER_DOZE:   return "doze";
    default:              return "?";
    }
}

static const char *row_state(const ag_power_watcher_t *w)
{
    if (w->fitness == AG_POWER_FIT_ANY) {
        return "fit for any mode";
    }
    if (w->fitness == AG_POWER_FIT_FULL_ONLY) {
        return "needs the full clock";
    }
    switch (w->answer) {
    case AG_POWER_OK:     return "carrying on";
    case AG_POWER_PARKED: return "parked";
    case AG_POWER_UNFIT:  return "said it is not fit";
    default:              return "has not answered";
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
                          name_of(w.pid), row_state(&w));
        if (w.fitness == AG_POWER_FIT_ASK && !w.listening) {
            ag_console_puts(" (not polling)");
        }
        if (w.why[0] != '\0') {
            ag_console_printf(" - %s", w.why);
        }
        ag_console_puts("\n");
    }

    if (!any) {
        ag_console_puts("  applications: none has said anything\n");
    }
}

static void print_cruise(void)
{
    ag_power_auto_t a;
    ag_powerctl_auto_get(&a);

    if (a.cruise_mhz == 0u) {
        ag_console_puts("  cruise: off - the clock stays at its maximum until "
                        "asked otherwise\n");
        return;
    }
    ag_console_printf("  cruise: %u MHz when %s\n", (unsigned)a.cruise_mhz,
                      a.cruise_strict
                          ? "every application has said any mode suits it"
                          : "nobody has said it needs the full clock");
}

static void print_auto(void)
{
    ag_power_auto_t a;
    ag_powerctl_auto_get(&a);

    if (!a.on) {
        ag_console_puts("  idle timer: off (`power auto on`)\n");
        return;
    }
    ag_console_printf("  idle timer: on - screen off after %u s, %u MHz after "
                      "%u s; idle %u s now\n",
                      (unsigned)a.screen_off_s, (unsigned)a.eco_mhz,
                      (unsigned)a.eco_s,
                      (unsigned)(ag_console_idle_ms() / 1000u));
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

    /*
     * What the machine is holding back, in its own words.  The list above is
     * what it accepts right now, and a step that has quietly gone missing is
     * exactly the kind of thing a person should be told rather than left to
     * notice.
     */
    {
        const char *note = ag_port_power_note();
        if (note != NULL) {
            ag_console_printf("  held back: %s\n", note);
        }
    }

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

    print_cruise();
    print_auto();
    print_watchers();
    return 0;
}

static void usage(void)
{
    ag_console_puts("usage: power [full | cruise [on|off] | eco [mhz] | "
                    "doze [mhz] | screen on|off]\n");
    ag_console_puts("       power cruise on|off|silence|declared\n");
    ag_console_puts("       power auto [on|off] [screen_s] [eco_s]\n");
    ag_console_puts("  cruise  a step down (160 MHz) the system takes by "
                    "itself while\n"
                    "          nothing has said it needs the full clock; "
                    "on|off configures that\n");
    ag_console_puts("  eco     pin the clock lower; default 80 MHz\n");
    ag_console_puts("  doze    the same, and the screen off\n");
    ag_console_puts("  eco and doze end an application that does not answer "
                    "that it is fit;\n"
                    "  cruise ends only one that says outright that it is not; "
                    "`power auto` never\n"
                    "  ends anything\n");
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

static int cmd_auto(int words, char **word)
{
    ag_power_auto_t a;
    ag_powerctl_auto_get(&a);

    if (words < 2) {
        print_auto();
        return 0;
    }
    if (ag_path_icmp(word[1], "on") == 0) {
        a.on = true;
    } else if (ag_path_icmp(word[1], "off") == 0) {
        a.on = false;
    } else {
        usage();
        return 1;
    }

    /* Seconds, and zero means never - which is how one half of the ladder is
     * switched off without switching off the other. */
    if (words > 2) {
        a.screen_off_s = (uint32_t)strtol(word[2], NULL, 10);
    }
    if (words > 3) {
        a.eco_s = (uint32_t)strtol(word[3], NULL, 10);
    }

    const ag_err_t err = ag_powerctl_auto_set(&a);
    if (err != AG_OK) {
        ag_console_printf("power auto: failed (%d)\n", (int)err);
        return 1;
    }
    print_auto();
    ag_console_puts("  (in SYSTEM.CFG as [power] auto / screen_off_s / eco_s "
                    "to survive a reboot)\n");
    return 0;
}

/*
 * `power cruise on|off` configures the rung the system takes by itself; `power
 * cruise` with nothing after it enters it now, like any other mode.  Turning it
 * off while the machine is on it puts the clock back at once, because that is
 * what somebody typing it wants to see happen.
 */
static int cmd_cruise(const char *word)
{
    ag_power_auto_t a;
    ag_powerctl_auto_get(&a);

    if (ag_path_icmp(word, "on") == 0) {
        a.cruise_mhz = ag_powerctl_cruise_default();
        if (a.cruise_mhz == 0u) {
            ag_console_puts("power: this part has no middle clock setting to "
                            "cruise at\n");
            return 1;
        }
    } else if (ag_path_icmp(word, "off") == 0) {
        a.cruise_mhz = 0u;
    } else if (ag_path_icmp(word, "silence") == 0) {
        a.cruise_strict = false;
    } else if (ag_path_icmp(word, "declared") == 0) {
        /*
         * Here as well as in SYSTEM.CFG because the difference is invisible
         * until something is running, and the person who wants to see it is
         * standing at the console with something running.
         */
        a.cruise_strict = true;
    } else {
        usage();
        return 1;
    }

    const ag_err_t err = ag_powerctl_auto_set(&a);
    if (err != AG_OK) {
        ag_console_printf("power cruise: failed (%d)\n", (int)err);
        return 1;
    }

    if (a.cruise_mhz == 0u && ag_power_mode() == AG_POWER_CRUISE) {
        ag_power_target_t want;
        ag_power_current(&want);
        uint16_t       steps[AG_PORT_PWR_STEPS_MAX];
        const uint32_t n = ag_powerctl_cpu_steps(steps, AG_PORT_PWR_STEPS_MAX);
        if (n > 0u) {
            want.mode = AG_POWER_FULL;
            want.cpu_min_mhz = steps[0];
            want.cpu_max_mhz = steps[0];
            (void)ag_powerctl_apply(&want, AG_POWER_USER, NULL, 0u, NULL);
        }
    }

    print_cruise();
    ag_console_puts("  (in SYSTEM.CFG as [power] cruise_mhz / cruise_when to "
                    "survive a reboot)\n");
    return 0;
}

static int report(const ag_power_ended_t *ended, uint32_t n,
                  const ag_power_target_t *want)
{
    /*
     * Read back rather than assumed.  On a build without frequency scaling the
     * mode changes and the clock does not, and saying so here - next to the
     * command that was typed - is the difference between a system that looks
     * broken and one that is understood.
     */
    ag_power_target_t got;
    ag_power_current(&got);
    if (got.cpu_max_mhz != want->cpu_max_mhz) {
        ag_console_printf("power: the clock stayed at %u MHz - this build "
                          "cannot move it (ARGON_ENABLE_PM)\n",
                          (unsigned)got.cpu_max_mhz);
    }

    for (uint32_t i = 0; i < n; i++) {
        ag_console_printf("power: ended %s (pid %u) - %s\n", ended[i].name,
                          (unsigned)ended[i].pid,
                          (ended[i].why[0] != '\0')
                              ? ended[i].why
                              : "did not answer that it is fit");
    }
    return 0;
}

int ag_cmd_power(int argc, char **argv)
{
    int   words = 0;
    char *word[4] = {NULL, NULL, NULL, NULL};

    for (int i = 1; i < argc && words < 4; i++) {
        word[words++] = argv[i];
    }

    if (words == 0) {
        return status();
    }
    if (ag_path_icmp(word[0], "auto") == 0) {
        return cmd_auto(words, word);
    }
    if (ag_path_icmp(word[0], "cruise") == 0 && words > 1) {
        return cmd_cruise(word[1]);
    }

    ag_power_target_t now;
    ag_power_current(&now);
    ag_power_target_t want = now;

    if (ag_path_icmp(word[0], "full") == 0) {
        uint16_t       steps[AG_PORT_PWR_STEPS_MAX];
        const uint32_t n = ag_powerctl_cpu_steps(steps, AG_PORT_PWR_STEPS_MAX);
        if (n == 0u) {
            ag_console_puts("power: this machine reports no clock settings\n");
            return 1;
        }
        want.mode = AG_POWER_FULL;
        want.cpu_min_mhz = steps[0];
        want.cpu_max_mhz = steps[0];
        want.screen_on = true;
    } else if (ag_path_icmp(word[0], "cruise") == 0) {
        ag_power_auto_t a;
        ag_powerctl_auto_get(&a);
        const uint32_t mhz =
            (a.cruise_mhz > 0u) ? a.cruise_mhz : ag_powerctl_cruise_default();
        if (mhz == 0u) {
            ag_console_puts("power: this part has no middle clock setting to "
                            "cruise at\n");
            return 1;
        }
        want.mode = AG_POWER_CRUISE;
        want.cpu_min_mhz = mhz;
        want.cpu_max_mhz = mhz;
    } else if (ag_path_icmp(word[0], "eco") == 0 ||
               ag_path_icmp(word[0], "doze") == 0) {
        const bool doze = (ag_path_icmp(word[0], "doze") == 0);
        const uint32_t mhz =
            (words > 1) ? step_or_zero(word[1]) : ag_powerctl_eco_default();
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
         * The screen on its own does not change the mode: a dark screen at full
         * speed is a legitimate thing to want, calling it `doze` would be a lie
         * in the one place a person looks to find out - and, because nothing is
         * ended without the mode going down, it is also the one way to darken a
         * board without asking anything of what is running on it.
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

    ag_power_ended_t ended[AG_PROC_MAX];
    uint32_t         n_ended = 0;
    const ag_err_t   err = ag_powerctl_apply(&want, AG_POWER_USER, ended,
                                             AG_PROC_MAX, &n_ended);
    if (err != AG_OK) {
        if (err == -AG_EBUSY) {
            ag_console_puts("power: a change is already in flight; try again\n");
        } else {
            ag_console_printf("power: failed (%d)\n", (int)err);
        }
        return 1;
    }

    (void)report(ended, n_ended, &want);
    return status();
}
