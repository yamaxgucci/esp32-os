/*
 * ArgonOS - FALLOUT.AXE: the application header, and the call into the engine.
 *
 * The engine keeps its own main, in plib/gnw/winmain.cc, exactly as upstream
 * wrote it.  Everything this file does is name the image and hand it argc and
 * argv - the same shape apps/doom uses for doomgeneric, and for the same
 * reason: a port that does not touch the engine's entry point is a port that
 * can take the next upstream tag without a merge.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

#include "ag_cxx.h"

/*
 * The default stack, deliberately.  Asking for 32 KB got it: the internal SRAM
 * had 31 KB free in one piece, so the stack was placed in PSRAM instead - and a
 * task whose stack is in PSRAM faulted before the first instruction of the
 * image.  Ask for what fits internal memory, and find out what the engine
 * really needs from the "stack bytes unused" line the kernel prints on exit.
 */
AG_APP("FALLOUT", "0.1", "argon", 0);

/* Defined by the engine.  Declared here rather than included: winmain.cc has
 * no header, and the signature is fixed by the language. */
int main(int argc, char **argv);

extern "C" int ag_cxx_main(int argc, char **argv)
{
    ag_printf("FALLOUT - fallout1-ce on ArgonOS\n");
    return main(argc, argv);
}
