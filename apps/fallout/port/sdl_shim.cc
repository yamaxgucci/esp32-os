/*
 * ArgonOS - SDL2's subset, over the ArgonOS ABI.
 *
 * Three kinds of thing live here.
 *
 * Done: everything that is pure memory or arithmetic - surfaces, palettes,
 * the blit, the string helpers, the clock.  None of it needs a device, and it
 * is the same code SDL would run.
 *
 * Stubbed and loud: video, events and audio.  Each one panics with its own
 * name.  They are not silent no-ops on purpose - a game that draws into
 * nothing and reports success is a game that looks like it works from the
 * outside and cannot be debugged from the inside.
 *
 * Absent: anything the engine does not call.  See port/SDL.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "SDL.h"

#include <argon/argon.h>
#include <stdio.h>

/* The cache miss counters, shared with the engine's cache.cc. */
#include "ag_diag.h"
#include "art_store.h"

/* apps/common/libc's watcher; its header is not on this build's include
 * path, and the engine's stdlib.h is the toolchain's. */
extern "C" size_t ag_libc_arena_low(void);
extern "C" void ag_libc_malloc_trace(size_t min_bytes,
                                    int max_reports);
extern "C" void ag_libc_malloc_profile(int on);
extern "C" void ag_libc_log_sink(void (*sink)(const char *line));
extern "C" void ag_libc_malloc_profile_dump(int top);
extern "C" void ag_libc_io_stats(uint64_t *us, uint32_t *reads,
                                 uint32_t *opens, uint32_t *seeks,
                                 uint64_t *open_us, uint64_t *seek_us);
extern "C" void ag_libc_io_reset(void);
extern "C" uint32_t ag_libc_io_misses(void);
extern "C" uint64_t ag_libc_io_bytes(void);

/*
 * What the art cache does, counted by the engine and printed by the port.
 * "It reads the same two hundred files over and over" is an observation; these
 * say whether the cache is too small, whether its eviction policy throws out
 * more than it needs, or whether the whole thing is being emptied because one
 * large entry would not fit.  See patches/fallout2-ce/0007.
 *
 * Defined here rather than beside the game's own main(): SDLTEST links this
 * file without the engine, and would otherwise not link at all.
 */
extern "C" unsigned ag_cache_fetches = 0;
extern "C" unsigned ag_cache_evicted = 0;
extern "C" unsigned ag_cache_flushes = 0;
extern "C" unsigned ag_cache_biggest = 0;

/*
 * And the same question asked properly: per cache, split by what the game was
 * doing at the time, with the hits beside the misses and the cost of a miss in
 * bytes and in milliseconds.  Declared in ag_diag.h, which says why;
 * written by cache.cc; defined here for the same reason as the four above.
 */
unsigned ag_cache_hit[AG_CACHE_SLOTS];
unsigned ag_cache_miss[AG_CACHE_SLOTS];
unsigned ag_cache_refetch[AG_CACHE_SLOTS];
unsigned ag_cache_miss_kb[AG_CACHE_SLOTS];
unsigned ag_cache_miss_ms[AG_CACHE_SLOTS];
unsigned ag_cache_out[AG_CACHE_SLOTS];
unsigned ag_cache_flush[AG_CACHE_SLOTS];
unsigned ag_cache_failed[AG_CACHE_SLOTS];

/* And the files those misses read, by name.  Filled by the caches' own read
 * procs, which are the only code that knows the path. */
char ag_miss_name[AG_NAME_MAX];
char ag_top_name[AG_TOP_MISSES][AG_NAME_MAX];
unsigned ag_top_count[AG_TOP_MISSES];
unsigned ag_top_ms[AG_TOP_MISSES];
unsigned ag_top_kb[AG_TOP_MISSES];
unsigned char ag_top_kind[AG_TOP_MISSES];
unsigned char ag_top_where[AG_TOP_MISSES];

/* And what loading a sound costs, which no cache counter can see: the cache
 * holds the compressed file and every play decodes it again. */
unsigned ag_sfx_loads;
unsigned ag_sfx_ms;
unsigned ag_sfx_kb;
unsigned ag_sfx_worst_ms;
char ag_sfx_worst[AG_NAME_MAX];
char ag_sfx_name[AG_NAME_MAX];

/* Held above zero by mapLoad while it runs. */
unsigned ag_map_loading;

/* Where the game is, for when it stops being anywhere.  See ag_diag.h. */
const char* volatile ag_step = "before main";
const char* volatile ag_step_audio = "not started";



/* Audio thread timings; see audio_report. */

static volatile uint32_t s_audio_mix_us, s_audio_write_us;

static volatile uint32_t s_audio_mix_worst_us, s_audio_write_worst_us;

static volatile uint32_t s_audio_blocks;
unsigned volatile ag_audio_laps;
void* volatile ag_ticker_now;
int volatile ag_script_now = -1;
int volatile ag_script_proc_now = -1;
int volatile ag_script_op = -1;
int volatile ag_script_ip = -1;
unsigned volatile ag_script_steps;
unsigned volatile ag_script_waits;
int volatile ag_moveto_from = -1;
int volatile ag_moveto_to = -1;
int volatile ag_moveto_result = -1;
int volatile ag_dir_origin = -1;
int volatile ag_dir_rotation = -1;
int volatile ag_dir_result = -1;
int volatile ag_rot_target = -1;

/* The engine asks the port for the clock rather than including argon.h: no
 * engine file does, and this is not the change that should start. */
unsigned ag_cache_now_us(void)
{
    return (unsigned)ag_micros();
}

/*
 * The worst lap of the window and what it was doing.
 *
 * Kept as it is measured - in microseconds, per step - and reset by the
 * report.  The cache figures are taken as differences across the lap, which
 * is what says whether the dip was a file read or something else entirely:
 * a 280 ms lap with two misses and 270 ms of miss time is answered, and a
 * 280 ms lap with no misses at all is a different bug.
 */
/* Longer than this and the lap was a screen, not a frame.  See ag_frame_note. */
#define AG_MODAL_US (2u * 1000u * 1000u)

static uint32_t s_modal_laps, s_modal_worst_us;
static uint32_t s_worst_us, s_worst_input, s_worst_keys, s_worst_scripts;
static uint32_t s_worst_maps, s_worst_present, s_worst_throttle;
static uint32_t s_worst_miss_n, s_worst_miss_ms, s_worst_kb;
static int s_worst_where = -1;
static uint32_t s_seen_miss_n, s_seen_miss_ms, s_seen_kb;

/*
 * ARGON: long laps kept with their times, printed only when asked.
 *
 * A quarter of a second, once a second, is what Maxim describes - and that is
 * a pattern in TIME, which every report here was averaging away.  Sixty-four
 * of them is a minute's worth at his rate, and the ring costs 1.5 KB.
 */
#define AG_STALLS 64u
#define AG_STALL_US 120000u   /* a lap this long is one he can see */

typedef struct {
    uint32_t at_ms;
    uint32_t cost_us;
    uint32_t input_us, keys_us, scripts_us, maps_us, present_us;
} ag_stall_t;

static ag_stall_t s_stalls[AG_STALLS];
static unsigned s_stall_n;

static void cache_totals(uint32_t *miss_n, uint32_t *miss_ms, uint32_t *kb)
{
    uint32_t n = 0, ms = 0, k = 0;
    for (int i = 0; i < AG_CACHE_SLOTS; i++) {
        n += ag_cache_miss[i];
        ms += ag_cache_miss_ms[i];
        k += ag_cache_miss_kb[i];
    }
    *miss_n = n;
    *miss_ms = ms;
    *kb = k;
}

void ag_frame_note(unsigned input_us, unsigned keys_us, unsigned scripts_us,
    unsigned maps_us, unsigned present_us, unsigned throttle_us, int where)
{
    uint32_t miss_n, miss_ms, kb;
    cache_totals(&miss_n, &miss_ms, &kb);

    /* Throttle is the deliberate sleep and is not a cost: a lap that slept is
     * a lap that had time to spare.  It is kept for the report because a slow
     * lap that still slept means the limiter, not the work. */
    const uint32_t cost = (uint32_t)input_us + keys_us + scripts_us + maps_us
        + present_us;

    /*
     * ARGON: a lap that ran a modal screen is not a slow frame.
     *
     * Measured on the board, 9 September 2026, and it made the whole report
     * useless: "125985 ms while walk: scripts 125876059" - a lap of two
     * minutes, almost all of it in the scripts phase.  Nothing was slow.  In
     * Fallout's main loop a conversation runs INSIDE scriptsHandleRequests and
     * does not return until it ends, and the inventory does the same inside
     * gameHandleKey: "82351 ms ... keys 82210105" was Maxim standing in his
     * own inventory.  The worst lap of every window was whatever screen he had
     * opened, and the dips he actually feels - half a second while walking -
     * were never reported at all.
     *
     * So laps over the ceiling are counted and set aside rather than kept.
     * Two seconds is far longer than any frame this game has ever taken here
     * and far shorter than any screen a person opens, so the two do not
     * overlap; the count is printed so that nothing is silently dropped.
     */
    if (cost > AG_MODAL_US) {
        s_modal_laps++;
        if (cost > s_modal_worst_us) {
            s_modal_worst_us = cost;
        }
    } else if (cost >= AG_STALL_US) {
        /*
         * ARGON: every long lap, with the clock time it happened at.
         *
         * "Once a second a freeze of 0.2 to 0.5 s" is a description of a
         * SCHEDULE, and no average can answer it - a report that prints the
         * worst lap of a window says how bad, never how often or how evenly
         * spaced.  This keeps the times themselves and prints nothing until
         * asked, because printing is itself 11 ms a line down a 115200 line
         * and measuring this way was changing what it measured.
         */
        ag_stall_t *e = &s_stalls[s_stall_n % AG_STALLS];

        e->at_ms = (uint32_t)ag_millis();
        e->cost_us = cost;
        e->input_us = input_us;
        e->present_us = present_us;
        e->keys_us = keys_us;
        e->scripts_us = scripts_us;
        e->maps_us = maps_us;
        s_stall_n++;
    }

    if (cost > AG_MODAL_US) {
        /* counted above */
    } else if (cost > s_worst_us) {
        s_worst_us = cost;
        s_worst_input = input_us;
        s_worst_keys = keys_us;
        s_worst_scripts = scripts_us;
        s_worst_maps = maps_us;
        s_worst_present = present_us;
        s_worst_throttle = throttle_us;
        s_worst_where = where;
        s_worst_miss_n = miss_n - s_seen_miss_n;
        s_worst_miss_ms = miss_ms - s_seen_miss_ms;
        s_worst_kb = kb - s_seen_kb;
    }

    s_seen_miss_n = miss_n;
    s_seen_miss_ms = miss_ms;
    s_seen_kb = kb;
}

/*
 * And what the art cache had to take out of the arena because its own memory
 * would not hold the block.  Set in cache.cc from the heap's own count; the
 * heap has been keeping it all along and nothing was reading it.
 */
extern "C" unsigned ag_heap_system_blocks = 0;
extern "C" unsigned ag_heap_system_bytes = 0;

/*
 * And what the hex-grid distance costs.  It walks tile by tile, so the second
 * of these is the work: it used to be one atan2 in software doubles per step.
 * The third counts walks that gave up - see patches/fallout2-ce/0008.
 */
extern "C" unsigned ag_tiledist_calls = 0;
extern "C" unsigned ag_tiledist_steps = 0;
extern "C" unsigned ag_tiledist_gaveup = 0;

/* Declared where the port can see them; not part of SDL. */
extern "C" bool sdl_shim_display(ag_gfxinfo_t *out);
extern "C" void sdl_shim_screenshot(const char *path, int after_frames,
                                    int after_ms);
extern "C" void sdl_shim_memstat(int every_ms, int trace_kb,
                                 int trace_max, int trace_prof);
extern "C" void sdl_shim_fps(int every_ms);
extern "C" void sdl_shim_screenshot_now(void);

extern "C" {

/* ---- not written yet ----------------------------------------------------- */

static void todo(const char *what)
{
    ag_print("SDL shim: not implemented yet: ");
    ag_print(what);
    ag_print("\n");
    ag_panic(what);
}

#define TODO(name) todo(#name)

/* ---- subsystems ---------------------------------------------------------- */

static Uint32 s_inited;

/* Fallout 2 calls this instead of the per-subsystem form; the subsystems here
 * cost nothing until they are used, so both are the same call. */
int SDL_Init(Uint32 flags) { return SDL_InitSubSystem(flags); }

int SDL_InitSubSystem(Uint32 flags)
{
    s_inited |= flags;
    return 0;
}

void SDL_QuitSubSystem(Uint32 flags) { s_inited &= ~flags; }

Uint32 SDL_WasInit(Uint32 flags)
{
    return flags == 0 ? s_inited : (s_inited & flags);
}

void SDL_Quit(void) { s_inited = 0; }

/* Hints are advice about backends this port does not have. */
SDL_bool SDL_SetHint(const char *name, const char *value)
{
    (void)name;
    (void)value;
    return SDL_TRUE;
}

int SDL_OutOfMemory(void)
{
    ag_panic("SDL: out of memory");
    return -1;
}

/* ---- surfaces ------------------------------------------------------------ */

/*
 * A surface is a width, a height, a pitch and a block of pixels, and that is
 * all the engine ever wants from one.  The format and palette structures are
 * allocated with it, in one block, so freeing a surface is one call and
 * nothing can outlive it by accident.
 */
struct SurfaceBlock {
    SDL_Surface surface;
    SDL_PixelFormat format;
    SDL_Palette palette;
    SDL_Color colors[256];
};

/* Defined with the timers at the end of the file; called from RenderPresent,
 * which is the only point the game passes through every frame. */
static void timers_service(void);

/* Defined with the event pump; called once the display is ours. */
static void input_devices_open(void);

/* Reads the virtual input drivers so they pump their sockets; see the pump. */
void sdl_shim_input_service(void);

/* The acquired display, and whether a surface has been given its memory. */
static ag_gfxinfo_t s_gfx;
static bool s_have_gfx;
static bool s_fb_lent;
static uint32_t s_gfx_since_ms; /* when it was taken, for -shotms */

/* What the last SDL_UpdateTexture was handed, for the panels that have no
 * framebuffer of their own to have been drawn into. */
static const void *s_frame_px;
static int s_frame_pitch;

/*
 * What has changed since the last present, as one rectangle.
 *
 * The engine redraws in pieces - GNW95_ShowRect hands over exactly the part of
 * the screen a window touched - and sending the whole 640x480 anyway costs
 * 614 KB down the panel's bus for a change of a few hundred bytes.  In QEMU
 * that is free and the waste is invisible; on a panel it is the difference
 * between eight frames a second and a game.
 *
 * One rectangle rather than a list, deliberately.  Two distant changes then
 * send the box that contains both, which is worse than sending two rectangles
 * and much better than sending the screen; and a list needs a merge rule, a
 * bound, and a decision about what to do when it overflows.  The bound is the
 * thing: the engine has no limit on how many rectangles it touches between
 * presents, so the list would have to collapse to this anyway.
 *
 * In QEMU this costs rather than saves, and knowingly.  The soft display shows
 * a frame by ringing a doorbell, and a kick that does not cover the one still
 * in flight has to wait for it or leave stale rows on screen - so a movie,
 * which is letterboxed and therefore never covers a previous full-height
 * kick, waits once a frame.  It costs 213 dropped frames out of 2333 in
 * Fallout 2's credits.  A panel has no doorbell: there the saving is the ratio
 * of the areas, and that is the machine this is for.
 */
static int s_dirty_x0, s_dirty_y0, s_dirty_x1, s_dirty_y1;

/* Microseconds of halving in the frame being built, reset by every present. */
static uint32_t s_blit_frame_us;


/*
 * What a block of the picture becomes on a smaller panel.
 *
 * Nearest is every second pixel of every second row, which is right for a
 * photograph and wrong for a font: a stroke one pixel wide survives half the
 * time, and Fallout's interface is full of them.
 *
 * The first attempt kept, out of each 2x2, the pixel whose brightness differed
 * most from the block's average - the reasoning being that a light stroke on
 * dark and a dark stroke on light would both come through, since this game has
 * both.  Measured against nearest on the interface bar, it was worse: on a
 * textured panel the outlier is a speck of background more often than it is a
 * stroke, so the text came out scattered.
 *
 * So: average the four.  It cannot lose a stroke, only dim one, and it is what
 * a hardware scaler would do.  Twelve table lookups and a dozen adds per output
 * pixel against one lookup, on a quarter as many pixels.
 */
static int s_scale_sharp;

void sdl_shim_scale_sharp(int on)
{
    s_scale_sharp = on ? 1 : 0;
}

/*
 * One to one, on a panel that cannot hold the whole picture.
 *
 * Reducing 640x480 to 320x240 takes Fallout's small font from eight pixels tall
 * to four, and four pixels is not a letter - no resampling puts back what is not
 * there, and averaging (the best of them) only keeps strokes from vanishing.
 * So the other half of the answer is not to reduce at all: show a 320x240 window
 * onto the picture, pixel for pixel, and let the player move it.
 *
 * F8 switches; the arrow keys move the window while it is on.  Both are taken
 * by the port and never reach the engine, which is why F8 - the game uses F1 to
 * F7, F10 and F12, and patch 0013 has F11.  The arrows are only swallowed while
 * one to one is on, so they do whatever they normally do the rest of the time.
 *
 * The window does not follow the pointer.  It could be made to, but the port
 * knows where the host's pointer is on the panel and not where the engine's
 * cursor is on the picture, and a window that lurches on a twitch of the mouse
 * while a dialogue is being read is worse than one that stays where it is put.
 */
static int s_one_to_one;      /* F8: showing the picture at its own size */

/*
 * The last thing blitted to the panel, so the view can be moved.
 *
 * Only the engine knows how to draw its picture, and it draws only what has
 * changed - so moving the window leaves whatever was under it before.  The
 * picture itself is still there in the eight-bit surface the engine composes
 * into, and blitting the whole of that again is what a moved window needs.
 * Kept as a pointer rather than a copy: it is the engine's surface, it does not
 * move, and it is 300 KB.
 */
static SDL_Surface *s_panel_src;
static SDL_Surface *s_panel_dst;
static int s_pan_x;           /* the window's origin, in picture pixels */
static int s_pan_y;
/* What the last repaint actually wrote, and what it read from: for the report
 * beside the key, which is the only way to tell a view that did not move from
 * one that moved and was not sent. */
static int s_pan_bx0, s_pan_by0, s_pan_bx1, s_pan_by1;
static int s_pan_sw, s_pan_sh;

/*
 * How much smaller the panel is than the picture, as a shift.  Zero when they
 * are the same size, which is every case but a small board.  Set by
 * make_surface when it lends the framebuffer out; read by the blit.
 */
static int s_panel_shift;

/* How far the window can go before it leaves the picture. */
static void pan_clamp(void)
{
    const int maxx = (int)(s_gfx.width << s_panel_shift) - (int)s_gfx.width;
    const int maxy = (int)(s_gfx.height << s_panel_shift) - (int)s_gfx.height;

    if (s_pan_x > maxx) {
        s_pan_x = maxx;
    }
    if (s_pan_y > maxy) {
        s_pan_y = maxy;
    }
    if (s_pan_x < 0) {
        s_pan_x = 0;
    }
    if (s_pan_y < 0) {
        s_pan_y = 0;
    }
}



/* ---- the session log -----------------------------------------------------
 *
 * Everything below prints to the console, and on a board the console is a
 * cable to a machine that is not there.  The shell can redirect a command's
 * output to a file, but not this one's: `run` hands the process to the session
 * and returns, so the redirection is undone before the game has drawn a frame.
 * Measured, not assumed - the [fps] lines came out on the console with
 * `> c:\fo2.log` on the command line.
 *
 * So the port keeps its own.  Every diagnostic line goes to the console as
 * before and to this file as well, and the engine's own debugPrint comes
 * through SDL_LogMessageV, which is where "Error loading skilldex dialog data!"
 * and the rest of the messages that precede a wedged game are written.
 *
 * Opened for append, so a second run adds to the first rather than erasing the
 * evidence of it.
 */
static ag_handle_t s_log = (ag_handle_t)-1;

static void log_line(const char *text);

void sdl_shim_log(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return;
    }
    s_log = ag_open(path, AG_O_WRONLY | AG_O_CREATE | AG_O_APPEND);
    if (s_log < 0) {
        s_log = (ag_handle_t)-1;
        ag_printf("log: cannot write %s\n", path);
        return;
    }
    ag_printf("log: %s\n", path);

    /* And the one line that explains a game which has just stopped working. */
    ag_libc_log_sink(log_line);
}

/*
 * The whole point of this file is to survive the thing that killed the game,
 * and for a while it did not: FAT puts the length in the directory entry and
 * updates it when the handle is closed, so a run that was reset - or that died
 * - left a log of exactly zero bytes.  Every line was written and none of them
 * could be read.  That is the diagnostic tool failing at the only job it has.
 *
 * So the handle is synced, but not on every line: the game prints hundreds a
 * second while a map loads, and a metadata write to the card for each of them
 * would change what is being measured.  Twice a second is far more often than
 * anything interesting happens and costs nothing that shows up in a frame rate.
 */
#define LOG_SYNC_MS 500u

static uint32_t s_log_synced;

static void log_line(const char *text)
{
    if (s_log < 0 || text == NULL) {
        return;
    }
    ag_write(s_log, text, strlen(text));

    const uint32_t now = (uint32_t)ag_millis();
    if (now - s_log_synced >= LOG_SYNC_MS) {
        s_log_synced = now;
        (void)ag_sync(s_log);
    }
}

/*
 * Print to the console and to the log, in one call.
 *
 * Formatted once into a buffer rather than twice through two printfs: the
 * second one would have to walk the varargs again, and a line that differs
 * between the console and the file is worse than no file.
 */
/* In fallout_main.cc: the arena reserve, re-armed from here because
 * this is the one place that runs every frame. */
extern "C" void fallout_reserve_tick(void);

/*
 * ARGON: the reports leave one line per frame, not twenty in one.
 *
 * The whole readout is printed every thirty seconds from inside the frame
 * loop, and the console is a slow device: the service that prints it
 * averaged 361 us per frame across a window, which is around six hundred
 * milliseconds landing on the single frame that did the printing.  That
 * frame was the worst of every window - 282 ms of a "282 ms stall" was
 * the instrument, not the game, and Maxim was being given that jolt every
 * thirty seconds while playing.
 *
 * So a report goes into a ring and one line leaves per frame.  Nothing is
 * lost unless the ring overflows, which is counted and said out loud.
 */
#define SHIM_REP_LINES 48
#define SHIM_REP_CHARS 640

static char     s_rep[SHIM_REP_LINES][SHIM_REP_CHARS];
static unsigned s_rep_head, s_rep_tail, s_rep_lost;
static bool     s_rep_on = true;

static void shim_print_now(const char *line)
{
    ag_print(line);
    log_line(line);
}

/*
 * The console is slow and the frame loop must not wait for it.
 *
 * Draining one line per frame was worse than printing everything at once:
 * at fifty-seven frames a second that is more traffic than the reports
 * themselves, and every line is milliseconds of UART.  So the printing
 * happens on a thread of its own, below the game, and the frame loop only
 * ever copies into the ring.
 */
static bool         s_rep_stop;
static ag_thread_t  s_rep_thread;

static void shim_printer(void *arg)
{
    (void)arg;
    while (!s_rep_stop) {
        if (s_rep_head == s_rep_tail) {
            if (s_rep_lost != 0u) {
                s_rep_lost = 0;
                shim_print_now("[report] lines were dropped: the ring "
                               "filled while the console caught up");
            }
            ag_delay(20);
            continue;
        }

        shim_print_now(s_rep[s_rep_tail]);
        s_rep_tail = (s_rep_tail + 1u) % SHIM_REP_LINES;
    }
    ag_thread_exit();
}

static void shim_drain(void)
{
    /* Kept as the one place that starts the printer, so nothing has to be
     * added to the port's start-up path. */
    if (s_rep_thread == NULL && !s_rep_stop) {
        s_rep_thread = ag_thread_create(shim_printer, NULL, "shimprint",
                                        4096, 0, 0);
        if (s_rep_thread == NULL) {
            s_rep_on = false;   /* no thread: print in place, as before */
        }
    }
}

static void shim_printf(const char *fmt, ...)
{
    /* Long enough for the longest report, which is the store's: 320 cut it
     * off silently, and what fell off was the end - where the newest and
     * usually most interesting numbers are. */
    char line[SHIM_REP_CHARS];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (!s_rep_on) {
        shim_print_now(line);
        return;
    }

    const unsigned next = (s_rep_head + 1u) % SHIM_REP_LINES;

    if (next == s_rep_tail) {
        s_rep_lost++;
        return;
    }
    memcpy(s_rep[s_rep_head], line, sizeof(s_rep[0]));
    s_rep[s_rep_head][SHIM_REP_CHARS - 1] = '\0';
    s_rep_head = next;
}

/* Bumped whenever the palette is written; palette_lut watches it. */
static uint32_t s_palette_gen = 1;

static void dirty_none(void)
{
    s_dirty_x0 = s_dirty_y0 = 0x7FFF;
    s_dirty_x1 = s_dirty_y1 = -1;
}

static void dirty_add(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    if (x < s_dirty_x0) {
        s_dirty_x0 = x;
    }
    if (y < s_dirty_y0) {
        s_dirty_y0 = y;
    }
    if (x + w > s_dirty_x1) {
        s_dirty_x1 = x + w;
    }
    if (y + h > s_dirty_y1) {
        s_dirty_y1 = y + h;
    }
}

