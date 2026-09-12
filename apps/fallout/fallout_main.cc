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
#include "port/art_store.h"

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

/*
 * The engine's art cache, declared rather than included: art.h drags in the
 * whole of the engine's headers and all this needs is the one function.  It
 * lives in namespace fallout, like everything else over there.
 */
namespace fallout {
int artCacheFlush();
/* ARGON: pinned animations cannot be swept, so they are let go before any
 * attempt to find memory.  See artPin in art.cc. */
void artDropPins();
/* Same shape and the same reason: every entry is a copy of a file on the card,
 * and nothing is playing while a map loads.  296 KB, measured. */
void soundEffectsCacheFlush();
/* And the one that actually gives the arena bytes rather than freeing entries
 * inside a block it keeps: 257 KB in one piece.  See patch 0045. */
bool soundEffectsCachePark();
/*
 * And the biggest single piece this program owns: the art cache's 1800 KB,
 * taken in one block at startup.  Maxim's idea - on a full-screen panel there
 * is no map drawn, so the block is idle, and lending it beats holding a
 * reserve that is idle always.  artCacheLend refuses while anything still
 * holds an entry, so it is safe to try.
 */
bool artCacheLend();
bool artCacheTakeBack();
int artCacheMaxSize();
bool artCacheResize(int maxSize);
bool artBorrow(int size, int* handleIndexPtr, unsigned char** bufferPtr);
void artReturn(int* handleIndexPtr);
bool soundEffectsCacheUnpark();
bool soundEffectsCacheIsParked();
}

/*
 * The shim s own, declared rather than included: this file is compiled with
 * the engine s include path, where <stdlib.h> is the toolchain s and not
 * apps/common/libc s.
 */
extern "C" {
void            ag_libc_oom_sink(int (*sink)(__SIZE_TYPE__ wanted));
void            ag_libc_borrow_sink(void *(*borrow)(__SIZE_TYPE__ wanted),
                                    int (*giveback)(void *p,
                                                    __SIZE_TYPE__ *sizePtr));
extern unsigned ag_libc_borrowed;
extern unsigned ag_libc_oom_rescues;
/* Does nothing unless the build asked for the profiler; see on_out_of_arena. */
void            ag_libc_malloc_profile_dump(int top);
}

/*
 * What to do when the arena refuses an allocation.
 *
 * The failure on this machine is fragmentation, not exhaustion: "235 of
 * 6050 KB free, largest block 94208" with 132 KB asked for.  The engine does
 * not check malloc - almost no engine does - so the refusal used to become a
 * write through a null pointer one instruction later, and the crash record
 * named whatever ran next.
 *
 * The art cache is 1800 KB of the arena and every byte of it is a copy of
 * something on the disk, so throwing it away costs a reload and nothing else.
 * That makes it the one part of this program's memory that can be given back
 * on demand - and giving it back in one piece is what a fragmented arena
 * needs.  Once per refusal; if letting go of all of it did not help, asking
 * again will not either.
 */
/*
 * ARGON: and a block held back for exactly this moment.
 *
 * The paragraph above is half wrong, and the board said so on 7 September
 * 2026.  artCacheFlush gives memory back *inside the cache's own heap* - the
 * 1800 KB block that heap sits in is taken once at startup and never
 * returned - so unless art had spilled past it into the arena, the flush
 * hands the arena nothing at all.  Measured at the moment of death, three
 * times over: `spilled 0 KB in 0 blocks`, the hook firing, and the game dying
 * anyway.
 *
 * What the arena actually runs out of is not bytes but a *hole*.  Entering the
 * Temple of Trials:
 *
 *     malloc: out of arena asking for 70066 bytes; 172 of 6000 KB free,
 *             largest block 65536
 *
 * A hundred and seventy kilobytes free, in pieces no bigger than sixty-four,
 * and sixty-eight wanted.  Nothing that frees scattered bytes can help that.
 *
 * So one block is taken at startup, while the arena is empty and a large
 * contiguous allocation is certain to succeed, and it is not used for
 * anything.  It is a hole, kept.  When a refusal arrives and the flush has not
 * made room, the hole is handed over and the caller retries into it.
 *
 * Three hundred and twenty kilobytes because that is the largest refusal seen:
 * 307 200, the 640x480 surface a dialogue or the save screen needs.  The
 * others were 70 066, 146 880 and 193 892.
 *
 * It re-arms itself once the crisis passes - see fallout_reserve_tick, called
 * from the shim's own frame timer. A reserve that is spent and never taken
 * again would save the first map change of a session and no other.
 */
/*
 * ARGON, second thought, same day: this is a knob and its default is off.
 *
 * The reserve answers fragmentation - free bytes that are in no hole big
 * enough - and that is not what the temple runs out of.  Measured there:
 *
 *     malloc: out of arena asking for 16904 bytes; 23 of 6000 KB free
 *     malloc: out of arena asking for 125760 bytes; 59 of 6000 KB free
 *
 * Twenty-three kilobytes free, not a hundred and seventy in pieces.  Against
 * exhaustion a reserve is worse than nothing: it holds three hundred and
 * twenty kilobytes out of the peak to give one allocation back once, and the
 * next refusal arrives with the reserve already spent.  The shortfall was
 * smaller than the reserve.
 *
 * So it is off unless asked for - `-reserve KB` - and it is worth asking for
 * when the failures come back with plenty free and no hole, which is the shape
 * it was written for.
 */
/*
 * ARGON: and TWO reserves, because one is worse than none.  The board, 8
 * September 2026, with -reserve 320 finally switched on:
 *
 *     refused 9614 B with 5996 KB used of 6000, hole 3264 B
 *     9614 bytes refused ...; spending the 320 KB reserve
 *     ...
 *     refused 307200 B ... largest 270336 B, and the reserve is already spent
 *     LOADSAVE: ** Error loading save game screen data! **
 *
 * Three hundred and twenty kilobytes handed to a request for nine, and then
 * nothing left for the one allocation the reserve exists for.  The save panel
 * would not open, which to Maxim is the game refusing to save.
 *
 * So the big reserve is now spent ONLY on a request big enough to need it -
 * anything at least half its size - and a small one sits beside it for the
 * ordinary refusals, which are a few kilobytes in an arena with no room left.
 * A small refusal can no longer eat the panel's hole.
 */
/* Defined with the loan machinery below; the tick borrows early, so the
 * counters it reads have to be visible here as well. */
static int slab_take(void);
static unsigned slabs_return_empty(void);
static unsigned slabs_held_kb(void);
extern unsigned freed_kb;
/* The loan sizes live here rather than beside the lender, because the tick
 * borrows early and needs to know how big a slab is. */
/*
 * ARGON: four slabs of 256 KB, not one of a megabyte, and each goes back on
 * its own.
 *
 * One big slab is easy to place and impossible to return: it only goes back
 * when nothing lives in it, and a map load always leaves something behind.
 * Measured on the board, 10 September 2026, entering a town:
 *
 *     the map has loaded but 14 blocks still live in the slab;
 *     1024 KB of the art cache stays on loan
 *
 * Fourteen blocks - a few tens of kilobytes between them - held a megabyte
 * hostage.  The art cache went from 3000 KB to 2000 against a working set of
 * 3572, emptied itself 67 times, read 99 MB off the card, and the town ran at
 * a quarter of a frame a second.
 *
 * Split into four, the stragglers land in one or two of them and the rest go
 * home.  It cannot be fixed properly - the blocks are reached through bare
 * pointers and cannot be moved - so the aim is to bound the damage, not to
 * abolish it.
 */
#define AG_SLAB_BYTES (256u * 1024u)
/*
 * ONE slab, not four.
 *
 * Four was meant to bound how much a straggler could strand; what it actually
 * bounded was nothing, because the loans are taken in a town where the cache
 * is already the scarce thing.  Measured 11 September 2026: two slabs and a
 * window put 1324 KB of a 3000 KB art cache out on loan, locked and immovable,
 * and the town then could not load its own scenery -
 *
 *     CACHE: ... would not load - 2800 of 3000 KB in the cache is in use
 *
 * - while the arena died anyway, at 6158 KB of 6160.  A loan that takes half
 * the cache to postpone a death by seconds is a bad trade.  One slab is enough
 * for the save panel and for a map change, which are the two things the loans
 * exist for.
 */
