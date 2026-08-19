/*
 * ArgonOS - the smallest thing that puts pixels on a panel.
 *
 * GFXDEMO exercises everything the soft renderer has, at coordinates chosen
 * for a 640x400 surface.  That makes it a poor first test on a board whose
 * surface is 160x120: when it stops, the question "did the pixels reach the
 * glass" is tangled up with a dozen clipping paths it also just walked.
 *
 * Two modes, and the second exists because of how the first failed.
 *
 *   gfxpix [seconds]   four corner squares and a word, held.  Says whether the
 *                      surface reaches the glass the right way round: a
 *                      mirrored or rotated panel shows up as the wrong corner
 *                      being red.
 *
 *   gfxpix cycle [n]   the whole surface, one flat colour at a time, changing
 *                      every second, n times.  For when "there was no picture"
 *                      has to be told apart from "the panel went black" and
 *                      from "the console text stayed" - three different faults
 *                      that look alike to somebody glancing at a board.  A flat
 *                      colour that changes cannot be mistaken for any of them.
 *
 * Every step prints, and the flushes are timed: a whole 320x240 frame over SPI
 * is tens of milliseconds, while a flush that reached no driver at all is tens
 * of microseconds.  That number says whether the pixels were sent without
 * anybody having to watch the board.
 *
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32-elf-gcc \
 *       --include sdk/include -o build/apps/GFXPIX.AXE apps/gfxpix/gfxpix.c
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/keys.h>

AG_APP("GFXPIX", "1.1", "argon", AG_AXE_NEEDS_GFX);

/* Digits, by hand: the SDK has no atoi and two arguments are not a reason to
 * want one. */
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

/* Flat fill, flushed and timed. */
static void show_flat(uint16_t w, uint16_t h, uint32_t rgb, const char *name)
{
    const ag_time_t t0 = ag_micros();
    ag_gfx_clear(rgb);
    ag_gfx_flush(0, 0, w, h);
    const ag_time_t t1 = ag_micros();
    ag_printf("%s in %u us\n", name, (unsigned)(t1 - t0));
}

int ag_main(int argc, char **argv)
{
    ag_gfxinfo_t info;
    uint32_t     hold_s = 0;
    int          cycle = 0;

    if (argc >= 2 && argv[1] != NULL) {
        if (argv[1][0] == 'c' || argv[1][0] == 'C') {
            cycle = 1;
            hold_s = (argc >= 3) ? number(argv[2]) : 60u;
        } else {
            hold_s = number(argv[1]);
        }
    }

    if (ag_api()->gfx == NULL) {
        ag_printf("no gfx in this build\n");
        return 1;
    }

    const ag_err_t err = ag_gfx_acquire(&info);
    if (err != AG_OK) {
        ag_printf("acquire: %s\n", ag_strerror(err));
        return 1;
    }
    ag_printf("1 acquired %ux%u stride=%u direct=%d\n", (unsigned)info.width,
              (unsigned)info.height, (unsigned)info.stride,
              info.direct ? 1 : 0);

    const uint16_t w = info.width;
    const uint16_t h = info.height;

    if (cycle) {
        static const uint32_t k_rgb[4] = {0x00FF0000u, 0x0000FF00u,
                                          0x000000FFu, 0x00FFFFFFu};
        static const char    *k_name[4] = {"RED", "GREEN", "BLUE", "WHITE"};
        const uint32_t        frames = (hold_s != 0) ? hold_s : 60u;

        for (uint32_t i = 0; i < frames; i++) {
            const int k = (int)(i & 3u);
            show_flat(w, h, k_rgb[k], k_name[k]);
            ag_delay(1000);
        }
        ag_gfx_release();
        return 0;
    }

    ag_gfx_clear(0x00000080u); /* navy, so an untouched panel is obvious */
    ag_printf("2 cleared\n");

    /* One corner each, so a mirrored or rotated surface shows up as such. */
    ag_gfx_fill_rect(0, 0, (uint16_t)(w / 4), (uint16_t)(h / 4), 0x00FF0000u);
    ag_gfx_fill_rect((int16_t)(w - w / 4), 0, (uint16_t)(w / 4),
                     (uint16_t)(h / 4), 0x0000FF00u);
    ag_gfx_fill_rect(0, (int16_t)(h - h / 4), (uint16_t)(w / 4),
                     (uint16_t)(h / 4), 0x00FFFF00u);
    ag_gfx_fill_rect((int16_t)(w - w / 4), (int16_t)(h - h / 4),
                     (uint16_t)(w / 4), (uint16_t)(h / 4), 0x00FFFFFFu);
    ag_printf("3 corners\n");

    (void)ag_gfx_text(4, (int16_t)(h / 2 - 8), "GFXPIX", 0x00FFFFFFu,
                      0x00000080u);
    ag_printf("4 text\n");

    {
        const ag_time_t t0 = ag_micros();
        ag_gfx_flush(0, 0, w, h);
        const ag_time_t t1 = ag_micros();
        ag_gfx_flush(0, 0, w, h);
        const ag_time_t t2 = ag_micros();
        ag_printf("5 flushed: first %u us, second %u us\n",
                  (unsigned)(t1 - t0), (unsigned)(t2 - t1));
    }

    if (hold_s != 0) {
        ag_delay(hold_s * 1000u);
        ag_printf("6 held %u s\n", (unsigned)hold_s);
        ag_gfx_release();
        return 0;
    }

    for (;;) {
        ag_event_t ev;
        while (ag_poll_event(&ev, 0)) {
            if (ev.type == AG_EV_QUIT || ev.type == AG_EV_KEY_DOWN) {
                ag_gfx_release();
                return 0;
            }
        }
        ag_delay(30);
    }
}
