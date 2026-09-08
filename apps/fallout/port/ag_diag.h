#ifndef AG_DIAG_H
#define AG_DIAG_H

/*
 * ARGON: why the frame rate drops, measured rather than guessed.
 *
 * Maxim, 8 September 2026: "Я это захотел из-за частых просадок фпс... а может
 * есть другие причины?" - and then the one observation worth more than any
 * counter: "всегда есть просадка когда я атакую цель".  Always.  The same
 * action, the same dip, every time.
 *
 * What the port could already say about a dip was this:
 *
 *     worst 295 ms (eng 281, wire 2)
 *
 * The wire is innocent and the engine took 281 ms - and that is the end of
 * what the numbers knew.  "Somewhere in the engine" is not a diagnosis, and
 * three things in this file turn it into one.
 *
 * 1. The caches, per cache and per activity (ag_cache_*).  The art and sound
 *    caches both go through cacheLock, so every earlier figure was the two of
 *    them added together, there was no count of hits to give the misses a
 *    scale, and a miss during a fight was the same number as a miss during a
 *    map load.
 *
 * 2. The files those misses read, by name and by total cost (ag_top_*).  An
 *    attack loads the attack animation, and a critter animation is a few
 *    hundred kilobytes off a FAT card.  If that is what the dip is, this table
 *    names it: the file, how many times it has been read, and the seconds it
 *    has cost so far.  A file that appears with a count of thirty is not a
 *    cold cache, it is a cache too small to hold it - which is a different
 *    fix.
 *
 * 3. The worst frame of the window, split by what the main loop was doing
 *    (ag_frame_note).  The six phases are already timed - patch 0036 - but
 *    only as averages per frame, and an average cannot show a dip: that is
 *    what an average is for.  So the worst lap is kept whole.
 *
 * Between them a dip has to land in one of: a file read (1 and 2), a script
 * or an animation (3, phase 3), the map (3, phase 4), or the picture going out
 * (3, phase 5, which is the wire and was already known).  If it lands nowhere,
 * that is worth knowing too, and it is the next thing to instrument.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Which cache.  Both go through cacheLock; nothing else uses one yet, and
 * OTHER is there so a third one is mis-attributed rather than out of bounds. */
enum {
    AG_CACHE_ART = 0,
    AG_CACHE_SOUND = 1,
    AG_CACHE_OTHER = 2,
    AG_CACHE_KINDS = 3
};

/* And what the game was doing.  WALK is the default on a live map with no
 * window open - walking, looking, waiting - and is where most of the playing
 * happens.  MENU is the main menu, where a miss costs nobody anything. */
enum {
    AG_WHERE_WALK = 0,
    AG_WHERE_COMBAT = 1,
    AG_WHERE_DIALOG = 2,
    AG_WHERE_UI = 3,
    AG_WHERE_MAPLOAD = 4,
    AG_WHERE_WORLDMAP = 5,
    AG_WHERE_MENU = 6,
    AG_WHERES = 7
};

/* Indexed [kind * AG_WHERES + where] - flat, because a two-dimensional array
 * shared between two files is a way to get the dimensions wrong in one of them
 * and have it link anyway.
 *
 * The counters live in the port (sdl_shim.cc) rather than in cache.cc because
 * SDLTEST links the shim without the engine, and a report naming symbols the
 * engine defines would not link at all. */
#define AG_CACHE_SLOTS (AG_CACHE_KINDS * AG_WHERES)

extern unsigned ag_cache_hit[AG_CACHE_SLOTS];
extern unsigned ag_cache_miss[AG_CACHE_SLOTS];
extern unsigned ag_cache_refetch[AG_CACHE_SLOTS];
extern unsigned ag_cache_miss_kb[AG_CACHE_SLOTS];
extern unsigned ag_cache_miss_ms[AG_CACHE_SLOTS];
extern unsigned ag_cache_out[AG_CACHE_SLOTS];
extern unsigned ag_cache_flush[AG_CACHE_SLOTS];
/* Misses the cache could not satisfy at all - the entry was too big, or the
 * heap would not give a block even after emptying itself.  The engine draws
 * nothing for these, so they are the ones a player sees as a missing sprite. */
extern unsigned ag_cache_failed[AG_CACHE_SLOTS];

/*
 * The files that cost the most, by name.
 *
 * Set where the read happens - artCacheReadDataImpl and its sound counterpart
 * both already know the path - and collected here by cache.cc while it holds
 * the clock.  Kept as a table of the worst by total time, merged by name, so a
 * file read thirty times shows as one row with a count of thirty rather than
 * pushing everything else out.
 */
#define AG_NAME_MAX 24
#define AG_TOP_MISSES 10

/* The file the current miss is reading.  Written by the cache's read proc,
 * consumed by cacheLock; empty means the read proc did not say. */
extern char ag_miss_name[AG_NAME_MAX];

extern char ag_top_name[AG_TOP_MISSES][AG_NAME_MAX];
extern unsigned ag_top_count[AG_TOP_MISSES];
extern unsigned ag_top_ms[AG_TOP_MISSES];
extern unsigned ag_top_kb[AG_TOP_MISSES];
extern unsigned char ag_top_kind[AG_TOP_MISSES];
extern unsigned char ag_top_where[AG_TOP_MISSES];

/*
 * The worst lap of the reporting window, kept whole.
 *
 * Called from the main loop with that lap's own six phase times; keeps the
 * worst and what the caches did inside it.  Throttle is the deliberate sleep
 * and is not part of the cost - it is reported so that a frame which was slow
 * *and* still slept can be told from one that was not.
 */
void ag_frame_note(unsigned input_us, unsigned keys_us, unsigned scripts_us,
    unsigned maps_us, unsigned present_us, unsigned throttle_us, int where);

/* What the game is doing right now, as one of AG_WHERE_*.  Implemented in
 * cache.cc, where the game's own state is reachable; the port never calls it. */
int ag_doing_now(void);

/* The clock the frame timings use, in microseconds.  Implemented in the port
 * so that no engine file has to include <argon/argon.h>. */
unsigned ag_cache_now_us(void);

/* Held above zero by mapLoad while it runs, so that a map change's two hundred
 * reads are not counted as the reads of walking around. */
extern unsigned ag_map_loading;

#ifdef __cplusplus
}
#endif

#endif /* AG_DIAG_H */
