/*
 * ArgonOS - a program that needs a screen, and does nothing else.
 *
 * The other half of needsnd.axe, and it exists for the harder half of
 * placement.  A board that has no sound at all is easy to reason about; a board
 * that HAS a screen and has already given it to something else is the case the
 * second-monitor model is really about, and nothing in the tree could state it:
 * every graphical program here draws and hands the glass back.
 *
 * It never acquires the screen.  The claim under test is that it was sent to a
 * board where a screen was free - not that it drew on one - and a program that
 * also drew would fail for a second reason on a headless node and make the
 * first unreadable.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

AG_APP("NEEDGFX", "1.0", "argon", AG_AXE_NEEDS_GFX);

int ag_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    ag_print("needgfx: ran where a screen was free\n");
    return 0;
}
