/*
 * ArgonOS - what ag_gfx_flush costs, and what it is really waiting for.
 *
 * Fallout on the board spends 22 ms a frame in flush, standing still on a map,
 * with no display driver loaded at all.  The frame counter cannot say why: it
 * sees one number for a call that copies a rectangle, walks the device registry
 * under the lock every device in the machine shares, and returns.
 *
 * Three questions, one program.
 *
 *   flushb              the same surface flushed at eight sizes.  A cost that
 *                       follows the pixels is the copy; a cost that is the same
 *                       for one pixel as for the whole screen is per-call
 *                       overhead and is answered somewhere else entirely.  The
 *                       memcpy figures under it say what a copy of those bytes
 *                       *should* cost on this memory.
 *
 *   flushb audio [N]    the same sweep with a second thread streaming sound in
 *                       N-frame writes, which is what the game is also doing.
 *                       Every device write in this system is made under one
 *                       lock (ag_dev_write2), and a flush walks the registry
 *                       under that same lock - so a write that blocks is a
 *                       flush that waits.
 *
 *   flushb mix [N] [s]  the shape of a game instead of a benchmark: flush the
 *                       whole surface about a thousand times a second for s
 *                       seconds while the sound streams, and report the worst
 *                       one rather than the average.  An average hides exactly
 *                       the kind of stall this is looking for.
 *
 * N defaults to 240 frames - one I2S descriptor.  The shim uses 1024, which is
 * more than the whole four-descriptor chain holds; pass it and watch.
 *
 * Build: argon apps --only FLUSHB.AXE
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_APP("FLUSHB", "1.1", "argon", AG_AXE_NEEDS_GFX);

static uint32_t number(const char *s)
{
    uint32_t v = 0;
    if (s == NULL) {
        return 0;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s - '0');
        s++;
    }
    return v;
}

/* Bytes per microsecond is megabytes per second, and neither overflows. */
static void rate(uint32_t bytes, uint32_t us, unsigned *mb, unsigned *frac)
{
    if (us == 0u) {
        *mb = 0;
        *frac = 0;
        return;
    }
    const uint64_t kbs = (uint64_t)bytes * 1000u / us;
    *mb = (unsigned)(kbs / 1000u);
    *frac = (unsigned)(kbs % 1000u);
}

/* ---- the other thing the board is doing --------------------------------- */

#define AUDIO_MAX 1024

static volatile int      s_audio_stop;
static volatile int      s_audio_frames = 240;
static int16_t           s_audio_block[AUDIO_MAX * 2];
static volatile unsigned s_audio_writes;
static volatile unsigned s_audio_worst_us;
static volatile unsigned s_audio_sum_us;

static void audio_thread(void *arg)
{
    (void)arg;
    while (!s_audio_stop) {
        const int n = s_audio_frames;
        if (ag_audio_space() < n) {
            ag_delay(2);
            continue;
        }
        const ag_time_t t0 = ag_micros();
        (void)ag_audio_write(s_audio_block, n);
        const unsigned us = (unsigned)(ag_micros() - t0);
        s_audio_writes++;
        s_audio_sum_us += us;
        if (us > s_audio_worst_us) {
            s_audio_worst_us = us;
        }
    }
    ag_thread_exit();
}

static ag_thread_t audio_start(int frames)
{
    const ag_audio_fmt_t fmt = {.rate = 22050u, .channels = 2, .bits = 16};
    const ag_err_t       err = ag_audio_open(&fmt);

    if (err != AG_OK) {
        ag_printf("audio: %s (measuring without it)\n", ag_strerror(err));
        return NULL;
    }
    s_audio_frames = frames;
    s_audio_stop = 0;

    ag_thread_t t = ag_thread_create(audio_thread, NULL, "sound", 4096, 0, 0);
    if (t == NULL) {
        ag_print("audio: no thread\n");
        ag_audio_close();
        return NULL;
    }
    ag_printf("audio: 22050 Hz stereo, %d-frame writes\n", frames);
    ag_delay(200); /* let it settle into the pace the clock gives it */
    return t;
}

