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
/* Ten was too few to answer "what is being re-read": one lantern animation
 * read 104 times filled the table and hid everything behind it. */
#define AG_TOP_MISSES 32

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
/*
 * Where a cache miss actually goes, 9 September 2026.
 *
 * The counters above said what a miss costs - 10979 of them while walking,
 * 118639 ms, 230695 KB read - but not what the time was spent on.  21 KB in
 * 10.8 ms is 1.9 MB/s, which is far under what the card can do, so the answer
 * matters: if it is the card, read in bigger pieces; if it is zlib, there is
 * nothing to win there and the size of the cache is the only lever left.
 *
 * Three places, and between them they account for the whole of a miss:
 *
 *   open     finding the entry and starting the zlib stream (dfileOpen)
 *   read     fread of the compressed bytes - the card
 *   inflate  zlib turning them back into pixels - the processor
 *
 * The chunk sizes on the way are worth knowing while reading these: the shim
 * reads the card 4096 bytes at a time (FILE_RBUF) and zlib is fed 1024 at a
 * time (DFILE_DECOMPRESSION_BUFFER_SIZE), so a 21 KB frame is twenty trips
 * through both.
 */
/*
 * How big the art cache would have to be, measured instead of argued.
 *
 * The counters say 91% of the misses while walking are for art the cache had
 * and threw out - which is thrashing, and the cure for thrashing is a cache
 * that holds the working set.  Nobody knows what that set weighs.  So every
 * key the cache is asked for is noted with its size, distinct keys only, and
 * the total is printed once per window: that total IS the cache size that
 * would have had no re-reads at all in that window.
 *
 * A fixed table with no allocation - it runs inside cacheLock, on the path
 * this whole exercise is about, so it must cost nothing worth measuring.  When
 * it fills, the overflow is counted and reported rather than silently making
 * the answer look smaller than it is.
 */
void ag_ws_note(int kind, int key, int size);

/*
 * How far the character has walked, in hexes.
 *
 * Maxim, 9 September 2026: "считать надо не на стоящем персонаже.
 * Отслеживай перемещения и меряй количество промахов на пройденый путь."
 * Exactly right, and it is the only way these sessions can be compared: a
 * count of misses measures how much he walked, and misses per hundred hexes
 * measures the cache.  Standing still has no misses and proves nothing.
 *
 * Accumulated in the main loop from the dude's own tile, through the closed
 * form hex distance - so a teleport across the map counts as the hexes it
 * really was, and a step counts as one.
 */
/*
 * Art misses by what kind of art it is.
 *
 * Measured 9 September 2026: walking costs 2916 art misses per hundred hexes -
 * twenty-nine misses for every single step, about 350 ms of card reading per
 * step.  The table of file names could not answer what they were (it keeps the
 * most expensive files, and a lantern animation read a hundred times filled
 * it), and the answer decides the fix: tiles and walls are the map itself and
 * arrive by the dozen as the view scrolls, scenery is objects standing on it,
 * critters are animations.  A cure for one is no cure for the others.
 *
 * Indexed by the object type in the fid - FID_TYPE - so it costs one shift and
 * one increment on the miss path.
 */
#define AG_ART_KINDS 11

extern unsigned ag_art_miss[AG_ART_KINDS];
extern unsigned ag_art_miss_ms[AG_ART_KINDS];

/* Of those misses, the ones the flash store answered.  A miss that flash
 * satisfied still costs a copy into the cache, but not a read from the card
 * and not an inflate - which is the whole point of the store, so the two are
 * counted apart.  See apps/fallout/port/art_store.h. */
extern unsigned ag_art_from_flash[AG_ART_KINDS];

/* ARGON: what one tick of _object_animate costs.  It calls
 * tileWindowRefreshRect once per animated object per frame, and each of those
 * re-renders the map inside that rectangle - tiles, objects, light.  Its
 * worst call is the largest single cost left in the game, and these say
 * whether that is many objects or one large rectangle. */