#define AG_SLABS 1

static struct {
    unsigned char *base;
    int handle;
    __SIZE_TYPE__ bytes;
    unsigned live;          /* blocks handed out of THIS slab */
} s_slab[AG_SLABS];

static unsigned s_slabs_taken, s_slab_out;

/*
 * ARGON: the loans belong to one thread, and it is the one that plays the game.
 *
 * malloc is called from the audio thread as well - the mixer allocates - and
 * the block list inside a slab has no lock.  Two threads in it at once would
 * not be "out of memory"; it would be a corrupted list, which is the kind of
 * fault that takes a week to find and wears a different face every time.
 *
 * A mutex would do, but the honest answer is cheaper: the audio thread has no
 * business borrowing.  It allocates small and rarely, and if the arena cannot
 * feed it then the game is over anyway.  So the borrower remembers which
 * thread first asked and answers nobody else.
 */
/* Raised by map.cc for the length of a map change; see ag_diag.h. */
extern "C" unsigned ag_map_loading;

/* -loans N; defined with the other flags further down. */
extern "C" int ag_loans_on;

static void *s_loan_thread;

static void *loan_thread_now(void)
{
    return (void *)g_ag_api->task->self();
}

/* Set while the art cache's block is out on loan; see on_out_of_arena. */
static int s_art_lent;

/*
 * ARGON: the art cache as the arena's slack, rather than a fixed number.
 *
 * Measured 9 September 2026: the art asked for in any twenty seconds weighs
 * 2118 KB at the median and 2940 at the worst, against a cache of 1800 - so
 * 91% of the misses while walking were for art the cache had just evicted.
 * The missing megabyte cannot simply be taken: the arena peaks at 5806 KB of
 * 6000 on a big map and 5977 in the Temple.
 *
 * So the size moves.  It grows into slack that is actually there and shrinks
 * the moment something else is refused, which makes the cache the place the
 * arena keeps its spare room - a far better use for it than a reserve held
 * idle, which is what this file used to do and what Maxim rightly objected to.
 *
 * The numbers, and why each is what it is:
 *
 *   ceiling  half the arena.  A fraction and not a constant, because a 16 MB
 *            board must get the benefit without anyone editing this file -
 *            and because half is comfortably above the 2940 KB measured here
 *            while leaving the other half for everything else.
 *   floor    1400 KB, which is where an earlier measurement showed real
 *            thrashing.  Below that the cache is worse than useless.
 *   step     512 KB, so a resize is worth its cost.
 *   slack    a grow needs the step plus a megabyte still free afterwards, or
 *            growing is just moving the refusal somewhere else.
 *   30 s     the least time between grows.  A resize EMPTIES the cache, so it
 *            is paid for by reloading the working set; doing it often would
 *            cost more than the thrashing it cures.
 *
 * Shrinking has no such timer: it happens only on a refusal, which is already
 * the rarest and most expensive thing that can happen.
 */
#define AG_ART_FLOOR_KB   1400
#define AG_ART_STEP_KB     512
#define AG_ART_GROW_EVERY 30000u

static unsigned s_art_resize_ms;
static unsigned s_art_grows, s_art_shrinks, s_art_refused;

static int art_ceiling_kb(const ag_meminfo_t *mi)
{
    return (int)(mi->arena_total / 1024u / 2u);
}

static unsigned s_small_bytes;
static void *s_small;
static unsigned s_small_spent;
static unsigned s_reserve_bytes;

/*
 * ARGON: TWO of them, because the save panel needs two.
 *
 * Measured on the board, 9 September 2026, leaving the Temple:
 *
 *     refused 307324 B ... 351 KB free in holes of 270336 B; spending the 320 KB reserve
 *     refused 307200 B ... 367 KB free, largest 270336 B, and both reserves are already spent
 *     LOADSAVE: ** Error loading save game screen data! **
 *
 * Opening the panel asks twice: once through the engine's own heap (307304,
 * which reaches the arena as 307324) and once for the window buffer itself
 * (307200).  One reserve covers one of them.  The save that did work covered
 * the first by parking the sound cache and the second with the reserve - luck,
 * not design, and the next map made the luck run out.
 *
 * Two blocks rather than one of 640 KB on purpose: two 320 KB holes are far
 * easier to place in a played-in arena than one twice the size, and they can
 * be re-armed one at a time as room appears.
 */
#define AG_RESERVES 2

static void *s_reserve[AG_RESERVES];
static unsigned s_reserve_spent;
static unsigned s_reserve_rearmed;

extern "C" void fallout_reserve_kb(int kb)
{
    s_reserve_bytes = (kb > 0) ? (unsigned)kb * 1024u : 0u;
}

static void reserve_take(void)
{
    if (s_small == NULL && s_small_bytes != 0u) {
        s_small = ag_malloc(s_small_bytes);
    }
    if (s_reserve_bytes != 0u) {
        int i;
        for (i = 0; i < AG_RESERVES; i++) {
            if (s_reserve[i] == NULL) {
                s_reserve[i] = ag_malloc(s_reserve_bytes);
            }
        }
    }
}

/* The index of a reserve still held, or -1. */
static int reserve_held(void)
{
    int i;
    for (i = 0; i < AG_RESERVES; i++) {
        if (s_reserve[i] != NULL) {
            return i;
        }
    }
    return -1;
}

/* The index of a reserve that has been spent and could be taken back, or -1. */
static int reserve_missing(void)
{
    int i;
    for (i = 0; i < AG_RESERVES; i++) {
        if (s_reserve[i] == NULL) {
            return i;
        }
    }
    return -1;
}

/*
 * Called about once a second from the shim, and cheap when there is nothing
 * to do: the test is a pointer.  ag_meminfo costs about 12 ms, so it is only
 * asked once the reserve is actually gone, and only every few seconds.
 */
