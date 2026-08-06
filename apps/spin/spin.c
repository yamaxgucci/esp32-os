/*
 * ArgonOS - an application that does not want to stop.
 *
 * The test case for the supervisor, in two flavours:
 *
 *   run t:\spin.axe        polite: checks whether it has been asked to stop,
 *                          and stops.  Ctrl+C ends it.
 *   run t:\spin.axe deaf   rude: never asks.  Only Ctrl+\ (or Ctrl-Alt-Del on a
 *                          real keyboard) gets rid of it, which is the case the
 *                          supervisor exists for.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

AG_APP("SPIN", "1.0", "argon", 0);

int ag_main(int argc, char **argv)
{
    const bool deaf = (argc > 1) && argv[1][0] == 'd';

    ag_printf("spinning%s; pid %d\n", deaf ? " and not listening" : "",
              (int)ag_getpid());

    for (unsigned round = 0;; round++) {
        /* Some work, so the loop is not optimised into nothing. */
        volatile unsigned sink = 0;
        for (unsigned i = 0; i < 200000u; i++) {
            sink += i;
        }
        (void)sink;

        if (!deaf && ag_interrupted()) {
            ag_printf("\nasked to stop after %u rounds; stopping\n", round);
            return 0;
        }

        /* A heartbeat is how a long job says it is still making progress. */
        ag_heartbeat();
        ag_print(".");
    }
}