/*
 * Not SDL.  The display is acquired once and acquiring twice is -EBUSY, so
 * anything else in the image that wants to know what it got has to ask here.
 * SDLTEST is the caller; the engine never needs it.
 */
bool sdl_shim_display(ag_gfxinfo_t *out)
{
    if (!s_have_gfx || out == NULL) {
        return false;
    }
    *out = s_gfx;
    return true;
}

/*
 * Not SDL either: a way to see what was drawn.
 *
 * An automated run has no eyes - the test harness reads the text screen, and
 * the text screen is exactly what a game that has taken the display is not
 * using.  So the frame can be written out as a PPM and looked at afterwards.
 * This is how the port is checked at all until there is a board with a panel
 * on it, and it stays useful after that: a screenshot of a wrong frame can be
 * diffed, and a photograph cannot.
 */
static char s_shot_path[128];
static int s_shot_after;
static uint32_t s_shot_at_ms;
static int s_present_count;

/*
 * after_frames counts presents; after_ms is wall time since the display was
 * taken.  Either may be zero, and whichever arrives first wins.
 *
 * Time is the one to reach for.  Counting frames means knowing how many the
 * game will draw before it gets where you want it, and being wrong by fifty
 * out of five thousand - which is what the two opening movies come to - means
 * the picture is never taken and twenty-five minutes are spent finding that
 * out.
 */
void sdl_shim_screenshot(const char *path, int after_frames, int after_ms)
{
    if (path == NULL || (after_frames <= 0 && after_ms <= 0)) {
        s_shot_after = 0;
        s_shot_at_ms = 0;
        return;
    }
    size_t i = 0;
    while (path[i] != '\0' && i + 1 < sizeof(s_shot_path)) {
        s_shot_path[i] = path[i];
        ++i;
    }
    s_shot_path[i] = '\0';
    s_shot_after = after_frames;
    s_shot_at_ms = (uint32_t)after_ms;
    s_present_count = 0;
}

/*
 * How much of the arena the game is actually using, reported as it goes.
 *
 * The question this answers is whether the whole thing fits on a part with
 * 8 MB of PSRAM rather than 16, and it cannot be answered by trying smaller
 * arenas one at a time: each attempt is ten minutes and tells you only that
 * this one was too small.  The low-water mark of free space is the answer, and
 * it costs one call per present to keep.
 */
static uint32_t s_memstat_ms;
/* The input handles, declared up here because the readout a thousand
 * lines below has to say when there are none - which is the difference
 * between a game that is broken and one that cannot be reached. */
static ag_handle_t s_kbd = -1;
static ag_handle_t s_mouse = -1;

/* Declared here rather than with the rest of the readout: the arena sampler
 * a few hundred lines below needs to know whether anyone is looking. */
static int s_hud;
static uint32_t s_memstat_next;
static size_t s_arena_low = (size_t)-1;

static void write_ppm(const char *path);

static size_t s_trace_min;
static int    s_trace_max;
static int    s_trace_prof;

void sdl_shim_memstat(int every_ms, int trace_kb, int trace_max,
                      int trace_prof)
{
    s_memstat_ms = (uint32_t)(every_ms > 0 ? every_ms : 0);
    s_memstat_next = 0;
    /* Anyone watching the arena wants to know who asked for the block that did
     * not fit, and that is off unless asked for.  Half a megabyte is too
     * coarse for the question and every allocation is far too many; 32 KB and
     * sixty-four reports name the ones that decide whether a map fits. */
    s_trace_min = (size_t)(trace_kb > 0 ? trace_kb : 32) * 1024u;
    s_trace_max = trace_max > 0 ? trace_max : 64;
    ag_libc_malloc_trace(every_ms > 0 ? s_trace_min : 0,
                         every_ms > 0 ? s_trace_max : 0);
    ag_libc_malloc_profile(every_ms > 0 && trace_prof > 0);
    s_trace_prof = trace_prof;
}

/*
 * Where a frame goes.
 *
 * "It feels slow" is not a number and cannot be optimised.  These three are:
 * the blit is the game's own picture converted through its palette, the flush
 * is handing it to the display, and whatever is left is the engine thinking.
 * Only one of the three is ours to make faster, and knowing which before
 * touching anything is the whole point.
 *
 * The pixel counts go with them.  This port sends only the rectangle that
 * changed, and how much that comes to per frame decides whether sending less
 * is worth anything at all - in QEMU it is worse than useless, because the
 * soft display takes a frame by ringing a doorbell and a partial kick has to
 * wait for the one in flight.
 *
 * Off by default: timing every blit costs two clock reads per blit and the
 * engine makes many small ones.
 */
static uint32_t s_fps_ms;

/* Events as they arrive, before the engine has an opinion about them. */
static uint32_t s_ev_keys;
static uint32_t s_ev_ptr_move;
static uint32_t s_ev_ptr_btn;

/* Where the pointer last was, and which of its buttons were down.
 * Declared here rather than beside the rest of the mouse state further
 * down, because the [input] line in memstat_service reports them and a
 * static has to be declared before the function that reads it. */
static int s_mouse_x = -1;
static int s_mouse_y = -1;
static Uint32 s_mouse_buttons;
static uint32_t s_fps_next;
static uint32_t s_frames;
static uint64_t s_blit_us;

/* The worst single conversion and how many pixels it covered. */

static uint32_t s_blit_worst_us, s_blit_worst_px;

/* Rows that took long enough to mean the thread was descheduled. */

static uint32_t s_preempt_n, s_preempt_us, s_preempt_worst_us;
static uint64_t s_flush_us;
static uint32_t s_flushes;
static uint64_t s_flush_px;
static uint64_t s_blit_px;
static ag_time_t s_window_start_us;

void sdl_shim_fps(int every_ms)
{
    s_fps_ms = (uint32_t)(every_ms > 0 ? every_ms : 0);
    s_fps_next = 0;
}

static uint32_t s_last_present_ms;
static uint32_t s_stall_next_ms;

/* ---------------------------------------------------------------------- */
/* Where a frame goes inside the engine                                     */
/* ---------------------------------------------------------------------- */

/*
 * The board says 47 ms a frame standing still in the Temple of Trials, of
 * which 8 is converting the picture and 8 is putting it on the wire.  The
 * other 31 are "the engine", which is not an answer - it is the name of the
 * part nobody has looked inside.
 *
 * Fallout does its per-frame work in tickers: a list of functions the input
 * layer walks once round the loop.  Music streaming, the mouse, animation, the
 * palette cycle and the script background pass are all on it.  So the list is
 * where the time is, and timing each entry by its own address is enough to
 * name it - addr2line does the rest, and no guess about which one it is has to
 * be made in advance.
 *
 * The cost is two reads of the clock per ticker per frame, which against a
 * ticker that takes milliseconds is nothing.  It stays in because "the engine
 * is slow" is a sentence this project should never have to write twice.
 */
/*
 * How often a palette change is allowed to redraw the whole picture.
 *
 * Fallout animates twenty-seven palette entries - slime, shoreline, two fires,
 * computer monitors, and the red lamp on the interface bar - and every change
 * re-blits the entire screen, because on a PC the display holds RGB and the
 * indices have to be run through the new palette again.  The lamp alone ticks
 * thirty times a second.  Measured on the board, colorCycleTicker was 10.1 ms
 * of a 47 ms frame: the single most expensive thing in the game, spent on a
 * blinking light.
 *
 * Skipping one of those re-blits loses nothing permanently - the next tick is
 * thirty milliseconds away and does the same work - so the honest thing is to
 * let them through at a rate somebody chose rather than at the rate the
 * fastest of four animations happens to run at.  -cycle sets it; 0 turns the
 * limit off and restores the original behaviour for a comparison.
 */
static uint32_t s_pal_blit_ms = 100;
static uint32_t s_pal_blit_at;

void sdl_shim_cycle_ms(int ms)
{
    s_pal_blit_ms = (uint32_t)(ms > 0 ? ms : 0);
}

/*
 * Which panel rows hold a colour that the cycle animates.
 *
 * The cycle only ever touches palette entries 229 to 255 - slime, shorelines,
 * two speeds of fire, computer monitors and the red lamp on the interface bar.
 * Everything else on the screen keeps the colour it had.  But a palette change
 * invalidates the whole conversion table, so the engine answers it by blitting
 * all 640x480 again, and on this machine that is the single most expensive
 * thing that happens: measured at 34 ms when memory is quiet and 132 ms when
 * it is not, once a second, which is exactly the stall Maxim describes.
 *
 * So the port remembers, row by row, whether that row contained any of those
 * indices the last time it was converted - and a palette-only pass converts
 * only those rows.  The flag is built during the ordinary conversion, which
 * already reads every source pixel, so it costs one comparison per pixel and
 * nothing else.  It cannot go stale: a row can only gain a cycled colour by
 * being drawn into, and drawing goes through this same conversion.
 *
 * Everything starts set, so the first pass after a resolution change builds
 * the map honestly rather than skipping work it has never looked at.
 */
#define AG_CYCLE_FIRST 229u
#define AG_ROWS_MAX 512u

static unsigned char s_row_cycled[AG_ROWS_MAX];
static bool s_row_cycled_ready;
static bool s_palette_only;      /* set around the cycle's own blit */
static uint32_t s_cycle_rows_done, s_cycle_rows_skipped, s_cycle_passes;

extern "C" int ag_palette_blit_wanted(void)
{
    if (s_pal_blit_ms == 0u) {
        return 1;
    }
    const uint32_t now = (uint32_t)ag_millis();
    if (s_pal_blit_at != 0u && (now - s_pal_blit_at) < s_pal_blit_ms) {
        return 0;
    }
    s_pal_blit_at = now;
    return 1;
}

/*
 * The engine is about to blit the whole surface because the palette moved.
 * Told so, the conversion below skips every row that has no cycled colour in
 * it.  Set and cleared around that one call; everything else blits normally.
 */
extern "C" void ag_palette_only_begin(void) { s_palette_only = true; }
extern "C" void ag_palette_only_end(void)   { s_palette_only = false; }

/* What renderPresent spends outside the flush; see the note where they are
 * taken.  Printed per frame in the [pres] line. */
static uint32_t s_pres_timers_us, s_pres_memstat_us, s_pres_fps_us;
static uint32_t s_pres_input_us, s_pres_beat_us;

#define AG_TICKERS 12
extern "C" {
uint32_t ag_us(void) { return (uint32_t)ag_micros(); }
void    *ag_ticker_who[AG_TICKERS];
uint32_t ag_ticker_us[AG_TICKERS];
uint32_t ag_ticker_calls[AG_TICKERS];
uint32_t ag_ticker_worst[AG_TICKERS];
uint32_t ag_bk_tickers_us;
uint32_t ag_bk_mouse_us;
/*
 * Which step of the engine's main loop is running, and how many times it has
 * been round.  Set by patch 0036; read by the thread below.
 */
volatile uint32_t ag_ml_phase;
volatile uint32_t ag_ml_laps;

/*
 * The one thing the game could never say: where it was when it stopped.
 *
 * It stops perhaps twice an hour, and every measure of it is healthy right up
 * to the last frame - 43 fps, memory half spare, the card answering in under a
 * millisecond - and then nothing, between one frame and the next.  The
 * supervisor kills it forty-five seconds later and writes a crash record, which
 * says what it can: how long it ran, how much it had.  Not where it was.
 *
 * A thread can say that, because the loop being stuck is exactly the condition
 * under which the loop cannot report.  This one wakes twice a second, and when
 * the lap counter has not moved for five seconds it writes one line into the
 * kernel's journal - which is what the crash record dumps.  Silent otherwise:
 * the counter moves forty times a second when all is well.
 *
 * The two numbers separate the two possible faults.  A phase that stays at 3
 * means the engine is inside scriptsHandleRequests and not coming out - its own
 * loop, its own bug.  A phase that stays at 5 means it is inside renderPresent,
 * which is the port's, and which is a call into the kernel that has not
 * returned.  Either way the next question is a different one from today's.
 */
#define WATCH_TICK_MS 500u
#define WATCH_STUCK_MS 5000u

/* Every frame ever handed over, never reset - s_frames is cleared by each
 * report, and a heartbeat that goes back to nought looks like a stall. */
static volatile uint32_t s_frames_total;

static volatile bool s_watch_stop;

static void watch_thread(void *arg)
{
    (void)arg;
    uint32_t last_laps = 0;
    uint32_t last_move_ms = (uint32_t)ag_millis();
    bool     said = false;

    /*
     * ARGON: it announces itself, because twice it has said nothing while the
     * loop was plainly stuck and there was no way to tell "the thread is not
     * there" from "the thread is there and the report does not reach me".
     *
     * 7 September 2026, a 558-second freeze; 8 September, the walk into the
     * second part of the Temple of Trials.  Both times the loop stopped dead,
     * both times this thread was supposed to name the step, and both times the
     * log holds not one line from it.  So it says hello on its first tick, and
     * everything it has to say now goes through shim_printf as well as the
     * kernel's journal - shim_printf is the path every other line in the log
     * arrives by, and one of the two paths evidently does not work.
     */
    shim_printf("[watch] on, checking every %u ms; a loop that has not moved "
              "for %u ms will be reported\n",
              (unsigned)WATCH_TICK_MS, (unsigned)WATCH_STUCK_MS);

    while (!s_watch_stop) {
        ag_delay(WATCH_TICK_MS);

        /*
         * ARGON: the heartbeat is the last frame handed over, not the main
         * loop's lap counter.
         *
         * Laps only move inside mainLoop, and the game spends a great deal of
         * its life elsewhere: the main menu has a loop of its own, so do the
         * save and load screens, the character editor, the world map and every
         * movie.  Maxim's game froze *at the main menu* on 8 September 2026 and
         * this thread said nothing at all, because laps were nought and the
         * guard for "the loop has not started yet" cannot tell that from "the
         * loop is not the thing running".  A frame handed to the display is
         * the one thing all of those do.
         */
        const uint32_t laps = ag_ml_laps + s_frames_total;
        const uint32_t now = (uint32_t)ag_millis();

        if (laps != last_laps) {
            if (said) {
                /* Before last_move_ms is overwritten: the interesting number
                 * is how long it was gone, and the old code printed the time
                 * since the move it had just recorded, which is nought. */
                shim_printf("[watch] moving again after %u ms; it was in "
                          "step %u at %s\n",
                          (unsigned)(now - last_move_ms),
                          (unsigned)ag_ml_phase,
                          ag_step != NULL ? ag_step : "?");
                said = false;
            }
            last_laps = laps;
            last_move_ms = now;
            continue;
        }
        /*
         * Nothing to report before the game has drawn anything.  It spends
         * about two minutes reading its data before the first frame, and a
         * watcher that calls that a stall says STUCK on every single run -
         * which is how a report gets ignored.
         */
        if (laps == 0u) {
            last_move_ms = now;
            continue;
        }
        if (!said && now - last_move_ms >= WATCH_STUCK_MS) {
            said = true;
            /*
             * Both ways out, because which of them works is the open
             * question.  The step number says which of the loop's six calls
             * it is inside; ag_step says where in the engine, which is the
             * one that names a function rather than a quarter of the program.
             */
            shim_printf("[watch] STUCK: no frame and no lap for %u ms."
                      "  Step %u (1 input, 2 keys, 3 scripts, 4 maps, "
                      "5 present, 6 throttle), %u laps, %u frames, at %s, "
                      "last sound %s\n",
                      (unsigned)(now - last_move_ms), (unsigned)ag_ml_phase,
                      (unsigned)ag_ml_laps, (unsigned)s_frames_total,
                      ag_step != NULL ? ag_step : "?",
                      ag_sfx_name[0] != '\0' ? ag_sfx_name : "-");
            /*
             * And the other end of it.  A deadlock needs two names, and the
             * first report of this one had only the game's: "step 1,
             * inputGetInput, at playing", which says the game is inside the
             * engine's background handlers and not which of them, nor what it
             * is waiting for.  The sound thread's own step, and its lap
             * counter taken twice a second apart, answer both: a counter that
             * moves means it is not the holder.
             */
            const unsigned laps_a = ag_audio_laps;
            const unsigned steps_a = ag_script_steps;
            const unsigned waits_a = ag_script_waits;
            const int ip_a = ag_script_ip;
            ag_delay(200);
            shim_printf("[watch] the sound thread is at %s, %u laps then %u "
                      "200 ms later (%s); background handler %p\n",
                      ag_step_audio != NULL ? ag_step_audio : "?",
                      laps_a, (unsigned)ag_audio_laps,
                      ag_audio_laps == laps_a ? "stopped too" : "still running",
                      ag_ticker_now);
            /*
             * And the interpreter, which is the last place a hang can hide.
             * Three different faults, told apart by which number moves:
             * steps climbing is a loop in the script's own bytecode, steps
             * frozen is one instruction that never returned (and op names it),
             * waits climbing is a wait whose condition cannot be met.
             */
            shim_printf("[watch] script %d proc %d: op 0x%x at ip %d->%d, "
                      "%u steps -> %u, %u waits -> %u (%s)\n",
                      ag_script_now, ag_script_proc_now,
                      (unsigned)ag_script_op, ip_a, ag_script_ip,
                      steps_a, (unsigned)ag_script_steps,
                      waits_a, (unsigned)ag_script_waits,
                      ag_script_waits != waits_a ? "waiting for something"
                          : (ag_script_steps != steps_a ? "running bytecode"
                              : "stopped inside one instruction"));
            ag_log(AG_LOG_ERROR, "fallout",
                   "main loop has not moved for %u ms, stuck in step %u "
                   "(1 input, 2 keys, 3 scripts, 4 maps, 5 present, 6 throttle)"
                   ", lap %u, at %s",
                   (unsigned)(now - last_move_ms), (unsigned)ag_ml_phase,
                   (unsigned)laps, ag_step != NULL ? ag_step : "?");
        }
    }
    ag_thread_exit();
}

uint32_t ag_ml_input_us;
uint32_t ag_ml_keys_us;
uint32_t ag_ml_scripts_us;
uint32_t ag_ml_maps_us;
uint32_t ag_ml_present_us;
uint32_t ag_ml_throttle_us;
}

static void tickers_report(void)
{
    if (s_frames == 0u) {
        return;
    }
    for (int i = 0; i < AG_TICKERS; ++i) {
        if (ag_ticker_who[i] == NULL) {
            break;
        }
        if (ag_ticker_us[i] / s_frames == 0u) {
            continue;
        }
        shim_printf("[tick] %p %u us/frame (%u calls), worst call %u us\n",
                    ag_ticker_who[i],
                    (unsigned)(ag_ticker_us[i] / s_frames),
                    (unsigned)ag_ticker_calls[i],
                    (unsigned)ag_ticker_worst[i]);
        ag_ticker_worst[i] = 0;
    }
    /* One known address, so the pointers above can be turned into names:
     * subtract this from them and add what nm says fps_service is at. */
    shim_printf("[tick base] fps_service at %p\n", (void *)&tickers_report);
    shim_printf("[tick t=%u] tickers %u us/frame, mouse %u us/frame\n",
                (unsigned)ag_millis(),
                (unsigned)(ag_bk_tickers_us / s_frames),
                (unsigned)(ag_bk_mouse_us / s_frames));
    shim_printf("[loop t=%u] input %u, keys %u, scripts %u, maps %u, present %u, throttle %u (us/frame)\n",
                (unsigned)ag_millis(),
                (unsigned)(ag_ml_input_us / s_frames),
                (unsigned)(ag_ml_keys_us / s_frames),
                (unsigned)(ag_ml_scripts_us / s_frames),
                (unsigned)(ag_ml_maps_us / s_frames),
                (unsigned)(ag_ml_present_us / s_frames),
                (unsigned)(ag_ml_throttle_us / s_frames));
    ag_ml_input_us = 0;
    ag_ml_keys_us = 0;
    ag_ml_scripts_us = 0;
    ag_ml_maps_us = 0;
    ag_ml_present_us = 0;
    ag_ml_throttle_us = 0;
    shim_printf("[pres t=%u] timers %u, memstat %u, fps %u, input %u, beat %u (us/frame)\n",
                (unsigned)ag_millis(),
                (unsigned)(s_pres_timers_us / s_frames),
                (unsigned)(s_pres_memstat_us / s_frames),
                (unsigned)(s_pres_fps_us / s_frames),
                (unsigned)(s_pres_input_us / s_frames),
                (unsigned)(s_pres_beat_us / s_frames));
    s_pres_timers_us = 0;
    s_pres_memstat_us = 0;
    s_pres_fps_us = 0;
    s_pres_input_us = 0;
    s_pres_beat_us = 0;
    for (int i = 0; i < AG_TICKERS; ++i) {
        ag_ticker_us[i] = 0;
        ag_ticker_calls[i] = 0;
    }
    ag_bk_tickers_us = 0;
    ag_bk_mouse_us = 0;
}

/* Armed on the first present; see there.  0 turns it off (-watchdog 0). */
static uint32_t s_watchdog_ms = 15000u;
static bool     s_watchdog_armed;

extern "C" void sdl_shim_watchdog(uint32_t ms)
{
    s_watchdog_ms = ms;
}

static void fps_service(void)
{
    if (s_fps_ms == 0) {
        return;
    }
    const uint32_t now = (uint32_t)ag_millis();
    if (s_fps_next == 0) {
        s_fps_next = now + s_fps_ms;
        s_window_start_us = ag_micros();
        return;
    }
    if (now < s_fps_next) {
        return;
    }

    const uint64_t span_us = (uint64_t)(ag_micros() - s_window_start_us);
    const uint32_t frames = s_frames;
    if (frames > 0u && span_us > 0u) {
        const uint64_t other_us =
            (span_us > s_blit_us + s_flush_us) ? span_us - s_blit_us - s_flush_us
                                               : 0u;
        /* The guest's clock goes in the line: it is the only timestamp in the
         * transcript that the engine's own printing can be measured against,
         * and "how long did the map take" is a question about seconds. */
        uint64_t io_us = 0, open_us = 0, seek_us = 0;
        uint32_t io_reads = 0, io_opens = 0, io_seeks = 0;
        ag_libc_io_stats(&io_us, &io_reads, &io_opens, &io_seeks, &open_us,
                         &seek_us);
        shim_printf("[fps t=%u] %u.%02u fps, %u us/frame: blit %u (%u KB), "
                  "flush %u in %u (%u KB), engine %u, io %u us "
                  "(open %u, seek %u, read %u; %u KB) "
                  "(%u reads, %u opens of which %u found nothing, %u seeks, "
                  "in the window)\n",
                  (unsigned)ag_millis(),
                  (unsigned)((uint64_t)frames * 1000000u / span_us),
                  (unsigned)(((uint64_t)frames * 100000000u / span_us) % 100u),
                  (unsigned)(span_us / frames),
                  (unsigned)(s_blit_us / frames),
                  (unsigned)(s_blit_px * 2u / 1024u / frames),
                  (unsigned)(s_flushes / frames),
                  (unsigned)(s_flush_us / frames),
                  (unsigned)(s_flush_px * 2u / 1024u / frames),
                  (unsigned)(other_us / frames),
                  (unsigned)(io_us / frames),
                  (unsigned)(open_us / frames),
                  (unsigned)(seek_us / frames),
                  (unsigned)((io_us - open_us - seek_us) / frames),
                  (unsigned)(ag_libc_io_bytes() / 1024u),
                  /* Totals over the window, not per frame: a few reads across
                   * ninety frames divide down to zero and read as "no I/O at
                   * all" next to an I/O time that says otherwise. */
                  (unsigned)io_reads,
                  (unsigned)io_opens,
                  (unsigned)ag_libc_io_misses(),
                  (unsigned)io_seeks);
        /*
         * On a line of its own.  The first one already wraps an eighty column
         * console twice, and a number split across two rows is a number that
         * has to be pieced together by hand every time it is read.
         */
        shim_printf("[art t=%u] %u read, %u evicted, %u flushed, largest %u; "
                  "spilled %u KB in %u blocks; "
                  "hexdist %u calls, %u steps, %u gave up\n",
                  (unsigned)ag_millis(),
                  ag_cache_fetches, ag_cache_evicted, ag_cache_flushes,
                  ag_cache_biggest,
                  ag_heap_system_bytes / 1024u, ag_heap_system_blocks,
                  ag_tiledist_calls, ag_tiledist_steps,
                  ag_tiledist_gaveup);
        /*
         * What the arena looks like right now, on the same schedule.  A leak
         * across a map change is invisible from the desk unless the number is
         * printed on both sides of it: the dialogue surface needs 300 KB in
         * one piece and works before the temple and not after, so the thing
         * to watch is not the total but the largest block - before entering
         * and after leaving.  Once per stats window, so the 12 ms ag_meminfo
         * costs is paid ninety frames apart.
         */
        {
            ag_meminfo_t mi;
            ag_meminfo(&mi);
            shim_printf("[arena t=%u] %u of %u KB free, largest %u B\n",
                      (unsigned)ag_millis(),
                      (unsigned)(mi.arena_free / 1024u),
                      (unsigned)(mi.arena_total / 1024u),
                      (unsigned)mi.arena_largest);
        }
        /*
         * And what the hands did, because on a board that is the question
         * nobody can answer by looking.  The screen is at the far end of a
         * wire and the console is somewhere else again, so "the mouse does
         * nothing" has no evidence behind it either way - a pointer that never
         * reaches the game and a game that ignores it look identical.  These
         * are counted where the events arrive, before the engine sees them.
         */
        shim_printf("[input t=%u] %u keys, %u pointer moves, %u buttons "
                  "(in the window)\n",
                  (unsigned)ag_millis(), (unsigned)s_ev_keys,
                  (unsigned)s_ev_ptr_move, (unsigned)s_ev_ptr_btn);
        s_ev_keys = 0;
        s_ev_ptr_move = 0;
        s_ev_ptr_btn = 0;
        tickers_report();
    }
    ag_tiledist_calls = 0;
    ag_tiledist_steps = 0;
    ag_cache_fetches = 0;
    ag_cache_evicted = 0;
    ag_cache_flushes = 0;

    s_frames = 0;
    s_blit_us = 0;
    s_flush_us = 0;
    s_flushes = 0;
    s_flush_px = 0;
    s_blit_px = 0;
    ag_libc_io_reset();
    s_window_start_us = ag_micros();
    s_fps_next = now + s_fps_ms;
}

