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
/*
 * Four megabytes of arena, asked for rather than taken.
 *
 * The default is a quarter of free extended memory, which here is about one and
 * a half - and the .DAT index alone does not fit in that.  Fallout 2's
 * master.dat has 23120 entries and critter.dat another 7120; each one is a
 * descriptor and a separately allocated path, and the two indices together are
 * a little over 1.7 MB before the game has drawn anything.  On top of that go
 * the 640x480 screen buffer and the art cache.
 *
 * Running out does not announce itself: dbaseOpen stops reading entries at the
 * first failed allocation and returns the half-built table anyway, and the
 * binary search over it then cannot find fonts that are demonstrably in the
 * archive.  That is what this number is here to prevent.
 *
 * On top of that goes the art cache, which the engine reserves whole at
 * startup: art_cache_size in the .cfg is in megabytes and its default is
 * eight.  Put 1 there - see apps/fallout/fallout2.cfg - or artInit fails and
 * says only "cache_init failed in art_init".
 *
 * A named size is required rather than preferred, so the process is refused if
 * it cannot be had.  With 8 MB of PSRAM and 1.9 MB of image data, five leaves
 * about one spare.
 */
#ifndef AG_FALLOUT_GAME
#define AG_FALLOUT_GAME 1
#endif

/*
 * The number is a build option because the right one depends on the board.
 * 5875 KB is what fits beside a 1.9 MB image on a part with 8 MB of PSRAM,
 * which is what QEMU emulates and what the smaller S3 modules carry.  On a
 * 16 MB module there is room for twice that, and Fallout 2 will take it:
 *
 *     python apps/fallout/build.py --arena-kb 12000
 */
#ifndef AG_FALLOUT_ARENA_KB
#define AG_FALLOUT_ARENA_KB 5875
#endif

#if AG_FALLOUT_GAME == 2
AG_APP_SIZED("FALLOUT2", "0.1", "argon", 0, 0, AG_FALLOUT_ARENA_KB * 1024);
#else
AG_APP_SIZED("FALLOUT", "0.1", "argon", 0, 0, AG_FALLOUT_ARENA_KB * 1024);
#endif

/* Defined by the engine.  Declared here rather than included: winmain.cc has
 * no header, and the signature is fixed by the language. */
int main(int argc, char **argv);

/* In port/sdl_shim.cc.  Not SDL: an automated run has no eyes. */
extern "C" void sdl_shim_screenshot(const char *path, int after_frames,
                                    int after_ms);
extern "C" void sdl_shim_memstat(int every_ms, int trace_kb,
                                 int trace_max, int trace_prof);
extern "C" void sdl_shim_fps(int every_ms);
extern "C" void sdl_shim_screenshot_now(void);

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
 *   -frames N      after this many presents
 *   -shotms N      or after this many milliseconds, whichever comes first
 *   -memstat N     print arena use, and the peak so far, every N ms
 *   -mallocs KB N  with -memstat, name the first N allocations of KB
 *                  or more (default 32 and 64)
 *   -prof N        with -memstat, count allocations by call site and
 *                  print the N biggest each window
 *   -fps N         print the frame rate and where a frame goes, every N ms
 *   -autostart     start a new game at once, premade character, no movie
 *   -scripts N     keep at most N script programs resident; the engine
 *                  reloads one when it runs again.  0, the default,
 *                  keeps every script a map has ever run - which is
 *                  1025 KB on the largest map in the game.
 *
 * -autostart is the one that made measuring possible.  Everything about
 * standing on a map has to reach a map first, and reaching one by sending
 * keys meant five minutes of opening movie per run and a main menu that
 * ignores the key for New Game while it is fading in.  Three runs in a row
 * spent twenty minutes each and never got there.  Two engine files know about
 * it - see patches/fallout2-ce/0006.
 *
 * Prefer -shotms.  Counting frames means knowing how many the game draws
 * before it reaches whatever you wanted to look at, and the two opening movies
 * alone are five thousand of them.
 */
/* Read by the engine's main menu and character selector; see the patch. */
extern "C" int ag_autostart = 0;

/* Read by scriptsTrimPrograms(); see patches/fallout2-ce/0014. */
extern "C" int ag_script_cache = 0;

extern "C" int ag_cxx_main(int argc, char **argv)
{
    /* argv[0] is not decoration here: sfall builds the path of ddraw.ini from
     * it, by cutting at the last backslash, so a setting that appears to do
     * nothing is usually a file the engine went looking for somewhere else. */
    ag_printf("fallout%d-ce on ArgonOS, argv[0] = %s\n", AG_FALLOUT_GAME,
              (argc > 0 && argv[0] != NULL) ? argv[0] : "(none)");

    const char *shot = NULL;
    int frames = 0;
    int shot_ms = 0;
    int memstat_ms = 0;
    int trace_kb = 0;
    int trace_max = 0;
    int trace_prof = 0;
    int fps_ms = 0;
    int out = 0;

    for (int i = 0; i < argc; ++i) {
        if (same(argv[i], "-shot") && i + 1 < argc) {
            shot = argv[++i];
        } else if (same(argv[i], "-frames") && i + 1 < argc) {
            frames = to_int(argv[++i]);
        } else if (same(argv[i], "-shotms") && i + 1 < argc) {
            shot_ms = to_int(argv[++i]);
        } else if (same(argv[i], "-memstat") && i + 1 < argc) {
            memstat_ms = to_int(argv[++i]);
        } else if (same(argv[i], "-mallocs") && i + 2 < argc) {
            trace_kb = to_int(argv[++i]);
            trace_max = to_int(argv[++i]);
        } else if (same(argv[i], "-prof") && i + 1 < argc) {
            trace_prof = to_int(argv[++i]);
        } else if (same(argv[i], "-fps") && i + 1 < argc) {
            fps_ms = to_int(argv[++i]);
        } else if (same(argv[i], "-autostart")) {
            ag_autostart = 1;
        } else if (same(argv[i], "-scripts") && i + 1 < argc) {
            ag_script_cache = to_int(argv[++i]);
        } else {
            argv[out++] = argv[i];
        }
    }
    argv[out] = NULL;

    if (shot != NULL) {
        if (frames <= 0 && shot_ms <= 0) {
            frames = 1;
        }
        sdl_shim_screenshot(shot, frames, shot_ms);
    }
    sdl_shim_memstat(memstat_ms, trace_kb, trace_max, trace_prof);
    sdl_shim_fps(fps_ms);

    const int rc = main(out, argv);

    /* Whatever was on screen when it stopped, if a screenshot was asked for
     * and its moment never came. */
    sdl_shim_screenshot_now();
    return rc;
}