extern "C" void fallout_reserve_tick(void)
{
    /*
     * ARGON: the sound cache comes back here, once the peak has passed.
     *
     * Parked by on_out_of_arena, which needed its 257 KB as one hole.  Taken
     * again when there is twice that free in one piece, so that taking it back
     * cannot itself cause the next refusal - the same rule the reserve below
     * uses, and for the same reason.  Sound effects are silent until then;
     * music is not affected, it does not come through this cache.
     */
    if (fallout::soundEffectsCacheIsParked()) {
        static unsigned sfx_ms;
        const unsigned  t = (unsigned)ag_millis();
        if (sfx_ms == 0u || t - sfx_ms >= 2000u) {
            sfx_ms = t;
            ag_meminfo_t si;
            ag_meminfo(&si);
            if (si.arena_largest >= 600u * 1024u) {
                (void)fallout::soundEffectsCacheUnpark();
            }
        }
    }

    if (s_small == NULL && s_small_bytes != 0u) {
        ag_meminfo_t smi;
        ag_meminfo(&smi);
        if ((__SIZE_TYPE__)smi.arena_largest >= (__SIZE_TYPE__)s_small_bytes * 4u) {
            s_small = ag_malloc(s_small_bytes);
        }
    }

    /*
     * ARGON: the loan belongs to the map change, and to nothing else.
     *
     * The first version borrowed whenever the arena "looked thin" - under a
     * megabyte and a half free.  That is not a rare emergency, it is what this
     * game looks like all the time, so the megabyte went out once and never
     * came back: the art cache spent the session at 1600 KB with a locked
     * block through the middle of it, and walking cost 4614 misses per hundred
     * hexes against 431 with the cache whole.  Worse than the 1800 KB cache
     * this all started from, and Maxim felt it immediately - "стало очень
     * сильно тормозить".
     *
     * A loan has to be as short as what it is for.  The only thing that needs
     * it is a map change: it wants some seven hundred kilobytes more than the
     * arena has, it is over in a couple of seconds, and while it runs there is
     * no map on the screen, so art evicted for it is art nobody is looking at.
     * ag_map_loading says exactly when that is, and outside it the cache keeps
     * every byte it has.
     */
    if (ag_map_loading != 0u) {
        /* Normally taken by ag_loan_map_begin below - the tick does not run
         * while a map loads - but harmless as a second chance. */
        if (s_slabs_taken == 0u) {
            (void)slab_take();
        }
    } else if (s_slabs_taken != 0u && s_slab[s_slabs_taken - 1u].live == 0u) {
        const unsigned freed = slabs_return_empty();

        if (freed != 0u) {
            ag_printf("fallout: gave back %u KB of the art cache (%u KB still "
                      "on loan)\n", freed_kb, slabs_held_kb());
        }
    } else if (s_slabs_taken != 0u && s_slab_out != 0u) {
        /*
         * The map has finished and something in the slab outlived it.  A
         * megabyte of the art cache stays hostage until that block is freed,
         * and the cache is a third smaller for as long as it lasts - which is
         * the whole of the slowdown above, so it is said out loud rather than
         * left to be discovered.
         */
        static unsigned said_ms;
        const unsigned now = (unsigned)ag_millis();

        if (said_ms == 0u || now - said_ms > 30000u) {
            said_ms = now;
            ag_printf("fallout: the map has loaded but %u blocks still live in "
                      "the slab; %u KB of the art cache stays on loan\n",
                      s_slab_out, (unsigned)(AG_SLAB_BYTES / 1024u));
        }
    }

    /*
     * The art cache first, and every tick while it is out: it is the game's
     * own memory and every frame without it is a frame reading art off the
     * card.  heapInit wants maxSize plus a thousandth of it in one piece, so
     * ask for a little more than that and let the next tick try again.
     */
    /*
     * Grow into slack, rarely, and only when the room is really there.
     */
    /*
     * ARGON: and it gives up.
     *
     * A resize needs the block empty of live entries, and the interface holds
     * art for as long as the game is on the screen, so on this board the
     * answer is always no.  Eight refusals is enough to establish that; after
     * them the check stops running, because a probe that can only ever fail
     * should not be charged to every thirtieth second of play.  A board where
     * it CAN work will succeed within the first eight and never reach this.
     */
    if (!s_art_lent && s_art_refused < 8u) {
        const unsigned now = (unsigned)ag_millis();
        if (now - s_art_resize_ms >= AG_ART_GROW_EVERY) {
            ag_meminfo_t gi;

            /*
             * The clock moves HERE, before the test and not after it.
             *
             * With it set only on the paths that acted, a check that found too
             * little room left the timer where it was, the "thirty seconds
             * have passed" test stayed true, and ag_meminfo - twelve
             * milliseconds - ran on every single frame.  During play there is
             * never enough room, so that was every frame of every fight.
             * Maxim: "как будто стало похуже", and he was right: the median
             * rose to 60 fps while the tenth percentile fell from 26.8 to 13.2
             * and the share of slow frames went from 4% to 13%.  A probe that
             * costs twelve milliseconds must be on a clock that cannot stop.
             */
            s_art_resize_ms = now;
            ag_meminfo(&gi);
            {
                const int have = fallout::artCacheMaxSize() / 1024;
                const int want = have + AG_ART_STEP_KB;

                /* Before art_init there is nothing to grow. */
                if (have < AG_ART_FLOOR_KB) {
                    return;
                }
                const unsigned need =
                    (unsigned)(AG_ART_STEP_KB + 1024) * 1024u;

                if (want <= art_ceiling_kb(&gi)
                    && gi.arena_largest >= need
                    && gi.arena_free >= need) {
                    if (fallout::artCacheResize(want * 1024)) {
                        s_art_grows++;
                        s_art_resize_ms = now;
                        ag_printf("fallout: art cache %d -> %d KB (%u KB free, "
                                  "largest %u B, %u grows)\n",
                                  have, want, (unsigned)(gi.arena_free / 1024u),
                                  (unsigned)gi.arena_largest, s_art_grows);
                    } else {
                        /*
                         * Refused, and say so.  This was silent for one build
                         * and cost an hour of wondering why nothing happened:
                         * a resize needs the block empty of live entries, and
                         * the interface holds art for as long as the game is
                         * on the screen (see the comment above artCacheFlush
                         * in map.cc).  If this line is all we ever see, then
                         * there is no moment when the cache can be resized and
                         * the whole idea is answered - by the board, not by an
                         * argument.
                         */
                        s_art_refused++;
                        if (s_art_refused >= 8u) {
                            ag_printf("fallout: the art cache cannot be "
                                      "resized while the game is drawing; "
                                      "not asking again\n");
                        }
                        if (s_art_refused <= 3u) {
                            ag_printf("fallout: art cache would grow %d -> %d "
                                      "KB but something holds art (%u refused)\n",
                                      have, want, s_art_refused);
                        }
                        s_art_resize_ms = now;
                    }
                }
            }
        }
    }

    if (s_art_lent) {
        ag_meminfo_t ai;
        ag_meminfo(&ai);
        const __SIZE_TYPE__ want =
            (__SIZE_TYPE__)fallout::artCacheMaxSize() + 64u * 1024u;
        if ((__SIZE_TYPE__)ai.arena_largest >= want
            && fallout::artCacheTakeBack()) {
            s_art_lent = 0;
            ag_printf("fallout: the art cache has its block back "
                      "(largest was %u B)\n",
                      (unsigned)ai.arena_largest);
        }
    }

    if (reserve_missing() < 0 || s_reserve_bytes == 0u) {
        return;
    }

    static unsigned last_ms;
    const unsigned now = (unsigned)ag_millis();
    if (last_ms != 0u && now - last_ms < 4000u) {
        return;
    }
    last_ms = now;

    ag_meminfo_t mi;
    ag_meminfo(&mi);
    /* Only when taking it back cannot itself cause the next refusal: the hole
     * has to exist with room to spare, not merely exist. */
    /* "Twice the reserve" was too strict to ever fire: a 320 KB reserve wanted
     * a 640 KB hole and this arena never has one - measured largest right
     * after a spend was 270336 B - so the reserve stayed spent for the rest of
     * the session and the save panel had nothing to open into.  Room to spare
     * is 128 KB. */
    /*
     * The hole has to hold the reserve, and taking it must still leave the
     * game a quarter of a megabyte to breathe in.
     *
     * Asking for a hole 128 KB bigger than the reserve was still too strict,
     * and for a reason worth writing down: the hole that appears when the save
     * panel closes is the panel's own 307200 bytes, which is smaller than the
     * 320 KB reserve plus anything.  So the reserve was spent on the first
     * save of a session and never came back, and the second save failed the
     * same way the first would have.  What matters is not the size of the hole
     * beyond the reserve but whether the arena is left with room afterwards.
     */
    if ((__SIZE_TYPE__)mi.arena_largest < (__SIZE_TYPE__)s_reserve_bytes
        || (__SIZE_TYPE__)mi.arena_free
               < (__SIZE_TYPE__)s_reserve_bytes + 256u * 1024u) {
        return;
    }
    {
        const int slot = reserve_missing();
        void *got = ag_malloc(s_reserve_bytes);
        s_reserve[slot] = got;
        if (got == NULL) {
            return;
        }
    }
    {
        s_reserve_rearmed++;
        ag_printf("fallout: a reserve taken back (%u KB free, largest %u B, "
                  "%u spent, %u re-armed)\n",
                  (unsigned)(mi.arena_free / 1024u),
                  (unsigned)mi.arena_largest, s_reserve_spent,
                  s_reserve_rearmed);
    }
}