/*
 * The picture that was on the screen when the game stopped.
 *
 * A screenshot armed for a deadline or a frame count never happens if the game
 * ends before either arrives, and then there is nothing to look at and no way
 * to tell where it got to.  Called from ag_cxx_main once main has returned,
 * while the display is still ours.
 */
void sdl_shim_screenshot_now(void)
{
    if (s_shot_path[0] == '\0' || (s_shot_after == 0 && s_shot_at_ms == 0)) {
        return;
    }
    s_shot_after = 0;
    s_shot_at_ms = 0;
    write_ppm(s_shot_path);
}

/*
 * How often the arena is asked how much of it is left.
 *
 * This used to be every frame, and unconditionally - before the check that
 * turns the rest of this function off.  ag_meminfo is not a counter read: it
 * walks the heap to find the free total and the largest hole, and the heap in
 * question is six megabytes of PSRAM holding twenty thousand blocks.  On the
 * board renderPresent cost 19 ms a frame of which the flush was 2.5, and this
 * was most of the difference - a diagnostic that the shipping configuration
 * paid for on every frame whether -memstat was given or not.
 *
 * Four times a second is enough for what it is for.  The low-water mark it
 * keeps is "what the game held between frames at its worst", and a peak that
 * lasts less than a quarter of a second is not what decides whether the next
 * map loads; the transient peak, which is the one that does, is counted in the
 * allocator itself and costs nothing.
 */
#define ARENA_SAMPLE_MS 250u

/*
 * What the two caches missed, and while the game was doing what.
 *
 * Cumulative since the run started, because the question is "how much of this
 * happens in a game" and not "how much happened in the last twenty seconds" -
 * and because the buckets already do the separating that a short window would
 * otherwise have to do.  The per-window figures are on the [art] line.
 *
 * One line per cache rather than one per bucket: twenty-one lines every
 * twenty seconds is what made the driver's RAW line unusable on an eighty
 * column console, and a report nobody can read is not a report.  Buckets with
 * no traffic at all are left out.
 */
static const char* const ag_where_name[AG_WHERES] = {
    "walk", "fight", "talk", "ui", "maps", "wmap", "menu"
};
static const char* const ag_kind_name[AG_CACHE_KINDS] = { "art", "snd", "other" };

/*
 * The worst lap since the last report, split by step.
 *
 * This is the line that says what a dip *was*.  Six numbers and the two that
 * matter beside them: how many cache misses happened inside that lap and how
 * long they took.  If scripts is the big one, it is the engine's own work -
 * an attack resolves in scriptsHandleRequests.  If the misses account for it,
 * it is the card.  If neither does, the time is somewhere nothing measures
 * yet, and that is the next thing to instrument.
 */
/* The working-set table; see ag_diag.h. */
#define AG_WS_SLOTS 2048

static int      s_ws_key[AG_WS_SLOTS];
static unsigned s_ws_size[AG_WS_SLOTS];
static unsigned s_ws_used, s_ws_bytes, s_ws_over;

void ag_ws_note(int kind, int key, int size)
{
    unsigned h;
    unsigned i;

    if (kind != AG_CACHE_ART || size <= 0) {
        return;
    }

    h = ((unsigned)key * 2654435761u) & (AG_WS_SLOTS - 1u);
    for (i = 0; i < AG_WS_SLOTS; i++) {
        const unsigned at = (h + i) & (AG_WS_SLOTS - 1u);
        if (s_ws_size[at] == 0u) {
            if (s_ws_used * 4u >= AG_WS_SLOTS * 3u) {
                s_ws_over++;   /* three quarters full: stop, and say so */
                return;
            }
            s_ws_key[at] = key;
            s_ws_size[at] = (unsigned)size;
            s_ws_used++;
            s_ws_bytes += (unsigned)size;
            return;
        }
        if (s_ws_key[at] == key) {
            return;            /* already counted */
        }
    }
    s_ws_over++;
}

static void ws_report(void)
{
    if (s_ws_used == 0u) {
        return;
    }
    shim_printf("[ws t=%u] art asked for %u distinct files, %u KB in this "
                "window%s\n",
                (unsigned)ag_millis(), s_ws_used, s_ws_bytes / 1024u,
                (s_ws_over != 0u) ? " (table full, the real figure is larger)"
                                  : "");
    memset(s_ws_size, 0, sizeof(s_ws_size));
    s_ws_used = 0u;
    s_ws_bytes = 0u;
    s_ws_over = 0u;
}

unsigned ag_art_miss[AG_ART_KINDS];
unsigned ag_art_from_flash[AG_ART_KINDS];
unsigned ag_anim_refresh_n, ag_anim_refresh_px;
unsigned ag_anim_worst_us, ag_anim_worst_refreshes;
unsigned ag_anim_worst_px, ag_anim_worst_sads;
unsigned ag_anim_seq_us, ag_anim_move_us, ag_anim_script_us;
unsigned ag_anim_worst_seq_us, ag_anim_worst_move_us;
unsigned ag_anim_worst_script_us;
unsigned ag_path_us, ag_path_n;
unsigned ag_layer_n;
unsigned ag_render_calls, ag_render_steps, ag_render_inrange;
unsigned ag_render_tiles, ag_render_objects, ag_render_draw_us;
unsigned ag_render_objects2, ag_render_draw2_us;
unsigned ag_draw_calls, ag_draw_hidden, ag_draw_missed, ag_draw_hit;
unsigned ag_draw_early;
unsigned ag_art_requests;
unsigned ag_miss_ram, ag_miss_flash, ag_miss_card, ag_miss_card_kb;
unsigned ag_draw_lock_us;
unsigned ag_layer_clear_us, ag_layer_floor_us, ag_layer_pre_us;
unsigned ag_layer_roof_us, ag_layer_post_us, ag_layer_window_us;
unsigned ag_move_refresh_us, ag_move_refresh_n, ag_move_refresh_px;
unsigned ag_art_lock_us, ag_art_lock_n;
unsigned ag_heaplock_us, ag_heaplock_n;
unsigned ag_tilerefresh_us, ag_tilerefresh_n, ag_tilerefresh_px;
unsigned ag_anim_worst_tref_us, ag_anim_worst_tref_n, ag_anim_worst_tref_px;
unsigned ag_anim_worst_lock_us, ag_anim_worst_rfsh_us;
unsigned ag_scroll_steps;
unsigned ag_scroll_move_us;
unsigned ag_scroll_strip_us;
unsigned ag_scroll_window_us;
unsigned ag_scroll_mouse_us;
unsigned ag_art_miss_ms[AG_ART_KINDS];
unsigned ag_walk_tiles;
unsigned ag_dat_open_us, ag_dat_open_n;
unsigned ag_dat_read_us, ag_dat_read_n, ag_dat_read_kb;
unsigned ag_dat_inflate_us, ag_dat_inflate_n;

/*
 * ARGON: what a cache miss is actually made of.
 *
 * The three shares add up to the whole of a miss, so the line answers the
 * question directly: 21 KB in 10.8 ms is 1.9 MB/s, and this says whether that
 * is the card or zlib.  Averages per call are printed beside the totals
 * because the chunk sizes make the call counts large - the card is read 4096
 * bytes at a time and zlib is fed 1024 at a time.
 */
/*
 * Misses per hundred hexes walked - the figure that can be compared between
 * two sessions, because it divides out how much walking there was.
 */
/* Which kind of art the misses are for; see ag_art_miss. */
/* What the flash art store did, and how much of each miss it took over. */
/* What a scroll step costs, split four ways.  Silent until the map moves. */
static void scroll_report(void)
{
    if (ag_scroll_steps == 0u) {
        return;
    }

    shim_printf("[scroll t=%u] %u steps | move %u us, strips %u us, "
                "window %u us, cursor %u us (per step)\n",
                (unsigned)ag_millis(), ag_scroll_steps,
                ag_scroll_move_us / ag_scroll_steps,
                ag_scroll_strip_us / ag_scroll_steps,
                ag_scroll_window_us / ag_scroll_steps,
                ag_scroll_mouse_us / ag_scroll_steps);
    ag_scroll_steps = 0;
    ag_scroll_move_us = 0;
    ag_scroll_strip_us = 0;
    ag_scroll_window_us = 0;
    ag_scroll_mouse_us = 0;
}

static void art_flash_report(void)
{
    /* 300 was not enough once the store started saying what it had done, and
     * the part that fell off the end was the map-load timing - the one number
     * the line had been extended to carry. */
    char line[560];
    unsigned from_flash = 0;
    int i;

    for (i = 0; i < AG_ART_KINDS; i++) {
        from_flash += ag_art_from_flash[i];
    }

    ag_art_store_report(line, sizeof(line));
    shim_printf("%s | %u misses came from flash\n", line, from_flash);
    ag_art_store_kinds(line, sizeof(line));
    shim_printf("%s\n", line);
}

static void art_kind_report(void)
{
    static const char *const name[AG_ART_KINDS] = {
        "item", "critter", "scenery", "wall", "tile", "misc",
        "iface", "inven", "head", "backgrnd", "skilldex"
    };
    char line[300];
    unsigned pos = 0;
    int i;
    unsigned total = 0;

    for (i = 0; i < AG_ART_KINDS; i++) {
        total += ag_art_miss[i];
    }
    if (total == 0u) {
        return;
    }

    for (i = 0; i < AG_ART_KINDS; i++) {
        if (ag_art_miss[i] == 0u) {
            continue;
        }
        pos += (unsigned)snprintf(line + pos, sizeof(line) - pos,
                                  "%s%s %u/%ums", (pos != 0u) ? " | " : "",
                                  name[i], ag_art_miss[i],
                                  ag_art_miss_ms[i]);
        if (pos >= sizeof(line) - 24u) {
            break;
        }
    }
    shim_printf("[artkind t=%u] %s\n", (unsigned)ag_millis(), line);
}

static void walk_report(void)
{
    /*
     * ARGON: the misses counted over the SAME window as the hexes.
     *
     * The first version divided the running total of misses by the hexes of
     * the windows that qualified, and the two covered different things: a
     * session that loaded a map and sat in a menu put all of that in the
     * numerator while only walking reached the denominator.  Comparing two
     * builds that way made eight pins look three times worse than four when
     * the only real difference was that Maxim had walked sixty hexes in one
     * run and a hundred and fifty in the other.  A rate is only a rate if both
     * halves are measured over the same stretch.
     */
    static unsigned seen_tiles, seen_miss, seen_ms;
    static unsigned seen_kind[AG_ART_KINDS], seen_kind_ms[AG_ART_KINDS];

    const unsigned tiles = ag_walk_tiles - seen_tiles;
    const unsigned miss = ag_cache_miss[AG_CACHE_ART * AG_WHERES + AG_WHERE_WALK]
        - seen_miss;
    const unsigned ms = ag_cache_miss_ms[AG_CACHE_ART * AG_WHERES + AG_WHERE_WALK]
        - seen_ms;
    unsigned crit, crit_ms, tile, tile_ms, all_ms = 0u;
    int i;

    crit = ag_art_miss[1] - seen_kind[1];              /* OBJ_TYPE_CRITTER */
    crit_ms = ag_art_miss_ms[1] - seen_kind_ms[1];
    tile = ag_art_miss[4] - seen_kind[4];              /* OBJ_TYPE_TILE */
    tile_ms = ag_art_miss_ms[4] - seen_kind_ms[4];
    for (i = 0; i < AG_ART_KINDS; i++) {
        all_ms += ag_art_miss_ms[i] - seen_kind_ms[i];
    }

    seen_tiles += tiles;
    seen_miss += miss;
    seen_ms += ms;
    for (i = 0; i < AG_ART_KINDS; i++) {
        seen_kind[i] += ag_art_miss[i] - seen_kind[i];
        seen_kind_ms[i] += ag_art_miss_ms[i] - seen_kind_ms[i];
    }

    if (tiles < 10u) {
        return;   /* standing still says nothing about the cache */
    }

    shim_printf("[walk t=%u] %u hexes | all %u misses %u ms | critter %u/%ums "
                "| tile %u/%ums | PER 100: %u misses, %u ms, crit %u, tile %u\n",
                (unsigned)ag_millis(), tiles, miss, ms,
                crit, crit_ms, tile, tile_ms,
                miss * 100u / tiles, all_ms * 100u / tiles,
                crit_ms * 100u / tiles, tile_ms * 100u / tiles);

    /*
     * And the same thing over the whole run.  A window is twenty seconds and
     * can hold thirteen hexes or fifty; the per-window figures swing between
     * 1465 and 8592 misses per hundred hexes on the same build, which is too
     * noisy to compare two builds by.  The running total settles.
     */
    {
        static unsigned run_hexes, run_miss, run_ms, run_crit_ms, run_tile_ms;

        run_hexes += tiles;
        run_miss += miss;
        run_ms += all_ms;
        run_crit_ms += crit_ms;
        run_tile_ms += tile_ms;

        shim_printf("[walkrun t=%u] %u hexes so far | PER 100: %u misses, "
                    "%u ms, crit %u ms, tile %u ms\n",
                    (unsigned)ag_millis(), run_hexes,
                    run_miss * 100u / run_hexes, run_ms * 100u / run_hexes,
                    run_crit_ms * 100u / run_hexes,
                    run_tile_ms * 100u / run_hexes);
    }
}

static void dat_report(void)
{
    if (ag_dat_read_n == 0u && ag_dat_open_n == 0u) {
        return;
    }
    shim_printf("[dat t=%u] open %u in %u ms | card %u reads of %u KB in %u ms "
                "(%u KB/s) | inflate %u in %u ms\n",
                (unsigned)ag_millis(),
                ag_dat_open_n, ag_dat_open_us / 1000u,
                ag_dat_read_n, ag_dat_read_kb, ag_dat_read_us / 1000u,
                (ag_dat_read_us >= 1000u)
                    ? (unsigned)((unsigned long long)ag_dat_read_kb * 1000000ull
                                 / ag_dat_read_us)
                    : 0u,
                ag_dat_inflate_n, ag_dat_inflate_us / 1000u);
}

/*
 * The stall ring, printed as one burst.  Times first: if they are a second
 * apart the cause is on a schedule and the schedule names it; if they are
 * scattered, it is work that happens to be heavy and the breakdown says which
 * step.  Either way nothing is printed between bursts.
 */
static void stall_report(void)
{
    if (s_stall_n == 0u) {
        return;
    }

    const unsigned have = (s_stall_n < AG_STALLS) ? s_stall_n : AG_STALLS;
    const unsigned first = (s_stall_n < AG_STALLS) ? 0u
                                                   : (s_stall_n % AG_STALLS);
    uint32_t prev = 0;

    shim_printf("[stalls t=%u] %u laps over %u ms since the last burst\n",
                (unsigned)ag_millis(), (unsigned)s_stall_n,
                (unsigned)(AG_STALL_US / 1000u));

    for (unsigned i = 0; i < have; i++) {
        const ag_stall_t *e = &s_stalls[(first + i) % AG_STALLS];

        shim_printf("[stall] t=%u +%u ms | %u ms = input %u, keys %u, "
                    "scripts %u, maps %u, present %u (us)\n",
                    (unsigned)e->at_ms,
                    (unsigned)(prev ? e->at_ms - prev : 0u),
                    (unsigned)(e->cost_us / 1000u),
                    (unsigned)e->input_us, (unsigned)e->keys_us,
                    (unsigned)e->scripts_us, (unsigned)e->maps_us,
                    (unsigned)e->present_us);
        prev = e->at_ms;
    }
    s_stall_n = 0;
}

static void worst_report(void)
{
    if (s_worst_us == 0u) {
        return;
    }
    shim_printf("[worst t=%u] %u ms while %s: input %u, keys %u, scripts %u, "
              "maps %u, present %u, slept %u (us); %u cache miss cost %u ms, "
              "%u KB\n",
              (unsigned)ag_millis(), (unsigned)(s_worst_us / 1000u),
              (s_worst_where >= 0 && s_worst_where < AG_WHERES)
                  ? ag_where_name[s_worst_where] : "?",
              (unsigned)s_worst_input, (unsigned)s_worst_keys,
              (unsigned)s_worst_scripts, (unsigned)s_worst_maps,
              (unsigned)s_worst_present, (unsigned)s_worst_throttle,
              (unsigned)s_worst_miss_n, (unsigned)s_worst_miss_ms,
              (unsigned)s_worst_kb);
    if (s_modal_laps != 0u) {
        shim_printf("[worst] and %u lap(s) over %u s set aside as screens, "
                    "worst %u ms\n",
                    (unsigned)s_modal_laps, (unsigned)(AG_MODAL_US / 1000000u),
                    (unsigned)(s_modal_worst_us / 1000u));
        s_modal_laps = 0u;
        s_modal_worst_us = 0u;
    }
    s_worst_us = 0u;
    s_worst_where = -1;
}

/*
 * And the files the misses read, worst first.
 *
 * Cumulative and merged by name, so the row to look at is the one whose count
 * is large: that file is being read, thrown out, and read again.
 */
static void top_report(void)
{
    /* Ordered by picking, with a local mask: the table itself is left exactly
     * as it was, because it goes on accumulating.  One that reset itself every
     * twenty seconds could never show the file read thirty times, which is the
     * whole point of keeping it. */
    unsigned done = 0;

    for (int printed = 0; printed < AG_TOP_MISSES; printed++) {
        int pick = -1;
        unsigned best = 0;

        for (int i = 0; i < AG_TOP_MISSES; i++) {
            if (ag_top_name[i][0] == '\0' || (done & (1u << i)) != 0u) {
                continue;
            }
            if (ag_top_ms[i] >= best) {
                best = ag_top_ms[i];
                pick = i;
            }
        }
        if (pick < 0) {
            break;
        }
        done |= 1u << pick;

        shim_printf("[file t=%u] %s %s x%u, %u KB, %u ms total, last while %s\n",
                  (unsigned)ag_millis(),
                  ag_kind_name[ag_top_kind[pick] < AG_CACHE_KINDS
                                   ? ag_top_kind[pick] : AG_CACHE_OTHER],
                  ag_top_name[pick], ag_top_count[pick], ag_top_kb[pick],
                  ag_top_ms[pick],
                  ag_where_name[ag_top_where[pick] < AG_WHERES
                                    ? ag_top_where[pick] : AG_WHERE_WALK]);
    }
}

/*
 * And the sound, which is not in the cache report because it is not a cache
 * miss: every play decodes the file again whether the cache had it or not.
 */
/* What the audio thread costs, and the worst single block.  See the note in
 * the thread loop: this exists to tell a slow ticker from a shared wait. */
/* The worst conversion of the window, with its area - see the note at the
 * timing site. */
/* How much of the colour-cycle repaint the row map saved. */
/* Every read from the card, and the worst single one - see posix_io.cc. */
extern "C" unsigned ag_io_read_n, ag_io_read_us, ag_io_read_kb;
extern "C" unsigned ag_io_read_worst_us, ag_io_read_worst_len;

/*
 * The slow-script trap.  Quiet by design: a tile script that behaves costs
 * tens of microseconds, and there are thousands of them in a game.  Only one
 * that takes longer than a frame is worth a line, and then it should say
 * where it was so the script can be found.
 */
#define AG_SLOW_SCRIPT_US 20000u

static unsigned s_rm_resolve_ms, s_rm_remove_ms, s_rm_n;

extern "C" void ag_remove_note(unsigned resolve_ms, unsigned remove_ms)
{
    s_rm_resolve_ms += resolve_ms;
    s_rm_remove_ms += remove_ms;
    s_rm_n++;
}

extern "C" void ag_direrase_note(const char *what, int files,
                                 unsigned list_ms, unsigned delete_ms)
{
    shim_printf("[direrase] %s: %d files, listing %u ms, deleting %u ms "
                "(%u removes: %u ms resolving, %u ms unlinking)\n",
                (what != NULL) ? what : "?", files, list_ms, delete_ms,
                s_rm_n, s_rm_resolve_ms, s_rm_remove_ms);
    s_rm_resolve_ms = 0;
    s_rm_remove_ms = 0;
    s_rm_n = 0;
}

extern "C" void ag_unpack_note(unsigned protos_ms, unsigned maps_ms,
                               unsigned rest_ms)
{
    shim_printf("[unpack] erasing the card %u ms, party protos %u ms, the rest %u ms\n",
                protos_ms, maps_ms, rest_ms);
}

extern "C" void ag_load_note(unsigned ms)
{
    shim_printf("[load] the whole wait was %u ms (%u.%u s)\n",
                ms, ms / 1000u, (ms % 1000u) / 100u);
}

extern "C" void ag_script_slow_note(unsigned us, int tile, int elevation)
{
    static unsigned said;

    if (us < AG_SLOW_SCRIPT_US || said >= 32u) {
        return;
    }
    said++;
    shim_printf("[slowscript t=%u] a tile script took %u ms at tile %d, "
                "elevation %d\n",
                (unsigned)ag_millis(), us / 1000u, tile, elevation);
}

static void miss_report(void)
{
    if (ag_miss_ram == 0u) {
        return;
    }
    shim_printf("[miss t=%u] %u art asked for | %u from RAM, "
                "%u from flash, %u from the card (%u KB)\n",
                (unsigned)ag_millis(), ag_art_requests,
                (ag_art_requests > ag_miss_ram)
                    ? ag_art_requests - ag_miss_ram : 0u,
                ag_miss_flash, ag_miss_card, ag_miss_card_kb);
    ag_art_requests = 0;
    ag_miss_ram = 0; ag_miss_flash = 0;
    ag_miss_card = 0; ag_miss_card_kb = 0;
}

static void draw_report(void)
{
    if (ag_draw_calls == 0u) {
        return;
    }
    shim_printf("[draw t=%u] %u calls | %u hidden, %u refused early, "
                "%u missed the rect, "
                "%u drew | locking the art cost %u ms\n",
                (unsigned)ag_millis(), ag_draw_calls, ag_draw_hidden,
                ag_draw_early, ag_draw_missed, ag_draw_hit,
                ag_draw_lock_us / 1000u);
    ag_draw_calls = 0; ag_draw_hidden = 0; ag_draw_missed = 0;
    ag_draw_hit = 0; ag_draw_lock_us = 0;
    ag_draw_early = 0;
}

static void render_report(void)
{
    if (ag_render_calls == 0u) {
        return;
    }
    shim_printf("[render t=%u] %u passes | %u steps, %u in range, "
                "%u tiles with objects | flat %u in %u ms, "
                "upright %u in %u ms\n",
                (unsigned)ag_millis(), ag_render_calls, ag_render_steps,
                ag_render_inrange, ag_render_tiles, ag_render_objects,
                ag_render_draw_us / 1000u, ag_render_objects2,
                ag_render_draw2_us / 1000u);
    ag_render_calls = 0; ag_render_steps = 0; ag_render_inrange = 0;
    ag_render_tiles = 0; ag_render_objects = 0; ag_render_draw_us = 0;
    ag_render_objects2 = 0; ag_render_draw2_us = 0;
}

static void layer_report(void)
{
    if (ag_layer_n == 0u) {
        return;
    }
    shim_printf("[layers t=%u] %u refreshes | clear %u, floor %u, "
                "under-roof %u, roof %u, over-roof %u, window %u (ms)\n",
                (unsigned)ag_millis(), ag_layer_n,
                ag_layer_clear_us / 1000u, ag_layer_floor_us / 1000u,
                ag_layer_pre_us / 1000u, ag_layer_roof_us / 1000u,
                ag_layer_post_us / 1000u, ag_layer_window_us / 1000u);
    ag_layer_n = 0;
    ag_layer_clear_us = 0; ag_layer_floor_us = 0; ag_layer_pre_us = 0;
    ag_layer_roof_us = 0; ag_layer_post_us = 0; ag_layer_window_us = 0;
}

static void move_report(void)
{
    if (ag_path_n == 0u && ag_move_refresh_n == 0u) {
        return;
    }
    shim_printf("[move t=%u] %u path searches costing %u ms | "
                "%u refreshes over %u px costing %u ms\n",
                (unsigned)ag_millis(), ag_path_n, ag_path_us / 1000u,
                ag_move_refresh_n, ag_move_refresh_px,
                ag_move_refresh_us / 1000u);
    ag_path_n = 0; ag_path_us = 0;
    ag_move_refresh_n = 0; ag_move_refresh_us = 0; ag_move_refresh_px = 0;
}

