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
extern "C" void ag_libc_malloc_trace(int on);
extern "C" void ag_libc_io_stats(uint64_t *us, uint32_t *reads,
                                 uint32_t *opens, uint32_t *seeks,
                                 uint64_t *open_us, uint64_t *seek_us);
extern "C" void ag_libc_io_reset(void);
extern "C" uint32_t ag_libc_io_misses(void);

/* Declared where the port can see them; not part of SDL. */
extern "C" bool sdl_shim_display(ag_gfxinfo_t *out);
extern "C" void sdl_shim_screenshot(const char *path, int after_frames,
                                    int after_ms);
extern "C" void sdl_shim_memstat(int every_ms);
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
static uint32_t s_memstat_next;
static size_t s_arena_low = (size_t)-1;

static void write_ppm(const char *path);

void sdl_shim_memstat(int every_ms)
{
    s_memstat_ms = (uint32_t)(every_ms > 0 ? every_ms : 0);
    s_memstat_next = 0;
    /* Anyone watching the arena wants to know who asked for the block that did
     * not fit, and that is off unless asked for. */
    ag_libc_malloc_trace(every_ms > 0);
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
        ag_printf("[fps t=%u] %u.%02u fps, %u us/frame: blit %u (%u KB), "
                  "flush %u in %u (%u KB), engine %u, io %u us "
                  "(open %u, seek %u, read %u) "
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
                  /* Totals over the window, not per frame: a few reads across
                   * ninety frames divide down to zero and read as "no I/O at
                   * all" next to an I/O time that says otherwise. */
                  (unsigned)io_reads,
                  (unsigned)io_opens,
                  (unsigned)ag_libc_io_misses(),
                  (unsigned)io_seeks);
    }

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

static void memstat_service(void)
{
    ag_meminfo_t mi;
    ag_meminfo(&mi);
    if (mi.arena_free < s_arena_low) {
        s_arena_low = mi.arena_free;
    }
    if (s_memstat_ms == 0) {
        return;
    }
    const uint32_t now = (uint32_t)ag_millis();
    if (now < s_memstat_next) {
        return;
    }
    s_memstat_next = now + s_memstat_ms;
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
    ag_printf("[mem] %u KB used of %u, peak %u (%u with transients), "
              "largest free block %u KB\n",
              (unsigned)((mi.arena_total - mi.arena_free) / 1024u),
              (unsigned)(mi.arena_total / 1024u),
              frame_peak,
              peak_alloc,
              (unsigned)(mi.arena_largest / 1024u));
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
        (int)s_gfx.width == w && (int)s_gfx.height == h &&
        s_gfx.fmt == AG_PIX_RGB565) {
        pixels = s_gfx.fb;
        pitch = (int)s_gfx.stride;
        owns = 0;
        s_fb_lent = true;
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
static const Uint16 *palette_lut(const SDL_Palette *pal)
{
    static Uint16 lut[256];
    static uint32_t built_gen;
    static const SDL_Palette *built_for;

    if (built_gen != s_palette_gen || built_for != pal) {
        const int n = pal->ncolors < 256 ? pal->ncolors : 256;
        for (int i = 0; i < n; ++i) {
            const SDL_Color c = pal->colors[i];
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
    if (dx + w > dst->w) {
        w = dst->w - dx;
    }
    if (dy + h > dst->h) {
        h = dst->h - dy;
    }
    if (w <= 0 || h <= 0) {
        return 0;
    }

    const ag_time_t blit_t0 = (s_fps_ms != 0u) ? ag_micros() : 0;

    for (int y = 0; y < h; ++y) {
        const Uint8 *s =
            (const Uint8 *)src->pixels + (size_t)(sy + y) * src->pitch + sx;
        Uint16 *d = (Uint16 *)((Uint8 *)dst->pixels +
                               (size_t)(dy + y) * dst->pitch) + dx;
        for (int x = 0; x < w; ++x) {
            d[x] = lut[s[x]];
        }
    }

    if (s_fps_ms != 0u) {
        s_blit_us += (uint64_t)(ag_micros() - blit_t0);
        s_blit_px += (uint64_t)w * (uint64_t)h;
    }

    /* Remember what changed, so that the present after it can send that and
     * not the whole screen.  Only when this surface is the panel: a blit into
     * a surface of our own is copied wholesale later anyway. */
    if (dst->pixels == s_gfx.fb) {
        dirty_add(dx, dy, w, h);
    }
    return 0;
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
void SDL_RenderPresent(SDL_Renderer *r)
{
    (void)r;
    if (!s_have_gfx) {
        return;
    }

    if (s_gfx.fb != NULL) {
        /* Only what was drawn into.  Nothing drawn means nothing to send -
         * the engine presents on a timer as well as after a change, and a
         * still screen should cost nothing. */
        if (s_dirty_x1 > s_dirty_x0 && s_dirty_y1 > s_dirty_y0) {
            const int fw = s_dirty_x1 - s_dirty_x0;
            const int fh = s_dirty_y1 - s_dirty_y0;
            const ag_time_t t0 = (s_fps_ms != 0u) ? ag_micros() : 0;
            ag_gfx_flush((uint16_t)s_dirty_x0, (uint16_t)s_dirty_y0,
                         (uint16_t)fw, (uint16_t)fh);
            if (s_fps_ms != 0u) {
                s_flush_us += (uint64_t)(ag_micros() - t0);
                s_flush_px += (uint64_t)fw * (uint64_t)fh;
                s_flushes++;
            }
            dirty_none();
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
     * the game passes through every frame and nowhere else. */
    timers_service();
    memstat_service();
    ++s_frames;
    fps_service();
    /* And the input drivers, which pump their sockets only when read.  A movie
     * runs for minutes without the engine asking for a single event, and a
     * driver unread for that long has lost its connection. */
    sdl_shim_input_service();
    ag_heartbeat();

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

void sdl_shim_input_service(void)
{
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
            const SDL_Scancode sc = scancode_of(e.key.keycode);
            if (sc == SDL_SCANCODE_UNKNOWN) {
                continue;
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
            /*
             * The pointer arrives in screen coordinates; the engine wants
             * movement.  MOUSEVIRT leaves dx and dy at zero and sends where
             * the host's cursor is, so the difference is taken here.  The
             * first event only establishes the origin - without that the
             * cursor would jump from the corner on the first move.
             */
            const int x = e.ptr.x;
            const int y = e.ptr.y;
            if (s_mouse_x >= 0) {
                s_mouse_dx += (e.ptr.dx != 0) ? e.ptr.dx : (x - s_mouse_x);
                s_mouse_dy += (e.ptr.dy != 0) ? e.ptr.dy : (y - s_mouse_y);
            }
            s_mouse_x = x;
            s_mouse_y = y;
            const Uint32 prev_buttons = s_mouse_buttons;
            s_mouse_buttons = buttons_of(e.ptr.buttons);

            if (e.type == AG_EV_POINTER_MOVE) {
                out.type = SDL_MOUSEMOTION;
                out.motion.x = x;
                out.motion.y = y;
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
    return s_mouse_buttons;
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
    s_audio_thread = ag_thread_create(audio_thread, NULL, "sdlaudio", 4096, 0, 0);
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
