/*
 * ArgonOS - an application that is told before the machine slows down.
 *
 * The other half of `power`, and the reason the command has a grace period at
 * all.  Four flavours, one per way an application can behave:
 *
 *   run /b t:\pwr.axe          polite: notices the transition, says it is
 *                              carrying on, and reports what changed.
 *   run /b t:\pwr.axe park     stops working while the clock is low and starts
 *                              again when it comes back - which is what a
 *                              player, a logger or a poller should do.
 *   run /b t:\pwr.axe hold     says it needs the clock and gives a reason: the
 *                              standing hold a realtime path takes.  `power
 *                              eco` then refuses and names this process;
 *                              `power eco /force` overrides it.
 *   run /b t:\pwr.axe deaf     never asks.  The transition happens anyway,
 *                              after the grace period, and `power` reports it
 *                              as not polling.  This is every application in
 *                              the tree that has not been changed.
 *
 * Ctrl+C ends it.  Run it in the background (/b) so the shell is free to
 * change the mode while it watches.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

AG_APP("PWR", "1.0", "argon", 0);

static const char *mode_name(uint8_t mode)
{
    switch (mode) {
    case AG_POWER_FULL: return "full";
    case AG_POWER_ECO:  return "eco";
    case AG_POWER_DOZE: return "doze";
    default:            return "?";
    }
}

int ag_main(int argc, char **argv)
{
    const char *how = (argc > 1) ? argv[1] : "";
    const bool  deaf = (how[0] == 'd');
    const bool  park = (how[0] == 'p');
    const bool  hold = (how[0] == 'h');

    ag_power_status_t probe;
    if (ag_power_status(&probe) == -AG_ENOSYS) {
        ag_print("pwr: this system has no power subtable (ABI < 0.34)\n");
        return 1;
    }

    ag_printf("pwr: pid %d, %s\n", (int)ag_getpid(),
              deaf ? "not listening at all"
                   : (hold ? "holding the clock"
                           : (park ? "will park when the clock drops"
                                   : "listening")));

    if (hold) {
        /*
         * A reason, not just a flag.  It is what the person who typed `power
         * eco` is shown, and "PWR.AXE says no" would tell them nothing they
         * could act on.
         */
        const ag_err_t err = ag_power_hold(true, "pretending to be realtime");
        if (err != AG_OK) {
            ag_printf("pwr: hold refused (%d)\n", (int)err);
            return 1;
        }
    }

    uint8_t was_mode = 0xffu;
    bool    was_screen = true;
    bool    parked = false;

    for (;;) {
        if (ag_interrupted()) {
            if (hold) {
                (void)ag_power_hold(false, NULL);
            }
            ag_print("pwr: asked to stop\n");
            return 0;
        }

        if (!deaf) {
            ag_power_status_t p;
            if (ag_power_status(&p) == AG_OK) {
                /*
                 * Something is coming: answer it.  Once is enough - the answer
                 * is remembered until the transition is over - but answering
                 * again is harmless, which matters because a loop like this
                 * cannot know whether it already did.
                 */
                if (p.pending != p.mode || p.pending_screen_on != p.screen_on ||
                    p.pending_cpu_max_mhz != p.cpu_max_mhz) {
                    const bool saving =
                        (p.pending != AG_POWER_FULL) || !p.pending_screen_on;
                    if (park && saving) {
                        (void)ag_power_answer(AG_POWER_PARKED, NULL);
                        ag_printf("pwr: %s at %u MHz is coming - parking\n",
                                  mode_name(p.pending),
                                  (unsigned)p.pending_cpu_max_mhz);
                    } else if (!hold) {
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

        /*
         * The work.  Parked means not doing it: that is the whole decision an
         * application gets to make here, and it is the one that actually saves
         * anything - a slower clock with the same loop running flat out in it
         * saves a good deal less than stopping the loop.
         */
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