static void anim_report(void)
{
    if (ag_anim_worst_us == 0u) {
        return;
    }
    shim_printf("[anim t=%u] worst tick %u us | %u refreshes over %u px, "
                "%u animations live | sequence %u us, move %u us, "
                "spatial script %u us, art lock %u us (heap %u us in %u), "
                "refresh %u us, map redraw %u us in %u over %u px, "
                "UNACCOUNTED %u us\n",
                (unsigned)ag_millis(), ag_anim_worst_us,
                ag_anim_worst_refreshes, ag_anim_worst_px,
                ag_anim_worst_sads, ag_anim_worst_seq_us,
                ag_anim_worst_move_us, ag_anim_worst_script_us,
                ag_anim_worst_lock_us, ag_heaplock_us, ag_heaplock_n,
                ag_anim_worst_rfsh_us, ag_anim_worst_tref_us,
                ag_anim_worst_tref_n, ag_anim_worst_tref_px,
                (ag_anim_worst_us > (ag_anim_worst_seq_us
                                     + ag_anim_worst_move_us
                                     + ag_anim_worst_script_us
                                     + ag_anim_worst_lock_us
                                     + ag_anim_worst_tref_us))
                    ? (ag_anim_worst_us - ag_anim_worst_seq_us
                       - ag_anim_worst_move_us - ag_anim_worst_script_us
                       - ag_anim_worst_lock_us - ag_anim_worst_tref_us)
                    : 0u);
    ag_anim_worst_us = 0;
    ag_tilerefresh_us = 0;
    ag_tilerefresh_n = 0;
    ag_tilerefresh_px = 0;
    ag_heaplock_us = 0;
    ag_heaplock_n = 0;
}

static void preempt_report(void)
{
    if (s_preempt_n == 0u) {
        return;
    }
    shim_printf("[preempt t=%u] %u rows lost %u ms in total, worst %u us "
                "(a row is 60 us of work)\n",
                (unsigned)ag_millis(), (unsigned)s_preempt_n,
                (unsigned)(s_preempt_us / 1000u),
                (unsigned)s_preempt_worst_us);
    s_preempt_n = 0;
    s_preempt_us = 0;
    s_preempt_worst_us = 0;
}

static void io_report(void)
{
    if (ag_io_read_n == 0u) {
        return;
    }
    shim_printf("[io t=%u] %u reads, %u KB, %u ms total | worst single read "
                "%u us for %u bytes\n",
                (unsigned)ag_millis(), ag_io_read_n, ag_io_read_kb,
                ag_io_read_us / 1000u, ag_io_read_worst_us,
                ag_io_read_worst_len);
    ag_io_read_n = 0;
    ag_io_read_us = 0;
    ag_io_read_kb = 0;
    ag_io_read_worst_us = 0;
    ag_io_read_worst_len = 0;
}

static void cycle_report(void)
{
    if (s_cycle_rows_done == 0u && s_cycle_rows_skipped == 0u) {
        return;
    }
    const uint32_t total = s_cycle_rows_done + s_cycle_rows_skipped;

    shim_printf("[cycle t=%u] palette passes: %u rows converted, %u skipped "
                "(%u%% saved)\n",
                (unsigned)ag_millis(), (unsigned)s_cycle_rows_done,
                (unsigned)s_cycle_rows_skipped,
                total ? (unsigned)(s_cycle_rows_skipped * 100u / total) : 0u);
    s_cycle_rows_done = 0;
    s_cycle_rows_skipped = 0;
}

static void blit_report(void)
{
    if (s_blit_worst_us == 0u) {
        return;
    }
    shim_printf("[blit t=%u] worst conversion %u us over %u px "
                "(%u ns per px)\n",
                (unsigned)ag_millis(), (unsigned)s_blit_worst_us,
                (unsigned)s_blit_worst_px,
                s_blit_worst_px ? (unsigned)((uint64_t)s_blit_worst_us * 1000u
                                             / s_blit_worst_px) : 0u);
    s_blit_worst_us = 0;
    s_blit_worst_px = 0;
}

static void audio_report(void)
{
    if (s_audio_blocks == 0u) {
        return;
    }
    shim_printf("[audio t=%u] %u blocks | mix %u us avg, worst %u us | "
                "write %u us avg, worst %u us\n",
                (unsigned)ag_millis(), (unsigned)s_audio_blocks,
                (unsigned)(s_audio_mix_us / s_audio_blocks),
                (unsigned)s_audio_mix_worst_us,
                (unsigned)(s_audio_write_us / s_audio_blocks),
                (unsigned)s_audio_write_worst_us);
    s_audio_blocks = 0;
    s_audio_mix_us = 0;
    s_audio_write_us = 0;
    s_audio_mix_worst_us = 0;
    s_audio_write_worst_us = 0;
}

static void sfx_report(void)
{
    if (ag_sfx_loads == 0u) {
        return;
    }
    shim_printf("[sfx t=%u] %u loads, %u ms total (%u ms each), %u KB; "
              "worst %u ms on %s\n",
              (unsigned)ag_millis(), ag_sfx_loads, ag_sfx_ms,
              ag_sfx_ms / ag_sfx_loads, ag_sfx_kb, ag_sfx_worst_ms,
              ag_sfx_worst[0] != '\0' ? ag_sfx_worst : "?");
}

static void cache_report(void)
{
    static const char* const* const where_name = ag_where_name;
    static const char* const* const kind_name = ag_kind_name;
    static bool said_legend;

    unsigned traffic = 0;
    for (int i = 0; i < AG_CACHE_SLOTS; i++) {
        traffic += ag_cache_miss[i];
    }
    if (traffic == 0) {
        return;
    }

    if (!said_legend) {
        shim_printf("[cache] each group is <doing> <hit rate> <misses>m "
                  "<of those, files it had and threw out>r <ms the game "
                  "stood still>; totals since the run began\n");
        said_legend = true;
    }

    for (int k = 0; k < AG_CACHE_KINDS; k++) {
        char line[240];
        size_t at = 0;
        bool any = false;

        for (int w = 0; w < AG_WHERES; w++) {
            const int s = k * AG_WHERES + w;
            const unsigned hit = ag_cache_hit[s];
            const unsigned miss = ag_cache_miss[s];
            if (hit == 0 && miss == 0) {
                continue;
            }
            const unsigned asked = hit + miss;
            const unsigned pc = (unsigned)((uint64_t)hit * 100u / asked);
            const int n = snprintf(line + at, sizeof(line) - at,
                                   "%s%s %u%% %um %ur %ums",
                                   any ? " | " : "", where_name[w], pc,
                                   miss, ag_cache_refetch[s],
                                   ag_cache_miss_ms[s]);
            if (n < 0) {
                break;
            }
            any = true;
            at += (size_t)n;
            /* Truncated rather than wrapped: snprintf returns what it would
             * have written, so this is the only way to know. */
            if (at >= sizeof(line) - 48u) {
                break;
            }
        }

        if (!any) {
            continue;
        }

        unsigned kb = 0, out = 0, fl = 0, bad = 0;
        for (int w = 0; w < AG_WHERES; w++) {
            const int s = k * AG_WHERES + w;
            kb += ag_cache_miss_kb[s];
            out += ag_cache_out[s];
            fl += ag_cache_flush[s];
            bad += ag_cache_failed[s];
        }

        shim_printf("[cache %s t=%u] %s || read %u KB, threw out %u, "
                  "emptied %u times, %u refused\n",
                  kind_name[k], (unsigned)ag_millis(), line, kb, out, fl, bad);
    }
}

/* Defined with the metric itself, further down. */
static void metric_report(void);

static void memstat_service(void)
{
    const uint32_t now = (uint32_t)ag_millis();
    static uint32_t sample_next;

    /* And only when somebody is going to read it.  One ag_meminfo on the
     * board's six megabyte arena measured about twelve milliseconds - four a
     * second is 1.5 ms a frame, which is five per cent of a frame spent
     * keeping a number for a report nobody asked for. */
    if ((s_memstat_ms != 0u || s_hud) && now >= sample_next) {
        sample_next = now + ARENA_SAMPLE_MS;
        ag_meminfo_t mi;
        ag_meminfo(&mi);
        if (mi.arena_free < s_arena_low) {
            s_arena_low = mi.arena_free;
        }
    }

    if (s_memstat_ms == 0) {
        return;
    }
    if (now < s_memstat_next) {
        return;
    }
    ag_meminfo_t mi;
    ag_meminfo(&mi);
    s_memstat_next = now + s_memstat_ms;
    /* The report budget is per window rather than per run.  A fixed budget is
     * spent entirely on startup - the engine asks for six megabytes before it
     * draws a frame - and the window that matters is always a later one.
     * Resetting here means "up to N names between two [mem] lines", which is
     * what a walk across a map wants. */
    ag_libc_malloc_trace(s_trace_min, s_trace_max);
    /*
     * Two peaks, because they answer different questions.  The first is what
     * the game holds between frames - what it costs to sit somewhere.  The
     * second counts the big transients too, and that is the one the board has
     * to satisfy: an engine that reads a file into a buffer, copies it and
     * frees it again still needs the buffer to exist.  Sampling once a frame
     * never sees those: in the Temple of Trials the two read 5610 and 6055 KB,
     * and the 445 KB between them is what decided whether the game fitted in
     * 8 MB.
     */
    const size_t   low = ag_libc_arena_low();
    const unsigned frame_peak =
        (unsigned)((mi.arena_total - s_arena_low) / 1024u);
    /* Before the first big allocation there is nothing extra to report, and a
     * zero there reads as "no transients ever", which is a different claim. */
    const unsigned peak_alloc =
        (low == (size_t)-1) ? frame_peak
                            : (unsigned)((mi.arena_total - low) / 1024u);
    shim_printf("[mem] %u KB used of %u, peak %u (%u with transients), "
              "largest free block %u KB\n",
              (unsigned)((mi.arena_total - mi.arena_free) / 1024u),
              (unsigned)(mi.arena_total / 1024u),
              frame_peak,
              peak_alloc,
              (unsigned)(mi.arena_largest / 1024u));
    ag_libc_malloc_profile_dump(s_trace_prof);
    /* The worst lap first: it is the one that says whether to read the rest. */
    stall_report();
    metric_report();   /* first: it is the one number that answers "how does
                        * it play", and the rest explain it. */
    worst_report();
    dat_report();
    ws_report();
    walk_report();
    art_kind_report();
    art_flash_report();
    scroll_report();
    cache_report();
    sfx_report();

    audio_report();

    blit_report();
    cycle_report();
    io_report();

    preempt_report();
    anim_report();
    move_report();
    layer_report();
    render_report();
    draw_report();
    miss_report();
    top_report();
    /*
     * ARGON: and what the hands did, on this schedule rather than only with
     * -fps.
     *
     * "The mouse worked at the menu and does nothing after loading a save"
     * (Maxim, 8 September 2026) has two completely different explanations -
     * the events stop arriving, or they arrive and the engine ignores them -
     * and no way at all to choose between them: the USB port said the mouse
     * had never been silent, and the counters that would have settled it were
     * behind a flag this run does not use.  Cumulative, and with the last
     * position the port saw, because "42 moves" and "42 moves, all at the same
     * place" are also different faults.
     */
    shim_printf("[input t=%u] %u keys, %u pointer moves, %u buttons; last at "
              "%d,%d buttons 0x%x\n",
              (unsigned)ag_millis(), (unsigned)s_ev_keys,
              (unsigned)s_ev_ptr_move, (unsigned)s_ev_ptr_btn,
              s_mouse_x, s_mouse_y, (unsigned)s_mouse_buttons);
}

static void write_ppm(const char *path)
{
    if (!s_have_gfx || s_gfx.fb == NULL) {
        return;
    }

    ag_handle_t f = ag_open(path, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (f < 0) {
        ag_printf("screenshot: cannot write %s\n", path);
        return;
    }

    char header[64];
    const int n = snprintf(header, sizeof(header), "P6\n%u %u\n255\n",
                           (unsigned)s_gfx.width, (unsigned)s_gfx.height);
    ag_write(f, header, (size_t)n);

    /* A row at a time; the whole picture in 24-bit would be another megabyte
     * of arena at the moment the game is using all of it. */
    unsigned char *row = (unsigned char *)ag_malloc((size_t)s_gfx.width * 3u);
    if (row == NULL) {
        ag_close(f);
        return;
    }
    for (unsigned y = 0; y < s_gfx.height; ++y) {
        const Uint16 *src =
            (const Uint16 *)((const unsigned char *)s_gfx.fb +
                             (size_t)y * s_gfx.stride);
        for (unsigned x = 0; x < s_gfx.width; ++x) {
            const Uint16 p = src[x];
            const unsigned r5 = (p >> 11) & 0x1Fu;
            const unsigned g6 = (p >> 5) & 0x3Fu;
            const unsigned b5 = p & 0x1Fu;
            /* Replicate the high bits downwards, so full white comes back as
             * 255 rather than 248. */
            row[x * 3 + 0] = (unsigned char)((r5 << 3) | (r5 >> 2));
            row[x * 3 + 1] = (unsigned char)((g6 << 2) | (g6 >> 4));
            row[x * 3 + 2] = (unsigned char)((b5 << 3) | (b5 >> 2));
        }
        ag_write(f, row, (size_t)s_gfx.width * 3u);
    }
    ag_free(row);
    ag_close(f);
    ag_printf("screenshot: %s (%ux%u) after %d frame(s)\n", path,
              (unsigned)s_gfx.width, (unsigned)s_gfx.height, s_present_count);
}

static SDL_Surface *make_surface(int w, int h, int depth, Uint32 format)
{
    if (w <= 0 || h <= 0) {
        return NULL;
    }

    const int bytes = depth / 8;
    int pitch = (w * bytes + 3) & ~3;

    SurfaceBlock *block = (SurfaceBlock *)ag_malloc(sizeof(SurfaceBlock));
    if (block == NULL) {
        return NULL;
    }
    memset(block, 0, sizeof(*block));

    /*
     * The one surface that does not get memory of its own: the 16-bit one the
     * engine presents, when it is exactly the shape of the panel.  Then it is
     * the panel, and a frame costs a flush rather than a flush and a 614 KB
     * copy through PSRAM.
     *
     * This is only reached because the engine asks the texture what format it
     * is and builds its surface to match - see SDL_QueryTexture below, which
     * answers RGB565 whatever it was asked for.  Without that question there
     * would be a 32-bit surface here and a conversion every frame.
     */
    void *pixels = NULL;
    int owns = 1;
    if (depth == 16 && s_have_gfx && !s_fb_lent && s_gfx.fb != NULL &&
        s_gfx.fmt == AG_PIX_RGB565) {
        /*
         * The panel does not have to be the size of the picture.
         *
         * Fallout 2 draws 640x480 and will not do otherwise: its interface is
         * bitmaps laid out for that, and asking the engine for less gets a
         * refusal from artInit onwards.  A CYD is 320x240.  So the reduction
         * happens on the way out - the engine keeps its 640x480 eight-bit
         * surface, and the blit that turns it into RGB565 takes every second
         * pixel of every second row.
         *
         * The surface is created at the panel's size rather than the engine's,
         * which is what keeps it *being* the panel: a frame is still a flush
         * and not a 614 KB copy.  The engine never reads this surface's width
         * or height - only its pixels and its pitch, in renderPresent - so the
         * difference stays inside this file.  Checked against svga.cc: the only
         * other uses are as a blit destination and SDL_FreeSurface.
         *
         * Whole powers of two only.  Anything else needs a resampler, and a
         * resampler is a frame's worth of arithmetic on a part with none to
         * spare.
         */
        for (int shift = 0; shift <= 2; ++shift) {
            if ((int)(s_gfx.width << shift) == w &&
                (int)(s_gfx.height << shift) == h) {
                w = (int)s_gfx.width;
                h = (int)s_gfx.height;
                pitch = (int)s_gfx.stride;
                pixels = s_gfx.fb;
                owns = 0;
                s_fb_lent = true;
                s_panel_shift = shift;
                if (shift != 0) {
                    ag_printf("SDL: panel %ux%u, picture %dx%d, "
                              "reducing by %d\n",
                              s_gfx.width, s_gfx.height,
                              (int)(s_gfx.width << shift),
                              (int)(s_gfx.height << shift), 1 << shift);
                }
                break;
            }
        }
    }

    if (pixels == NULL) {
        pixels = ag_malloc((size_t)pitch * (size_t)h);
        if (pixels == NULL) {
            ag_free(block);
            return NULL;
        }
    }
    memset(pixels, 0, (size_t)pitch * (size_t)h);

    block->palette.ncolors = 256;
    block->palette.colors = block->colors;

    block->format.format = format;
    block->format.BitsPerPixel = (Uint8)depth;
    block->format.BytesPerPixel = (Uint8)bytes;
    block->format.palette = (depth == 8) ? &block->palette : NULL;
    if (depth == 32) {
        block->format.Rmask = 0x00FF0000u;
        block->format.Gmask = 0x0000FF00u;
        block->format.Bmask = 0x000000FFu;
    }

    if (depth == 16) {
        block->format.Rmask = 0xF800u;
        block->format.Gmask = 0x07E0u;
        block->format.Bmask = 0x001Fu;
    }

    block->surface.format = &block->format;
    block->surface.w = w;
    block->surface.h = h;
    block->surface.pitch = pitch;
    block->surface.pixels = pixels;
    block->surface.owns_pixels = owns;
    return &block->surface;
}

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int width, int height,
                                  int depth, Uint32 Rmask, Uint32 Gmask,
                                  Uint32 Bmask, Uint32 Amask)
{
    (void)flags;
    (void)Rmask;
    (void)Gmask;
    (void)Bmask;
    (void)Amask;
    return make_surface(width, height, depth,
                        depth == 8 ? SDL_PIXELFORMAT_INDEX8
                                   : SDL_PIXELFORMAT_RGB888);
}

SDL_Surface *SDL_CreateRGBSurfaceWithFormat(Uint32 flags, int width, int height,
                                            int depth, Uint32 format)
{
    (void)flags;
    return make_surface(width, height, depth, format);
}

void SDL_FreeSurface(SDL_Surface *surface)
{
    if (surface == NULL) {
        return;
    }
    if (surface->owns_pixels) {
        ag_free(surface->pixels);
    }
    ag_free(surface);
}

/* Nothing here moves a surface's pixels behind the caller's back, so locking
 * is bookkeeping.  It is still counted, because an unbalanced pair means the
 * caller believes something this port does not do. */
int SDL_LockSurface(SDL_Surface *surface)
{
    if (surface == NULL) {
        return -1;
    }
    surface->locked++;
    return 0;
}

void SDL_UnlockSurface(SDL_Surface *surface)
{
    if (surface != NULL && surface->locked > 0) {
        surface->locked--;
    }
}

int SDL_SetSurfacePalette(SDL_Surface *surface, SDL_Palette *palette)
{
    if (surface == NULL || surface->format == NULL) {
        return -1;
    }
    surface->format->palette = palette;
    return 0;
}

int SDL_SetPaletteColors(SDL_Palette *palette, const SDL_Color *colors,
                         int firstcolor, int ncolors)
{
    if (palette == NULL || colors == NULL) {
        return -1;
    }
    if (firstcolor < 0 || ncolors < 0 ||
        firstcolor + ncolors > palette->ncolors) {
        return -1;
    }
    for (int i = 0; i < ncolors; ++i) {
        palette->colors[firstcolor + i] = colors[i];
    }
    ++s_palette_gen;
    return 0;
}

/*
 * The palette as RGB565, built once and kept until the palette changes.
 *
 * It used to be rebuilt inside every blit - 256 conversions each time.  That
 * is nothing against a full screen and everything against the mouse cursor,
 * which is a few hundred pixels and is blitted every single frame: the table
 * cost more than the picture.  A counter bumped by SDL_SetPaletteColors says
 * when it is stale, so a fade still gets a fresh one per step, which is what a
 * fade is.
 */
/*
 * The palette again, split into channels, on the same schedule as the colour
 * table.
 *
 * Only the reducing blit wants these, and only so that four pixels can be
 * averaged without unpacking four RGB565 words to do it.  Five, six and five
 * bits, ready to be summed four at a time and shifted back down.
 */
static Uint8 s_pal_r[256];
static Uint8 s_pal_g[256];
static Uint8 s_pal_b[256];

static const Uint16 *palette_lut(const SDL_Palette *pal)
{
    static Uint16 lut[256];
    static uint32_t built_gen;
    static const SDL_Palette *built_for;

    if (built_gen != s_palette_gen || built_for != pal) {
        const int n = pal->ncolors < 256 ? pal->ncolors : 256;
        for (int i = 0; i < n; ++i) {
            const SDL_Color c = pal->colors[i];
            s_pal_r[i] = (Uint8)(c.r >> 3);
            s_pal_g[i] = (Uint8)(c.g >> 2);
            s_pal_b[i] = (Uint8)(c.b >> 3);
            lut[i] = (Uint16)(((c.r & 0xF8u) << 8) | ((c.g & 0xFCu) << 3) |
                              (c.b >> 3));
        }
        built_gen = s_palette_gen;
        built_for = pal;
    }
    return lut;
}

/*
 * The one blit the engine performs: the 8-bit game surface through its palette
 * into the 16-bit surface that gets presented - which, when it is the shape of
 * the panel, is the panel.  SDL would pick a converter out of a table; there
 * is one case here, and anything else says so rather than producing a picture
 * that is subtly wrong.
 *
 * The palette is turned into a table of RGB565 once per blit rather than per
 * pixel: 256 conversions instead of 307200.  Rebuilding it every time rather
 * than caching it is deliberate - the game fades the palette to black for
 * every transition, so a cache would be invalidated on most frames anyway, and
 * a stale one shows the wrong colours rather than failing.
 */
