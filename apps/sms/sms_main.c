/*
 * ArgonOS - Master System player (SMS Plus GX core).
 *
 *   python tools/mkaxe.py ... -o SMS.AXE (see apps/sms/README.md)
 *   run t:\sms.axe t:\rom.sms
 *   run t:\sms.axe 60 nolivepad wav   # PSG → t:\sms.wav
 *   run t:\sms.axe 60 nolivepad mock  # PSG, discard samples
 *
 * Core: GPLv2+ (SMS Plus GX).  This file: Apache-2.0.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/keys.h>

#include "shared.h"
#include "sms_cfg.h"
#include "sound_output.h"

#ifdef ARGON_SMS_SMALL
/*
 * The CYD build: 16 KB of stack (10 overflowed on Sonic) and a small arena.  What is needed only
 * sometimes - the cartridge's RAM (32 KB, for the games that have it) and the
 * built-in ROM when no cartridge is named (16 KB) - comes from the system heap
 * when it is asked for (sms_alloc_internal) instead of being reserved at start
 * by every game.  The cartridge itself is read out of flash and costs nothing.
 */
AG_APP_SIZED("SMS", "1.0", "argon", AG_AXE_NEEDS_GFX, 16 * 1024, 13 * 1024);
#else
AG_APP_SIZED("SMS", "1.0", "argon", AG_AXE_NEEDS_GFX, 32 * 1024, 2 * 1024 * 1024);
#endif

/* Built-in tiny ROM: clear VRAM-ish loop so a missing cart still draws something.
 * Six bytes, padded to a 16 KB page by load_rom_mem: on an ESP32 a constant is
 * in RAM like everything else in the data part. */
static const uint8_t k_tiny_rom[] = {
    0xF3,                   /* di */
    0x31, 0xF0, 0xDF,       /* ld sp,$DFF0 */
    0x18, 0xFE,             /* jr $ */
};

static sms_cfg_t s_cfg;

/*
 * The emulator renders straight into the gfx back buffer: bitmap.pitch is the
 * display stride and bitmap.data points at where line 0 lands.  There is no
 * intermediate frame and no blit - the old path copied 96 KB per frame for
 * nothing, and then flushed all 640x400 on top of that.
 *
 * Room for 224 lines is reserved even though most games run 192, because the
 * VDP can switch to the taller mode at runtime while the origin cannot move:
 * render_reset() clears pitch x height bytes from it once, at power-on.
 */
#define SMS_MAX_LINES 224
static int          s_ox;
#ifdef ARGON_SMS_SMALL
/*
 * One band of the picture on its way to the panel (render.h): the CYD has no
 * frame to draw into.  8 lines are 4 KB and twenty-four presents a frame.
 */
static uint16_t    *s_band;      /* the arena's: bands_alloc() */
static uint16_t    *s_bands_mem; /* SMS_RING bands, or one */
static int          s_bands_n;
static uint32_t     s_present_us;
/* What each band on the glass was, so an unchanged one is not sent again. */
static uint32_t     s_band_hash[(SMS_MAX_LINES + ARGON_SMS_BAND - 1) / ARGON_SMS_BAND];
static int          s_band_valid;
static uint32_t     s_bands_skipped;

/*
 * The SPI on another core.
 *
 * Sending a band is the driver pushing 4 KB down a 40 MHz bus with the
 * processor waiting on it - 1.6 ms a band, 15 ms for a picture that scrolls,
 * which is most of a frame.  The CYD has two cores and the system's is nearly
 * idle, and each has its own flash cache.  So the lines are drawn here, on the
 * application core, into a ring of three bands, and a thread on the system
 * core sends them.  The lines come out spread over the whole frame - the VDP
 * draws a line as the Z80 reaches it - so three are enough for the drawing
 * not to wait on the bus: measured, not hoped (s_ring_wait_us).
 *
 * The ring is the process arena's (12 KB, free whenever a cartridge is mapped
 * rather than copied); without it the bands go out the old way, here.
 *
 * Counters, not indices: s_ring_put bands published, s_ring_got sent.  The one
 * being drawn is s_ring_put % SMS_RING, and it may not be one still waiting.
 */
#define SMS_RING 3
static uint16_t         *s_ring[SMS_RING];
static uint16_t          s_ring_y[SMS_RING];
static uint16_t          s_ring_rows[SMS_RING];
static volatile uint32_t s_ring_put;
static volatile uint32_t s_ring_got;
static volatile int      s_ring_run;
static ag_thread_t       s_presenter;
static int               s_async;
static uint32_t          s_ring_wait_us;
/* What the glass said: bands it took, and the last refusal if it refused. */
static volatile uint32_t s_present_ok, s_present_bad;
static volatile int32_t  s_present_err;

/*
 * Which bands this frame draws, when the bus cannot take them all.
 *
 * The emulation never waits for the glass: a band is drawn only if the ring
 * has room for it when its first line comes up, and a frame that meets a full
 * ring stops there.  The next frame starts from the band where this one
 * stopped (s_cursor), so the picture is renewed top to bottom continuously at
 * whatever rate the bus allows, instead of the top half every frame and the
 * bottom never.  A game keeps its own time; the picture tears when it scrolls
 * faster than the bus, which is the trade a slow screen has always made.
 */
static int32_t  s_cursor;       /* first band the next frame should send */
static int32_t  s_from;         /* first band this frame sends */
static int      s_frame_open;   /* still accepting bands this frame */
static int      s_line_on = 1;  /* the current band is being drawn */
static uint32_t s_bands_dropped;
#endif
static int          s_oy;
static ag_gfxinfo_t s_gi;
static uint16_t    *s_frame; /* offscreen when sms.cfg fullscreen=1 */
static int          s_fullscreen;