static void audio_stop(ag_thread_t t)
{
    if (t == NULL) {
        return;
    }
    s_audio_stop = 1;
    ag_thread_join(t, 3000);
    ag_audio_close();
    ag_printf("audio: %u writes, %u us each, worst %u us\n", s_audio_writes,
              (s_audio_writes != 0u) ? s_audio_sum_us / s_audio_writes : 0u,
              s_audio_worst_us);
}

/* ---- the sweep ---------------------------------------------------------- */

struct rect {
    const char *what;
    int         x, y, w, h;
    int         calls;
};

static void sweep(uint16_t w, uint16_t h)
{
    const struct rect k_rects[] = {
        {"whole surface", 0, 0, w, h, 20},
        {"half, by rows", 0, 0, w, h / 2, 20},
        {"a quarter    ", 0, 0, w, h / 4, 20},
        {"eight rows   ", 0, 0, w, 8, 20},
        {"one row      ", 0, 0, w, 1, 20},
        {"tall + narrow", w / 4, 0, w / 4, h, 20},
        {"one pixel    ", 0, 0, 1, 1, 20},
        {"empty (w=0)  ", 0, 0, 0, 0, 20},
    };
    const int n_rects = (int)(sizeof(k_rects) / sizeof(k_rects[0]));

    for (int i = 0; i < n_rects; i++) {
        const struct rect *r = &k_rects[i];
        uint32_t           worst = 0;

        ag_gfx_flush((uint16_t)r->x, (uint16_t)r->y, (uint16_t)r->w,
                     (uint16_t)r->h); /* warm */

        const ag_time_t t0 = ag_micros();
        for (int c = 0; c < r->calls; c++) {
            const ag_time_t c0 = ag_micros();
            ag_gfx_flush((uint16_t)r->x, (uint16_t)r->y, (uint16_t)r->w,
                         (uint16_t)r->h);
            const uint32_t one = (uint32_t)(ag_micros() - c0);
            if (one > worst) {
                worst = one;
            }
        }
        const uint32_t us = (uint32_t)(ag_micros() - t0) / (uint32_t)r->calls;
        const uint32_t px =
            (r->w == 0) ? ((uint32_t)w * h) : ((uint32_t)r->w * (uint32_t)r->h);
        unsigned mb = 0, frac = 0;
        rate(px * 2u, us, &mb, &frac);
        ag_printf("  %s %5dx%-4d %6u B  %6u us  worst %6u us  %u.%03u MB/s\n",
                  r->what, (r->w == 0) ? (int)w : r->w,
                  (r->w == 0) ? (int)h : r->h, (unsigned)(px * 2u),
                  (unsigned)us, (unsigned)worst, mb, frac);
    }
}

/* ---- what the memory underneath can do ---------------------------------- */

static void memcpy_baseline(uint16_t w, uint16_t h)
{
    const size_t bytes = (size_t)w * h * 2u;
    uint8_t     *a = (uint8_t *)ag_malloc(bytes);
    uint8_t     *b = (uint8_t *)ag_malloc(bytes);

    if (a == NULL || b == NULL) {
        ag_printf("memcpy baseline: no arena for 2 x %u B\n", (unsigned)bytes);
        ag_free(a);
        ag_free(b);
        return;
    }
    for (size_t i = 0; i < bytes; i++) {
        a[i] = (uint8_t)i;
    }
    const size_t sizes[4] = {bytes, bytes / 2u, bytes / 4u, 640u};
    for (int i = 0; i < 4; i++) {
        const size_t sz = sizes[i];
        const int    calls = 20;

        memcpy(b, a, sz); /* warm */
        const ag_time_t t0 = ag_micros();
        for (int c = 0; c < calls; c++) {
            memcpy(b, a, sz);
        }
        const uint32_t us = (uint32_t)(ag_micros() - t0) / (uint32_t)calls;
        unsigned       mb = 0, frac = 0;
        rate((uint32_t)sz, us, &mb, &frac);
        ag_printf("  memcpy arena  %8u B  %6u us  %u.%03u MB/s\n",
                  (unsigned)sz, (unsigned)us, mb, frac);
    }
    ag_free(a);
    ag_free(b);
}

