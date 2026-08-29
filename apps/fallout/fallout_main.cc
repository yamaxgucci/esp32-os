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

/* In port/sdl_shim.cc.  Not SDL: an automated run has no eyes. */
extern "C" void sdl_shim_screenshot(const char *path, int after_frames);

static bool same(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

static int to_int(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        ++s;
    }
    return v;
}

/*
 * Two options of our own, taken out of argv before the engine sees it.
 *
 * The engine parses argv itself and would object to anything it does not know,
 * so they are removed rather than ignored.  Both exist for the same reason:
 * there is no board yet, the test harness reads the text screen, and a game
 * that has taken the display is not using the text screen.
 *
 *   -shot PATH     write the frame out as a PPM
 *   -frames N      after this many presents (default 1)
 */
extern "C" int ag_cxx_main(int argc, char **argv)
{
    ag_printf("FALLOUT - fallout1-ce on ArgonOS\n");

    const char *shot = NULL;
    int frames = 1;
    int out = 0;

    for (int i = 0; i < argc; ++i) {
        if (same(argv[i], "-shot") && i + 1 < argc) {
            shot = argv[++i];
        } else if (same(argv[i], "-frames") && i + 1 < argc) {
            frames = to_int(argv[++i]);
        } else {
            argv[out++] = argv[i];
        }
    }
    argv[out] = NULL;

    if (shot != NULL) {
        sdl_shim_screenshot(shot, frames > 0 ? frames : 1);
    }
    return main(out, argv);
}
