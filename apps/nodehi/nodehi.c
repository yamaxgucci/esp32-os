/*
 * ArgonOS - the smallest program worth sending to another board.
 *
 * It prints one recognisable line and its arguments, and that is the whole of
 * it.  The point is what it does NOT carry: no buffers, no tables, nothing
 * that needs memory a small board does not have.
 *
 * Why it exists at all, when HELLO.AXE is right there: HELLO deliberately
 * carries `static unsigned char s_big[512 * 1024]` - a buffer larger than the
 * whole executable arena - because its job is to prove that a large .bss lands
 * OUTSIDE the arena and still works.  That makes it an excellent test of the
 * loader on a board with PSRAM and an impossible program on a board without
 * any: the CYD has 203 KB of internal SRAM in total and the C3 has 174.
 *
 * So a remote-run check written with HELLO measures the wrong thing.  It fails
 * with -12 on both boards for a reason that has nothing to do with the wire,
 * and reads as "the cluster cannot run programs".
 *
 * Built for every instruction set in the tree, because the whole point of it
 * is to be handed to a board that is not the same kind of machine as this one.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

AG_APP("NODEHI", "1.0", "argon", 0);

int ag_main(int argc, char **argv)
{
    /*
     * One line, and it says the name it was given.  A remote run's output
     * comes back to the node that ASKED for it, so this is what the asking
     * board sees - and the argument is how a test tells one run from another
     * when both boards are printing at once.
     */
    ag_print("nodehi: here");
    for (int i = 1; i < argc; i++) {
        ag_print(" ");
        ag_print(argv[i]);
    }
    ag_print("\n");
    return 0;
}
