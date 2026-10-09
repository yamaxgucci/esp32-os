/*
 * ArgonOS - a program that needs sound, and does nothing else.
 *
 * It exists to be PLACED.  On a system made of several boards, a program that
 * declares a need is the input to the decision "which board should run this",
 * and testing that decision needs a program whose need is the only interesting
 * thing about it: it must start and finish at once, print one line that could
 * not have come from anywhere else, and never open the device it asked for.
 *
 * Never opening it is the point.  The claim under test is that the program was
 * sent to a node that CAN make a noise - not that a noise was made - and a
 * program that also played something would fail for a second reason on a board
 * with a silent helper and make the first one unreadable.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

AG_APP("NEEDSND", "1.0", "argon", AG_AXE_NEEDS_AUDIO);

int ag_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    ag_print("needsnd: ran on a node with sound\n");
    return 0;
}
