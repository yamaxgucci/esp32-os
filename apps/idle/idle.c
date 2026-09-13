/*
 * ArgonOS - a process that stays, and does nothing at all.
 *
 *   run /b t:\idle.axe [name]
 *
 * There was no way to have a process simply EXIST.  spin.axe is the nearest
 * thing and is the opposite of this on purpose: it is the supervisor's test
 * case, so it burns a core and prints a dot every few milliseconds.  Used as a
 * stand-in for "something is running over there" it drowned the other board's
 * console and made it too busy to answer - which failed nine checks that had
 * nothing to do with it, in a way that pointed at the wire.
 *
 * So: one line when it starts, then asleep until something stops it.  It is
 * what a listing needs to show, what a kill needs to end, and what a process
 * being moved from one board to another will need to be.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

AG_APP("IDLE", "1.0", "argon", 0);

int ag_main(int argc, char **argv)
{
    const char *who = (argc > 1) ? argv[1] : "idle";

    ag_printf("%s: here as pid %d, doing nothing\n", who, (int)ag_getpid());

    /*
     * Asked, not spun.  A second at a time so that a stop is noticed promptly
     * without the process being awake for any of it: this exists to be present,
     * and a stand-in that costs something is a stand-in that changes what it
     * stands in for.
     */
    while (!ag_interrupted()) {
        ag_delay(1000);
    }
    ag_printf("%s: asked to stop\n", who);
    return 0;
}