/*
 * ARGON: the last resort - memory out of the art cache's own block.
 *
 * Maxim, 9 September 2026: "надо не просто очистить кэш арта на отказе, а
 * убрать столько сколько не хватает и дать память именно из кэша арта. Это
 * возможно?"  It is.  The art cache is 2600 KB with its own allocator inside
 * it, that allocator can evict art to make room, and every byte it gives up is
 * a copy of a file on the card - so what a loan costs is a re-read, not a
 * failure.
 *
 * Two kinds of loan, and the second exists because of how the first failed.
 *
 * A LARGE request - a 640x480 window, 307200 bytes - is borrowed directly, and
 * that worked on the first try: "arena empty, lent 300 KB out of the art
 * cache".
 *
 * A SMALL request cannot be, for the same reason that broke pinned animations:
 * a borrowed block is locked, a locked block cannot move when the heap
 * compacts, and a scatter of small locked blocks cuts the heap into pieces too
 * small for the next big frame - black holes where the art should be.  So the
 * first small request borrows ONE slab and every small request after it is
 * served from inside that slab: the cache sees one locked block, not fifty.
 *
 * Without the slab the game died entering a town - 12952 bytes wanted, 43 KB
 * free in pieces of 12032, and the direct lender refused because 12952 was
 * under its floor.  A lender of last resort must not have a floor under which
 * it gives up.
 *
 * The slab's own allocator is deliberately the dullest thing that can work:
 * blocks in address order with a header each, first fit, and a free that merges
 * forward.  It runs only where the arena has already refused, a few times a
 * map, so nothing here needs to be quick - it needs to be obviously right.
 */
#define AG_LOANS      8               /* large loans outstanding at once */
/*
 * One megabyte, in one piece, and taken while the heap is still whole.
 *
 * 256 KB at a time did not work, and the reason is the one that has bitten
 * this file twice already: a borrowed block is LOCKED and cannot move when the
 * heap compacts.  Six of them - two slabs and four direct loans - left the
 * cache with no contiguous 256 KB anywhere, so the third slab could not be
 * taken and the town load died with 256 bytes free in the arena.  Locked
 * blocks must be few and early, or they cut the heap to ribbons.
 *
 * A megabyte out of a 2600 KB cache leaves 1600 - which is the size the cache
 * had for months - and it is only held while the arena is thin.
 */
/*
 * And almost everything comes out of it.  Only a full-screen window (307200)
 * is borrowed on its own now; the 44 KB blocks that used to be direct loans
 * were four more locked blocks for no reason.
 */
#define AG_SLAB_BIG   (128u * 1024u)

static struct {
    void *p;
    int handle;
    __SIZE_TYPE__ size;
} s_loan[AG_LOANS];

static unsigned s_loans_out, s_loans_made, s_loans_refused;

typedef struct ag_slab_block {
    __SIZE_TYPE__ size;   /* payload bytes */
    unsigned      used;
} ag_slab_block_t;

#define AG_SLAB_HDR ((__SIZE_TYPE__)sizeof(ag_slab_block_t))

/*
 * More than one slab, because one was not enough and the game said so.
 *
 * Entering a town asks for block after block of about thirteen kilobytes -
 * nineteen of them fill a 256 KB slab, and the map wanted far more than
 * nineteen.  With a single slab the loans simply stopped and the game died on
 * 6386 bytes with eight kilobytes free in pieces of three.  A lender that runs
 * out is a lender that was not there.
 *
 * Four slabs, taken one at a time as they fill: a megabyte at most, out of a
 * 2600 KB cache, for as long as a map is loading - and every byte of what it
 * displaces is a file on the card that can be read again.  Four locked blocks
 * the heap cannot move is a price worth paying; twenty would not be, which is
 * the whole reason the small loans go through slabs at all.
 */

static void slab_init(int i)
{
    ag_slab_block_t *b = (ag_slab_block_t *)s_slab[i].base;

    b->size = s_slab[i].bytes - AG_SLAB_HDR;
    b->used = 0u;
}

static void *slab_alloc_in(int i, __SIZE_TYPE__ want)
{
    unsigned char *const base = s_slab[i].base;
    const __SIZE_TYPE__ bytes = s_slab[i].bytes;
    __SIZE_TYPE__ at = 0;

    while (at + AG_SLAB_HDR <= bytes) {
        ag_slab_block_t *b = (ag_slab_block_t *)(base + at);

        if (b->used == 0u && b->size >= want) {
            /* Split, if what is left could hold anything at all. */
            if (b->size >= want + AG_SLAB_HDR + 32u) {
                ag_slab_block_t *rest =
                    (ag_slab_block_t *)(base + at + AG_SLAB_HDR + want);

                rest->size = b->size - want - AG_SLAB_HDR;
                rest->used = 0u;
                b->size = want;
            }
            b->used = 1u;
            s_slab_out++;
            s_slab[i].live++;
            return base + at + AG_SLAB_HDR;
        }

        at += AG_SLAB_HDR + b->size;
    }

    return NULL;
}

/*
 * Take one more slab, and say so.  Separate from slab_alloc because the tick
 * calls it too - see the note on borrowing early.
 */
static int slab_take(void)
{
    /*
     * Ask for a megabyte, take what is given.
     *
     * The cache heap is not empty even after a flush: the engine keeps the
     * interface and the party locked in it, and a locked block cannot be moved
     * out of the way.  Measured at the moment of failure, 10 September 2026:
     * 55 locked blocks holding 1312780 bytes - and no contiguous megabyte
     * anywhere among them.  The heap then went looking in the arena, which was
     * empty, and printed its own fatal error.
     *
     * So the sizes come down until one fits.  Half a slab is worth having;
     * failing outright because the best size was not available is not.
     */
    /*
     * Half a megabyte first, not a whole one.
     *
     * A megabyte fits only in a heap that is nearly empty - which is true
     * right after a map change and false ever afterwards.  So the loan was
     * taken during the load, given back when it ended, and then could never be
     * taken again: 230 refusals of eight and sixteen kilobytes went round in a
     * circle with no lender to answer them.  512 KB placed itself every time
     * it was asked, and it was enough for a map change.
     */
    static const unsigned try_kb[] = { 512u, 256u, 128u, 64u };
    int handle = -1;
    unsigned char *got = NULL;
    unsigned i, k;
    unsigned bytes = 0u;

    if (s_slabs_taken >= AG_SLABS) {
        return -1;
    }

    for (k = 0; k < sizeof(try_kb) / sizeof(try_kb[0]); k++) {
        bytes = try_kb[k] * 1024u;
        if (fallout::artBorrow((int)bytes, &handle, &got)) {
            break;
        }
        got = NULL;
    }

    if (got == NULL) {
        return -1;
    }

    i = s_slabs_taken;
    s_slab[i].base = got;
    s_slab[i].handle = handle;
    s_slab[i].bytes = bytes;
    slab_init((int)i);
    s_slabs_taken++;
    s_loans_made++;

    ag_printf("fallout: took slab %u of %u KB out of the art cache\n",
              s_slabs_taken, (unsigned)(bytes / 1024u));

    return (int)i;
}

static void *slab_alloc(__SIZE_TYPE__ want)
{
    unsigned i;

    /* Eight-byte alignment: the engine puts structures in these. */
    want = (want + 7u) & ~(__SIZE_TYPE__)7u;

    for (i = 0; i < s_slabs_taken; i++) {
        void *p = slab_alloc_in((int)i, want);

        if (p != NULL) {
            return p;
        }
    }

    /* All full: take another one. */
    if (slab_take() >= 0) {
        return slab_alloc_in((int)(s_slabs_taken - 1u), want);
    }

    return NULL;
}

static int slab_of(const void *p)
{
    unsigned i;

    for (i = 0; i < s_slabs_taken; i++) {
        if ((const unsigned char *)p >= s_slab[i].base
            && (const unsigned char *)p < s_slab[i].base + s_slab[i].bytes) {
            return (int)i;
        }
    }

    return -1;
}

static int slab_free(void *p)
{
    const int i = slab_of(p);
    unsigned char *base;
    __SIZE_TYPE__ bytes, at = 0;

    if (i < 0) {
        return 0;
    }

    base = s_slab[i].base;
    bytes = s_slab[i].bytes;

    /*
     * Walk from the start.  The header sits immediately before the payload, but
     * trusting a pointer's own header without finding it in the chain would
     * turn one stray free into corruption of the whole slab.
     */
    while (at + AG_SLAB_HDR <= bytes) {
        ag_slab_block_t *b = (ag_slab_block_t *)(base + at);

        if (base + at + AG_SLAB_HDR == (unsigned char *)p) {
            if (b->used == 0u) {
                return 1;   /* freed twice: ours, and already free */
            }
            b->used = 0u;
            s_slab_out--;
            if (s_slab[i].live != 0u) {
                s_slab[i].live--;
            }

            for (;;) {
                __SIZE_TYPE__ next = at + AG_SLAB_HDR + b->size;
                ag_slab_block_t *n;

                if (next + AG_SLAB_HDR > bytes) {
                    break;
                }
                n = (ag_slab_block_t *)(base + next);
                if (n->used != 0u) {
                    break;
                }
                b->size += AG_SLAB_HDR + n->size;
            }
            return 1;
        }

        at += AG_SLAB_HDR + b->size;
    }

    /* Inside a slab but not a block start: not something we handed out, and
     * not something to guess about. */
    return 0;
}

