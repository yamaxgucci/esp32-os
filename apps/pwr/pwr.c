/*
 * ArgonOS - an application that is told before the machine slows down.
 *
 * The other half of `power`, and the reason the command has a grace period at
 * all.  Five flavours, one per way a program can behave:
 *
 *   run /b t:\pwr.axe          polite: notices the transition, answers that it
 *                              is carrying on, and reports what changed.
 *   run /b t:\pwr.axe park     answers that it has stopped working, and does -
 *                              which is what a player, a logger or a poller
 *                              should do, because a slower clock with the same
 *                              loop running flat out in it saves very little.
 *   run /b t:\pwr.axe any      says once that any mode suits it and never polls
 *                              again.  What a program with nothing to decide
 *                              should do.
 *   run /b t:\pwr.axe amp      says once that it needs the full clock: the idle
 *                              timer then leaves the clock alone while it runs,
 *                              and `power eco` ends it and prints its reason.
 *   run /b t:\pwr.axe deaf     never asks.  An automatic transition happens
 *                              around it and costs it nothing; `power eco`
 *                              ends it.  This is every application in the tree
 *                              that has not been taught about power modes.
 *
 * Ctrl+C ends it.  Run it in the background (/b) so the shell is free to change
 * the mode while it watches.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

AG_APP("PWR", "1.0", "argon", 0);

static const char *mode_name(uint8_t mode)
{
    switch (mode) {
    case AG_POWER_FULL:   return "full";
    case AG_POWER_CRUISE: return "cruise";
    case AG_POWER_ECO:    return "eco";
    case AG_POWER_DOZE:   return "doze";
    default:              return "?";
    }
}

int ag_main(int argc, char **argv)
{
    const char *how = (argc > 1) ? argv[1] : "";
    const bool  deaf = (how[0] == 'd');
    const bool  park = (how[0] == 'p');
    const bool  amp = (how[0] == 'a' && how[1] == 'm');
    const bool  any = (how[0] == 'a' && how[1] == 'n');

    ag_power_status_t probe;
    if (ag_power_status(&probe) == -AG_ENOSYS) {
        ag_print("pwr: this system has no power subtable (ABI < 0.35)\n");
        return 1;
    }

    ag_printf("pwr: pid %d, %s\n", (int)ag_getpid(),
              deaf ? "not listening at all"
                   : (amp ? "needs the full clock"
                          : (any ? "fit for any mode"
                                 : (park ? "will park when asked to save"
                                         : "listening"))));

    if (amp || any) {
        /*
         * A reason, not just a flag.  For the one that needs the clock it is
         * what the person who typed `power eco` is shown as it dies, and
         * "PWR.AXE was not fit" would tell them nothing they could act on.
         */
        const ag_err_t err = ag_power_declare(
            amp ? AG_POWER_FIT_FULL_ONLY : AG_POWER_FIT_ANY,
            amp ? "pretending to be realtime" : "nothing to decide");
        if (err != AG_OK) {
            ag_printf("pwr: declare refused (%d)\n", (int)err);
            return 1;
        }
    }

    uint8_t was_mode = 0xffu;
    bool    was_screen = true;
    bool    parked = false;

    for (;;) {
        if (ag_interrupted()) {
            ag_print("pwr: asked to stop\n");
            return 0;
        }

        /* A declaration is a standing answer; polling after making one would be
         * asking to be asked again. */
        if (!deaf && !amp && !any) {
            ag_power_status_t p;
            if (ag_power_status(&p) == AG_OK) {
                /*
                 * Something is coming: answer it.  Once is enough - the answer
                 * is remembered until the transition is over - but answering
                 * again is harmless, which matters because a loop like this
                 * cannot know whether it already did.
                 */
                if (p.pending != p.mode ||
                    p.pending_screen_on != p.screen_on ||
                    p.pending_cpu_max_mhz != p.cpu_max_mhz) {
                    const bool saving = (p.pending != AG_POWER_FULL) ||
                                        !p.pending_screen_on;
                    if (park && saving) {
                        (void)ag_power_answer(AG_POWER_PARKED, NULL);
                        ag_printf("pwr: %s at %u MHz is coming (%s) - "
                                  "parking\n",
                                  mode_name(p.pending),
                                  (unsigned)p.pending_cpu_max_mhz,
                                  (p.cause == AG_POWER_USER) ? "asked for"
                                                             : "idle timer");
                    } else {
                        (void)ag_power_answer(AG_POWER_OK, NULL);
                    }
                }

                if (p.mode != was_mode || p.screen_on != was_screen) {
                    ag_printf("pwr: now %s, cpu %u MHz, screen %s\n",
                              mode_name(p.mode), (unsigned)p.cpu_mhz,
                              p.screen_on ? "on" : "off");
                    was_mode = p.mode;
                    was_screen = p.screen_on;
                }

                /* Working again as soon as the clock is back. */
                parked = park && (p.mode != AG_POWER_FULL);
            }
        }

        if (!parked) {
            volatile unsigned sink = 0;
            for (unsigned i = 0; i < 20000u; i++) {
                sink += i;
            }
            (void)sink;
        }

        ag_heartbeat();
        ag_delay(50);
    }
}