int SDL_UpperBlit(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst,
                  SDL_Rect *dstrect)
{
    if (src == NULL || dst == NULL || src->format == NULL ||
        dst->format == NULL) {
        return -1;
    }
    if (src->format->BitsPerPixel != 8 || dst->format->BitsPerPixel != 16) {
        ag_panic("SDL blit: only 8-bit paletted to RGB565 is implemented");
    }

    const SDL_Palette *pal = src->format->palette;
    if (pal == NULL) {
        return -1;
    }

    const Uint16 *lut = palette_lut(pal);

    int sx = srcrect ? srcrect->x : 0;
    int sy = srcrect ? srcrect->y : 0;
    int w = srcrect ? srcrect->w : src->w;
    int h = srcrect ? srcrect->h : src->h;
    int dx = dstrect ? dstrect->x : 0;
    int dy = dstrect ? dstrect->y : 0;

    if (sx < 0 || sy < 0 || w < 0 || h < 0) {
        return -1;
    }
    if (sx + w > src->w) {
        w = src->w - sx;
    }
    if (sy + h > src->h) {
        h = src->h - sy;
    }
    if (w <= 0 || h <= 0) {
        return 0;
    }

    /* Timed unconditionally.  It used to be timed only under -fps, and the
     * readout on the glass needs the same number when nothing is printing. */
    const ag_time_t blit_t0 = ag_micros();

    /*
     * The reduction, when the panel is smaller than the picture.  The rectangle
     * the caller gave is in the picture's coordinates, so it is divided down
     * here and the destination is walked rather than the source: that way an
     * odd left edge cannot drop the column next to it, and a rectangle one
     * pixel wide still lands somewhere.  With shift zero this is the same loop
     * as before, one shift and one add per pixel more.
     */
    const int to_panel = (dst->pixels == s_gfx.fb);
    /* One to one is the reduction turned off and an offset turned on. */
    const int shift = (to_panel && !s_one_to_one) ? s_panel_shift : 0;
    const int panx = (to_panel && s_one_to_one) ? s_pan_x : 0;
    const int pany = (to_panel && s_one_to_one) ? s_pan_y : 0;
    const int step = 1 << shift;

    dx -= panx;
    dy -= pany;

    int ddx = dx >> shift;
    int ddy = dy >> shift;
    int ddw = ((dx + w + step - 1) >> shift) - ddx;
    int ddh = ((dy + h + step - 1) >> shift) - ddy;

    /* The destination clamp, in the destination's own units - which is why it
     * is here and not up with the source's. */
    if (ddx < 0) {
        ddw += ddx;
        ddx = 0;
    }
    if (ddy < 0) {
        ddh += ddy;
        ddy = 0;
    }
    if (ddx + ddw > dst->w) {
        ddw = dst->w - ddx;
    }
    if (ddy + ddh > dst->h) {
        ddh = dst->h - ddy;
    }
    if (ddw <= 0 || ddh <= 0) {
        return 0;
    }


    /*
     * What actually came out different, in destination coordinates.  Every
     * loop below stores through CHANGED(), which writes only when the pixel is
     * not already what it should be and records where that happened.
     *
     * The reason is a measurement.  Standing still in the Temple of Trials the
     * board spent 8.2 ms a frame converting the whole 640x480 and another
     * 8.1 ms hashing the result at the far end - 16 of every 47 ms - on a
     * picture that was identical to the last one.  The engine asks for it:
     * Fallout cycles its palette, and every cycle calls
     * directDrawSetPaletteInRange, which re-blits the entire screen.  The
     * fastest of those cycles runs at thirty hertz and animates one entry -
     * the red light on the interface bar.  So the whole screen was being
     * redrawn thirty times a second for one blinking lamp.
     *
     * Comparing before storing is close to free where it counts: the cache
     * line has to be fetched to be written anyway, so the load is already
     * paid for, and a pixel that does not change saves the store.  What it
     * buys is that the damage rectangle handed to the panel is the truth
     * rather than "the caller touched this area".
     */
    int cx0 = ddw, cx1 = -1, cy0 = ddh, cy1 = -1;
#define CHANGED(dp, xi, val)                                                       do {                                                                               const Uint16 v_ = (Uint16)(val);                                               if ((dp)[xi] != v_) {                                                              (dp)[xi] = v_;                                                                 if ((xi) < rx0) rx0 = (xi);                                                    if ((xi) > rx1) rx1 = (xi);                                                }                                                                          } while (0)
#define ROW_DONE(yi)                                                               do {                                                                               if (rx1 >= rx0) {                                                                  if (rx0 < cx0) cx0 = rx0;                                                      if (rx1 > cx1) cx1 = rx1;                                                      if ((yi) < cy0) cy0 = (yi);                                                    if ((yi) > cy1) cy1 = (yi);                                                }                                                                          } while (0)

    if (shift == 0) {
        /* The ordinary case, and the one that runs three hundred thousand
         * times a frame: no arithmetic in the inner loop that was not there
         * before the panel could be smaller. */
        /*
         * Where the source starts, after the destination was clamped.
         *
         * The clamp above pushes ddx and ddy up to zero when the destination
         * origin is negative and takes the difference off the width; it does
         * not move the source, and this path used to read from sx and sy as
         * written. That is harmless while dx and dy are non-negative, which
         * they always were - until one to one made dx = -s_pan_x. Then the
         * window moved, the destination was clamped straight back to the left
         * edge, and the same pixels as before were copied: the panel did not
         * change, the comparison found nothing to write, and the report said
         * `wrote 32767,32767..-1,-1` - an empty rectangle - after every arrow.
         *
         * The two scaled paths below never had this: they compute the source
         * column as sx + (((ddx + x) << shift) - dx), which carries the clamp
         * with it. This is the same expression with shift zero, hoisted out of
         * the loop because it does not depend on x or y.
         */
        const int soffx = sx + (ddx - dx);
        const int soffy = sy + (ddy - dy);
        for (int y = 0; y < ddh; ++y) {
            const Uint8 *s = (const Uint8 *)src->pixels +
                             (size_t)(soffy + y) * src->pitch + soffx;
            Uint16 *d = (Uint16 *)((Uint8 *)dst->pixels +
                                   (size_t)(ddy + y) * dst->pitch) + ddx;
            int rx0 = ddw, rx1 = -1;
            for (int x = 0; x < ddw; ++x) {
                CHANGED(d, x, lut[s[x]]);
            }
            ROW_DONE(y);
        }
    } else if (!s_scale_sharp) {
        /*
         * The reduction, and it is the second most expensive thing in a frame:
         * measured on the board at 10.5 ms against 34 ms of engine and 7 ms of
         * putting the picture on the wire.  Most of that is memory - 300 KB
         * read and 150 KB written per frame, and the read is strided so the
         * cache lines are half wasted - but the arithmetic was not helping.
         *
         * The source column and both of its bounds checks are the same
         * expression for every row and depend on x only through a shift, so
         * the range of x that lands inside the source is computed once here
         * and the inner loop walks a pointer.  Which x are in range:
         *
         *   sxx(x) = base + (x << shift),  base = sx + (ddx << shift) - dx
         *   sxx >= 0        =>  x >= ceil(-base / step)
         *   sxx <= src->w-1 =>  x <  ceil((src->w - base) / step)
         */
        /* Whether this pass covers a whole panel row; see the flag below. */
        const bool full_width = (ddx == 0 && ddw >= (int)s_gfx.width);
        const int base = sx + ((ddx << shift) - dx);
        int       x0 = (base < 0) ? (((-base) + step - 1) >> shift) : 0;
        int       x1 = ddw;
        {
            const int room = src->w - base;
            const int lim = (room <= 0) ? 0 : ((room + step - 1) >> shift);
            if (lim < x1) {
                x1 = lim;
            }
        }
        for (int y = 0; y < ddh; ++y) {
            const int syy = sy + (((ddy + y) << shift) - dy);
            if (syy < 0 || syy >= src->h) {
                continue;
            }
            const unsigned row = (unsigned)(ddy + y);

            /* ARGON: a palette-only pass has nothing to do in a row that
             * holds no animated colour.  See the note by s_row_cycled. */
            if (s_palette_only && row < AG_ROWS_MAX && s_row_cycled_ready
                    && s_row_cycled[row] == 0u) {
                s_cycle_rows_skipped++;
                continue;
            }
            if (s_palette_only) {
                s_cycle_rows_done++;
            }

            const Uint8 *sp = (const Uint8 *)src->pixels +
                              (size_t)syy * src->pitch + base + (x0 << shift);
            Uint16 *dp = (Uint16 *)((Uint8 *)dst->pixels +
                                    (size_t)(ddy + y) * dst->pitch) + ddx;
            int rx0 = ddw, rx1 = -1;
            unsigned cycled = 0;

            /*
             * ARGON: how long one row took.
             *
             * A row is a few hundred pixels - sixty microseconds of work.  A
             * row that takes twenty milliseconds did not do more work; the
             * thread was taken off the processor in the middle of it.  That
             * is the difference between "this code is slow" and "something
             * else is running", and nothing else here can tell them apart.
             * Counting them also gives a rate, which can be held against the
             * rate of every other periodic thing on the board.
             */
            const uint32_t row_t0 = (uint32_t)ag_micros();
            for (int x = x0; x < x1; ++x, sp += step) {
                const Uint8 idx = *sp;

                cycled |= (unsigned)(idx >= AG_CYCLE_FIRST);
                CHANGED(dp, x, lut[idx]);
            }
            /*
             * A pass that did not cover the whole width saw only part of the
             * row, so it may only ADD the flag - clearing it on the strength
             * of a fragment would leave the water in the rest of the row
             * frozen, which is a wrong picture rather than a slow one.  Only
             * a full-width pass knows enough to clear.
             */
            {
                const uint32_t row_us = (uint32_t)ag_micros() - row_t0;

                if (row_us >= 3000u) {
                    s_preempt_n++;
                    s_preempt_us += row_us;
                    if (row_us > s_preempt_worst_us) {
                        s_preempt_worst_us = row_us;
                    }
                }
            }

            if (row < AG_ROWS_MAX && !s_palette_only) {
                if (full_width) {
                    s_row_cycled[row] = (unsigned char)cycled;
                } else if (cycled != 0u) {
                    s_row_cycled[row] = 1u;
                }
            }
            ROW_DONE(y);
        }
    } else {
        /* Two rows at a time, because the choice is made across a 2x2 block.
         * Larger shifts still look at a 2x2 - sixteen samples for a quarter
         * panel would cost more than the picture is worth. */
        for (int y = 0; y < ddh; ++y) {
            const int syy = sy + (((ddy + y) << shift) - dy);
            if (syy < 0 || syy >= src->h) {
                continue;
            }
            const int syy2 = (syy + 1 < src->h) ? (syy + 1) : syy;
            const Uint8 *r0 =
                (const Uint8 *)src->pixels + (size_t)syy * src->pitch;
            const Uint8 *r1 =
                (const Uint8 *)src->pixels + (size_t)syy2 * src->pitch;
            Uint16 *d = (Uint16 *)((Uint8 *)dst->pixels +
                                   (size_t)(ddy + y) * dst->pitch) + ddx;
            int rx0 = ddw, rx1 = -1;
            for (int x = 0; x < ddw; ++x) {
                const int sxx = sx + (((ddx + x) << shift) - dx);
                if (sxx < 0 || sxx >= src->w) {
                    continue;
                }
                const int sxx2 = (sxx + 1 < src->w) ? (sxx + 1) : sxx;

                const Uint8 p0 = r0[sxx];
                const Uint8 p1 = r0[sxx2];
                const Uint8 p2 = r1[sxx];
                const Uint8 p3 = r1[sxx2];

                const unsigned rr = ((unsigned)s_pal_r[p0] + s_pal_r[p1] +
                                     s_pal_r[p2] + s_pal_r[p3]) >> 2;
                const unsigned gg = ((unsigned)s_pal_g[p0] + s_pal_g[p1] +
                                     s_pal_g[p2] + s_pal_g[p3]) >> 2;
                const unsigned bb = ((unsigned)s_pal_b[p0] + s_pal_b[p1] +
                                     s_pal_b[p2] + s_pal_b[p3]) >> 2;

                CHANGED(d, x, (rr << 11) | (gg << 5) | bb);
            }
            ROW_DONE(y);
        }
    }
#undef CHANGED
#undef ROW_DONE

    /* Once a pass has covered the whole panel, the map is complete and may
     * be trusted to skip rows. */
    if (!s_palette_only && ddx == 0 && ddy == 0
            && ddw >= (int)s_gfx.width && ddh >= (int)s_gfx.height) {
        s_row_cycled_ready = true;
    }

    {
        const uint32_t took = (uint32_t)(ag_micros() - blit_t0);
        s_blit_frame_us += took;
        if (s_fps_ms != 0u) {
            s_blit_us += (uint64_t)took;
            s_blit_px += (uint64_t)ddw * (uint64_t)ddh;
        }
        /*
         * ARGON: the worst single conversion, with the area it covered.
         *
         * The average says 10 ms for a whole screen and the worst frame says
         * 144, and the two cannot both be about the same work.  Either the
         * slow one covered far more pixels - in which case the area says so -
         * or it covered the same pixels far more slowly, which would mean it
         * was waiting for memory rather than doing arithmetic.  Without the
         * area the number cannot tell those apart, and that is exactly the
         * question.
         */
        if (took > s_blit_worst_us) {
            s_blit_worst_us = took;
            s_blit_worst_px = (uint32_t)ddw * (uint32_t)ddh;
        }
    }

    /* Remember what changed, so that the present after it can send that and
     * not the whole screen.  Only when this surface is the panel: a blit into
     * a surface of our own is copied wholesale later anyway.  In panel
     * coordinates, which is what the flush wants. */
    if (dst->pixels == s_gfx.fb) {
        if (cx1 >= cx0 && cy1 >= cy0) {
            dirty_add(ddx + cx0, ddy + cy0, cx1 - cx0 + 1, cy1 - cy0 + 1);
        }
        /*
         * Only the whole picture is worth remembering.
         *
         * The engine composes into one 640x480 surface and blits that, but it
         * also blits individual windows straight at the panel - a dialogue
         * box, a message line - and whichever came last used to be what a
         * moved window redrew.  So moving the view redrew a dialogue box at a
         * new offset and left everything around it where it was: "a little
         * piece of the screen moves and the rest stands still", reported
         * exactly so, twice.
         *
         * The picture is the panel scaled back up by the reduction: 320x240
         * and a shift of one is 640x480.  Anything smaller is a part, not the
         * whole, and a part cannot repaint the view.
         */
        if (src->w >= (int)(s_gfx.width << s_panel_shift) &&
            src->h >= (int)(s_gfx.height << s_panel_shift)) {
            s_panel_src = src;
            s_panel_dst = dst;
        }
    }
    return 0;
}

/*
 * Draw the whole picture again, at whatever the view is now.
 *
 * Called when F8 or an arrow key moves the window.  It goes through the same
 * blit rather than repeating it, so there is one place where the reduction and
 * the offset are decided.
 */
static void panel_repaint(void)
{
    if (s_panel_src == NULL || s_panel_dst == NULL) {
        return;
    }

    SDL_Rect srcrect = { 0, 0, s_panel_src->w, s_panel_src->h };
    SDL_Rect dstrect = { 0, 0, 0, 0 };

    /*
     * What the blit itself thinks changed, kept for the report beside the key.
     * "The arrows do nothing" has one more meaning than the three already
     * listed: the window moved, the whole panel was marked, and the pixels
     * written turned out to be the same ones - which would say the source is
     * not the picture at all.  Cleared first so the box below is this blit's
     * own and not last frame's; nothing is lost, because the whole panel is
     * marked immediately after.
     */
    dirty_none();
    SDL_UpperBlit(s_panel_src, &srcrect, s_panel_dst, &dstrect);
    s_pan_bx0 = s_dirty_x0;
    s_pan_by0 = s_dirty_y0;
    s_pan_bx1 = s_dirty_x1;
    s_pan_by1 = s_dirty_y1;
    s_pan_sw = s_panel_src->w;
    s_pan_sh = s_panel_src->h;

    /*
     * And send it, here, rather than leaving it for the next frame.
     *
     * The blit above only fills the panel in memory; what puts it on the far
     * end is the flush, and the flush runs on the engine's frame.  Moving the
     * window is precisely the case where the engine is not drawing - it is
     * sitting on a menu or a dialogue box, which is why somebody wanted a
     * closer look in the first place - so the new view sat in memory and the
     * only thing that reached the glass was whatever small rectangle some
     * other part of the game happened to dirty.  Reported exactly so: "one
     * block moves and blinks and the picture as a whole stands still".
     *
     * The whole panel, because the whole panel changed: every pixel of it now
     * comes from somewhere else in the picture.
     *
     * Marked, not flushed.  This runs on the event pump, and flushing from
     * there reaches into a display device the wire task is already holding -
     * the first attempt did exactly that and the key stopped being handled at
     * all: not even the line below F8 was printed, because the flush above it
     * never came back.  The present on the engine's own thread is a few
     * milliseconds away and it sends whatever is marked, which is the whole
     * panel.
     */
    dirty_add(0, 0, (int)s_gfx.width, (int)s_gfx.height);
}

/* ---- time ---------------------------------------------------------------- */

Uint32 SDL_GetTicks(void) { return (Uint32)ag_millis(); }

/*
 * ARGON: and a delay that cannot be asked to sleep for ever.
 *
 * The frame limiter's unsigned underflow (see fps_limiter.cc) asked for
 * 4294967295 ms and got it - the game thread simply stopped, seven minutes
 * into a session, with everything else still running.  That bug is fixed at
 * the source, but a sleep of more than a few seconds is not a thing this game
 * ever legitimately wants, and a program that has stopped for ever is the one
 * failure the player cannot tell from a crash.  So it is capped, and it says
 * so once.
 */
void SDL_Delay(Uint32 ms)
{
    if (ms > 5000u) {
        static int said;

        if (!said) {
            said = 1;
            ag_printf("SDL: a delay of %u ms was asked for; sleeping 5000 "
                      "instead\n", (unsigned)ms);
        }
        ms = 5000u;
    }

    ag_delay(ms);
}

/* ---- string helpers the engine borrows ----------------------------------- */

void *SDL_malloc(size_t size) { return ag_malloc(size); }

void SDL_free(void *mem) { ag_free(mem); }

size_t SDL_strlen(const char *str) { return strlen(str); }