static void *arena_borrow(__SIZE_TYPE__ wanted)
{
    int i;

    if (wanted == 0u || ag_loans_on == 0) {
        return NULL;
    }

    /* One thread only; see s_loan_thread. */
    if (s_loan_thread == NULL) {
        s_loan_thread = loan_thread_now();
    } else if (loan_thread_now() != s_loan_thread) {
        return NULL;
    }

    if (wanted < AG_SLAB_BIG) {
        void *p = slab_alloc(wanted);

        if (p == NULL) {
            s_loans_refused++;
        }
        return p;
    }

    for (i = 0; i < AG_LOANS; i++) {
        if (s_loan[i].p == NULL) {
            int handle = -1;
            unsigned char *got = NULL;

            if (!fallout::artBorrow((int)wanted, &handle, &got)) {
                s_loans_refused++;
                return NULL;
            }

            s_loan[i].p = got;
            s_loan[i].handle = handle;
            s_loan[i].size = wanted;
            s_loans_out++;
            s_loans_made++;

            ag_printf("fallout: arena empty, lent %u KB out of the art cache "
                      "(%u on loan, %u made)\n",
                      (unsigned)(wanted / 1024u), s_loans_out, s_loans_made);
            return got;
        }
    }

    s_loans_refused++;
    return NULL;
}

static int arena_giveback(void *p, __SIZE_TYPE__ *sizePtr)
{
    int i;

    /*
     * Nothing borrowed can reach another thread - the borrower refuses every
     * thread but one - so a pointer offered from anywhere else is certainly
     * not ours, and saying so without touching the lists is both correct and
     * safe.
     */
    if (s_loan_thread != NULL && loan_thread_now() != s_loan_thread) {
        return 0;
    }

    for (i = 0; i < AG_LOANS; i++) {
        if (s_loan[i].p == p) {
            if (sizePtr != NULL) {
                *sizePtr = s_loan[i].size;
            }
            fallout::artReturn(&(s_loan[i].handle));
            s_loan[i].p = NULL;
            s_loan[i].handle = -1;
            s_loan[i].size = 0;
            s_loans_out--;
            return 1;
        }
    }

    if (slab_of(p) >= 0) {
        if (sizePtr != NULL) {
            const ag_slab_block_t *b =
                (const ag_slab_block_t *)((unsigned char *)p - AG_SLAB_HDR);
            *sizePtr = b->size;
        }
        return slab_free(p);
    }

    return 0;
}

/*
 * ARGON: the map change says when it starts and when it is done.
 *
 * The tick cannot see a map load - loading is one long call and nothing is
 * drawn while it runs, so renderPresent, and with it the tick, does not
 * happen.  map.cc calls these instead, around the same place it raises
 * ag_map_loading.
 */
extern "C" void ag_loan_map_begin(void)
{
    if (ag_loans_on == 0) {
        return;
    }
    if (s_slabs_taken == 0u) {
        (void)slab_take();
    }
}

/*
 * Give back every slab that has emptied, and keep the ones still lived in.
 *
 * The slabs are a stack - slab_alloc fills them in order and slab_take pushes -
 * so returning one from the middle would leave a hole in the array.  Instead
 * the tail is returned while the tail is empty, which is the common case: the
 * stragglers are usually in the first slab, taken earliest during the load.
 */
/* How much of the art cache is out on loan as slabs, in KB. */
static unsigned slabs_held_kb(void)
{
    unsigned i, kb = 0;

    for (i = 0; i < s_slabs_taken; i++) {
        kb += (unsigned)(s_slab[i].bytes / 1024u);
    }
    return kb;
}

/* Set by slabs_return_empty to what it actually handed back, in KB - the
 * sizes are negotiated, so a count of slabs says nothing. */
unsigned freed_kb;

static unsigned slabs_return_empty(void)
{
    unsigned freed = 0;

    freed_kb = 0;

    while (s_slabs_taken != 0u && s_slab[s_slabs_taken - 1u].live == 0u) {
        const unsigned i = s_slabs_taken - 1u;

        freed_kb += (unsigned)(s_slab[i].bytes / 1024u);
        fallout::artReturn(&(s_slab[i].handle));
        s_slab[i].base = NULL;
        s_slab[i].bytes = 0;
        s_slab[i].live = 0u;
        s_slabs_taken--;
        freed++;
    }

    return freed;
}

extern "C" void ag_loan_map_end(void)
{
    const unsigned freed = slabs_return_empty();

    if (freed != 0u) {
        ag_printf("fallout: map loaded, gave back %u KB of the art cache "
                  "(%u KB still on loan)\n",
                  freed_kb, slabs_held_kb());
    }
}

