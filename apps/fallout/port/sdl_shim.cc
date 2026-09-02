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

/* How often one strip of the panel is sent even when nothing changed - see the
 * comment where it is used.  A quarter of a second a strip covers the screen
 * in seven and a half, which is the driver's own repair interval, and costs
 * about five kilobytes each time instead of arriving in one lump. */
#define REPAINT_EVERY_MS 250u
static uint32_t s_repaint_next_ms;
static int      s_repaint_strip;

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
static void shim_printf(const char *fmt, ...)
{
    char line[320];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    ag_print(line);
    log_line(line);
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
static uint32_t s_fps_next;
static uint32_t s_frames;
static uint64_t s_blit_us;
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
uint32_t ag_bk_tickers_us;
uint32_t ag_bk_mouse_us;
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
        shim_printf("[tick] %p %u us/frame (%u calls)\n", ag_ticker_who[i],
                    (unsigned)(ag_ticker_us[i] / s_frames),
                    (unsigned)ag_ticker_calls[i]);
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
        for (int y = 0; y < ddh; ++y) {
            const Uint8 *s = (const Uint8 *)src->pixels +
                             (size_t)(sy + y) * src->pitch + sx;
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
            const Uint8 *sp = (const Uint8 *)src->pixels +
                              (size_t)syy * src->pitch + base + (x0 << shift);
            Uint16 *dp = (Uint16 *)((Uint8 *)dst->pixels +
                                    (size_t)(ddy + y) * dst->pitch) + ddx;
            int rx0 = ddw, rx1 = -1;
            for (int x = x0; x < x1; ++x, sp += step) {
                CHANGED(dp, x, lut[*sp]);
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

    {
        const uint32_t took = (uint32_t)(ag_micros() - blit_t0);
        s_blit_frame_us += took;
        if (s_fps_ms != 0u) {
            s_blit_us += (uint64_t)took;
            s_blit_px += (uint64_t)ddw * (uint64_t)ddh;
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
        s_panel_src = src;
        s_panel_dst = dst;
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
    SDL_UpperBlit(s_panel_src, &srcrect, s_panel_dst, &dstrect);
}

/* ---- time ---------------------------------------------------------------- */

Uint32 SDL_GetTicks(void) { return (Uint32)ag_millis(); }

void SDL_Delay(Uint32 ms) { ag_delay(ms); }

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
    for (int i = 0; k_hud_letters[i] != ' '; ++i) {
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
    for (; *text != ' '; ++text) {
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
    uint64_t sum_us;
    uint64_t blit_us;
    uint64_t wire_us;
    uint64_t wire_px;
} hud_bucket_t;

static hud_bucket_t s_hb[HUD_BUCKETS];
static unsigned     s_hb_at;
static uint32_t     s_hb_next_ms;
static uint32_t     s_hud_last_us;

/* Filled in by hud_tick, drawn by hud_draw, printed by hud_log. */
static unsigned s_hud_fps100;   /* mean frames per second, hundredths */
static unsigned s_hud_min100;   /* the worst frame as a rate, hundredths */
static unsigned s_hud_slow_pct;
static unsigned s_hud_eng_ms, s_hud_blit_ms, s_hud_wire_ms, s_hud_wire_kbs;
static unsigned s_hud_worst_ms, s_hud_worst_eng_ms, s_hud_worst_wire_ms;
static unsigned s_hud_used_kb, s_hud_hole_kb;

/*
 * One frame's worth, from the present that ends it.  Called whether or not
 * anything is being drawn or printed: the cost is a subtraction, and a readout
 * that only starts counting when it is switched on cannot show the second it
 * was switched on in.
 */
static void hud_frame(uint32_t frame_us, uint32_t blit_us, uint32_t wire_us,
                      uint32_t wire_px)
{
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
    }
}

/*
 * A frame rate of its own, because -fps prints once a window and the readout
 * wants a number every second whether anything is being printed or not.
 */
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

    /* Everything the five buckets hold, before the oldest is thrown away. */
    uint32_t frames = 0, slow = 0, worst = 0, worst_blit = 0, worst_wire = 0;
    uint64_t sum = 0, blit = 0, wire = 0, px = 0;
    for (unsigned i = 0; i < HUD_BUCKETS; ++i) {
        frames += s_hb[i].frames;
        slow += s_hb[i].slow;
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
        s_hud_slow_pct = (unsigned)(slow * 100u / frames);
        const uint64_t frame_us = sum / frames;
        const uint64_t blit_us = blit / frames;
        const uint64_t wire_us = wire / frames;
        /* The engine is what is left when the two costs this port can name are
         * taken out.  Named by subtraction on purpose: an unaccounted
         * millisecond then shows up as engine rather than vanishing. */
        const uint64_t eng_us =
            (frame_us > blit_us + wire_us) ? frame_us - blit_us - wire_us : 0u;
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
     * To the log as well as the glass.  The glass is the point - the person
     * playing it is the instrument that matters - but a run left going while
     * nobody is watching still has to be readable afterwards, and the two
     * numbers must be the same numbers or there is nothing to compare.
     */
    shim_printf("[hud t=%u] fps %u.%02u min %u.%02u lo %u%%  "
                "eng %u blit %u wire %u ms  wire %u KB/s  "
                "worst %u ms (eng %u, wire %u)  "
                "arena %u used, hole %u\n",
                (unsigned)now, s_hud_fps100 / 100u, s_hud_fps100 % 100u,
                s_hud_min100 / 100u, s_hud_min100 % 100u, s_hud_slow_pct,
                s_hud_eng_ms, s_hud_blit_ms, s_hud_wire_ms, s_hud_wire_kbs,
                s_hud_worst_ms, s_hud_worst_eng_ms, s_hud_worst_wire_ms,
                s_hud_used_kb, s_hud_hole_kb);

    s_hb_at = (s_hb_at + 1u) % HUD_BUCKETS;
    s_hb[s_hb_at].frames = 0;
    s_hb[s_hb_at].worst_us = 0;
    s_hb[s_hb_at].slow = 0;
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

#define HUD_W 116
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
    (void)hud_text(x, 1, "%", white);

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
    (void)hud_number(x, 15, s_hud_hole_kb, 4, mem_c);

    /*
     * And say that these pixels changed.
     *
     * The readout used to be drawn after the flush, on the assumption that the
     * next frame would paint over it and send it - which was true while the
     * whole screen was being flushed every frame for the palette cycle.  It is
     * not true now that only real changes are sent: a still screen sends
     * nothing, and a readout nobody sends is a readout nobody can read.
     */
    dirty_add(0, 0, w, h);
}

void SDL_RenderPresent(SDL_Renderer *r)
{
    (void)r;
    if (!s_have_gfx) {
        return;
    }

    if (s_gfx.fb != NULL) {
        /*
         * Eight rows of the panel, four times a second, whether anything drew
         * into them or not - walking down the screen and starting again.
         *
         * Two things need this.  A monitor at the end of a wire can be
         * unplugged and plugged back in, and when it comes back the driver
         * forgets everything it believed the far end had - but it keeps hashes,
         * not pixels, so it cannot repaint on its own and has to wait for a
         * flush.  And a frame the wire loses to noise leaves that part of the
         * glass showing the frame before it, because over there the hash says
         * it was sent.  Both come right on a flush that covers those rows.
         *
         * While the blit dirtied whatever the caller touched, such a flush
         * arrived thirty times a second by accident - the palette cycle
         * re-blitting the screen.  Now that only real changes are sent, a
         * still screen sends nothing at all, so the repaint is asked for on
         * purpose.
         *
         * A strip rather than the whole panel, and this is the second version:
         * the first asked for all 320x240 every two seconds, which is 150 KB
         * to hash and up to 40 KB to send in one go - a hitch of about a
         * tenth of a second, twice a minute apart, and what that looks like
         * from the chair is a mouse pointer that stalls.  Eight rows is five
         * kilobytes, under a fiftieth of a second, and the whole screen is
         * covered every seven and a half - which is the same ceiling the
         * driver's own repair sweep already documents.
         */
        const uint32_t now_ms = (uint32_t)ag_millis();
        if (s_repaint_next_ms == 0u) {
            s_repaint_next_ms = now_ms + REPAINT_EVERY_MS;
        } else if (now_ms >= s_repaint_next_ms) {
            s_repaint_next_ms = now_ms + REPAINT_EVERY_MS;
            const int strip = 8;
            const int rows = (int)s_gfx.height / strip;
            if (rows > 0) {
                s_repaint_strip = (s_repaint_strip + 1) % rows;
                dirty_add(0, s_repaint_strip * strip, (int)s_gfx.width, strip);
            }
        }

        /* The readout first, so that its own pixels are part of the damage
         * this present sends rather than waiting for the next one. */
        hud_tick();
        hud_draw();

        /* Only what was drawn into.  Nothing drawn means nothing to send -
         * the engine presents on a timer as well as after a change, and a
         * still screen should cost nothing. */
        uint32_t wire_us = 0, wire_px = 0;
        if (s_dirty_x1 > s_dirty_x0 && s_dirty_y1 > s_dirty_y0) {
            /*
             * Grown out to the panel driver's block, which is eight pixels
             * square.  REMDISP compares a rectangle against its record of what
             * the far end has only when the rectangle starts on a block
             * boundary; one that does not is sent whole and makes those rows
             * forget, so an exact three-pixel-wide rectangle costs more on the
             * wire than a sixteen-pixel one that can be trimmed.  Snapping
             * here means the driver sees rectangles it can work with, and the
             * few extra pixels are compared and dropped rather than sent.
             */
            const int blk = 8;
            int fx1 = (s_dirty_x1 + blk - 1) & ~(blk - 1);
            int fy1 = (s_dirty_y1 + blk - 1) & ~(blk - 1);
            if (fx1 > (int)s_gfx.width) {
                fx1 = (int)s_gfx.width;
            }
            if (fy1 > (int)s_gfx.height) {
                fy1 = (int)s_gfx.height;
            }
            s_dirty_x0 &= ~(blk - 1);
            s_dirty_y0 &= ~(blk - 1);
            s_dirty_x1 = fx1;
            s_dirty_y1 = fy1;
            const int fw = s_dirty_x1 - s_dirty_x0;
            const int fh = s_dirty_y1 - s_dirty_y0;
            /* Always timed now.  It used to be timed only under -fps, and
             * the readout needs the same number without it. */
            const ag_time_t t0 = ag_micros();
            ag_gfx_flush((uint16_t)s_dirty_x0, (uint16_t)s_dirty_y0,
                         (uint16_t)fw, (uint16_t)fh);
            wire_us = (uint32_t)(ag_micros() - t0);
            wire_px = (uint32_t)fw * (uint32_t)fh;
            if (s_fps_ms != 0u) {
                s_flush_us += (uint64_t)wire_us;
                s_flush_px += (uint64_t)wire_px;
                s_flushes++;
            }
            dirty_none();
        }

        /* And the frame this present ends, into the rolling window. */
        {
            const uint32_t now_us = (uint32_t)ag_micros();
            const uint32_t frame_us =
                (s_hud_last_us != 0u) ? (now_us - s_hud_last_us) : 0u;
            s_hud_last_us = now_us;
            if (frame_us != 0u && frame_us < 5000000u) {
                hud_frame(frame_us, (uint32_t)s_blit_frame_us, wire_us,
                          wire_px);
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
    const uint32_t p2 = (uint32_t)ag_micros();
    ++s_frames;
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
static Uint32 s_mouse_buttons;

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
static int s_mouse_x = -1;
static int s_mouse_y = -1;

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
static ag_handle_t s_kbd = -1;
static ag_handle_t s_mouse = -1;

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

void sdl_shim_input_service(void)
{
    stall_service();

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
            if (sc == SDL_SCANCODE_UNKNOWN) {
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
        if (s_audio_paused || s_spec.callback == NULL) {
            ag_delay(10);
            continue;
        }
        if (ag_audio_space() < AUDIO_FRAMES) {
            /* Short: the queue drains in about 46 ms at 22 kHz, and sleeping
             * a whole tick here is how a gap gets into the sound. */
            ag_delay(2);
            continue;
        }
        s_spec.callback(s_spec.userdata, (Uint8 *)s_audio_block,
                        (int)sizeof(s_audio_block));
        ag_audio_write(s_audio_block, AUDIO_FRAMES);
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