char *SDL_strdup(const char *str)
{
    if (str == NULL) {
        return NULL;
    }
    size_t n = strlen(str) + 1;
    char *out = (char *)ag_malloc(n);
    if (out != NULL) {
        memcpy(out, str, n);
    }
    return out;
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int upper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

int SDL_strcasecmp(const char *a, const char *b)
{
    while (*a != '\0' && lower((unsigned char)*a) == lower((unsigned char)*b)) {
        ++a;
        ++b;
    }
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

int SDL_strncasecmp(const char *a, const char *b, size_t len)
{
    while (len > 0 && *a != '\0' &&
           lower((unsigned char)*a) == lower((unsigned char)*b)) {
        ++a;
        ++b;
        --len;
    }
    if (len == 0) {
        return 0;
    }
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

char *SDL_strlwr(char *str)
{
    for (char *p = str; p != NULL && *p != '\0'; ++p) {
        *p = (char)lower((unsigned char)*p);
    }
    return str;
}

char *SDL_strupr(char *str)
{
    for (char *p = str; p != NULL && *p != '\0'; ++p) {
        *p = (char)upper((unsigned char)*p);
    }
    return str;
}

char *SDL_itoa(int value, char *str, int radix)
{
    static const char k_digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char buf[36];
    int n = 0;
    unsigned int v;
    int negative = 0;

    if (radix < 2 || radix > 36 || str == NULL) {
        return str;
    }
    if (value < 0 && radix == 10) {
        negative = 1;
        v = (unsigned int)(-(long)value);
    } else {
        v = (unsigned int)value;
    }
    do {
        buf[n++] = k_digits[v % (unsigned int)radix];
        v /= (unsigned int)radix;
    } while (v != 0);
    if (negative) {
        buf[n++] = '-';
    }
    for (int i = 0; i < n; ++i) {
        str[i] = buf[n - 1 - i];
    }
    str[n] = '\0';
    return str;
}

int SDL_snprintf(char *text, size_t maxlen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(text, maxlen, fmt, ap);
    va_end(ap);
    return n;
}

void SDL_LogMessageV(int category, int priority, const char *fmt, va_list ap)
{
    (void)category;
    (void)priority;
    char line[256];
    vsnprintf(line, sizeof(line), fmt, ap);
    ag_print(line);
    ag_print("\n");
    /* And to the session log, which is the only record a board keeps. */
    log_line(line);
    log_line("\n");
}

int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title,
                             const char *message, SDL_Window *window)
{
    (void)flags;
    (void)window;
    ag_printf("%s: %s\n", title ? title : "message",
              message ? message : "(none)");
    return 0;
}

/* ---- video --------------------------------------------------------------- */

/*
 * There is no window system, so there is no window: creating one takes the
 * display away from the text console and that is all.  The pointer handed back
 * is an address inside this file, because the engine only ever stores it and
 * passes it back.
 *
 * The renderer and the texture are the same kind of nothing.  What makes the
 * picture appear is the arrangement around them: the engine asks the texture
 * its format, is told RGB565, builds a surface of that format, and that
 * surface turns out to be the framebuffer itself.  By the time RenderPresent
 * is called the pixels are already where the panel will read them, and all
 * that is left is to say which part changed.
 */
static char s_window_token;
static char s_renderer_token;

struct SDL_Texture {
    int w, h;
};

static SDL_Texture s_texture;

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h,
                             Uint32 flags)
{
    (void)title;
    (void)x;
    (void)y;
    (void)flags;

    if (!s_have_gfx) {
        if (ag_gfx_acquire(&s_gfx) != AG_OK) {
            ag_print("SDL: cannot take the display\n");
            return NULL;
        }
        s_have_gfx = true;
        s_gfx_since_ms = (uint32_t)ag_millis();
        input_devices_open();
        ag_gfx_clear(0);
        ag_gfx_flush(0, 0, s_gfx.width, s_gfx.height);
        dirty_none();
        ag_printf("SDL: display %ux%u %s, %u bytes per row\n",
                  (unsigned)s_gfx.width, (unsigned)s_gfx.height,
                  s_gfx.fb != NULL ? "with a framebuffer" : "by present()",
                  (unsigned)s_gfx.stride);
    }

    if ((int)s_gfx.width != w || (int)s_gfx.height != h) {
        /* Not fatal: the presenting surface then gets memory of its own and
         * RenderPresent copies the overlap.  Worth saying out loud, because
         * the interface is bitmaps laid out for 640x480 and anything else
         * loses part of it. */
        ag_printf("SDL: game is %dx%d, panel is %ux%u; set [display] "
                  "width/height in C:\\SYSTEM.CFG to match\n",
                  w, h, (unsigned)s_gfx.width, (unsigned)s_gfx.height);
    }
    return (SDL_Window *)&s_window_token;
}

void SDL_DestroyWindow(SDL_Window *w)
{
    (void)w;
    if (s_have_gfx) {
        ag_gfx_release();
        s_have_gfx = false;
        s_fb_lent = false;
    }
}

void SDL_SetWindowTitle(SDL_Window *w, const char *title)
{
    (void)w;
    (void)title;
}

SDL_Renderer *SDL_CreateRenderer(SDL_Window *w, int index, Uint32 flags)
{
    (void)w;
    (void)index;
    (void)flags;
    return (SDL_Renderer *)&s_renderer_token;
}

void SDL_DestroyRenderer(SDL_Renderer *r) { (void)r; }

int SDL_RenderSetLogicalSize(SDL_Renderer *r, int w, int h)
{
    (void)r;
    (void)w;
    (void)h;
    return 0;
}

/* The framebuffer is fully painted every frame by the engine; clearing it here
 * would be one wasted pass over 600 KB. */
int SDL_RenderClear(SDL_Renderer *r)
{
    (void)r;
    return 0;
}

int SDL_RenderCopy(SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *s,
                   const SDL_Rect *d)
{
    (void)r;
    (void)t;
    (void)s;
    (void)d;
    return 0;
}

/*
 * Three ways a frame reaches the panel, and which one applies is decided by
 * what the display gave us rather than by a build option.
 *
 *   the surface is the framebuffer  - a flush, and nothing else;
 *   there is a framebuffer, elsewhere - SDL_UpdateTexture copied into it, and
 *                                       this flushes;
 *   there is no framebuffer at all    - a panel that is scanned out by its own
 *                                       driver, so hand it the rectangle.
 *
 * The third is the case a real panel takes; QEMU's soft display is the first.
 */

/* ---- the readout ---------------------------------------------------------
 *
 * Everything this port measures goes to the console, and on a board the
 * console is a cable to a machine that is not there.  So the same three
 * numbers can be drawn into the corner of the panel instead: the frame rate,
 * the arena in use, and the free arena, which between them say whether a
 * stutter was the picture or the memory.
 *
 * Three by five pixels a digit, because a font is the one thing this port does
 * not have and Fallout's own is inside the game's data.  Drawn straight into
 * the framebuffer after the flush, so the next frame paints over it and it is
 * redrawn - which costs a few hundred pixels and no state.
 */

void sdl_shim_hud(int on)
{
    s_hud = on ? 1 : 0;
}

/* 0-9 then a space, five rows of three bits, high bit leftmost. */
static const unsigned char k_hud_font[11][5] = {
    { 7, 5, 5, 5, 7 }, /* 0 */
    { 2, 6, 2, 2, 7 }, /* 1 */
    { 7, 1, 7, 4, 7 }, /* 2 */
    { 7, 1, 7, 1, 7 }, /* 3 */
    { 5, 5, 7, 1, 1 }, /* 4 */
    { 7, 4, 7, 1, 7 }, /* 5 */
    { 7, 4, 7, 5, 7 }, /* 6 */
    { 7, 1, 1, 1, 1 }, /* 7 */
    { 7, 5, 7, 5, 7 }, /* 8 */
    { 7, 5, 7, 1, 7 }, /* 9 */
    { 0, 0, 0, 0, 0 }, /* space */
};

/*
 * And the letters, because a column of bare numbers is a column somebody has
 * to remember the order of.  Three pixels wide is not much for an alphabet and
 * it does not need to be one: these are the only letters the readout uses, and
 * they are read in a known word rather than one at a time.
 */
static const char k_hud_letters[] = "FPSMINLOEGWRBTKAV%/.";
static const unsigned char k_hud_letter_font[][5] = {
    { 7, 4, 6, 4, 4 }, /* F */
    { 7, 5, 7, 4, 4 }, /* P */
    { 3, 4, 2, 1, 6 }, /* S */
    { 5, 7, 7, 5, 5 }, /* M */
    { 7, 2, 2, 2, 7 }, /* I */
    { 5, 7, 7, 7, 5 }, /* N */
    { 4, 4, 4, 4, 7 }, /* L */
    { 7, 5, 5, 5, 7 }, /* O */
    { 7, 4, 7, 4, 7 }, /* E */
    { 7, 4, 5, 5, 7 }, /* G */
    { 5, 5, 7, 7, 5 }, /* W */
    { 7, 5, 7, 6, 5 }, /* R */
    { 7, 5, 6, 5, 7 }, /* B */
    { 7, 2, 2, 2, 2 }, /* T */
    { 5, 5, 6, 5, 5 }, /* K */
    { 7, 5, 7, 5, 5 }, /* A */
    { 5, 5, 5, 5, 2 }, /* V */
    { 5, 1, 2, 4, 5 }, /* % */
    { 1, 1, 2, 4, 4 }, /* / */
    { 0, 0, 0, 2, 0 }, /* . */
};

static void hud_glyph(int x, int y, int glyph, Uint16 colour)
{
    if (glyph < 0 || glyph > 10) {
        return;
    }
    for (int row = 0; row < 5; ++row) {
        const unsigned bits = k_hud_font[glyph][row];
        const int py = y + row;
        if (py < 0 || py >= (int)s_gfx.height) {
            continue;
        }
        Uint16 *d = (Uint16 *)((Uint8 *)s_gfx.fb + (size_t)py * s_gfx.stride);
        for (int col = 0; col < 3; ++col) {
            if ((bits & (4u >> col)) == 0) {
                continue;
            }
            const int px = x + col;
            if (px >= 0 && px < (int)s_gfx.width) {
                d[px] = colour;
            }
        }
    }
}

/* Right-aligned, so the numbers do not dance as they change width. */
static int hud_number(int x, int y, unsigned value, int digits, Uint16 colour)
{
    for (int i = digits - 1; i >= 0; --i) {
        hud_glyph(x + i * 4, y, (int)(value % 10u), colour);
        value /= 10u;
    }
    return x + digits * 4;
}

/* One character.  Digits come from the digit table, letters from the other
 * one, and anything unknown is a gap - the caller is not going to be told off
 * for a stray character in a readout. */
static void hud_char(int x, int y, char c, Uint16 colour)
{
    if (c >= '0' && c <= '9') {
        hud_glyph(x, y, c - '0', colour);
        return;
    }
    if (c == ' ') {
        return;
    }
    for (int i = 0; k_hud_letters[i] != '\0'; ++i) {
        if (k_hud_letters[i] != c) {
            continue;
        }
        for (int row = 0; row < 5; ++row) {
            const unsigned bits = k_hud_letter_font[i][row];
            const int py = y + row;
            if (py < 0 || py >= (int)s_gfx.height) {
                continue;
            }
            Uint16 *d =
                (Uint16 *)((Uint8 *)s_gfx.fb + (size_t)py * s_gfx.stride);
            for (int col = 0; col < 3; ++col) {
                if ((bits & (4u >> col)) == 0) {
                    continue;
                }
                const int px = x + col;
                if (px >= 0 && px < (int)s_gfx.width) {
                    d[px] = colour;
                }
            }
        }
        return;
    }
}

static int hud_text(int x, int y, const char *text, Uint16 colour)
{
    for (; *text != '\0'; ++text) {
        hud_char(x, y, *text, colour);
        x += 4;
    }
    return x;
}

/* ---------------------------------------------------------------------- */
/* What the readout is actually for                                       */
/* ---------------------------------------------------------------------- */

/*
 * An average is the number that hides the complaint.
 *
 * This port has been measured at thirty frames a second standing still in the
 * Temple of Trials while the person playing it was seeing five, and both were
 * true: the harness stands still with no pointer and never scrolls, and a
 * screen that does not change costs almost nothing on the wire.  What a player
 * sees is the worst frames, not the mean of all of them, and what the worst
 * frames are made of is the whole question - a frame the engine took a fifth
 * of a second to think about and a frame that took a fifth of a second to
 * reach the glass look identical from the chair.
 *
 * So the readout carries three kinds of number:
 *
 *   how many   - the mean rate, the worst frame in the window, and the share
 *                of frames slower than a tenth of a second;
 *   where      - milliseconds a frame in the engine, in the halving, and on
 *                the wire, side by side, which is the separation that cannot
 *                be made by watching;
 *   room       - the arena in use and the largest hole in it, because a
 *                stutter that is really the art cache thrashing shows here
 *                first.
 *
 * Five one-second buckets, rolled forward, so nothing jumps back to zero while
 * it is being read.
 */
#define HUD_BUCKETS 5u
#define HUD_SLOW_US 100000u /* a frame slower than this is one of the bad ones */

typedef struct {
    uint32_t frames;
    uint32_t worst_us;
    /* And what that one frame was made of.  An average over a second says the
     * machine is fine; the frame a person actually noticed is one frame, and
     * "the engine took a fifth of a second" and "the wire took a fifth of a
     * second" are different faults with the same symptom. */
    uint32_t worst_blit_us;
    uint32_t worst_wire_us;
    uint32_t slow;
    uint64_t slow_us;   /* and the time they took; see hud_frame */
    uint64_t sum_us;
    uint64_t blit_us;
    uint64_t wire_us;
    uint64_t wire_px;
} hud_bucket_t;

static hud_bucket_t s_hb[HUD_BUCKETS];
static unsigned     s_hb_at;
static uint32_t     s_hb_next_ms;
static uint32_t     s_hud_last_us;

/* ---------------------------------------------------------------------- */
/* The event log                                                          */
/* ---------------------------------------------------------------------- */

/*
 * A line when something happens, not a line every second.
 *
 * A per-frame trace of a game that runs for ten minutes is a megabyte of
 * numbers nobody reads, and a once-a-second average is the thing that hid the
 * problem in the first place.  What is actually wanted is the shape of the
 * complaint: it was fine, then it dropped, then it settled somewhere worse,
 * and here is what the bad frames were made of.
 *
 * So two kinds of line.  A slow frame - anything over a tenth of a second -
 * with its engine/halving/wire split and, which is the part that matters, the
 * rectangle that was sent: "wire 274 ms, rect 320x240" and "wire 274 ms, rect
 * 320x64" are a saturated link and a driver that is repeating itself, and they
 * are not the same fault.  And a level change, when the mean settles somewhere
 * a third away from where it was, because "it drops when I scroll" is a
 * statement about levels.
 *
 * Rate limited to four lines a second and coalesced in between, so a hundred
 * consecutive bad frames are one line that says a hundred.
 */
static int      s_events;
static uint32_t s_ev_next_ms;
static uint32_t s_ev_slow;      /* slow frames since the last line */
static uint32_t s_ev_worst_us, s_ev_worst_blit_us, s_ev_worst_wire_us;
static int      s_ev_rx, s_ev_ry, s_ev_rw, s_ev_rh;
static unsigned s_ev_level100;  /* the level last reported */

void sdl_shim_events(int on)
{
    s_events = on ? 1 : 0;
}

/*
 * Write down every key and every click, so that a session at the keyboard can
 * be replayed at a board that has no screen attached to it.
 *
 * The recording is taken where the events arrive in the game, not where the
 * host helper sent them from: the helper works in the window's pixels and maps
 * them onto the guest, and the two do not agree closely enough to click a
 * button with.  What is written here is what the engine acted on, which is the
 * only thing worth replaying.  tools/inputplay.py speaks the panel numbers
 * back.
 */
static int s_recinput;

void sdl_shim_recinput(int on)
{
    s_recinput = on ? 1 : 0;
}

static void events_frame(uint32_t frame_us, uint32_t blit_us, uint32_t wire_us,
                         int x, int y, int w, int h)
{
    if (!s_events || frame_us < HUD_SLOW_US) {
        return;
    }
    s_ev_slow++;
    if (frame_us > s_ev_worst_us) {
        s_ev_worst_us = frame_us;
        s_ev_worst_blit_us = blit_us;
        s_ev_worst_wire_us = wire_us;
        s_ev_rx = x;
        s_ev_ry = y;
        s_ev_rw = w;
        s_ev_rh = h;
    }

    const uint32_t now = (uint32_t)ag_millis();
    if (s_ev_next_ms != 0u && now < s_ev_next_ms) {
        return;
    }
    s_ev_next_ms = now + 250u;

    const uint32_t in_frame =
        s_ev_worst_blit_us + s_ev_worst_wire_us;
    const uint32_t eng_us =
        (s_ev_worst_us > in_frame) ? (s_ev_worst_us - in_frame) : 0u;
    shim_printf("[ev t=%u] %u slow, worst %u ms: eng %u blit %u wire %u; "
                "rect %dx%d at %d,%d (%u KB)\n",
                (unsigned)now, (unsigned)s_ev_slow,
                (unsigned)(s_ev_worst_us / 1000u), (unsigned)(eng_us / 1000u),
                (unsigned)(s_ev_worst_blit_us / 1000u),
                (unsigned)(s_ev_worst_wire_us / 1000u),
                s_ev_rw, s_ev_rh, s_ev_rx, s_ev_ry,
                (unsigned)((uint32_t)s_ev_rw * (uint32_t)s_ev_rh * 2u / 1024u));
    s_ev_slow = 0;
    s_ev_worst_us = 0;
    s_ev_worst_blit_us = 0;
    s_ev_worst_wire_us = 0;
}

/* Filled in by hud_tick, drawn by hud_draw, printed by hud_log. */
static unsigned s_hud_fps100;   /* mean frames per second, hundredths */
static unsigned s_hud_min100;   /* the worst frame as a rate, hundredths */
static unsigned s_hud_slow_pct;
static unsigned s_hud_slow_frames;
static unsigned s_hud_eng_ms, s_hud_blit_ms, s_hud_wire_ms, s_hud_wire_kbs;
static unsigned s_hud_worst_ms, s_hud_worst_eng_ms, s_hud_worst_wire_ms;
static uint32_t s_io_misses_last;
static unsigned s_hud_io_bad;
static unsigned s_hud_used_kb, s_hud_hole_kb;

/*
 * ARGON: the playability metric Maxim asked for on 13 September 2026.
 *
 * m = p*s + (1-p)*a, where p is the share of TIME spent in slow frames,
 * s is the rate during them and a the rate outside them.  So it is a
 * frame rate, and a stumble weighs what it costs in seconds rather than
 * what it costs in entries of a list.  Higher is better.
 *
 * Standing still flatters any such number - nothing is being drawn - and
 * throwing the still frames away would lose half the game, so both are
 * kept and the still part is worth a third:
 *
 *     M = (W*T_move*m_move + T_still*m_still) / (W*T_move + T_still)
 *
 * with W = 3 by default (-mweight N).  A frame counts as movement when
 * the map was redrawn in it or the blit was of a real size; standing at
 * a menu or on an unchanged map is stillness.
 */
extern "C" int ag_metric_weight = 3;

struct metric_side {
    uint32_t frames;
    uint64_t sum_us;
    uint32_t slow;
    uint64_t slow_us;
    uint64_t sum_ms2;   /* of the squares, in milliseconds, for the spread */
};

static metric_side s_m_move, s_m_still;
static metric_side s_m_move_all, s_m_still_all;

/* The worst window of each kind, because an average hides the minute that
 * made the game unplayable - Maxim asked for it and he is right. */
static uint32_t s_m_move_worst = 0xFFFFFFFFu, s_m_still_worst = 0xFFFFFFFFu;

static void metric_add(metric_side *side, uint32_t frame_us)
{
    const uint64_t ms = frame_us / 1000u;

    side->frames++;
    side->sum_us += frame_us;
    side->sum_ms2 += ms * ms;
    if (frame_us >= HUD_SLOW_US) {
        side->slow++;
        side->slow_us += frame_us;
    }
}

/* m for one side, in hundredths of a frame per second.  Zero if it never
 * ran: a side with no frames must not drag the average anywhere. */
static uint32_t metric_of(const metric_side *side)
{
    if (side->frames == 0u || side->sum_us == 0ull) {
        return 0u;
    }

    const uint64_t fast_frames = side->frames - side->slow;
    const uint64_t fast_us = side->sum_us - side->slow_us;
    /* p, in parts per million of the time. */
    const uint64_t p = side->slow_us * 1000000ull / side->sum_us;
    const uint64_t s_rate = (side->slow_us > 0ull)
        ? ((uint64_t)side->slow * 100000000ull / side->slow_us) : 0ull;
    const uint64_t a_rate = (fast_us > 0ull)
        ? (fast_frames * 100000000ull / fast_us) : 0ull;

    return (uint32_t)((p * s_rate + (1000000ull - p) * a_rate) / 1000000ull);
}

/*
 * ARGON: how even the game is, in hundredths of a frame per second.
 *
 *     smooth = 1000 / (mean_ms + K * sigma_ms)
 *
 * Maxim put three cases in the order he would want them: a steady thirty
 * beats a sixty that stumbles, and a rare deep stall beats frequent small
 * ones.  A plain average ranks all three backwards; this ranks them 22.3,
 * 14.0, 13.3 - his order - because what it punishes is the spread.
 */
extern "C" int ag_metric_ksigma = 2;

static uint32_t metric_smooth(const metric_side *side)
{
    if (side->frames == 0u) {
        return 0u;
    }

    const uint64_t n = side->frames;
    const uint64_t mean_ms = (side->sum_us / 1000ull) / n;
    const uint64_t mean2 = side->sum_ms2 / n;
    /* variance = E[t^2] - (E[t])^2, floored at zero for the rounding. */
    const uint64_t var = (mean2 > mean_ms * mean_ms)
        ? (mean2 - mean_ms * mean_ms) : 0ull;

    uint64_t sigma = 0ull;

    while ((sigma + 1ull) * (sigma + 1ull) <= var) {
        sigma++;
    }

    const uint64_t k = (ag_metric_ksigma >= 0)
        ? (uint64_t)ag_metric_ksigma : 2ull;
    const uint64_t denom = mean_ms + k * sigma;

    return (denom > 0ull) ? (uint32_t)(100000ull / denom) : 0u;
}

static uint32_t metric_together(const metric_side *mv, const metric_side *st)
{
    const uint64_t w = (ag_metric_weight > 0) ? (uint64_t)ag_metric_weight : 1ull;
    const uint64_t tm = mv->sum_us * w;
    const uint64_t ts = st->sum_us;

    if (tm + ts == 0ull) {
        return 0u;
    }
    return (uint32_t)((metric_of(mv) * tm + metric_of(st) * ts) / (tm + ts));
}

static void metric_report(void)
{
    if (s_m_move.frames == 0u && s_m_still.frames == 0u) {
        return;
    }

    /* Two numbers, not one.  Maxim asked for them apart: they answer
     * different questions and a single average hides which half moved. */
    const uint32_t mv = metric_of(&s_m_move);
    const uint32_t st = metric_of(&s_m_still);
    const uint32_t mv_all = metric_of(&s_m_move_all);
    const uint32_t st_all = metric_of(&s_m_still_all);

    /* A window only counts towards the worst if it had enough of that
     * kind in it to mean anything - three seconds. */
    if (s_m_move.sum_us >= 3000000ull && mv < s_m_move_worst) {
        s_m_move_worst = mv;
    }
    if (s_m_still.sum_us >= 3000000ull && st < s_m_still_worst) {
        s_m_still_worst = st;
    }

    const uint32_t mv_sm = metric_smooth(&s_m_move);
    const uint32_t st_sm = metric_smooth(&s_m_still);
    const uint32_t mv_sm_all = metric_smooth(&s_m_move_all);
    const uint32_t st_sm_all = metric_smooth(&s_m_still_all);

    shim_printf("[metric t=%u] moving %u.%02u (smooth %u.%02u) over %u s | "
                "still %u.%02u (smooth %u.%02u) over %u s || whole session: "
                "moving %u.%02u (smooth %u.%02u) over %u s, "
                "still %u.%02u (smooth %u.%02u) over %u s | "
                "WORST window: moving %u.%02u, still %u.%02u\n",
                (unsigned)ag_millis(),
                mv / 100u, mv % 100u, mv_sm / 100u, mv_sm % 100u,
                (unsigned)(s_m_move.sum_us / 1000000ull),
                st / 100u, st % 100u, st_sm / 100u, st_sm % 100u,
                (unsigned)(s_m_still.sum_us / 1000000ull),
                mv_all / 100u, mv_all % 100u,
                mv_sm_all / 100u, mv_sm_all % 100u,
                (unsigned)(s_m_move_all.sum_us / 1000000ull),
                st_all / 100u, st_all % 100u,
                st_sm_all / 100u, st_sm_all % 100u,
                (unsigned)(s_m_still_all.sum_us / 1000000ull),
                (s_m_move_worst == 0xFFFFFFFFu) ? 0u : s_m_move_worst / 100u,
                (s_m_move_worst == 0xFFFFFFFFu) ? 0u : s_m_move_worst % 100u,
                (s_m_still_worst == 0xFFFFFFFFu) ? 0u : s_m_still_worst / 100u,
                (s_m_still_worst == 0xFFFFFFFFu) ? 0u : s_m_still_worst % 100u);

    memset(&s_m_move, 0, sizeof(s_m_move));
    memset(&s_m_still, 0, sizeof(s_m_still));
}

/*
 * One frame's worth, from the present that ends it.  Called whether or not
 * anything is being drawn or printed: the cost is a subtraction, and a readout
 * that only starts counting when it is switched on cannot show the second it
 * was switched on in.
 */
static void hud_frame(uint32_t frame_us, uint32_t blit_us, uint32_t wire_us,
                      uint32_t wire_px)
{
    /*
     * ARGON: movement or stillness, decided per frame.
     *
     * The map being redrawn is what movement is - walking, scrolling, a
     * critter animating - and it is exactly what the engine counts in
     * ag_move_refresh_n.  A large blit catches the rest (a dialogue, the
     * inventory), because a frame that repaints the screen is not a frame
     * the player is standing in.
     */
    {
        static unsigned last_refresh_n;
        const bool moved = (ag_move_refresh_n != last_refresh_n)
            || (wire_px >= 16384u);

        last_refresh_n = ag_move_refresh_n;

        /*
         * A map load is not play, and it is minutes of frames that take
         * seconds each: left in, it dragged a window of standing still
         * from 58 down to 3.  The metric is about the game once it is
         * running, so loading frames are not counted at all.
         */
        if (ag_map_loading == 0u) {
            metric_add(moved ? &s_m_move : &s_m_still, frame_us);
            metric_add(moved ? &s_m_move_all : &s_m_still_all, frame_us);
        }
    }

    hud_bucket_t *b = &s_hb[s_hb_at];
    b->frames++;
    b->sum_us += frame_us;
    b->blit_us += blit_us;
    b->wire_us += wire_us;
    b->wire_px += wire_px;
    if (frame_us > b->worst_us) {
        b->worst_us = frame_us;
        b->worst_blit_us = blit_us;
        b->worst_wire_us = wire_us;
    }
    if (frame_us >= HUD_SLOW_US) {
        b->slow++;
        /*
         * ARGON: and how long they lasted, which is the number that means
         * something.  Counted as a share of FRAMES, one 200 ms frame among
         * fifty good ones reads as 2% - and it is a fifth of the second the
         * person was looking at.  Maxim asked for it by time on 11 September
         * 2026 and he was right: "lo" is meant to say how much of the time
         * the game was not answering, not how many entries in a list were
         * bad.
         */
        b->slow_us += frame_us;
    }
}

/*
 * A frame rate of its own, because -fps prints once a window and the readout
 * wants a number every second whether anything is being printed or not.
 */
/*
 * ARGON: set when hud_tick has new numbers to show, cleared when they are on
 * the screen.  See the note at the end of hud_draw for why it exists.
 */
static bool s_hud_changed;

static void hud_tick(void)
{
    const uint32_t now = (uint32_t)ag_millis();

    if (s_hb_next_ms == 0u) {
        s_hb_next_ms = now + 1000u;
        return;
    }
    if (now < s_hb_next_ms) {
        return;
    }
    s_hb_next_ms = now + 1000u;
    s_hud_changed = true;

    /* Everything the five buckets hold, before the oldest is thrown away. */
    uint32_t frames = 0, slow = 0, worst = 0, worst_blit = 0, worst_wire = 0;
    uint64_t slow_us = 0;
    uint64_t sum = 0, blit = 0, wire = 0, px = 0;
    for (unsigned i = 0; i < HUD_BUCKETS; ++i) {
        frames += s_hb[i].frames;
        slow += s_hb[i].slow;
        slow_us += s_hb[i].slow_us;
        sum += s_hb[i].sum_us;
        blit += s_hb[i].blit_us;
        wire += s_hb[i].wire_us;
        px += s_hb[i].wire_px;
        if (s_hb[i].worst_us > worst) {
            worst = s_hb[i].worst_us;
            worst_blit = s_hb[i].worst_blit_us;
            worst_wire = s_hb[i].worst_wire_us;
        }
    }

    if (frames > 0u && sum > 0u) {
        uint64_t fps100 = (uint64_t)frames * 100000000ull / sum;
        s_hud_fps100 = (unsigned)((fps100 > 99999ull) ? 99999ull : fps100);
        s_hud_slow_pct = (unsigned)(slow_us * 100ull / sum);
        s_hud_slow_frames = slow;
        const uint64_t frame_us = sum / frames;
        const uint64_t blit_us = blit / frames;
        const uint64_t wire_us = wire / frames;
        /*
         * The engine is what is left when the costs this port can name are
         * taken out - and which of them are *in* the frame depends on where
         * the wire is being driven from.  With a thread of its own it is not:
         * the game did not wait for it, so subtracting it would invent engine
         * time that never happened and hide the milliseconds that did.  Named
         * by subtraction either way, so that an unaccounted millisecond shows
         * up as engine rather than vanishing.
         */
        const uint64_t in_frame =
            blit_us + wire_us;
        const uint64_t eng_us =
            (frame_us > in_frame) ? frame_us - in_frame : 0u;
        s_hud_eng_ms = (unsigned)(eng_us / 1000u);
        s_hud_blit_ms = (unsigned)(blit_us / 1000u);
        s_hud_wire_ms = (unsigned)(wire_us / 1000u);
        /* Kilobytes a second the wire was asked to carry.  Asked, not sent:
         * the driver trims a rectangle against what the far end already has
         * and this side never learns by how much.  A number that is much
         * larger than the wire can carry is the answer either way. */
        s_hud_wire_kbs = (unsigned)(px * 2ull / 1024ull / HUD_BUCKETS);
    }
    if (worst > 0u) {
        uint64_t min100 = 100000000ull / worst;
        s_hud_min100 = (unsigned)((min100 > 99999ull) ? 99999ull : min100);
        s_hud_worst_ms = worst / 1000u;
        s_hud_worst_wire_ms = worst_wire / 1000u;
        s_hud_worst_eng_ms =
            (worst > worst_blit + worst_wire)
                ? ((worst - worst_blit - worst_wire) / 1000u)
                : 0u;
    }

    /* The arena, sampled here rather than every frame: one ag_meminfo on the
     * board's six megabytes measured about twelve milliseconds. */
    if (s_hud || s_memstat_ms != 0u) {
        ag_meminfo_t mi;
        ag_meminfo(&mi);
        s_hud_used_kb = (unsigned)((mi.arena_total - mi.arena_free) / 1024u);
        s_hud_hole_kb = (unsigned)(mi.arena_largest / 1024u);
    }

    /*
     * The card, which is the one failure that looks exactly like a hang.
     *
     * Fallout reads from the card on every frame - art, sounds, the music
     * stream - and does not check most of those reads.  When the card stops
     * answering, which this one does (`sdmmc_init_ocr ... 0x107`, and only
     * unplugging the board fixes it), the engine keeps looping: the frame rate
     * stays where it was, the readout keeps printing sixty a second, the
     * picture stops changing and the sound stops.  It cost two evenings and
     * two reports of a game that had frozen while the board was measurably
     * running at fifty frames a second.
     *
     * An open that finds nothing is already counted, and when the card goes
     * every open finds nothing.  So a spike in that number is the card, said
     * out loud, once.
     */
    /* Said once a minute while it is true, because it is the difference
     * between a game that is broken and one that cannot be reached. */
    if (s_events && s_kbd < 0 && s_mouse < 0) {
        static uint32_t said_ms;
        if (said_ms == 0u || (now - said_ms) >= 60000u) {
            said_ms = now;
            shim_printf("[ev t=%u] there is no keyboard and no pointer: "
                        "/dev/kbd0 and /dev/mouse0 do not exist, so nothing "
                        "can be clicked and the menu cannot be left\n",
                        (unsigned)now);
        }
    }

    {
        const uint32_t misses = ag_libc_io_misses();
        if (misses < s_io_misses_last) {
            s_io_misses_last = 0; /* the counters were reset under us */
        }
        const uint32_t d = misses - s_io_misses_last;
        s_io_misses_last = misses;
        if (d >= 8u) {
            s_hud_io_bad = 4; /* seconds of complaint left on the glass */
            if (s_events) {
                shim_printf("[ev t=%u] the filesystem is not answering: "
                            "%u opens found nothing in the last second - if "
                            "this does not stop, the card has dropped out and "
                            "the board needs its power taken away\n",
                            (unsigned)now, (unsigned)d);
            }
        } else if (s_hud_io_bad > 0u) {
            s_hud_io_bad--;
        }
    }

    /*
     * A level change: the mean has settled a third below or half above the
     * level last reported.  Two thresholds rather than one, because a drop is
     * what gets noticed and a recovery only matters once it is clearly back.
     */
    if (s_events && s_hud_fps100 > 0u) {
        const unsigned was = s_ev_level100;
        if (was == 0u || s_hud_fps100 * 3u < was * 2u ||
            s_hud_fps100 * 2u > was * 3u) {
            shim_printf("[ev t=%u] fps level %u.%02u (was %u.%02u): "
                        "eng %u blit %u wire %u ms, lo %u%%, "
                        "wire %u KB/s, hole %u KB\n",
                        (unsigned)now, s_hud_fps100 / 100u,
                        s_hud_fps100 % 100u, was / 100u, was % 100u,
                        s_hud_eng_ms, s_hud_blit_ms, s_hud_wire_ms,
                        s_hud_slow_pct, s_hud_wire_kbs, s_hud_hole_kb);
            s_ev_level100 = s_hud_fps100;
        }
    }

    /*
     * To the log as well as the glass.  The glass is the point - the person
     * playing it is the instrument that matters - but a run left going while
     * nobody is watching still has to be readable afterwards, and the two
     * numbers must be the same numbers or there is nothing to compare.
     */
    shim_printf("[hud t=%u] fps %u.%02u min %u.%02u lo %u%% of the time "
                "(%u frames)  "
                "eng %u blit %u wire %u ms  wire %u KB/s  "
                "worst %u ms (eng %u, wire %u)  "
                "arena %u used, hole %u\n",
                (unsigned)now, s_hud_fps100 / 100u, s_hud_fps100 % 100u,
                s_hud_min100 / 100u, s_hud_min100 % 100u, s_hud_slow_pct,
                s_hud_slow_frames,
                s_hud_eng_ms, s_hud_blit_ms, s_hud_wire_ms, s_hud_wire_kbs,
                s_hud_worst_ms, s_hud_worst_eng_ms, s_hud_worst_wire_ms,
                s_hud_used_kb, s_hud_hole_kb);

    s_hb_at = (s_hb_at + 1u) % HUD_BUCKETS;
    s_hb[s_hb_at].frames = 0;
    s_hb[s_hb_at].worst_us = 0;
    s_hb[s_hb_at].slow = 0;
    s_hb[s_hb_at].slow_us = 0;
    s_hb[s_hb_at].sum_us = 0;
    s_hb[s_hb_at].blit_us = 0;
    s_hb[s_hb_at].wire_us = 0;
    s_hb[s_hb_at].wire_px = 0;
}

/* Green, amber, red by how bad the number is - so the readout can be taken in
 * without being read. */
static Uint16 hud_rate_colour(unsigned rate100)
{
    if (rate100 >= 2000u) {
        return 0x07E0u; /* twenty a second or better */
    }
    if (rate100 >= 1000u) {
        return 0xFFE0u; /* ten to twenty */
    }
    return 0xF800u;
}

/* A number with two decimals, right where the cursor is. */
static int hud_rate(int x, int y, unsigned rate100, Uint16 colour)
{
    x = hud_number(x, y, rate100 / 100u, (rate100 >= 10000u) ? 3 : 2, colour);
    x = hud_text(x, y, ".", colour);
    return hud_number(x, y, rate100 % 100u, 2, colour);
}

#define HUD_W 144
#define HUD_H 22

static void hud_draw(void)
{
    if (!s_hud || s_gfx.fb == NULL) {
        return;
    }

    const int w = (HUD_W < (int)s_gfx.width) ? HUD_W : (int)s_gfx.width;
    const int h = (HUD_H < (int)s_gfx.height) ? HUD_H : (int)s_gfx.height;

    /* A black strip behind it, or the numbers vanish into the picture. */
    for (int y = 0; y < h; ++y) {
        Uint16 *d = (Uint16 *)((Uint8 *)s_gfx.fb + (size_t)y * s_gfx.stride);
        for (int x = 0; x < w; ++x) {
            d[x] = 0;
        }
    }

    const Uint16 white = 0xFFFFu;
    const Uint16 fps_c = hud_rate_colour(s_hud_fps100);
    const Uint16 min_c = hud_rate_colour(s_hud_min100);

    /* Row one: how many.  The mean, the worst frame in the last five seconds
     * as a rate, and the share of frames slower than a tenth of a second. */
    int x = 1;
    x = hud_text(x, 1, "FPS", white);
    x = hud_rate(x + 2, 1, s_hud_fps100, fps_c) + 3;
    x = hud_text(x, 1, "MIN", white);
    x = hud_rate(x + 2, 1, s_hud_min100, min_c) + 3;
    x = hud_text(x, 1, "LO", white);
    x = hud_number(x + 2, 1, s_hud_slow_pct, 2,
                   (s_hud_slow_pct >= 20u) ? 0xF800u : 0x07E0u);
    x = hud_text(x, 1, "%", white);
    /* The card, in red, when it has stopped answering - the one fault that
     * otherwise looks like a frame rate that is fine and a game that is not. */
    if (s_hud_io_bad != 0u) {
        (void)hud_text(x + 3, 1, "IO", 0xF800u);
    }

    /*
     * Row two: where it went, and this is the row the whole readout exists
     * for.  A frame the engine thought about for two hundred milliseconds and
     * a frame that took two hundred milliseconds to reach the glass are the
     * same stutter from the chair and different problems entirely.
     */
    x = 1;
    x = hud_text(x, 8, "EG", white);
    x = hud_number(x + 2, 8, s_hud_eng_ms, 3, 0xFD20u) + 3;   /* orange */
    x = hud_text(x, 8, "BT", white);
    x = hud_number(x + 2, 8, s_hud_blit_ms, 3, 0x07FFu) + 3;  /* cyan */
    x = hud_text(x, 8, "WR", white);
    x = hud_number(x + 2, 8, s_hud_wire_ms, 3, 0xF81Fu) + 4;  /* magenta */
    /* The same split for the worst frame in the window, which is the frame
     * that was actually noticed.  White so it reads as a different kind of
     * number from the averages beside it. */
    x = hud_number(x, 8, s_hud_worst_eng_ms, 3, 0xFD20u);
    x = hud_text(x, 8, "/", white);
    (void)hud_number(x, 8, s_hud_worst_wire_ms, 3, 0xF81Fu);

    /* Row three: the wire's load, and the room left in the arena.  Green while
     * there is space for a full-screen buffer, amber under half of one, red
     * under a quarter - the three states that decide whether the next
     * dialogue opens. */
    Uint16 mem_c = 0x07E0u;
    if (s_hud_hole_kb < 300u) {
        mem_c = 0xFFE0u;
    }
    if (s_hud_hole_kb < 150u) {
        mem_c = 0xF800u;
    }
    x = 1;
    x = hud_text(x, 15, "KBS", white);
    x = hud_number(x + 2, 15, s_hud_wire_kbs, 3, 0xF81Fu) + 3;
    x = hud_text(x, 15, "MEM", white);
    x = hud_number(x + 2, 15, s_hud_used_kb, 4, mem_c) + 3;
    x = hud_text(x, 15, "/", white);
    x = hud_number(x, 15, s_hud_hole_kb, 4, mem_c);

    /*
     * And, in red, when there is nothing to click with.
     *
     * This one is here because it was reported as a hang three times in one
     * evening.  With no pointer the main menu cannot be left: the picture
     * animates, the frame rate is sixty, the sound plays, and nothing a person
     * does has any effect - which is indistinguishable from a game that has
     * stopped, and is not one.  Every other number on this readout was fine
     * each time.  `usb` at the shell says "host up, nothing plugged in", and
     * now so does the glass.
     */
    if (s_kbd < 0 && s_mouse < 0) {
        (void)hud_text(x + 4, 15, "NOMOUSE", 0xF800u);
    }

    /*
     * And say that these pixels changed.
     *
     * The readout used to be drawn after the flush, on the assumption that the
     * next frame would paint over it and send it - which was true while the
     * whole screen was being flushed every frame for the palette cycle.  It is
     * not true now that only real changes are sent: a still screen sends
     * nothing, and a readout nobody sends is a readout nobody can read.
     *
     * ARGON: but only when they did change, or when the game had already
     * dirtied this corner.
     *
     * Unconditionally, this one line cost the wire every frame the game drew.
     * The readout's numbers are recomputed once a second and nothing else in
     * it moves, while the display driver copies the *whole* 150 KB surface for
     * every frame it accepts - so a still screen that should have sent nothing
     * at all was sending sixty full frames a second, and Maxim reported the
     * game as "much less smooth than this morning" the first time it was left
     * on.  Once a second is 1/60th of that, and when the game has already
     * damaged this corner the readout rides along for nothing.
     */
    const bool overlaps = s_dirty_x1 > s_dirty_x0 && s_dirty_y1 > s_dirty_y0 &&
                          s_dirty_x0 < w && s_dirty_y0 < h;
    if (s_hud_changed || overlaps) {
        dirty_add(0, 0, w, h);
        s_hud_changed = false;
    }
}

void SDL_RenderPresent(SDL_Renderer *r)
{
    (void)r;
    if (!s_have_gfx) {
        return;
    }

    if (s_gfx.fb != NULL) {
        /* The readout first, so its own pixels are part of the damage this
         * present hands over rather than waiting for the next one. */
        hud_tick();
        hud_draw();

        /* The arena reserve takes itself back once the crisis has
         * passed.  Cheap: a pointer test, and ag_meminfo only when it
         * is actually gone.  See on_out_of_arena in fallout_main.cc. */
        fallout_reserve_tick();

        /*
         * Only what was drawn into, handed to the display with one flush.
         * Whether that display is a window under QEMU or a panel at the end of
         * a wire is the driver's business - the game asks for the rectangle it
         * changed and nothing more.  Nothing drawn means nothing to send: the
         * engine presents on a timer as well as after a change, and a still
         * screen should cost nothing.
         */
        uint32_t wire_us = 0, wire_px = 0;
        int flush_x = 0, flush_y = 0, flush_w = 0, flush_h = 0;
        if (s_dirty_x1 > s_dirty_x0 && s_dirty_y1 > s_dirty_y0) {
            flush_x = s_dirty_x0;
            flush_y = s_dirty_y0;
            flush_w = s_dirty_x1 - s_dirty_x0;
            flush_h = s_dirty_y1 - s_dirty_y0;
            dirty_none();
            const ag_time_t t0 = ag_micros();
            ag_gfx_flush((uint16_t)flush_x, (uint16_t)flush_y,
                         (uint16_t)flush_w, (uint16_t)flush_h);
            wire_us = (uint32_t)(ag_micros() - t0);
            wire_px = (uint32_t)flush_w * (uint32_t)flush_h;
        }

        if (s_fps_ms != 0u && wire_px != 0u) {
            s_flush_us += (uint64_t)wire_us;
            s_flush_px += (uint64_t)wire_px;
            s_flushes++;
        }

        /* The frame this present ends, into the rolling window the readout
         * keeps: the flush time is counted as the frame's, because it happened
         * in it. */
        {
            const uint32_t now_us = (uint32_t)ag_micros();
            const uint32_t frame_us =
                (s_hud_last_us != 0u) ? (now_us - s_hud_last_us) : 0u;
            s_hud_last_us = now_us;
            if (frame_us != 0u && frame_us < 5000000u) {
                hud_frame(frame_us, (uint32_t)s_blit_frame_us, wire_us,
                          wire_px);
                events_frame(frame_us, (uint32_t)s_blit_frame_us, wire_us,
                             flush_x, flush_y, flush_w, flush_h);
            }
            s_blit_frame_us = 0;
        }
    } else if (s_frame_px != NULL && AG_HAS(g_ag_api->gfx, present)) {
        const ag_blit_t b = {
            .px = s_frame_px,
            .stride = (uint32_t)s_frame_pitch,
            .surf_w = s_gfx.width,
            .surf_h = s_gfx.height,
            .x = 0,
            .y = 0,
            .w = s_gfx.width,
            .h = s_gfx.height,
        };
        ag_gfx_present(&b);
    }

    /* The engine's frame loop has no other place to be interrupted, and a
     * process that never yields is a process the supervisor kills.  The
     * timers are serviced from here for the same reason: it is the one point
     * the game passes through every frame and nowhere else.
     *
     * Timed, because renderPresent turned out to cost 19 ms a frame against
     * 2.5 for the flush inside it, and everything else it does is here. */
    const uint32_t p0 = (uint32_t)ag_micros();
    timers_service();
    const uint32_t p1 = (uint32_t)ag_micros();
    memstat_service();
    /* One queued report line per frame: see shim_printf. */
    shim_drain();
    const uint32_t p2 = (uint32_t)ag_micros();
    ++s_frames;
    ++s_frames_total;
    s_last_present_ms = (uint32_t)ag_millis();
    s_stall_next_ms = 0u;
    fps_service();
    const uint32_t p3 = (uint32_t)ag_micros();
    /* And the input drivers, which pump their sockets only when read.  A movie
     * runs for minutes without the engine asking for a single event, and a
     * driver unread for that long has lost its connection. */
    sdl_shim_input_service();
    const uint32_t p4 = (uint32_t)ag_micros();
    ag_heartbeat();

    /*
     * And on the first frame, promise to keep doing that.
     *
     * The heartbeat above has been here from the start, with a comment saying
     * "a process that never yields is a process the supervisor kills" - and it
     * was not true, because nothing ever armed the watchdog.  A process with
     * watchdog_ms of zero is one the supervisor skips, so a game that stopped
     * stopped in silence: no message, no crash record, and a console whose
     * input had gone with it.  Six hours went into "the board hangs" before it
     * turned out the board was fine and the game was not.
     *
     * Armed here and not before main(), because the seconds before the first
     * frame are spent reading two .DAT indices and nothing presents during
     * them; a deadline that starts at the beginning would fire on the loading
     * rather than on the fault.
     *
     * Fifteen seconds is not a frame budget - the worst frame measured on this
     * board is 600 ms and a map change is a couple of seconds.  It is the
     * distance between "slow" and "stopped", and it wants to be far from both.
     */
    if (s_watchdog_ms != 0u && !s_watchdog_armed) {
        s_watchdog_armed = true;
        ag_watchdog(s_watchdog_ms);
        /* And the thread that will say where it stopped, if it does.  Three
         * kilobytes: it holds a few integers and a printf.
         *
         * ARGON: the result is read now.  It was thrown away with a (void),
         * and a thread that was never created is indistinguishable from one
         * that is running and has nothing to report - which is exactly the
         * doubt that two unexplained freezes left me with.  If it cannot be
         * had, the run should say so at the top of its log rather than leave
         * the silence to be interpreted later.
         *
         * It returns a handle, not a status - ag_thread_t is a void* - so null
         * is the failure and anything else is the thread.  The first cut of
         * this tested `!= 0` and duly reported the running thread as "NOT
         * started (1070490252)", which is its address. */
        s_watch_stop = false;
        const ag_thread_t watcher = ag_thread_create(watch_thread, NULL,
                                                     "fowatch", 3072, 0, 0);
        if (watcher == NULL) {
            shim_printf("[watch] NOT started - nothing will say where the "
                      "game is if it stops\n");
        }
    }
    s_pres_timers_us += p1 - p0;
    s_pres_memstat_us += p2 - p1;
    s_pres_fps_us += p3 - p2;
    s_pres_input_us += p4 - p3;
    s_pres_beat_us += (uint32_t)ag_micros() - p4;

    ++s_present_count;
    if ((s_shot_after > 0 && s_present_count >= s_shot_after) ||
        (s_shot_at_ms != 0 &&
         (uint32_t)ag_millis() - s_gfx_since_ms >= s_shot_at_ms)) {
        s_shot_after = 0;
        s_shot_at_ms = 0;
        write_ppm(s_shot_path);
    }
}

SDL_Texture *SDL_CreateTexture(SDL_Renderer *r, Uint32 format, int access,
                               int w, int h)
{
    (void)r;
    (void)format;
    (void)access;
    s_texture.w = w;
    s_texture.h = h;
    return &s_texture;
}

void SDL_DestroyTexture(SDL_Texture *t) { (void)t; }

/*
 * The question that makes the whole arrangement work.  The engine asks what
 * format its texture came out as and builds its presenting surface to match,
 * so answering RGB565 - whatever format was asked for - is what puts the game
 * one blit away from the panel instead of two.
 */
int SDL_QueryTexture(SDL_Texture *t, Uint32 *format, int *access, int *w,
                     int *h)
{
    if (t == NULL) {
        return -1;
    }
    if (format != NULL) {
        *format = SDL_PIXELFORMAT_RGB565;
    }
    if (access != NULL) {
        *access = SDL_TEXTUREACCESS_STREAMING;
    }
    if (w != NULL) {
        *w = t->w;
    }
    if (h != NULL) {
        *h = t->h;
    }
    return 0;
}

/*
 * Nothing to update: when the presenting surface is the framebuffer, these are
 * the same pixels.  When it is not - a panel of a different size - the copy
 * happens here, clipped to the overlap, because this is the call that knows
 * both the source pitch and that the frame is finished.
 */
int SDL_UpdateTexture(SDL_Texture *t, const SDL_Rect *rect, const void *pixels,
                      int pitch)
{
    (void)rect;

    s_frame_px = pixels;
    s_frame_pitch = pitch;

    if (!s_have_gfx || pixels == NULL || pixels == s_gfx.fb ||
        s_gfx.fb == NULL) {
        return 0;
    }

    int w = (int)s_gfx.width;
    int h = (int)s_gfx.height;
    if (t != NULL) {
        if (t->w < w) {
            w = t->w;
        }
        if (t->h < h) {
            h = t->h;
        }
    }
    for (int y = 0; y < h; ++y) {
        memcpy((Uint8 *)s_gfx.fb + (size_t)y * s_gfx.stride,
               (const Uint8 *)pixels + (size_t)y * pitch,
               (size_t)w * sizeof(Uint16));
    }
    return 0;
}

/* ---- events -------------------------------------------------------------- */

/*
 * One queue, drained from ag_poll_event, which is where both the keyboard and
 * the pointer arrive however they were produced - a real key, a KBDVIRT
 * injection, a mouse packet from MOUSEVIRT.
 *
 * The queue is small on purpose.  Fallout polls once per frame and drains
 * everything; a backlog longer than this means the game is not running, and
 * dropping the oldest events then is better than growing without bound.
 */
#define SDL_QUEUE_LEN 64

static SDL_Event s_queue[SDL_QUEUE_LEN];
static int s_q_head;
static int s_q_tail;

/* Relative motion since the last SDL_GetRelativeMouseState, and the buttons
 * as they are now.  Fallout reads movement this way and never asks for a
 * position, because it keeps the cursor itself. */
static int s_mouse_dx;
static int s_mouse_dy;

/*
 * Buttons that have been down at any point since the engine last looked.
 *
 * Fallout reads the button *level*, once a frame, and works out presses and
 * releases from how it changes.  Everything that has arrived from the driver
 * since the last frame is drained in one go, so a press and its release both
 * land between two reads and the level it sees never changes: the click did
 * not happen.  At forty milliseconds a frame that is most clicks, and it is
 * why the game could be pointed at but not told to do anything.
 *
 * So a button that goes down stays reported until it has been read once.  The
 * engine then sees it down on one frame and up on the next, which is a click,
 * which is what it was.
 */
static Uint32 s_mouse_latch;

/*
 * The keyboard as a table of flags indexed by scancode - which is to say by
 * HID usage id, since here they are the same number.  Kept from the same
 * events the queue gets rather than asked of the system each time, because the
 * ABI reports one key at a time and the caller wants a whole array to point
 * at.  Fallout 2's sfall helpers read it; Fallout 1 never does.
 */
static Uint8 s_keystate[SDL_NUM_SCANCODES];

static void queue_push(const SDL_Event *e)
{
    const int next = (s_q_head + 1) % SDL_QUEUE_LEN;
    if (next == s_q_tail) {
        /* Full: drop the oldest.  A dropped key is better than a stuck one. */
        s_q_tail = (s_q_tail + 1) % SDL_QUEUE_LEN;
    }
    s_queue[s_q_head] = *e;
    s_q_head = next;
}

/*
 * ArgonOS reports a key as a USB HID usage id, and an SDL scancode is a USB
 * HID usage id - the tables are the same table.  So this is very nearly the
 * identity, and the exceptions are the two keypad keys whose HID number is not
 * where SDL's name for them ended up.
 *
 * The keypad full stop is deliberately not one of them.  It is HID 0x63, which
 * is SDL_SCANCODE_KP_PERIOD, and Fallout 1's key tables look for KP_DECIMAL
 * instead - so that key does nothing there.  It does nothing on a desktop
 * either, for exactly the same reason, and making it work here would be this
 * port disagreeing with SDL about what the hardware said.  Fallout 2 looks for
 * KP_PERIOD and works.
 */
static SDL_Scancode scancode_of(uint16_t hid)
{
    switch (hid) {
    case 0x67:
        return SDL_SCANCODE_KP_EQUALS;
    case 0x85:
        return SDL_SCANCODE_KP_COMMA;
    default:
        break;
    }
    if (hid >= SDL_NUM_SCANCODES) {
        return SDL_SCANCODE_UNKNOWN;
    }
    return (SDL_Scancode)hid;
}

static Uint16 mods_of(uint16_t ag)
{
    Uint16 out = 0;
    if (ag & AG_MOD_SHIFT) {
        out |= KMOD_LSHIFT;
    }
    if (ag & AG_MOD_CTRL) {
        out |= KMOD_LCTRL;
    }
    if (ag & AG_MOD_ALT) {
        out |= KMOD_LALT;
    }
    if (ag & AG_MOD_GUI) {
        out |= KMOD_LGUI;
    }
    if (ag & AG_MOD_CAPS) {
        out |= KMOD_CAPS;
    }
    if (ag & AG_MOD_NUM) {
        out |= KMOD_NUM;
    }
    return out;
}

/*
 * ArgonOS numbers the buttons left, right, middle; SDL numbers them left,
 * middle, right.  Two bits change places, and getting it wrong makes the
 * right mouse button - which is how Fallout switches cursor mode, so half the
 * game - do nothing at all.
 */
static Uint32 buttons_of(uint8_t ag)
{
    Uint32 out = 0;
    if (ag & 0x1u) {
        out |= SDL_BUTTON(SDL_BUTTON_LEFT);
    }
    if (ag & 0x2u) {
        out |= SDL_BUTTON(SDL_BUTTON_RIGHT);
    }
    if (ag & 0x4u) {
        out |= SDL_BUTTON(SDL_BUTTON_MIDDLE);
    }
    return out;
}

/*
 * The virtual input drivers, opened only to be read.
 *
 * KBDVIRT and MOUSEVIRT carry the host's keyboard and mouse over TCP, and each
 * of them pumps its socket inside its own read entry point - a driver on this
 * system has no thread of its own, so being read *is* its service tick.  They
 * also inject what arrives into the same queue ag_poll_event drains, which is
 * where this port takes its events from.
 *
 * So the bytes returned here are deliberately thrown away: the events they
 * describe arrive through the queue a moment later.  What the read is for is
 * the pumping.  Without it the driver accepts the connection, never reads the
 * socket, and the host's end is reset - which looks exactly like a keyboard
 * that does not work, and was diagnosed as one for a while.
 */

static void input_devices_open(void)
{
    s_kbd = ag_dev_open("/dev/kbd0");
    s_mouse = ag_dev_open("/dev/mouse0");
    ag_printf("SDL: input %s%s%s\n",
              s_kbd >= 0 ? "kbd0 " : "",
              s_mouse >= 0 ? "mouse0" : "",
              (s_kbd < 0 && s_mouse < 0) ? "none (drv install the .SYS files)"
                                         : "");
}

/*
 * A device that cannot be read is silent, and silence here is exactly what a
 * host with its hands off the keyboard looks like.  So each device says once
 * that it has produced something, and says any error it gives - which is the
 * difference between "input does not work" and knowing which half of it does
 * not.  Not every read: during a game these run sixty times a second and the
 * interesting fact is the first one, not the four hundredth.
 */
static void report_read(const char *what, ag_handle_t h, int32_t n)
{
    static bool    s_said[2];
    static int32_t s_err[2];
    const int      i = (h == s_kbd) ? 0 : 1;

    if (n > 0 && !s_said[i]) {
        s_said[i] = true;
        ag_printf("SDL: %s is delivering events\n", what);
    } else if (n < 0 && n != s_err[i]) {
        s_err[i] = n;
        ag_printf("SDL: %s read failed: %d\n", what, (int)n);
    }
}

/*
 * A frame that never arrives looks the same from outside as a game that has
 * stopped, and the two want different investigations.
 *
 * The engine asks for input in every loop it has - the main one, dialogue, the
 * character editor, a movie - so a line printed from here says it is going
 * round and not drawing.  Silence says it is inside a call that has not
 * returned, which is a different bug and a different place to look.
 */
/*
 * How long the panel has been showing the same frame, in milliseconds.
 *
 * Read by the movie player, which cannot tell on its own that it has stopped
 * working: see patches/fallout2-ce/0009.
 */
extern "C" unsigned ag_frame_stalled_ms(void)
{
    if (s_last_present_ms == 0u) {
        return 0u;
    }
    return (unsigned)((uint32_t)ag_millis() - s_last_present_ms);
}

/*
 * Whether there is room to play a movie, asked before one is started.
 *
 * The MVE decoder allocates through internal_malloc_safe, and "safe" here means
 * that a failed allocation prints a line and takes the process with it.  That
 * is a reasonable choice on a machine where running out of memory means
 * something is wrong; it is the wrong one here, where it means the player
 * walked into a scene with a cut-scene in it while standing on a loaded map.
 *
 * Measured on the board: the decoder's buffers come to about 730 KB and the
 * largest single request is 204900 bytes.  A map that is loaded leaves 450 to
 * 500 KB, so the answer is usually no, and the run that found this died on
 * vsuit.mve about ninety seconds into a new game.
 *
 * Refusing early turns that into a cut-scene that does not play, which the
 * callers already handle - gameMoviePlay returns -1 for a movie file that is
 * missing, and every caller copes.  A skipped scene is a small loss; the loss
 * it replaces is the whole session.
 */
#define MOVIE_NEEDS_FREE    (900u * 1024u)
#define MOVIE_NEEDS_LARGEST (256u * 1024u)

extern "C" int ag_movie_affordable(void)
{
    ag_meminfo_t mi;
    ag_meminfo(&mi);
    return (mi.arena_free >= MOVIE_NEEDS_FREE &&
            mi.arena_largest >= MOVIE_NEEDS_LARGEST)
               ? 1
               : 0;
}

static void stall_service(void)
{
    const uint32_t now = (uint32_t)ag_millis();

    if (s_last_present_ms == 0u) {
        s_last_present_ms = now;
        return;
    }
    const uint32_t since = now - s_last_present_ms;
    if (since < 3000u) {
        return;
    }
    if (s_stall_next_ms != 0u && (uint32_t)(now - s_stall_next_ms) > 0x80000000u) {
        return;
    }
    s_stall_next_ms = now + 3000u;
    shim_printf("[stall t=%u] no frame for %u ms, still asking for input\n",
              (unsigned)now, (unsigned)since);
}

/*
 * Look for a keyboard and a pointer again, once a second, until both are
 * found.
 *
 * They used to be opened once, when the display was taken, and never again -
 * which is fine on a machine whose input is soldered on and wrong on this one.
 * A USB mouse plugged in after the game started did not exist as far as the
 * game was concerned, and neither did one the host lost and re-enumerated.
 * What that looks like from the chair is a game that has hung: the picture
 * keeps moving, the frame rate is fine, and nothing responds to anything.  It
 * cost an hour of somebody's evening and a message saying the game had frozen,
 * with the board running at sixty-three frames a second at the time.
 *
 * Cheap enough to do for ever: two opens of a name that is not there.
 */
static void input_devices_rescan(void)
{
    static uint32_t next_ms;

    if (s_kbd >= 0 && s_mouse >= 0) {
        return;
    }
    const uint32_t now = (uint32_t)ag_millis();
    if (next_ms != 0u && now < next_ms) {
        return;
    }
    next_ms = now + 1000u;

    if (s_kbd < 0) {
        s_kbd = ag_dev_open("/dev/kbd0");
        if (s_kbd >= 0) {
            ag_printf("SDL: kbd0 appeared\n");
        }
    }
    if (s_mouse < 0) {
        s_mouse = ag_dev_open("/dev/mouse0");
        if (s_mouse >= 0) {
            ag_printf("SDL: mouse0 appeared\n");
        }
    }
}

void sdl_shim_input_service(void)
{
    stall_service();
    input_devices_rescan();

    /* Enough for a burst; anything beyond it is read on the next call. */
    uint8_t drain[128];
    if (s_kbd >= 0) {
        report_read("kbd0", s_kbd, ag_dev_read(s_kbd, drain, sizeof(drain)));
    }
    if (s_mouse >= 0) {
        report_read("mouse0", s_mouse,
                    ag_dev_read(s_mouse, drain, sizeof(drain)));
    }
}

static void pump(void)
{
    /*
     * Also from SDL_RenderPresent, and that is the one that matters: the
     * engine does not poll for events while a movie is running, and a driver
     * that goes unread for five minutes has had its connection dropped long
     * before anybody presses a key to skip it.
     */
    sdl_shim_input_service();

    ag_event_t e;
    while (ag_poll_event(&e, 0)) {
        SDL_Event out;
        memset(&out, 0, sizeof(out));
        out.common.timestamp = (Uint32)ag_millis();

        switch (e.type) {
        case AG_EV_KEY_DOWN:
        case AG_EV_KEY_UP: {
            s_ev_keys++;
            const SDL_Scancode sc = scancode_of(e.key.keycode);
            /*
             * Recorded before the unknown-key test, so a key the shim has no
             * scancode for is still in the transcript: a step marker that
             * silently did not happen is worse than a useless one.
             */
            if (s_recinput && e.type == AG_EV_KEY_DOWN) {
                const unsigned kc = (unsigned)e.key.keycode;
                shim_printf("[rec t=%u] key %u%s%c%s\n", (unsigned)ag_millis(),
                            kc, (kc >= 32u && kc < 127u) ? " '" : "",
                            (kc >= 32u && kc < 127u) ? (char)kc : ' ',
                            (kc >= 32u && kc < 127u) ? "'" : "");
            }
            if (sc == SDL_SCANCODE_UNKNOWN) {
                continue;
            }

            /*
             * ARGON: F9 shows and hides the frame counter.
             *
             * Asked for on 9 September 2026, and the reason is worth keeping:
             * without it there is no way from the chair to tell a frozen game
             * from a dead mouse.  This setup has no sound and the menu is a
             * still picture, so a stopped game looks exactly like a running
             * one.  The counter answers that in a glance - and being able to
             * turn it off means it can stay out of the way while playing.
             *
             * The key is consumed here and never reaches the engine, next to
             * F8 which the port already owns.
             */
            if (sc == SDL_SCANCODE_F9 && e.type == AG_EV_KEY_DOWN) {
                s_hud = !s_hud;
                if (!s_hud) {
                    /* Paint the last frame over the numbers, or they sit on
                     * the glass until something else happens to redraw. */
                    panel_repaint();
                }
                ag_printf("hud: %s\n", s_hud ? "on" : "off");
                continue;
            }

            /* The port's own keys, on a panel smaller than the picture. */
            if (s_panel_shift != 0 && e.type == AG_EV_KEY_DOWN) {
                if (sc == SDL_SCANCODE_F8) {
                    s_one_to_one = !s_one_to_one;
                    panel_repaint();
                    ag_printf("panel: %s\n",
                              s_one_to_one ? "one to one, arrows to move"
                                           : "whole picture");
                    continue;
                }
                if (s_one_to_one) {
                    const int stepx = (int)s_gfx.width / 4;
                    const int stepy = (int)s_gfx.height / 4;
                    int moved = 1;

                    switch (sc) {
                    case SDL_SCANCODE_LEFT:  s_pan_x -= stepx; break;
                    case SDL_SCANCODE_RIGHT: s_pan_x += stepx; break;
                    case SDL_SCANCODE_UP:    s_pan_y -= stepy; break;
                    case SDL_SCANCODE_DOWN:  s_pan_y += stepy; break;
                    default: moved = 0; break;
                    }
                    if (moved) {
                        pan_clamp();
                        panel_repaint();
                        /* Said out loud because "the arrows do nothing" has
                         * three possible meanings - the key never arrived, the
                         * window did not move, or it moved and the far end was
                         * not told - and from the desk they look the same. */
                        ag_printf("panel: window at %d,%d of %dx%d; source "
                                  "%dx%d wrote %d,%d..%d,%d; damage "
                                  "%d,%d..%d,%d\n",
                                  s_pan_x, s_pan_y, (int)s_gfx.width,
                                  (int)s_gfx.height, s_pan_sw, s_pan_sh,
                                  s_pan_bx0, s_pan_by0, s_pan_bx1, s_pan_by1,
                                  s_dirty_x0, s_dirty_y0, s_dirty_x1,
                                  s_dirty_y1);
                        continue;
                    }
                }
            }
            out.type = (e.type == AG_EV_KEY_DOWN) ? SDL_KEYDOWN : SDL_KEYUP;
            out.key.state =
                (e.type == AG_EV_KEY_DOWN) ? SDL_PRESSED : SDL_RELEASED;
            out.key.repeat = e.key.repeat ? 1 : 0;
            out.key.keysym.scancode = sc;
            out.key.keysym.mod = mods_of(e.key.mods);
            s_keystate[sc] = (e.type == AG_EV_KEY_DOWN) ? 1 : 0;
            queue_push(&out);
            break;
        }

        case AG_EV_POINTER_DOWN:
        case AG_EV_POINTER_UP:
        case AG_EV_POINTER_MOVE: {
            if (e.type == AG_EV_POINTER_MOVE) {
                s_ev_ptr_move++;
            } else {
                s_ev_ptr_btn++;
            }
            /*
             * The pointer arrives in screen coordinates; the engine wants
             * movement.  MOUSEVIRT leaves dx and dy at zero and sends where
             * the host's cursor is, so the difference is taken here.  The
             * first event only establishes the origin - without that the
             * cursor would jump from the corner on the first move.
             */
            /*
             * And scaled back up when the panel is smaller than the picture.
             * The pointer arrives in the panel's pixels; the engine's cursor
             * lives in the 640x480 one it draws, so on a half-size panel every
             * movement would be half of what the hand did and the far corner
             * would be out of reach.
             */
            const int x = e.ptr.x;
            const int y = e.ptr.y;
            if (s_mouse_x >= 0) {
                const int mdx = (e.ptr.dx != 0) ? e.ptr.dx : (x - s_mouse_x);
                const int mdy = (e.ptr.dy != 0) ? e.ptr.dy : (y - s_mouse_y);
                /* Not while one to one: there a panel pixel is a picture
                 * pixel, and doubling would make the pointer skip every
                 * second one. */
                const int in_shift = s_one_to_one ? 0 : s_panel_shift;
                s_mouse_dx += mdx << in_shift;
                s_mouse_dy += mdy << in_shift;
            }
            s_mouse_x = x;
            s_mouse_y = y;
            const Uint32 prev_buttons = s_mouse_buttons;
            s_mouse_buttons = buttons_of(e.ptr.buttons);
            s_mouse_latch |= s_mouse_buttons;

            if (e.type == AG_EV_POINTER_MOVE) {
                out.type = SDL_MOUSEMOTION;
                out.motion.x = s_one_to_one ? (x + s_pan_x)
                                            : (x << s_panel_shift);
                out.motion.y = s_one_to_one ? (y + s_pan_y)
                                            : (y << s_panel_shift);
                out.motion.state = s_mouse_buttons;
            } else {
                /*
                 * ArgonOS reports the whole mask on an edge, not which button
                 * made it, so the changed bit is found by difference.  Fallout
                 * itself reads the mask and ignores this field - it is filled
                 * in properly anyway, because an event that says LEFT when the
                 * right button was pressed is a trap for whoever reads this
                 * next.
                 */
                const Uint32 changed = s_mouse_buttons ^ prev_buttons;
                out.type = (e.type == AG_EV_POINTER_DOWN) ? SDL_MOUSEBUTTONDOWN
                                                          : SDL_MOUSEBUTTONUP;
                out.button.state = (e.type == AG_EV_POINTER_DOWN) ? SDL_PRESSED
                                                                  : SDL_RELEASED;
                out.button.x = x;
                out.button.y = y;
                out.button.button =
                    (changed & SDL_BUTTON(SDL_BUTTON_RIGHT))
                        ? SDL_BUTTON_RIGHT
                        : (changed & SDL_BUTTON(SDL_BUTTON_MIDDLE)
                               ? SDL_BUTTON_MIDDLE
                               : SDL_BUTTON_LEFT);
                out.button.clicks = 1;
                /*
                 * Where the click landed, as the *game* received it - not as
                 * the host believed it had sent it.  The two disagree: the
                 * helper maps a letterboxed window onto the guest's size, and
                 * the guest's own pointer has its own idea of where it is.
                 * Only this end can be replayed, so only this end is written
                 * down.  Both coordinate systems go in the line: the panel's,
                 * which is what inputplay.py speaks, and the picture's, which
                 * is what stays the same when the panel changes size.
                 */
                if (s_recinput) {
                    const int gx = s_one_to_one ? (x + s_pan_x)
                                                : (x << s_panel_shift);
                    const int gy = s_one_to_one ? (y + s_pan_y)
                                                : (y << s_panel_shift);
                    shim_printf("[rec t=%u] %s panel=%d,%d game=%d,%d "
                                "buttons=%u\n",
                                (unsigned)ag_millis(),
                                (e.type == AG_EV_POINTER_DOWN) ? "press"
                                                               : "release",
                                x, y, gx, gy, (unsigned)s_mouse_buttons);
                }
            }
            queue_push(&out);
            break;
        }

        case AG_EV_WHEEL:
            /* One axis, in dy, the way MOUSEVIRT sends it. */
            out.type = SDL_MOUSEWHEEL;
            out.wheel.x = e.ptr.dx;
            out.wheel.y = e.ptr.dy;
            queue_push(&out);
            break;

        case AG_EV_QUIT:
            out.type = SDL_QUIT;
            queue_push(&out);
            break;

        case AG_EV_FOCUS_GAINED:
        case AG_EV_FOCUS_LOST:
            out.type = SDL_WINDOWEVENT;
            out.window.event = (e.type == AG_EV_FOCUS_GAINED)
                                   ? SDL_WINDOWEVENT_FOCUS_GAINED
                                   : SDL_WINDOWEVENT_FOCUS_LOST;
            queue_push(&out);
            break;

        default:
            break;
        }
    }
}

/*
 * Push an event in from outside.  Fallout 1 never does; Fallout 2 does, from
 * its sfall keyboard helpers, which synthesise key presses for the hot keys a
 * mod can bind.  It goes in the same queue as everything else.
 */
int SDL_PushEvent(SDL_Event *event)
{
    if (event == NULL) {
        return -1;
    }
    queue_push(event);
    return 1;
}

const Uint8 *SDL_GetKeyboardState(int *numkeys)
{
    pump();
    if (numkeys != NULL) {
        *numkeys = SDL_NUM_SCANCODES;
    }
    return s_keystate;
}

int SDL_PollEvent(SDL_Event *event)
{
    pump();
    if (s_q_tail == s_q_head) {
        return 0;
    }
    if (event != NULL) {
        *event = s_queue[s_q_tail];
    }
    s_q_tail = (s_q_tail + 1) % SDL_QUEUE_LEN;
    return 1;
}

void SDL_PumpEvents(void) { pump(); }

void SDL_FlushEvents(Uint32 minType, Uint32 maxType)
{
    /* The engine flushes to drop everything, never a range, so the range is
     * checked and then ignored rather than half-implemented. */
    (void)minType;
    (void)maxType;
    pump();
    s_q_head = 0;
    s_q_tail = 0;
}

Uint32 SDL_GetRelativeMouseState(int *x, int *y)
{
    pump();
    if (x != NULL) {
        *x = s_mouse_dx;
    }
    if (y != NULL) {
        *y = s_mouse_dy;
    }
    s_mouse_dx = 0;
    s_mouse_dy = 0;
    const Uint32 buttons = s_mouse_buttons | s_mouse_latch;
    s_mouse_latch = 0;
    return buttons;
}

/*
 * There is no cursor to capture and no window to leave, so relative mode is
 * what this always was.  Reporting success matters: dxinput_mouse_init makes
 * it the test for whether the mouse works at all.
 */
int SDL_SetRelativeMouseMode(SDL_bool enabled)
{
    (void)enabled;
    return 0;
}

SDL_Keymod SDL_GetModState(void)
{
    return (SDL_Keymod)mods_of(ag_mods());
}

int SDL_ShowCursor(int toggle)
{
    (void)toggle;
    return 0;
}

SDL_Cursor *SDL_GetCursor(void) { return NULL; }

SDL_Cursor *SDL_CreateSystemCursor(SDL_SystemCursor id)
{
    (void)id;
    return NULL;
}

void SDL_SetCursor(SDL_Cursor *c) { (void)c; }

void SDL_FreeCursor(SDL_Cursor *c) { (void)c; }

void SDL_StartTextInput(void) {}

void SDL_StopTextInput(void) {}

/* ---- audio --------------------------------------------------------------- */

/*
 * The engine wants what SDL gives it: a device that calls back for a block of
 * samples whenever it needs one, and a resampler per sound so that a clip
 * recorded at 8 kHz can be mixed with one at 22.
 *
 * ArgonOS's audio is the other shape - a queue that is written to, with
 * ag_audio_space saying how much room is left.  So the callback is driven from
 * a thread here: it asks the queue for room, calls the engine's mixer to fill
 * a block, and writes it.  That thread is the audio device.
 */
#define AUDIO_FRAMES 1024
#define AUDIO_CHANNELS 2

static SDL_AudioSpec s_spec;
static bool s_audio_open;
static volatile bool s_audio_paused = true;
static volatile bool s_audio_stop;
static ag_thread_t s_audio_thread;
static int16_t s_audio_block[AUDIO_FRAMES * AUDIO_CHANNELS];

static void audio_thread(void *arg)
{
    (void)arg;
    while (!s_audio_stop) {
        /* ARGON: the lap counter says whether this thread is running at all,
         * which is the difference between "spinning inside the mixer" and
         * "blocked in the device write" - and the game's thread waits on
         * mutexes this thread takes, so when the game stops this is the other
         * half of the answer.  See ag_diag.h. */
        ag_audio_laps++;

        if (s_audio_paused || s_spec.callback == NULL) {
            ag_step_audio = "paused";
            ag_delay(10);
            continue;
        }
        if (ag_audio_space() < AUDIO_FRAMES) {
            /* Short: the queue drains in about 46 ms at 22 kHz, and sleeping
             * a whole tick here is how a gap gets into the sound. */
            ag_step_audio = "waiting for room in the queue";
            ag_delay(2);
            continue;
        }
        /*
         * ARGON: how long each half takes, worst case.
         *
         * Four unrelated tickers in the game's thread all peak at the same
         * 150-240 ms, which is not four slow functions - it is one thing they
         * all wait for.  The mixer runs here and takes locks the game thread
         * takes too, so if it is the thing, this is where it shows.
         */
        ag_step_audio = "calling the mixer";
        const uint32_t au0 = ag_us();
        s_spec.callback(s_spec.userdata, (Uint8 *)s_audio_block,
                        (int)sizeof(s_audio_block));
        const uint32_t au1 = ag_us();
        ag_step_audio = "writing to the device";
        ag_audio_write(s_audio_block, AUDIO_FRAMES);
        const uint32_t au2 = ag_us();
        ag_step_audio = "between blocks";

        if (au1 - au0 > s_audio_mix_worst_us) {
            s_audio_mix_worst_us = au1 - au0;
        }
        if (au2 - au1 > s_audio_write_worst_us) {
            s_audio_write_worst_us = au2 - au1;
        }
        s_audio_mix_us += au1 - au0;
        s_audio_write_us += au2 - au1;
        s_audio_blocks++;
    }
    ag_thread_exit();
}

SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture,
                                      const SDL_AudioSpec *desired,
                                      SDL_AudioSpec *obtained,
                                      int allowed_changes)
{
    (void)device;
    (void)allowed_changes;
    if (iscapture || desired == NULL) {
        return 0;
    }

    s_spec = *desired;
    /* What this port can actually produce, whatever was asked for.  The engine
     * passes SDL_AUDIO_ALLOW_ANY_CHANGE and reads the answer back out of
     * gAudioEngineSpec, so telling it the truth here is enough. */
    s_spec.format = AUDIO_S16;
    s_spec.channels = AUDIO_CHANNELS;
    s_spec.samples = AUDIO_FRAMES;
    s_spec.silence = 0;
    s_spec.size = sizeof(s_audio_block);

    ag_printf("SDL: audio %d Hz, %d channels, blocks of %d frames\n",
              s_spec.freq, AUDIO_CHANNELS, AUDIO_FRAMES);

    const ag_audio_fmt_t fmt = {
        .rate = (uint32_t)s_spec.freq,
        .channels = AUDIO_CHANNELS,
        .bits = 16,
    };
    if (ag_audio_open(&fmt) != AG_OK) {
        ag_print("SDL audio: no device (load PCMVIRT.SYS or an I2S driver)\n");
        return 0;
    }

    s_audio_stop = false;
    s_audio_paused = true;
    /*
     * Sixteen kilobytes, not four.
     *
     * The thread's own work is small, but what it calls is not its own: the
     * callback is the engine's mixer, which puts a kilobyte of scratch on the
     * stack, walks every active sound buffer, takes a C++ lock on each and
     * runs the resampler underneath.  Four kilobytes covered that while one
     * track was playing and did not while a map was being entered with music,
     * ambience and a critter's sounds all live - and what a mixing thread does
     * when it runs off the end of its stack is not fail, it is corrupt
     * whatever is next and stop the machine somewhere else entirely.
     */
    s_audio_thread = ag_thread_create(audio_thread, NULL, "sdlaudio", 16384, 0,
                                      0);
    if (s_audio_thread == NULL) {
        ag_audio_close();
        return 0;
    }

    s_audio_open = true;
    if (obtained != NULL) {
        *obtained = s_spec;
    }
    return 1;
}