static int on_out_of_arena(__SIZE_TYPE__ wanted)
{
    /*
     * ARGON: the census first, before anything is given back.
     *
     * "What was the game holding when it was refused" is the question every
     * one of these failures raises, and the two flushes below are the first
     * thing that changes the answer.  With the live-bytes profiler compiled in
     * and switched on (-prof N) this prints the table at exactly the moment of
     * the refusal; without it the call does nothing and costs nothing.
     */
    {
        ag_meminfo_t before;
        ag_meminfo(&before);
        ag_printf("fallout: refused %u B with %u KB used of %u, hole %u B - "
                  "what is held:\n",
                  (unsigned)wanted,
                  (unsigned)((before.arena_total - before.arena_free) / 1024u),
                  (unsigned)(before.arena_total / 1024u),
                  (unsigned)before.arena_largest);
        ag_libc_malloc_profile_dump(32);
    }

    /* Every pin first: a held entry is one the sweep below cannot touch, and
     * under real pressure a fast walk animation is worth less than a frame
     * that happens at all. */
    fallout::artDropPins();

    const int flushed = fallout::artCacheFlush() ? 1 : 0;
    /*
     * And the sound cache, which is worth as much as the art flush is not.
     * Measured at the same place on the starting map, one config line apart:
     *
     *     sound on   5257 KB used of 6000, largest hole  94 KB
     *     sound off  4961 KB used of 6000, largest hole 512 KB
     *
     * 296 KB held, and it fragments as much as it fills: the largest hole goes
     * from 512 KB to 94 with sound on.  A map change wants about 720 KB more
     * than standing still, and 5257 + 720 is 5977 of a 6000 KB arena - which
     * is the number on the console before every temple crash.
     */
    fallout::soundEffectsCacheFlush();

    ag_meminfo_t mi;
    ag_meminfo(&mi);
    if ((__SIZE_TYPE__)mi.arena_largest >= wanted) {
        ag_printf("fallout: %u bytes refused; the art cache going was enough "
                  "(largest now %u B)\n",
                  (unsigned)wanted, (unsigned)mi.arena_largest);
        return 1;
    }

    /*
     * ARGON: and then the sound cache's whole block, which is the one thing
     * here that hands the arena a large *contiguous* piece.
     *
     * Measured at the moment a save load was refused, 7 September 2026:
     *
     *     refused 127888 B: 5449 KB used of 5604, largest hole 118784 B
     *
     * Both flushes above had already run and neither returned a byte to the
     * arena - a flush frees the entries inside a cache and keeps the block.
     * The shortfall was nine kilobytes of contiguity. This gives back 257 KB
     * in one piece, and fallout_reserve_tick takes it again a second later
     * once the peak has passed.
     */
    if (fallout::soundEffectsCachePark()) {
        ag_meminfo(&mi);
        if ((__SIZE_TYPE__)mi.arena_largest >= wanted) {
            ag_printf("fallout: %u bytes refused; parking the sound cache was "
                      "enough (largest now %u B)\n",
                      (unsigned)wanted, (unsigned)mi.arena_largest);
            return 1;
        }
    }

    /*
     * ARGON: and then the art cache's whole block - the largest contiguous
     * thing this program owns, and on a full-screen panel the least used.
     *
     * This step is deliberately here and not at the panel's own door.  A board
     * with room never refuses an allocation, so this hook never runs and the
     * cache is never lent: on 16 MB the game keeps every byte of its cache and
     * behaves exactly as its author built it.  The lending is not an
     * optimisation to apply everywhere - it is what a 6000 KB arena has to do
     * to show a 640x480 panel, and nothing more.
     *
     * It is taken back by fallout_reserve_tick as soon as there is a hole to
     * put it in, which in practice is the moment the panel closes and gives
     * back its own 600 KB.
     */
    /*
     * Shrink before anything else is spent: this is the arena's own slack
     * coming back, and unlike a flush it hands over contiguous bytes.
     */
    {
        const int have = fallout::artCacheMaxSize() / 1024;
        if (!s_art_lent && have - AG_ART_STEP_KB >= AG_ART_FLOOR_KB) {
            const int want = have - AG_ART_STEP_KB;
            if (fallout::artCacheResize(want * 1024)) {
                s_art_shrinks++;
                s_art_resize_ms = (unsigned)ag_millis();
                ag_meminfo(&mi);
                ag_printf("fallout: %u bytes refused; art cache %d -> %d KB "
                          "(largest now %u B, %u shrinks)\n",
                          (unsigned)wanted, have, want,
                          (unsigned)mi.arena_largest, s_art_shrinks);
                if ((__SIZE_TYPE__)mi.arena_largest >= wanted) {
                    return 1;
                }
            }
        }
    }

    if (!s_art_lent && fallout::artCacheLend()) {
        s_art_lent = 1;
        ag_meminfo(&mi);
        ag_printf("fallout: %u bytes refused; lent the art cache's block "
                  "(largest now %u B, %u KB free)\n",
                  (unsigned)wanted, (unsigned)mi.arena_largest,
                  (unsigned)(mi.arena_free / 1024u));
        if ((__SIZE_TYPE__)mi.arena_largest >= wanted) {
            return 1;
        }
    }

    /* The small refusals first, so they can never reach the big hole. */
    if (s_small != NULL && wanted < s_reserve_bytes / 2u) {
        ag_free(s_small);
        s_small = NULL;
        s_small_spent++;
        ag_printf("fallout: %u bytes refused with %u KB free in holes of %u B; "
                  "spending the %u KB small reserve (%u spent), big one kept\n",
                  (unsigned)wanted, (unsigned)(mi.arena_free / 1024u),
                  (unsigned)mi.arena_largest, s_small_bytes / 1024u,
                  s_small_spent);
        return 1;
    }

    if (reserve_held() >= 0 && wanted >= s_reserve_bytes / 2u) {
        s_reserve_spent++;
        ag_free(s_reserve[reserve_held()]);
        s_reserve[reserve_held()] = NULL;
        ag_printf("fallout: %u bytes refused with %u KB free in holes of %u B; "
                  "spending the %u KB reserve\n",
                  (unsigned)wanted, (unsigned)(mi.arena_free / 1024u),
                  (unsigned)mi.arena_largest, s_reserve_bytes / 1024u);
        return 1;
    }

    /*
     * ARGON: and last of all, the big reserve to a small request after all.
     *
     * Keeping it back for the save panel is right until the alternative is
     * dying, and on 8 September 2026 it was: the board asked for 9614 bytes
     * with 9 KB free in holes of 6400, the small reserve already spent, and
     * this function said no while holding 320 KB.  interpreter.cc took the
     * refusal and the game went.  A run that cannot open the save panel is
     * worth having; a run that has stopped is not.  Preferring the small
     * reserve first is what the priority is for - it is not a promise to sit
     * on the big one while the program dies.
     */
    if (reserve_held() >= 0) {
        s_reserve_spent++;
        ag_free(s_reserve[reserve_held()]);
        s_reserve[reserve_held()] = NULL;
        ag_printf("fallout: %u bytes refused with %u KB free in holes of %u B "
                  "and no small reserve left; spending the %u KB reserve as a "
                  "last resort - the save panel may refuse until it is taken "
                  "back\n",
                  (unsigned)wanted, (unsigned)(mi.arena_free / 1024u),
                  (unsigned)mi.arena_largest, s_reserve_bytes / 1024u);
        return 1;
    }

    ag_printf("fallout: %u bytes refused; %u KB free, largest %u B, and both "
              "reserves are already spent\n",
              (unsigned)wanted, (unsigned)(mi.arena_free / 1024u),
              (unsigned)mi.arena_largest);
    return flushed;
}

/* In port/sdl_shim.cc.  Not SDL: an automated run has no eyes. */
extern "C" void sdl_shim_screenshot(const char *path, int after_frames,
                                    int after_ms);
extern "C" void sdl_shim_memstat(int every_ms, int trace_kb,
                                 int trace_max, int trace_prof);
extern "C" void sdl_shim_fps(int every_ms);
extern "C" void sdl_shim_scale_sharp(int on);
extern "C" void sdl_shim_cycle_ms(int ms);
extern "C" void sdl_shim_events(int on);
extern "C" void sdl_shim_recinput(int on);
extern "C" void sdl_shim_watchdog(unsigned ms);
extern "C" void sdl_shim_hud(int on);
extern "C" void sdl_shim_log(const char *path);
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
 *   -reserve KB    hold this much of the arena back at startup and hand it
 *                  over on a refused allocation.  Off by default: it answers
 *                  fragmentation, and against plain exhaustion it makes the
 *                  peak worse by its own size.  See on_out_of_arena.
 *   -memstat N     print arena use, and the peak so far, every N ms
 *   -mallocs KB N  with -memstat, name the first N allocations of KB
 *                  or more (default 32 and 64)
 *   -prof N        with -memstat, count allocations by call site and
 *                  print the N biggest each window
 *   -fps N         print the frame rate and where a frame goes, every N ms
 *   -autostart     start a new game at once, premade character, no movie
 *   -log PATH      write every diagnostic line to this file as well as
 *                  to the console, including the engine's own errors and
 *                  the out-of-arena line.  The shell's `>` cannot do this
 *                  for a game: `run` hands the process to the session and
 *                  returns, and the redirection goes with it.
 *
 *                  On a board write it to the card or to C: - `-log c:\fo2.log`
 *                  or `-log a:\fo2.log`.  H: is the host folder QEMU forwards
 *                  and only exists when the emulator was started with -HostFs;
 *                  pointing at it anywhere else quietly produces no log, which
 *                  is how one session was lost
 *   -hud           three numbers in the corner of the panel: frames per
 *                  second, arena in use, largest free block - all in KB,
 *                  green while a full-screen buffer would still fit,
 *                  amber under half of one, red under a quarter.  For a
 *                  board, where the console is a cable to a machine that
 *                  is not there
 *   -smooth        when the panel is smaller than the picture, average
 *                  each 2x2 rather than taking its top-left pixel.  A
 *                  stroke one pixel wide is dimmed rather than dropped,
 *                  and an interface is mostly strokes one pixel wide
 *   -inplace MASK  which kinds of art may be read straight out of flash,
 *                  a bit per object type (1 item, 2 critter, 4 scenery,
 *                  8 wall, 16 tile); 31 by default, 0 to copy everything.
 *   -artflash N    the flash art store: 0 off, 1 on (default), 2 empty it
 *                  first.  Off means the same binary reads everything from
 *                  the card, which is what an A/B against it needs.
 *   -artcache N    the art cache in kilobytes, overriding fallout2.cfg.  Its
 *                  floor is a hard one - a single art item in this game is
 *                  505100 bytes, and a cache that cannot hold it kills the
 *                  engine - and its ceiling was measured standing on a map,
 *                  which is not the same as playing.  This is the flag for
 *                  finding out during a real game
 *   -watchdog N    seconds the game may go without drawing a frame before
 *                  the supervisor kills it, armed on the first frame.  15 by
 *                  default; 0 turns it off.  A game that stops with this off
 *                  stops in silence - no message, no crash record, and the
 *                  console's input goes with it, because what stopped is the
 *                  one process that owns the session.  That is what "the board
 *                  hangs" turned out to be
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

