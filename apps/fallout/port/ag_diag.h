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
 * What loading a sound costs, which is not a cache miss and so is invisible to
 * everything above.
 *
 * The sound effects cache holds the *compressed* file.  Every play then reads
 * it through the ACM decoder into a buffer of its own, copies that into an
 * audio buffer and frees the first - so the decode is paid again on every
 * play, and a cache hit does not save it.  Maxim's hour of play: 88 loads of
 * HMWARRGA.ACM for 26 attacks, each one a malloc of 56086 bytes, a decode, and
 * a second buffer the same size.
 *
 * Timed around soundLoad, so it covers the open, the read, the decode and the
 * copy - the whole of what the game waits for.
 */
extern unsigned ag_sfx_loads;
extern unsigned ag_sfx_ms;
extern unsigned ag_sfx_kb;
extern unsigned ag_sfx_worst_ms;
extern char ag_sfx_worst[AG_NAME_MAX];
/* The name being loaded, for the line above; written by soundLoad. */
extern char ag_sfx_name[AG_NAME_MAX];

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

/*
 * Where the game is, in one word, for when it stops being anywhere.
 *
 * The engine has a watcher thread (watch_thread in sdl_shim.cc) whose whole
 * job is to say where the main loop was when it stopped, and twice now it has
 * said nothing at all while the loop was plainly stuck.  The step number it
 * reports is also too coarse to act on: "step 2, gameHandleKey" covers half
 * the engine.
 *
 * So: a pointer to a literal, stored at the places where the game goes away
 * for a while.  A store of one word, no lock - a torn read of an aligned word
 * cannot happen on this machine, and the worst case is one stale name in a
 * report that is only printed when something has already gone wrong.
 *
 * Set it with AG_STEP("what"), and set it back to something coarse afterwards
 * rather than leaving the last fine-grained name lying around.
 */
extern const char* volatile ag_step;

#define AG_STEP(where) (ag_step = (where))

/*
 * And the same for the sound thread, because a deadlock has two ends and one
 * of them is no use on its own.
 *
 * 8 September 2026: the watcher finally spoke, and said the game was in step 1
 * - inputGetInput - with the breadcrumb at "playing".  inputGetInput runs the
 * engine's background handlers, one of which is the sound's, and that one takes
 * a sound buffer's mutex.  So the question became "who is holding it", and
 * nothing could answer: the holder is the other thread, and it had no
 * breadcrumb of its own.  These are its.  ag_audio_laps says whether that
 * thread is running at all, which separates "spinning inside the mixer" from
 * "blocked in the device write" without any further guessing.
 */
extern const char* volatile ag_step_audio;
extern unsigned volatile ag_audio_laps;

/* Which background handler is running, by address - the list belongs to the
 * engine, so a pointer that addr2line names later beats a table of names
 * that has to be kept in step.  Null between handlers. */
extern void* volatile ag_ticker_now;

/* Which script is running, and which of its procedures.  A trap fires a
 * spatial script, an animation handler runs it, and a script that does not
 * come back looks from outside exactly like an animation that does not come
 * back.  -1 between scripts. */
extern int volatile ag_script_now;
extern int volatile ag_script_proc_now;

/*
 * And what the interpreter is doing, which is the last thing that can hide.
 *
 * A script that does not come back is one of three things and they need
 * different fixes: a loop in its own bytecode (ag_script_steps climbs), a
 * single instruction that never returns (steps frozen, ag_script_op names it),
 * or a wait whose condition can never be met (ag_script_waits climbs).  The
 * watcher samples them twice, 200 ms apart, and says which.
 */
extern int volatile ag_script_op;
extern int volatile ag_script_ip;
extern unsigned volatile ag_script_steps;
extern unsigned volatile ag_script_waits;

/* The last move_to a script asked for: where the object was, where it was
 * asked to go, and what came back.  The Arroyo spear trap walks its spear one
 * tile at a time towards the player and loops until it arrives - so a step
 * that does not move is a game that does not come back, and these three
 * numbers are what is needed to work out why. */
extern int volatile ag_moveto_from;
extern int volatile ag_moveto_to;
extern int volatile ag_moveto_result;

/* And the step that decided where to go: origin, the rotation asked for, and
 * the tile that came back.  A result equal to the origin is a step that does
 * not move, which is what an infinite walk is made of.  The first cut of this
 * recorded move_to's "from" *after* the move, so it always equalled "to" and
 * proved nothing - hence the whole step, in order. */
extern int volatile ag_dir_origin;
extern int volatile ag_dir_rotation;
extern int volatile ag_dir_result;
extern int volatile ag_rot_target;

#ifdef __cplusplus
} /* the scope guard below is C++ */

/*
 * Names the call that is about to take a lock, and puts the previous name back
 * on the way out - so a breadcrumb inside a map load does not erase "mapLoad".
 * Used with __func__, which costs nothing and cannot go stale.
 */
struct AgStepScope {
    const char* was;
    explicit AgStepScope(const char* what)
        : was(ag_step)
    {
        ag_step = what;
    }
    ~AgStepScope() { ag_step = was; }
};

#define AG_STEP_SCOPE(what) AgStepScope ag_step_scope_(what)

extern "C" {
#endif

#ifdef __cplusplus
}
#endif

#endif /* AG_DIAG_H */