void SDL_CloseAudioDevice(SDL_AudioDeviceID dev)
{
    (void)dev;
    if (!s_audio_open) {
        return;
    }
    s_audio_stop = true;
    ag_thread_join(s_audio_thread, 1000);
    ag_audio_close();
    s_audio_open = false;
}

void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on)
{
    (void)dev;
    s_audio_paused = (pause_on != 0);
}

/*
 * Add src into dst at volume/128, saturating.  Only the one format, because
 * the device above only ever reports the one.
 */
void SDL_MixAudioFormat(Uint8 *dst, const Uint8 *src, SDL_AudioFormat format,
                        Uint32 len, int volume)
{
    if (dst == NULL || src == NULL || volume <= 0) {
        return;
    }
    if (format != AUDIO_S16) {
        ag_panic("SDL_MixAudioFormat: only signed 16-bit is implemented");
    }

    Sint16 *d = (Sint16 *)dst;
    const Uint32 n = len / sizeof(Sint16);
    for (Uint32 i = 0; i < n; ++i) {
        Sint16 s;
        memcpy(&s, src + i * sizeof(Sint16), sizeof(s));
        int32_t v = (int32_t)d[i] + ((int32_t)s * volume) / SDL_MIX_MAXVOLUME;
        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
        d[i] = (Sint16)v;
    }
}