/* Read by artInit(); see patches/fallout2-ce/0021.  Zero means the setting in
 * fallout2.cfg stands. */
extern "C" int ag_art_cache_kb = 0;
/*
 * How many of the character's animations are held against eviction; -pins N.
 *
 * OFF, and it stays off until the flaw below is answered.
 *
 * The measurement said it worked: at four pins, critter misses per hundred
 * hexes fell from 358.9 to 224.6 and the total time lost to reading fell a
 * quarter.  Then Maxim played on it and the map came up with black holes in
 * it, and the board said why:
 *
 *     CACHE: ... would not load - 1666 of 1800 KB in the cache is in use, 106 entries
 *     [hud] fps 0.34  eng 2799 ms
 *
 * The cache's heap COMPACTS - that is what its handles are for - but it can
 * only move blocks nobody is holding.  Four pinned animations of 120 KB stand
 * still in the middle of it and cut the free space into pieces too small for
 * the next big frame, so art stops loading at all: black where the picture
 * should be, and a frame rate of a third of a frame a second while the engine
 * retries.  It is not a matter of four pins against eight; a pinned block is
 * immovable by design, and immovable is what breaks the heap.
 *
 * Whatever replaces this has to keep the art movable - a preference in the
 * eviction sweep rather than a reference count.
 */
extern "C" int ag_art_pins = 0;

/*
 * How much a cache sweep evicts beyond what was asked for, in KB; -evict N.
 *
 * The engine's own rule is "the requested size plus a fifth of the cache",
 * which on a 1800 KB cache means throwing away 363 KB to find room for a 3 KB
 * floor tile - and then reading most of it back, because the least recently
 * used art on a map you are walking across is the part of the map just behind
 * you.  32 KB is a starting point, not an answer; the flag is here so the
 * answer can be measured.
 */
extern "C" int ag_cache_slack_kb = 32;

/*
 * The hit credit an entry gets when the cache fetches it back after throwing
 * it out; -boost N.
 *
 * The eviction sweep picks by hit count, and an evicted entry loses its count
 * along with itself - so what came back was always first in line to go again.
 * 93% of the misses while walking were exactly that loop.  Eight is a guess to
 * be measured, not an answer.
 */
extern "C" int ag_refetch_boost = 8;

/*
 * Which entry the art cache throws out; -lru N.
 *
 * 1: the one nobody has asked for longest - Maxim's suggestion, and the right
 * one for a map, where what you needed a step ago is what the next step wants.
 * 0: the engine's own order, fewest hits first, which evicts art the moment
 * after it was read and never lets a re-read entry recover.  See the note in
 * cacheEntriesCompareByUsage.
 */
extern "C" int ag_cache_lru = 1;

/*
 * Whether the arena may borrow from the art cache at all; -loans N.
 *
 * On by default because it is what lets the save panel open and a map change
 * finish.  Off, because in a town it may be the disease: a borrowed block is
 * LOCKED, the heap cannot move it, and a big city decoration - mabromaa.frm,
 * 174 KB - then finds no run of free space long enough and spills into the
 * arena instead.  The arena refuses, the ladder empties the whole cache, and
 * all five hundred files of the town are read again.  In the town run the
 * cache was emptied 229 times and 312 MB came off the card.
 */
int ag_loans_on = 1;

/*
 * The directories a save needs, made before the engine is entered.
 *
 * The engine makes them itself - and cannot, here. It builds them as
 * "<master_patches>\\SAVEGAME", and this port runs with master_patches empty
 * (see fallout2.cfg: the loose-folder search path costs about 75 KB of arena
 * and there is no patched art to install). Empty leaves "\\SAVEGAME", while the
 * file it then opens is the relative "SAVEGAME\\SLOT01\\SAVE.DAT" - and whether
 * those two land in the same place is not something the engine's own mkdir
 * checks: it ignores the result and carries on to fail at the fopen with
 * "** Error opening save game for writing! **".
 *
 * Measured on the board: no A:\\SAVEGAME, no A:\\MAPS, and saving impossible.
 * Rather than rewrite twenty-five path expressions in the engine, make the
 * directories here, with exactly the relative paths the engine's writes use.
 * Ten slots because the save menu offers ten, and proto/critters and
 * proto/items because lsgPerformSaveGame makes them per slot.
 *
 * Failure is not reported: a directory that exists already fails the same way
 * as one that cannot be made, and this runs on every start.
 */