/*
 * Preferred input: kernel pad layer (HostFS PADPUSH → inp->btnp).  Real level
 * state, so diagonals and move+fire work with no per-frame file I/O.
 *
 * `nolivepad` forces serial KEY_DOWN sticky keys.  Terminals have no KEY_UP,
 * so that path is a degraded reserve only.
 */
static int s_use_live_pad = 1;
static int s_quit;

#define PAD_HOLD_MS 150u /* matches console sticky TTL */
static uint32_t s_pad_until[2][6]; /* millis deadline per bit */
static uint8_t  s_pause_ttl;

static const uint8_t k_act_bit[SMS_ACT_COUNT] = {
    INPUT_UP, INPUT_DOWN, INPUT_LEFT, INPUT_RIGHT,
    INPUT_BUTTON1, INPUT_BUTTON2, 0, 0,
};

static int arg_is_livepad(const char *s)
{
    if (s == NULL) {
        return 0;
    }
    const char *a = "livepad";
    for (; *a && *s; a++, s++) {
        char ca = *a;
        char cb = *s;
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return *a == '\0' && *s == '\0';
}

static int arg_is_nolivepad(const char *s)
{
    if (s == NULL) {
        return 0;
    }
    const char *a = "nolivepad";
    for (; *a && *s; a++, s++) {
        char ca = *a;
        char cb = *s;
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return *a == '\0' && *s == '\0';
}

static int arg_is_mock(const char *s)
{
    if (s == NULL) {
        return 0;
    }
    const char *a = "mock";
    for (; *a && *s; a++, s++) {
        char ca = *a;
        char cb = *s;
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return *a == '\0' && *s == '\0';
}

static int arg_is_sound(const char *s)
{
    if (s == NULL) {
        return 0;
    }
    const char *a = "sound";
    for (; *a && *s; a++, s++) {
        char ca = *a;
        char cb = *s;
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return *a == '\0' && *s == '\0';
}

static int arg_is_wav(const char *s)
{
    if (s == NULL) {
        return 0;
    }
    const char *a = "wav";
    for (; *a && *s; a++, s++) {
        char ca = *a;
        char cb = *s;
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return *a == '\0' && *s == '\0';
}

static int arg_eq(const char *s, const char *lit)
{
    if (s == NULL) {
        return 0;
    }
    for (; *lit && *s; lit++, s++) {
        char cb = *s;
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (*lit != cb) {
            return 0;
        }
    }
    return *lit == '\0' && *s == '\0';
}

/*
 * "% realtime" is the number that decides whether this is playable: one NTSC
 * frame is 16667 us of guest time, so spending that much on it means keeping up
 * exactly.  Everything here is wall time on the emulator's own work - the pacing
 * sleep is deliberately outside it.
 */
static void report_stats(const char *tag, uint32_t frames, uint64_t span_us,
                         uint64_t work_sum, uint64_t emu_sum,
                         uint64_t present_sum, uint64_t work_max)
{
    if (frames == 0u || span_us == 0u) {
        return;
    }
    const unsigned avg = (unsigned)(work_sum / frames);
    const unsigned emu = (unsigned)(emu_sum / frames);
    const unsigned pres = (unsigned)(present_sum / frames);
    const unsigned fps = (unsigned)((uint64_t)frames * 1000000u / span_us);
    const unsigned pct = (avg > 0u) ? (unsigned)(1666700u / avg) : 0u;
    ag_printf("%s: %u fps, work %u us (emu %u, show %u), max %u us, "
              "%u%% realtime\n",
              tag, fps, avg, emu, pres, (unsigned)work_max, pct);
}

static int load_cart(const char *path)
{
#ifdef ARGON_SMS_SMALL
    if (path != NULL && path[0] != '\0') {
        const void *ptr = NULL;
        uint64_t    len = 0;
        const ag_time_t t0 = ag_micros();
        if (!AG_HAS(ag_api()->fs, map)) {
            ag_printf("sms: this kernel cannot map a file (needs ABI 0.32)\n");
            return 0;
        }
        const ag_err_t err = ag_map(path, &ptr, &len);
        if (err != AG_OK) {
            ag_printf("sms: %s: %s\n", path, ag_strerror(err));
            return 0;
        }
        ag_printf("sms: %s, %u KB, staged in flash in %u ms\n", path,
                  (unsigned)(len / 1024u),
                  (unsigned)((ag_micros() - t0) / 1000u));
        return (int)load_rom_mapped((const uint8_t *)ptr, (size_t)len);
    }
#endif
    if (path == NULL || path[0] == '\0') {
        return (int)load_rom_mem((const char *)k_tiny_rom, sizeof(k_tiny_rom));
    }

    const ag_handle_t h = ag_open(path, AG_O_RDONLY);
    if (h < 0) {
        ag_printf("rom open failed: %s\n", path);
        return (int)load_rom_mem((const char *)k_tiny_rom, sizeof(k_tiny_rom));
    }

    const int64_t sz = ag_seek(h, 0, AG_SEEK_END);
    ag_seek(h, 0, AG_SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) {
        ag_close(h);
        return 0;
    }

    uint8_t *buf = (uint8_t *)ag_malloc((size_t)sz);
    if (buf == NULL) {
        ag_close(h);
        return 0;
    }
    size_t got = 0;
    while (got < (size_t)sz) {
        const int32_t n = ag_read(h, buf + got, (size_t)sz - got);
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    ag_close(h);

    const int ok = (int)load_rom_mem((const char *)buf, got);
    ag_free(buf);
    return ok;
}

/* Aim the emulator at the acquired framebuffer (or an offscreen for scale).
 * Must run before system_poweron(), which clears the bitmap through these. */
static void bind_frame_to_fb(const ag_gfxinfo_t *info)
{
    s_gi = *info;
#ifdef ARGON_SMS_SMALL
    s_ox = 0;
    s_oy = 0;
    sms_bitmap = s_band;
    memset(&bitmap, 0, sizeof(bitmap));
    bitmap.data = (uint8_t *)s_band;
    bitmap.width = VIDEO_WIDTH_SMS;
    bitmap.height = ARGON_SMS_BAND;
    bitmap.pitch = VIDEO_WIDTH_SMS * (int)sizeof(uint16_t);
    bitmap.depth = 16;
    bitmap.viewport.w = VIDEO_WIDTH_SMS;
    bitmap.viewport.h = VIDEO_HEIGHT_SMS;
    s_band_valid = 0; /* the glass may have been drawn over */
    if (s_async) {
        bitmap.data = (uint8_t *)s_ring[s_ring_put % SMS_RING];
    }
    return;
#endif
    uint8_t *origin;
    int32_t  pitch;

    if (s_fullscreen && s_frame != NULL) {
        s_ox = 0;
        s_oy = 0;
        origin = (uint8_t *)s_frame;
        pitch = (int32_t)(VIDEO_WIDTH_SMS * (int)sizeof(uint16_t));
    } else {
        s_fullscreen = 0;
        s_ox = ((int)info->width > VIDEO_WIDTH_SMS)
                   ? ((int)info->width - VIDEO_WIDTH_SMS) / 2
                   : 0;
        s_oy = ((int)info->height > SMS_MAX_LINES)
                   ? ((int)info->height - SMS_MAX_LINES) / 2
                   : 0;
        origin = (uint8_t *)info->fb + (size_t)s_oy * info->stride +
                 (size_t)s_ox * sizeof(uint16_t);
        pitch = (int32_t)info->stride;
    }

    sms_bitmap = (uint16_t *)origin;
    memset(&bitmap, 0, sizeof(bitmap));
    bitmap.data = origin;
    bitmap.width = VIDEO_WIDTH_SMS;
    bitmap.height = s_fullscreen ? (uint32_t)SMS_MAX_LINES : VIDEO_HEIGHT_SMS;
    bitmap.pitch = pitch;
    bitmap.depth = 16;
    bitmap.viewport.w = VIDEO_WIDTH_SMS;
    bitmap.viewport.h = VIDEO_HEIGHT_SMS;
}

/* Nearest-neighbor stretch of the SMS viewport onto the soft display. */
static void present_fullscreen(void)
{
    if (s_gi.fb == NULL || s_frame == NULL) {
        return;
    }
    const int sw = VIDEO_WIDTH_SMS;
    int       sh = (int)bitmap.viewport.h;
    if (sh <= 0) {
        sh = VIDEO_HEIGHT_SMS;
    }
    if (sh > SMS_MAX_LINES) {
        sh = SMS_MAX_LINES;
    }
    const int dw = (int)s_gi.width;
    const int dh = (int)s_gi.height;
    if (dw <= 0 || dh <= 0) {
        return;
    }

    const uint32_t x_step = ((uint32_t)sw << 16) / (uint32_t)dw;
    const uint32_t y_step = ((uint32_t)sh << 16) / (uint32_t)dh;
    uint32_t       y_acc = 0;
    for (int y = 0; y < dh; y++) {
        const int       sy = (int)(y_acc >> 16);
        const uint16_t *src = s_frame + (size_t)sy * (size_t)sw;
        uint16_t       *dst =
            (uint16_t *)((uint8_t *)s_gi.fb + (size_t)y * s_gi.stride);
        uint32_t x_acc = 0;
        for (int x = 0; x < dw; x++) {
            dst[x] = src[x_acc >> 16];
            x_acc += x_step;
        }
        y_acc += y_step;
    }
    ag_gfx_flush(0, 0, s_gi.width, s_gi.height);
}

#ifdef ARGON_SMS_SMALL
static void band_present(const uint16_t *px, uint16_t y0, uint16_t rows);

static int32_t band_count(void)
{
    return ((int32_t)bitmap.viewport.h + ARGON_SMS_BAND - 1) / ARGON_SMS_BAND;
}

static void band_sent(int32_t band)
{
    s_cursor = (band + 1 >= band_count()) ? 0 : band + 1;
}

int sms_line_wanted(int32_t line)
{
    if (!s_async) {
        return 1;
    }
    const int32_t row = line % ARGON_SMS_BAND;
    const int32_t band = line / ARGON_SMS_BAND;
    if (row == 0) {
        if (band == 0) {
            s_from = s_cursor;
            s_frame_open = 1;
        }
        s_line_on = s_frame_open && band >= s_from &&
                    (s_ring_put - s_ring_got) < SMS_RING;
        if (!s_line_on && s_frame_open && band >= s_from) {
            s_frame_open = 0; /* the bus is full: resume from here next frame */
            s_cursor = band;
            s_bands_dropped++;
        } else if (!s_line_on && band >= s_from) {
            s_bands_dropped++;
        }
    }
    return s_line_on;
}

/*
 * A line has gone into the band; send the band when it is full or the picture
 * ends.  The driver centres a surface of the picture's size on the glass.
 */
void sms_line_done(int32_t line)
{
    static uint32_t s_acc;
    const int32_t h = (int32_t)bitmap.viewport.h;
    const int32_t row = line % ARGON_SMS_BAND;

    /*
     * A band that is what the glass already shows is not sent again.  The SPI
     * is the slowest thing in the frame - 98 KB at 40 MHz is 19.6 ms, more than
     * the whole emulation - and most of a frame is usually unchanged: a score
     * line, a still background, a menu.  The hash costs nothing extra: render.c
     * folds each line's into sms_line_hash while converting it.  It is reset
     * when the glass may have been drawn over (focus regained), so a band is
     * never skipped against a picture that is no longer there.
     */
    s_acc = ((row == 0) ? 0x9e3779b9u : ((s_acc << 7) | (s_acc >> 25))) ^ sms_line_hash;
    if (row != ARGON_SMS_BAND - 1 && line != h - 1) {
        return;
    }
    {
        const uint32_t hash = s_acc;
        const int32_t  band = line / ARGON_SMS_BAND;
        if (band < (int32_t)(sizeof(s_band_hash) / sizeof(s_band_hash[0]))) {
            if (s_band_valid && s_band_hash[band] == hash) {
                s_bands_skipped++;
                band_sent(band);
                return;
            }
            s_band_hash[band] = hash;
        }
    }
    if (s_async) {
        /* Published; the system core sends it.  Wait only if the next band
         * to draw into has not gone out yet. */
        const uint32_t slot = s_ring_put % SMS_RING;
        s_ring_y[slot] = (uint16_t)(line - row);
        s_ring_rows[slot] = (uint16_t)(row + 1);
        __sync_synchronize();
        s_ring_put++;
        band_sent(line / ARGON_SMS_BAND);
        /*
         * No waiting here for the next slot: the next band is drawn only if
         * its slot is free when its first line comes up (sms_line_wanted),
         * and if it is not, nothing is written to it.
         */
        bitmap.data = (uint8_t *)s_ring[s_ring_put % SMS_RING];
        return;
    }
    band_present(s_band, (uint16_t)(line - row), (uint16_t)(row + 1));
}

/*
 * One band to the glass, from whichever core sends it.
 */
static void band_present(const uint16_t *px, uint16_t y0, uint16_t rows)
{
    const int32_t h = (int32_t)bitmap.viewport.h;
    /*
     * The surface is the whole glass, with the picture placed in its middle
     * here, rather than a 256x192 surface the driver would scale.  The CYD's
     * driver fits a surface to the panel by the largest ratio that fits -
     * five quarters for this one - and 320x240 of scaled pixels is 41 ms of
     * SPI a frame, measured; one to one is 98 KB instead of 154 and no column
     * map.  A Master System picture was never meant to fill a 4:3 tube anyway.
     */
    const uint16_t sw = (s_gi.width >= VIDEO_WIDTH_SMS) ? s_gi.width
                                                        : VIDEO_WIDTH_SMS;
    const uint16_t sh = (s_gi.height >= (uint32_t)h) ? s_gi.height
                                                     : (uint16_t)h;
    const ag_blit_t b = {
        .px = px,
        .stride = VIDEO_WIDTH_SMS * sizeof(uint16_t),
        .surf_w = sw,
        .surf_h = sh,
        .x = (uint16_t)((sw - VIDEO_WIDTH_SMS) / 2u),
        .y = (uint16_t)((sh - h) / 2u + y0),
        .w = VIDEO_WIDTH_SMS,
        .h = rows,
    };
    const ag_time_t t0 = ag_micros();
    const ag_err_t perr = ag_gfx_present(&b);
    if (perr == AG_OK) {
        s_present_ok++;
    } else {
        s_present_bad++;
        s_present_err = perr;
    }
    if (!s_async) {
        s_present_us += (uint32_t)(ag_micros() - t0);
    }
}

static void presenter(void *arg)
{
    (void)arg;
    while (s_ring_run) {
        if (s_ring_got != s_ring_put) {
            const uint32_t slot = s_ring_got % SMS_RING;
            __sync_synchronize();
            band_present(s_ring[slot], s_ring_y[slot], s_ring_rows[slot]);
            __sync_synchronize();
            s_ring_got++;
        } else {
            ag_delay(1);
        }
    }
    ag_thread_exit();
}

/*
 * The bands live in the process arena, not in the image: 4 KB of data in one
 * block that the image no longer has to find (the CYD has 90 KB at load, in
 * pieces).  Three for the ring when there is room, one when there is not -
 * then the bands go out from here, as before the ring existed.
 */
static int bands_alloc(void)
{
    const size_t one = (size_t)VIDEO_WIDTH_SMS * ARGON_SMS_BAND * sizeof(uint16_t);
    s_bands_mem = (uint16_t *)ag_malloc((size_t)SMS_RING * one);
    s_bands_n = SMS_RING;
    if (s_bands_mem == NULL) {
        s_bands_mem = (uint16_t *)ag_malloc(one);
        s_bands_n = 1;
    }
    if (s_bands_mem == NULL) {
        ag_printf("sms: no memory for even one band of the picture\n");
        return 0;
    }
    s_band = s_bands_mem;
    return 1;
}

static void async_start(void)
{
    uint16_t *all = s_bands_mem;
    if (s_bands_n < SMS_RING) {
        ag_printf("sms: no arena for the band ring - sending from here\n");
        return;
    }
    for (int i = 0; i < SMS_RING; i++) {
        s_ring[i] = all + (size_t)i * VIDEO_WIDTH_SMS * ARGON_SMS_BAND;
    }
    s_ring_put = s_ring_got = 0;
    s_ring_run = 1;
    s_presenter = ag_thread_create(presenter, NULL, "smsband", 4096, 4,
                                   AG_THREAD_SYS_CORE);
    if (s_presenter == NULL) {
        s_ring_run = 0;
        bitmap.data = (uint8_t *)s_band;
        ag_printf("sms: no thread for the bands - sending from here\n");
        return;
    }
    s_async = 1;
    bitmap.data = (uint8_t *)s_ring[0];
    ag_printf("sms: bands sent from the system core\n");
}

static void async_stop(void)
{
    if (!s_async) {
        return;
    }
    while (s_ring_got != s_ring_put) {
        ag_delay(1);
    }
    s_ring_run = 0;
    (void)ag_thread_join(s_presenter, 1000);
    s_async = 0;
}

void *sms_alloc_internal(size_t bytes)
{
    void *p = ag_malloc_caps(bytes, AG_MEM_FAST);
    if (p == NULL) {
        p = ag_malloc(bytes); /* the arena, if it happens to have room */
    }
    if (p == NULL) {
        ag_meminfo_t mi;
        ag_meminfo(&mi);
        ag_printf("sms: %u bytes refused: internal %u free to this process, "
                  "arena %u of %u free\n",
                  (unsigned)bytes, (unsigned)mi.fast_free,
                  (unsigned)mi.arena_free, (unsigned)mi.arena_total);
    }
    return p;
}

void sms_out_of_memory(const char *what)
{
    ag_printf("sms: no memory for %s\n", what);
    ag_exit(1);
}
#endif

static void present_frame(void)
{
#ifdef ARGON_SMS_SMALL
    return; /* the bands went out as the lines were drawn */
#endif
    if (s_fullscreen) {
        present_fullscreen();
        return;
    }
    ag_gfx_flush((uint16_t)s_ox, (uint16_t)s_oy, VIDEO_WIDTH_SMS,
                 (uint16_t)bitmap.viewport.h);
}

static void refresh_held(int pad)
{
    const uint32_t until = ag_millis() + PAD_HOLD_MS;
    for (unsigned i = 0; i < 6u; i++) {
        if ((input.pad[pad] & (uint8_t)(1u << i)) != 0u) {
            s_pad_until[pad][i] = until;
        }
    }
}

static void apply_action(int pad, int act, bool down)
{
    if (act == SMS_ACT_PAUSE) {
        if (down) {
            s_pause_ttl = 3;
        }
        return;
    }
    if (act == SMS_ACT_QUIT) {
        if (down) {
            ag_exit(0);
        }
        return;
    }
    if (act < 0 || act > SMS_ACT_B2 || pad < 0 || pad > 1) {
        return;
    }

    const uint8_t bit = k_act_bit[act];
    unsigned      idx = 0;
    for (uint8_t t = bit; t > 1u; t >>= 1) {
        idx++;
    }
    if (down) {
        /* Opposing directions cancel so Left+Right cannot both stick. */
        if (bit == INPUT_UP) {
            input.pad[pad] &= (uint8_t)~INPUT_DOWN;
            s_pad_until[pad][1] = 0;
        } else if (bit == INPUT_DOWN) {
            input.pad[pad] &= (uint8_t)~INPUT_UP;
            s_pad_until[pad][0] = 0;
        } else if (bit == INPUT_LEFT) {
            input.pad[pad] &= (uint8_t)~INPUT_RIGHT;
            s_pad_until[pad][3] = 0;
        } else if (bit == INPUT_RIGHT) {
            input.pad[pad] &= (uint8_t)~INPUT_LEFT;
            s_pad_until[pad][2] = 0;
        }
        input.pad[pad] |= bit;
        /* Autorepeat only refreshes the last key — keep every held bit alive. */
        refresh_held(pad);
        if (idx < 6u) {
            s_pad_until[pad][idx] = ag_millis() + PAD_HOLD_MS;
        }
    } else {
        input.pad[pad] &= (uint8_t)~bit;
        if (idx < 6u) {
            s_pad_until[pad][idx] = 0;
        }
    }
}

/* FOCUS_GAINED: reclaim gfx after the kernel force-released it. */
static int handle_session_ev(const ag_event_t *ev)
{
    if (ev->type == AG_EV_FOCUS_GAINED) {
        ag_gfxinfo_t info;
        if (ag_gfx_acquire(&info) == AG_OK) {
            /*
             * acquire() seeds the back buffer from the front (often the other
             * slot's console/FM).  Clear + present so letterbox / scale wipe it.
             */
            ag_gfx_clear(0x00000000u);
            bind_frame_to_fb(&info);
            present_frame();
        }
        return 1;
    }
    if (ev->type == AG_EV_FOCUS_LOST) {
        return 1;
    }
    if (ev->type == AG_EV_QUIT) {
        if (ag_focused()) {
            s_quit = 1;
        }
        return 1;
    }
    return 0;
}

static void poll_pad_events(void)
{
    ag_event_t ev;
    while (ag_poll_event(&ev, 0)) {
        if (handle_session_ev(&ev)) {
            continue;
        }
        if (!ag_focused()) {
            continue;
        }
        if (ev.type != AG_EV_KEY_DOWN && ev.type != AG_EV_KEY_UP) {
            continue;
        }
        int pad = 0;
        int act = 0;
        if (!sms_cfg_lookup(&s_cfg, ev.key.keycode, &pad, &act)) {
            continue;
        }
        apply_action(pad, act, ev.type == AG_EV_KEY_DOWN);
    }

    if (s_pause_ttl > 0u) {
        input.system |= (uint8_t)INPUT_PAUSE;
        s_pause_ttl--;
    } else {
        input.system &= (uint8_t)~INPUT_PAUSE;
    }

    const uint32_t now = ag_millis();
    for (int pad = 0; pad < 2; pad++) {
        for (unsigned i = 0; i < 6u; i++) {
            if (s_pad_until[pad][i] == 0u) {
                continue;
            }
            if ((int32_t)(now - s_pad_until[pad][i]) >= 0) {
                s_pad_until[pad][i] = 0;
                input.pad[pad] &= (uint8_t)~(1u << i);
            }
        }
    }
}

static void poll_pad_live(void)
{
    static const int     k_ids[6] = {AG_BTN_UP,    AG_BTN_DOWN, AG_BTN_LEFT,
                                     AG_BTN_RIGHT, AG_BTN_B1,   AG_BTN_B2};
    static const uint8_t k_bits[6] = {INPUT_UP,    INPUT_DOWN, INPUT_LEFT,
                                      INPUT_RIGHT, INPUT_BUTTON1, INPUT_BUTTON2};
    ag_event_t ev;
    while (ag_poll_event(&ev, 0)) {
        (void)handle_session_ev(&ev);
    }
    if (!ag_focused()) {
        input.pad[0] = 0;
        input.pad[1] = 0;
        input.system &= (uint8_t)~(INPUT_PAUSE);
        return;
    }
    uint8_t p0 = 0;
    uint8_t p1 = 0;
    for (unsigned i = 0; i < 6u; i++) {
        if (ag_btnp(0, k_ids[i])) {
            p0 |= k_bits[i];
        }
        if (ag_btnp(1, k_ids[i])) {
            p1 |= k_bits[i];
        }
    }
    input.pad[0] = p0;
    input.pad[1] = p1;
    if (ag_btnp(0, AG_BTN_PAUSE) || ag_btnp(1, AG_BTN_PAUSE)) {
        input.system |= (uint8_t)INPUT_PAUSE;
    } else {
        input.system &= (uint8_t)~INPUT_PAUSE;
    }
    if (ag_btnp(0, AG_BTN_QUIT) || ag_btnp(1, AG_BTN_QUIT)) {
        s_quit = 1;
    }
}

static void poll_pad(void)
{
    if (s_use_live_pad) {
        poll_pad_live();
        return;
    }
    poll_pad_events();
}

static int parse_frames(const char *s, int fallback)
{
    if (s == NULL || s[0] < '0' || s[0] > '9') {
        return fallback;
    }
    int frames = 0;
    for (const char *p = s; *p >= '0' && *p <= '9'; p++) {
        frames = frames * 10 + (*p - '0');
    }
    return frames > 0 ? frames : fallback;
}

static int looks_like_frames_only(const char *s)
{
    if (s == NULL || s[0] == '\0') {
        return 0;
    }
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
    }
    return 1;
}

static int looks_like_wav_path(const char *s)
{
    size_t n = 0;
    if (s == NULL) {
        return 0;
    }
    while (s[n]) {
        n++;
    }
    if (n < 5) {
        return 0;
    }
    return (s[n - 4] == '.') &&
           (s[n - 3] == 'w' || s[n - 3] == 'W') &&
           (s[n - 2] == 'a' || s[n - 2] == 'A') &&
           (s[n - 1] == 'v' || s[n - 1] == 'V');
}

static int looks_like_sound_target(const char *s)
{
    if (s == NULL || s[0] == '\0') {
        return 0;
    }
    if (looks_like_wav_path(s)) {
        return 1;
    }
    /* Device paths only — do not steal POSIX ROM paths like /sys/Sega/x.sms. */
    if ((s[0] == 'd' || s[0] == 'D') && s[1] == ':') {
        return 1;
    }
    if (s[0] == '/' &&
        (s[1] == 'd' || s[1] == 'D') &&
        (s[2] == 'e' || s[2] == 'E') &&
        (s[3] == 'v' || s[3] == 'V') &&
        (s[4] == '/' || s[4] == '\0')) {
        return 1;
    }
    return 0;
}

int ag_main(int argc, char **argv)
{
    /*
     * The system cruises at a lower clock while nothing says otherwise, and
     * this does: the arithmetic below is paced by a wall clock and does not fit
     * in two thirds of the speed.  Said in the first line rather than answered
     * later, because the first buffer is as real as the thousandth.
     */
    (void)ag_power_declare(AG_POWER_FIT_FULL_ONLY,
                           "60 frames a second");

    const char *rom = NULL;
    /* <0 = run until Esc/Q. Explicit N frames still works for benches. */
    int         frames = -1;
    int         want_livepad = 1; /* default: try host push pad */
    int         force_sticky = 0;
    int         want_sound = 0;
    const char *wav_path = "mock";
    int         present_div = 1; /* frames of emulation per frame shown */
    int         div_forced = 0;  /* fps30/fps60 named: do not adjust it */
    int         stats = 0;

    for (int i = 1; i < argc; i++) {
        if (arg_eq(argv[i], "fps30")) {
            present_div = 2;
            div_forced = 1;
            continue;
        }
        if (arg_eq(argv[i], "fps60")) {
            present_div = 1;
            div_forced = 1;
            continue;
        }
        if (arg_eq(argv[i], "stats")) {
            stats = 1;
            continue;
        }
        if (arg_is_nolivepad(argv[i])) {
            force_sticky = 1;
            want_livepad = 0;
            continue;
        }
        if (arg_is_livepad(argv[i])) {
            want_livepad = 1;
            continue;
        }
        if (arg_is_mock(argv[i]) || arg_is_sound(argv[i])) {
            want_sound = 1;
            wav_path = "pcmnull";
            continue;
        }
        if (arg_eq(argv[i], "pcmnull") || arg_eq(argv[i], "pcmvirt") ||
            arg_eq(argv[i], "pcmmix") || arg_eq(argv[i], "mix") ||
            arg_eq(argv[i], "net") || arg_eq(argv[i], "tcp") ||
            arg_eq(argv[i], "audio") || arg_eq(argv[i], "i2s") ||
            arg_eq(argv[i], "pcm0")) {
            want_sound = 1;
            wav_path = argv[i];
            continue;
        }
        if (arg_is_wav(argv[i])) {
            want_sound = 1;
            if (!looks_like_wav_path(wav_path)) {
                wav_path = "t:\\sms.wav";
            }
            continue;
        }
        if (looks_like_sound_target(argv[i])) {
            want_sound = 1;
            wav_path = argv[i];
            continue;
        }
        if (rom == NULL && !looks_like_frames_only(argv[i])) {
            rom = argv[i];
            continue;
        }
        if (looks_like_frames_only(argv[i])) {
            frames = parse_frames(argv[i], -1);
        }
    }

    memset(&option, 0, sizeof(option));
    option.console = 2; /* SMS2 (tiny built-in ROM has no TMR SEGA header) */
    option.country = 1; /* USA / NTSC */
    option.fm = want_sound ? 1 : 0; /* OPLL regs → ag_fm lite synth */
    option.nosound = want_sound ? 0 : 1;
    option.soundlevel = 2;
    option.spritelimit = 1;

    char cfg_from[AG_PATH_MAX];
    const char *exe = (argc > 0) ? argv[0] : NULL;
    sms_cfg_load(&s_cfg, rom, exe, cfg_from, sizeof(cfg_from));
    s_fullscreen = s_cfg.fullscreen ? 1 : 0;
#ifdef ARGON_SMS_SMALL
    s_fullscreen = 0; /* no frame to stretch from */
    if (!AG_HAS(ag_api()->gfx, present)) {
        ag_printf("sms: this kernel cannot show an application's own pixels "
                  "(needs ABI 0.31)\n");
        return 1;
    }
#endif
    if (s_fullscreen) {
        const size_t nbytes =
            (size_t)VIDEO_WIDTH_SMS * (size_t)SMS_MAX_LINES * sizeof(uint16_t);
        s_frame = (uint16_t *)ag_malloc(nbytes);
        if (s_frame == NULL) {
            ag_printf("sms: fullscreen buffer alloc failed; using 1:1\n");
            s_fullscreen = 0;
        }
    }

    /* Graphics: 1:1 draws into the gfx back buffer; fullscreen uses s_frame. */
#ifdef ARGON_SMS_SMALL
    if (!bands_alloc()) {
        return 1;
    }
#endif
    if (ag_gfx_acquire(&s_gi) != AG_OK) {
        ag_printf("gfx acquire failed\n");
        if (s_frame != NULL) {
            ag_free(s_frame);
            s_frame = NULL;
        }
        return 1;
    }
    ag_gfx_clear(0x00000000u);
    bind_frame_to_fb(&s_gi);

    s_use_live_pad = (want_livepad && !force_sticky) ? 1 : 0;
    if (cfg_from[0] != '\0') {
        ag_printf("sms: cfg %s\n", cfg_from);
    } else {
        ag_printf("sms: cfg (defaults)\n");
    }
    if (s_use_live_pad) {
        ag_printf("sms: controls = live pad (inp / HostFS PADPUSH)\n");
    } else {
        ag_printf("sms: controls = serial sticky (no key-up)\n");
    }
    if (s_fullscreen) {
        ag_printf("sms: video = fullscreen (nearest stretch)\n");
    } else {
        ag_printf("sms: video = 1:1 centered\n");
    }

    if (!load_cart(rom)) {
        ag_printf("failed to load rom\n");
        ag_gfx_release();
        if (s_frame != NULL) {
            ag_free(s_frame);
            s_frame = NULL;
        }
        return 1;
    }

    if (want_sound) {
        Sound_SetPath(wav_path);
    }
    system_poweron();
#ifdef ARGON_SMS_SMALL
    async_start();
#endif
    if (want_sound) {
        Sound_Init();
    }

    if (s_fullscreen) {
        ag_printf("sms: %ux%u -> %ux%u fullscreen, present every %d frame(s)\n",
                  (unsigned)VIDEO_WIDTH_SMS, (unsigned)VIDEO_HEIGHT_SMS,
                  (unsigned)s_gi.width, (unsigned)s_gi.height, present_div);
    } else {
        ag_printf("sms: %ux%u at %d,%d, present every %d frame(s)\n",
                  (unsigned)VIDEO_WIDTH_SMS, (unsigned)VIDEO_HEIGHT_SMS, s_ox,
                  s_oy, present_div);
    }

    /*
     * Emulation always runs at the guest's 60 Hz - the Z80 and the sound chips
     * have to, or the game plays slowly rather than less smoothly.  The divider
     * only skips rasterising and showing, which is where the cost is.
     */
    uint64_t work_sum = 0; /* emu + present, no pacing sleep */
    uint64_t emu_sum = 0;
    uint64_t present_sum = 0;
    uint64_t work_max = 0;
    uint32_t window = 0;
    ag_time_t window_t0 = ag_micros();
    int       ran = 0;

    for (; !s_quit && (frames < 0 || ran < frames);) {
        poll_pad();
        if (s_quit) {
            break;
        }
        if (!ag_focused()) {
            ag_heartbeat();
            ag_delay(50);
            continue;
        }

        const uint32_t  t0 = ag_millis();
        const ag_time_t f0 = ag_micros();

        const int show = (present_div <= 1) || ((ran % present_div) == 0);
#ifdef ARGON_SMS_SMALL
        s_present_us = 0;
#endif
        system_frame(show ? 0 : 1);
#ifdef ARGON_SMS_SMALL
        if (show) {
            /*
             * Once a second every band goes again, changed or not.  The glass
             * is not only ours: the console paints on it - a printed line,
             * the loading view - and an unchanged band is then not what the
             * glass shows.  Measured: with `stats` printing every two seconds
             * Sonic's title stayed under the text, because nothing on it moved
             * and so nothing was sent.  24 bands a second is 4% of the bus.
             */
            s_band_valid = ((ran % 60) != 0);
        }
#endif

        const ag_time_t f1 = ag_micros();
        if (show) {
            present_frame();
        }
        const ag_time_t f2 = ag_micros();

        const uint64_t work = (uint64_t)(f2 - f0);
        work_sum += work;
#ifdef ARGON_SMS_SMALL
        /* the bands are sent from inside the frame, as lines are drawn */
        emu_sum += (uint64_t)(f1 - f0) - s_present_us;
        present_sum += (uint64_t)(f2 - f1) + s_present_us;
#else
        emu_sum += (uint64_t)(f1 - f0);
        present_sum += (uint64_t)(f2 - f1);
#endif
        if (work > work_max) {
            work_max = work;
        }
        window++;
        ran++;

#ifdef ARGON_SMS_SMALL
        /*
         * How often a frame is shown, decided by measuring, as GB does: the
         * game must keep its own time, and the picture is what gives.  Every
         * two seconds: below 97% of real time, show one frame in one more;
         * at real time for two windows running, one fewer.  Four is the floor
         * of what still reads as motion - fifteen pictures a second.
         */
        {
            static uint32_t  a_frames, a_good;
            static ag_time_t a_t0;
            if (a_t0 == 0) {
                a_t0 = f2;
            }
            a_frames++;
            if (!div_forced && (uint64_t)(f2 - a_t0) >= 2000000u) {
                const uint32_t pct = (uint32_t)(((uint64_t)a_frames * 16667u * 100u) /
                                                (uint64_t)(f2 - a_t0));
                if (s_async) {
                    /* bands decide what is drawn; every frame is offered */
                } else if (pct < 97u && present_div < 4) {
                    present_div++;
                    a_good = 0;
                } else if (pct >= 99u && present_div > 1 && ++a_good >= 2) {
                    present_div--;
                    a_good = 0;
                }
                a_frames = 0;
                a_t0 = f2;
            }
        }
#endif
        if (stats && (uint64_t)(f2 - window_t0) >= 2000000u) {
            report_stats("sms", window, (uint64_t)(f2 - window_t0), work_sum,
                         emu_sum, present_sum, work_max);
#ifdef ARGON_SMS_SMALL
            ag_printf("sms: showing 1 in %d; drawing waited %u us on the bus; "
                      "%u bands left for later\n",
                      present_div, (unsigned)s_ring_wait_us,
                      (unsigned)s_bands_dropped);
            ag_printf("sms: the glass took %u bands, refused %u (last %d)\n",
                      (unsigned)s_present_ok, (unsigned)s_present_bad,
                      (int)s_present_err);
            s_present_ok = s_present_bad = 0;
            s_ring_wait_us = 0;
            s_bands_dropped = 0;
            ag_printf("sms: %u bands of %u not sent, unchanged\n",
                      (unsigned)s_bands_skipped,
                      (unsigned)(window * ((bitmap.viewport.h + ARGON_SMS_BAND - 1) /
                                           ARGON_SMS_BAND)));
            s_bands_skipped = 0;
#endif
#ifdef ARGON_SMS_PROF
            {
                extern uint32_t sms_prof[5];
                const uint32_t  per = window ? window : 1u;
                ag_printf("prof us/frame: bg %u obj %u sat %u remap %u out %u\n",
                          (unsigned)(sms_prof[0] / 240u / per),
                          (unsigned)(sms_prof[1] / 240u / per),
                          (unsigned)(sms_prof[2] / 240u / per),
                          (unsigned)(sms_prof[3] / 240u / per),
                          (unsigned)(sms_prof[4] / 240u / per));
                memset(sms_prof, 0, sizeof(sms_prof));
            }
#endif
            work_sum = emu_sum = present_sum = work_max = 0;
            window = 0;
            window_t0 = f2;
        }

        /* Cap ~60 fps (without QEMU RGB busy-wait the guest ran away). */
        ag_yield();
        const uint32_t dt = ag_millis() - t0;
        if (dt < 16u) {
            ag_delay(16u - dt);
        }
    }
    if (window > 0) {
        report_stats("sms", window, (uint64_t)(ag_micros() - window_t0),
                     work_sum, emu_sum, present_sum, work_max);
    }
    ag_printf("sms: %d frames, cart %u KB crc=%08x\n", ran,
              (unsigned)(cart.size / 1024u), (unsigned)cart.crc);
#ifdef ARGON_SMS_SMALL
    async_stop();
#endif
    Sound_Close();
    ag_gfx_release();
    system_poweroff();
    if (s_frame != NULL) {
        ag_free(s_frame);
        s_frame = NULL;
    }
    return 0;
}