extern unsigned ag_anim_refresh_n, ag_anim_refresh_px;
extern unsigned ag_anim_worst_us, ag_anim_worst_refreshes;
extern unsigned ag_anim_worst_px, ag_anim_worst_sads;
extern unsigned ag_anim_seq_us, ag_anim_move_us, ag_anim_script_us;
extern unsigned ag_anim_worst_seq_us, ag_anim_worst_move_us;
extern unsigned ag_anim_worst_script_us;

/* Inside _object_move: the path search taken when the way is blocked, and the
 * map refresh that ends every step. */
/* A trap for a slow tile script, kept for the whole game: silent until one
 * call passes the threshold, then it names the tile.  See animation.cc. */
void ag_script_slow_note(unsigned us, int tile, int elevation);

/*
 * How long the player waited for a load, start to finish.
 *
 * The store timed the window in which art may be written and reported tens of
 * seconds while the stopwatch in the room said three and a half minutes.  Both
 * were honest about different things; this one is about the wait.
 */
void ag_load_note(unsigned ms);

/* The six layers of one map refresh; see tileRefreshGame in tile.cc. */
/* Inside the object pass: how many turns of the loop, how many were in range,
 * how many tiles held anything, how many objects were drawn and what the
 * drawing itself cost.  See _obj_render_pre_roof in object.cc. */
extern unsigned ag_render_calls, ag_render_steps, ag_render_inrange;
extern unsigned ag_render_tiles, ag_render_objects, ag_render_draw_us;
extern unsigned ag_render_objects2, ag_render_draw2_us;
/* Inside _obj_render_object: how many calls drew nothing, and what the art
 * lock cost before anyone asked whether they would. */
extern unsigned ag_draw_calls, ag_draw_hidden, ag_draw_missed, ag_draw_hit;
extern unsigned ag_draw_early;

/*
 * The three kinds of "not in hand", counted apart.
 *
 * ag_miss_ram is what the engine's own cache calls a miss: it had nothing.
 * Most of those are answered by the flash store and cost almost nothing.
 * ag_miss_card is the one that matters - neither RAM nor flash had it and the
 * card had to be read, which is milliseconds rather than microseconds.  Using
 * one word for all three is how a report can say "five thousand misses" about
 * a game that touched the card two thousand times.
 */
extern unsigned ag_art_requests;
extern unsigned ag_miss_ram, ag_miss_flash, ag_miss_card, ag_miss_card_kb;
extern unsigned ag_draw_lock_us;

extern unsigned ag_layer_n;
extern unsigned ag_layer_clear_us, ag_layer_floor_us, ag_layer_pre_us;
extern unsigned ag_layer_roof_us, ag_layer_post_us, ag_layer_window_us;

extern unsigned ag_path_us, ag_path_n;
extern unsigned ag_move_refresh_us, ag_move_refresh_n, ag_move_refresh_px;

/* ARGON: what one scroll step costs, in its four parts.

 * gameMouseRefresh was measured at 39.7 ms a frame while the map was moving -
 * two thirds of everything - and the scroll is what it is doing.  mapScroll
 * already shifts the window with memmove and redraws only the strip that came
 * into view, so "it redraws the whole map" is not the answer; these four say
 * which part it really is.  See mapScroll in map.cc. */
extern unsigned ag_scroll_steps;
extern unsigned ag_scroll_move_us;    /* shifting the window buffer      */
extern unsigned ag_scroll_strip_us;   /* redrawing the exposed strips    */
extern unsigned ag_scroll_window_us;  /* windowRefresh of the whole map  */
extern unsigned ag_scroll_mouse_us;   /* hiding and showing the cursor   */

extern unsigned ag_walk_tiles;

extern unsigned ag_dat_open_us, ag_dat_open_n;
extern unsigned ag_dat_read_us, ag_dat_read_n, ag_dat_read_kb;
extern unsigned ag_dat_inflate_us, ag_dat_inflate_n;

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