static void make_save_dirs(void)
{
    /* Built by hand: this file keeps clear of the toolchain's stdio, and two
     * digits and a tail are not worth an snprintf. */
    static const char *const kTails[] = {
        "", "\\proto", "\\proto\\critters", "\\proto\\items",
    };
    char path[64];

    ag_mkdir("SAVEGAME");
    ag_mkdir("MAPS");
    /*
     * And where the engine keeps modified prototypes.  Saving copies these
     * into the slot with a plain fopen - not through the archive - so they
     * have to exist as loose files, and they cannot exist if the directory
     * the engine writes them to does not.  Without these two the save dies at
     * the first party member with "Error writing save function #3 data", and
     * the only trace is opendir(proto/critters) refused: not found.
     */
    ag_mkdir("proto");
    ag_mkdir("proto\\critters");
    ag_mkdir("proto\\items");
    for (int slot = 1; slot <= 10; slot++) {
        for (unsigned t = 0; t < sizeof(kTails) / sizeof(kTails[0]); t++) {
            const char *tail = kTails[t];
            unsigned at = 0;
            for (const char *p = "SAVEGAME\\SLOT"; *p != '\0'; p++) {
                path[at++] = *p;
            }
            path[at++] = (char)('0' + slot / 10);
            path[at++] = (char)('0' + slot % 10);
            for (const char *p = tail; *p != '\0'; p++) {
                path[at++] = *p;
            }
            path[at] = '\0';
            ag_mkdir(path);
        }
    }
}

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
    int reserve_kb = 0;
    int trace_kb = 0;
    int trace_max = 0;
    int trace_prof = 0;
    int sharp = 0;
    int cycle_ms = 100;
    int events = 0;
    int recinput = 0;
    int hud = 0;
    const char *logpath = NULL;
    int fps_ms = 0;
    int watchdog_s = 0;
    int watchdog_set = 0;
    int out = 0;

    /*
     * What arrived, before anything is taken out of it.  Printed after the
     * parse it looked identical whether every flag had been understood or
     * none had been passed at all - one line, two opposite meanings, and I
     * read it the wrong way round.
     */
    {
        char got[400];
        int at = 0;

        for (int i = 0; i < argc && at < (int)sizeof(got) - 1; i++) {
            const char* one = argv[i] ? argv[i] : "?";
            got[at++] = ' ';
            for (int k = 0; one[k] != 0 && at < (int)sizeof(got) - 1; k++) {
                got[at++] = one[k];
            }
            got[at] = 0;
        }
        ag_printf("fallout: %d arguments arrived:%s\n", argc, (at > 0) ? got : " none");
    }

    for (int i = 0; i < argc; ++i) {
        if (same(argv[i], "-shot") && i + 1 < argc) {
            shot = argv[++i];
        } else if (same(argv[i], "-frames") && i + 1 < argc) {
            frames = to_int(argv[++i]);
        } else if (same(argv[i], "-shotms") && i + 1 < argc) {
            shot_ms = to_int(argv[++i]);
        } else if (same(argv[i], "-memstat") && i + 1 < argc) {
            memstat_ms = to_int(argv[++i]);
        } else if (same(argv[i], "-reserve") && i + 1 < argc) {
            reserve_kb = to_int(argv[++i]);
        } else if (same(argv[i], "-mallocs") && i + 2 < argc) {
            trace_kb = to_int(argv[++i]);
            trace_max = to_int(argv[++i]);
        } else if (same(argv[i], "-prof") && i + 1 < argc) {
            trace_prof = to_int(argv[++i]);
        } else if (same(argv[i], "-fps") && i + 1 < argc) {
            fps_ms = to_int(argv[++i]);
        } else if (same(argv[i], "-autostart")) {
            ag_autostart = 1;
        } else if (same(argv[i], "-smooth") || same(argv[i], "-sharp")) {
            sharp = 1;
        } else if (same(argv[i], "-cycle") && i + 1 < argc) {
            cycle_ms = to_int(argv[++i]);
        } else if (same(argv[i], "-events")) {
            events = 1;
        } else if (same(argv[i], "-recinput")) {
            recinput = 1;
        } else if (same(argv[i], "-hud")) {
            hud = 1;
        } else if (same(argv[i], "-log") && i + 1 < argc) {
            logpath = argv[++i];
        } else if (same(argv[i], "-scripts") && i + 1 < argc) {
            ag_script_cache = to_int(argv[++i]);
        } else if (same(argv[i], "-loans") && i + 1 < argc) {
            ag_loans_on = to_int(argv[++i]);
        } else if (same(argv[i], "-lru") && i + 1 < argc) {
            ag_cache_lru = to_int(argv[++i]);
        } else if (same(argv[i], "-boost") && i + 1 < argc) {
            ag_refetch_boost = to_int(argv[++i]);
        } else if (same(argv[i], "-evict") && i + 1 < argc) {
            ag_cache_slack_kb = to_int(argv[++i]);
        } else if (same(argv[i], "-pins") && i + 1 < argc) {
            ag_art_pins = to_int(argv[++i]);
        } else if (same(argv[i], "-artcache") && i + 1 < argc) {
            ag_art_cache_kb = to_int(argv[++i]);
        } else if (same(argv[i], "-artflash") && i + 1 < argc) {
            ag_art_store_mode(to_int(argv[++i]));
        } else if (same(argv[i], "-inplace") && i + 1 < argc) {
            ag_art_store_in_place((unsigned)to_int(argv[++i]));
        } else if (same(argv[i], "-flashbatch") && i + 1 < argc) {
            ag_art_store_batch_kb((unsigned)to_int(argv[++i]));
        } else if (same(argv[i], "-flashwhen") && i + 1 < argc) {
            ag_art_store_write_in_play(to_int(argv[++i]));
        } else if (same(argv[i], "-flashcap") && i + 1 < argc) {
            ag_art_store_cap_kb((unsigned)to_int(argv[++i]));
        } else if (same(argv[i], "-flashstage") && i + 1 < argc) {
            ag_art_store_stage_kb((unsigned)to_int(argv[++i]));
        } else if (same(argv[i], "-watchdog") && i + 1 < argc) {
            watchdog_s = to_int(argv[++i]);
            watchdog_set = 1;
        } else {
            argv[out++] = argv[i];
        }
    }
    argv[out] = NULL;

    /*
     * ARGON: what the game believes it was started with.
     *
     * A switch was set on the command line, the console echoed it, and the
     * behaviour did not change - and there was no way to tell whether the
     * argument had reached the parser, whether the parser had understood it,
     * or whether the board was still running yesterday's binary.  Three
     * different faults with one symptom.  So the game says it out loud: this
     * is the argument list as the program sees it, after its own parsing.
     */
    {
        char seen[400];
        int at = 0;

        for (int i = 1; i < out && at >= 0 && at < (int)sizeof(seen) - 1; i++) {
            const char* one = argv[i] ? argv[i] : "?";
            seen[at++] = ' ';
            for (int k = 0; one[k] != '\0' && at < (int)sizeof(seen) - 1; k++) {
                seen[at++] = one[k];
            }
            seen[at] = '\0';
        }
        /* ag_printf, not ag_log: an application's log reaches nobody. */
        ag_printf("fallout: flags the game kept:%s\n", (at > 0) ? seen : " none");
    }

    /* ARGON: the flash art store, once the flags have been read and before
     * anything asks for a picture.  On a machine whose partition table has no
     * store this does nothing at all and the game behaves exactly as before -
     * which is also what -artflash 0 gives. */
    ag_art_store_init();

    if (shot != NULL) {
        if (frames <= 0 && shot_ms <= 0) {
            frames = 1;
        }
        sdl_shim_screenshot(shot, frames, shot_ms);
    }
/*
     * ARGON: where this image actually landed.
     *
     * The allocation profiler reports call sites as runtime addresses, and the
     * loader puts the code wherever it likes - so the table it prints cannot be
     * turned back into function names without knowing the slide.  The kernel
     * logs the load address, but the kernel log is invisible while a game runs
     * and a reset wipes the journal, which cost an hour of chasing it.
     *
     * One line, once: the runtime address of a function whose address in the
     * ELF is a matter of nm.  The difference is the slide, and every profile
     * from this run decodes offline.
     */
    ag_printf("fallout: ag_loan_map_begin is at %p (subtract its ELF address "
              "for the slide)\n", (void *)&ag_loan_map_begin);

    ag_libc_oom_sink(on_out_of_arena);
    ag_libc_borrow_sink(arena_borrow, arena_giveback);
    /* Before the engine has taken anything, so the hole is certain and is one
     * piece - and only if asked for.  See on_out_of_arena. */
    /*
     * ARGON: the art cache scales with the arena instead of being a number.
     *
     * Measured 9 September 2026: the art the game asks for in twenty seconds
     * weighs 2118 KB at the median and 2940 at the worst, against a cache of
     * 1800 - and 91% of the misses while walking were for art it had just
     * evicted.  It wants to be bigger.  On THIS board it cannot be: the block
     * is held whole whether or not anything is in it, the arena peaks at 5977
     * of 6000 in the Temple, and it cannot be handed back under pressure
     * because the interface holds art for as long as the game is drawing.
     *
     * So the size is a quarter of the arena, floored at whatever the settings
     * asked for and capped at 3000 KB:
     *
     *   - a quarter of 6000 is 1500, below the configured 1800, so this board
     *     keeps exactly what it has today and nothing about it changes;
     *   - a 16 MB board reaches the cap on its own, with nobody editing this
     *     file, which is the rule Maxim set: no memory compromise hard-coded,
     *     and the author's own behaviour on a board with room;
     *   - the cap is 3000 because 2940 KB was the largest working set ever
     *     measured here.  Past that a bigger cache buys nothing at all.
     *
     * -artcache N still overrides everything, because whoever is playing can
     * answer questions a measurement cannot.
     */
    if (ag_art_cache_kb <= 0) {
        ag_meminfo_t ai;
        ag_meminfo(&ai);
        {
            const int quarter = (int)(ai.arena_total / 1024u / 4u);
            const int floor_kb = 1800;   /* what the settings ask for today */
            const int cap_kb = 3000;
            int want = quarter;

            if (want < floor_kb) {
                want = floor_kb;
            }
            if (want > cap_kb) {
                want = cap_kb;
            }
            if (want != floor_kb) {
                ag_art_cache_kb = want;
                ag_printf("fallout: art cache %d KB, a quarter of a %u KB "
                          "arena\n",
                          want, (unsigned)(ai.arena_total / 1024u));
            }
        }
    }

    fallout_reserve_kb(reserve_kb);
    /* A fifth of the big one: enough for every small refusal seen (2984, 9614)
     * many times over, and not worth stealing from the panel. */
    s_small_bytes = (reserve_kb > 0) ? 64u * 1024u : 0u;
    reserve_take();

    if (watchdog_set) {
        sdl_shim_watchdog((unsigned)watchdog_s * 1000u);
    }
    sdl_shim_log(logpath);
    sdl_shim_memstat(memstat_ms, trace_kb, trace_max, trace_prof);
    sdl_shim_fps(fps_ms);
    sdl_shim_scale_sharp(sharp);
    sdl_shim_cycle_ms(cycle_ms);
    /* The readout and the event log are the same instrument seen two
     * ways, so -hud brings both.  -events alone is for a run left going
     * with nobody watching the glass. */
    sdl_shim_recinput(recinput);
    sdl_shim_events(events || hud);
    sdl_shim_hud(hud);

    make_save_dirs();

    const int rc = main(out, argv);

    /* Whatever was on screen when it stopped, if a screenshot was asked for
     * and its moment never came. */
    sdl_shim_screenshot_now();
    return rc;
}