/* ---- a game's shape rather than a benchmark's --------------------------- */

static void mix(uint16_t w, uint16_t h, uint32_t seconds)
{
    const ag_time_t end = ag_micros() + (ag_time_t)seconds * 1000000u;
    uint32_t        frames = 0, worst = 0, over5 = 0, over20 = 0;
    uint64_t        sum = 0;

    while (ag_micros() < end) {
        const ag_time_t t0 = ag_micros();
        ag_gfx_flush(0, 0, w, h);
        const uint32_t us = (uint32_t)(ag_micros() - t0);
        frames++;
        sum += us;
        if (us > worst) {
            worst = us;
        }
        if (us > 5000u) {
            over5++;
        }
        if (us > 20000u) {
            over20++;
        }
        ag_delay(1); /* somebody else's turn */
    }
    ag_printf("mix: %u flushes, %u us each, worst %u us, "
              "%u over 5 ms, %u over 20 ms\n",
              (unsigned)frames,
              (unsigned)((frames != 0u) ? (sum / frames) : 0u),
              (unsigned)worst, (unsigned)over5, (unsigned)over20);
}

int ag_main(int argc, char **argv)
{
    ag_gfxinfo_t info;

    if (ag_api()->gfx == NULL) {
        ag_print("no gfx in this build\n");
        return 1;
    }

    int      want_audio = 0;
    int      want_mix = 0;
    uint32_t frames = 240;
    uint32_t seconds = 5;
    uint32_t seen = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i] == NULL) {
            continue;
        }
        if (argv[i][0] == 'a' || argv[i][0] == 'A') {
            want_audio = 1;
        } else if (argv[i][0] == 'm' || argv[i][0] == 'M') {
            want_audio = 1;
            want_mix = 1;
        } else if (++seen == 1u) {
            frames = number(argv[i]);
        } else {
            seconds = number(argv[i]);
        }
    }
    if (frames == 0u || frames > AUDIO_MAX) {
        frames = 240;
    }
    if (seconds == 0u) {
        seconds = 5;
    }

    const ag_err_t err = ag_gfx_acquire(&info);
    if (err != AG_OK) {
        ag_printf("acquire: %s\n", ag_strerror(err));
        return 1;
    }
    const uint16_t w = info.width;
    const uint16_t h = info.height;

    ag_printf("FLUSHB %ux%u stride=%u fmt=%d double_buf=%d direct=%d\n",
              (unsigned)w, (unsigned)h, (unsigned)info.stride, (int)info.fmt,
              info.double_buf ? 1 : 0, info.direct ? 1 : 0);

    /* Bands, so nothing is uniform and the panel shows the test ran. */
    ag_gfx_clear(0x00203040u);
    for (int y = 0; y < (int)h; y += 8) {
        ag_gfx_fill_rect(0, (int16_t)y, w, 4, 0x00806040u);
    }

    {
        const ag_time_t t0 = ag_micros();
        for (int i = 0; i < 2000; i++) {
            (void)ag_micros();
        }
        ag_printf("ag_micros: %u ns a call\n",
                  (unsigned)((ag_micros() - t0) * 1000u / 2000u));
    }

    ag_thread_t audio = want_audio ? audio_start((int)frames) : NULL;

    if (want_mix) {
        mix(w, h, seconds);
    } else {
        sweep(w, h);
        memcpy_baseline(w, h);
    }

    audio_stop(audio);
    ag_gfx_release();
    ag_print("FLUSHB done\n");
    return 0;
}