/*
 * The resampler.
 *
 * Fallout's sounds are recorded at whatever rate they were recorded at and
 * mixed together at 22 kHz, so every voice goes through one of these.  Linear
 * interpolation between the two input frames either side of the output
 * instant: a quarter of the cost of anything better and, for speech and
 * gunfire already limited to 8 or 11 kHz, below what the material shows.
 *
 * The engine hands over one source frame at a time and takes back whatever
 * comes out, so the output has to be buffered - a frame in can be nought,
 * one, two or three frames out depending on the ratio.
 */
#define STREAM_OUT_FRAMES 512
#define PHASE_ONE 65536u

struct SDL_AudioStream {
    uint32_t step;   /* src frames per dst frame, 16.16 */
    uint32_t phase;  /* where in the current input interval, 16.16 */
    bool have_last;
    Sint16 last[AUDIO_CHANNELS];
    Uint8 src_channels;
    SDL_AudioFormat src_format;

    Sint16 out[STREAM_OUT_FRAMES * AUDIO_CHANNELS];
    int head;
    int tail;
};

SDL_AudioStream *SDL_NewAudioStream(SDL_AudioFormat src_format,
                                    Uint8 src_channels, int src_rate,
                                    SDL_AudioFormat dst_format,
                                    Uint8 dst_channels, int dst_rate)
{
    if (src_rate <= 0 || dst_rate <= 0 || src_channels < 1 ||
        src_channels > 2 || dst_channels != AUDIO_CHANNELS ||
        dst_format != AUDIO_S16) {
        return NULL;
    }
    if (src_format != AUDIO_S16 && src_format != AUDIO_S8 &&
        src_format != AUDIO_U8) {
        return NULL;
    }

    SDL_AudioStream *s = (SDL_AudioStream *)ag_malloc(sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    s->step = (uint32_t)(((uint64_t)src_rate * PHASE_ONE) / (uint32_t)dst_rate);
    s->src_channels = src_channels;
    s->src_format = src_format;
    return s;
}

static int stream_free_frames(const SDL_AudioStream *s)
{
    const int used = (s->head - s->tail + STREAM_OUT_FRAMES) % STREAM_OUT_FRAMES;
    return STREAM_OUT_FRAMES - 1 - used;
}

static void stream_emit(SDL_AudioStream *s, const Sint16 *a, const Sint16 *b,
                        uint32_t phase)
{
    if (stream_free_frames(s) <= 0) {
        return; /* the consumer is behind; dropping is better than blocking */
    }
    Sint16 *dst = &s->out[s->head * AUDIO_CHANNELS];
    for (int c = 0; c < AUDIO_CHANNELS; ++c) {
        const int32_t lo = a[c];
        const int32_t hi = b[c];
        dst[c] = (Sint16)(lo + (((hi - lo) * (int32_t)phase) >> 16));
    }
    s->head = (s->head + 1) % STREAM_OUT_FRAMES;
}

int SDL_AudioStreamPut(SDL_AudioStream *s, const void *buf, int len)
{
    if (s == NULL || buf == NULL || len <= 0) {
        return -1;
    }

    const int src_bytes = (s->src_format == AUDIO_S16 ? 2 : 1) * s->src_channels;
    const int frames = len / src_bytes;
    const Uint8 *p = (const Uint8 *)buf;

    for (int i = 0; i < frames; ++i) {
        Sint16 cur[AUDIO_CHANNELS];
        for (int c = 0; c < AUDIO_CHANNELS; ++c) {
            /* Mono is duplicated rather than panned: the engine does its own
             * panning by volume, on two buffers. */
            const int sc = (s->src_channels == 1) ? 0 : c;
            if (s->src_format == AUDIO_S16) {
                Sint16 v;
                memcpy(&v, p + (size_t)sc * 2u, sizeof(v));
                cur[c] = v;
            } else if (s->src_format == AUDIO_S8) {
                cur[c] = (Sint16)((Sint8)p[sc] * 256);
            } else {
                cur[c] = (Sint16)(((int)p[sc] - 128) * 256);
            }
        }
        p += src_bytes;

        if (!s->have_last) {
            s->last[0] = cur[0];
            s->last[1] = cur[1];
            s->have_last = true;
            continue;
        }
        while (s->phase < PHASE_ONE) {
            stream_emit(s, s->last, cur, s->phase);
            s->phase += s->step;
        }
        s->phase -= PHASE_ONE;
        s->last[0] = cur[0];
        s->last[1] = cur[1];
    }
    return 0;
}

int SDL_AudioStreamGet(SDL_AudioStream *s, void *buf, int len)
{
    if (s == NULL || buf == NULL || len <= 0) {
        return -1;
    }

    const int frame_bytes = (int)sizeof(Sint16) * AUDIO_CHANNELS;
    int want = len / frame_bytes;
    const int have = (s->head - s->tail + STREAM_OUT_FRAMES) % STREAM_OUT_FRAMES;
    if (want > have) {
        want = have;
    }

    Uint8 *out = (Uint8 *)buf;
    for (int i = 0; i < want; ++i) {
        memcpy(out, &s->out[s->tail * AUDIO_CHANNELS], (size_t)frame_bytes);
        out += frame_bytes;
        s->tail = (s->tail + 1) % STREAM_OUT_FRAMES;
    }
    return want * frame_bytes;
}

void SDL_FreeAudioStream(SDL_AudioStream *s)
{
    if (s != NULL) {
        ag_free(s);
    }
}

/* ---- timers -------------------------------------------------------------- */

/*
 * One timer, because the engine registers one: the 40 ms tick that fades
 * sounds out.  It is not a thread and not an interrupt - it is checked when
 * the frame is presented, which is the only moment this port is certain to
 * reach regularly.  A fade therefore advances at the frame rate rather than
 * every 40 ms, which is a fade that takes longer on a slower machine; the
 * alternative is a fourth thread for something nobody hears.
 */
#define SDL_TIMERS 4

static struct {
    bool used;
    Uint32 interval;
    Uint32 due;
    SDL_TimerCallback fn;
    void *param;
} s_timers[SDL_TIMERS];

static void timers_service(void)
{
    const Uint32 now = (Uint32)ag_millis();
    for (int i = 0; i < SDL_TIMERS; ++i) {
        if (!s_timers[i].used || (Sint32)(now - s_timers[i].due) < 0) {
            continue;
        }
        const Uint32 next = s_timers[i].fn(s_timers[i].interval,
                                           s_timers[i].param);
        if (next == 0) {
            s_timers[i].used = false;
        } else {
            s_timers[i].interval = next;
            s_timers[i].due = now + next;
        }
    }
}

SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_TimerCallback callback,
                         void *param)
{
    if (callback == NULL) {
        return 0;
    }
    for (int i = 0; i < SDL_TIMERS; ++i) {
        if (s_timers[i].used) {
            continue;
        }
        s_timers[i].used = true;
        s_timers[i].interval = interval;
        s_timers[i].due = (Uint32)ag_millis() + interval;
        s_timers[i].fn = callback;
        s_timers[i].param = param;
        return (SDL_TimerID)(i + 1);
    }
    return 0;
}

SDL_bool SDL_RemoveTimer(SDL_TimerID id)
{
    if (id == 0 || id > SDL_TIMERS || !s_timers[id - 1].used) {
        return SDL_FALSE;
    }
    s_timers[id - 1].used = false;
    return SDL_TRUE;
}

}  // extern "C"
