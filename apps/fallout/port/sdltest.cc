/*
 * ArgonOS - SDLTEST.AXE: the SDL shim without the engine.
 *
 * Fallout checks for its data files before it opens a window, so an ArgonOS
 * without 400 MB of .DAT on the card never reaches the video path at all.  This
 * drives that path directly, making exactly the calls svga.cc makes and in the
 * same order, so the shim can be finished and verified before the data exists.
 *
 *   run h:\sdltest.axe
 *
 * It draws a picture whose correctness can be checked by arithmetic rather than
 * by eye - colour bars at known palette entries, a diagonal, a border - then
 * reads the framebuffer back and prints a checksum, and writes the frame out as
 * a PPM next to itself so it can also be looked at.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

#include "SDL.h"
#include "ag_cxx.h"

#include <stdio.h>
#include <string.h>

AG_APP("SDLTEST", "1.0", "argon", 0);

/* Not SDL: the shim holds the display, and a second ag_gfx_acquire is -EBUSY,
 * so this is how anything else in the image asks what it got. */
extern "C" bool sdl_shim_display(ag_gfxinfo_t *out);

#define GAME_W 640
#define GAME_H 480

static int s_failures;

static bool same(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

static void check(const char *what, bool ok)
{
    ag_printf("%-38s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) {
        ++s_failures;
    }
}

/* The eight bars, as the palette entries the game would use for them. */
static const SDL_Color k_bars[8] = {
    { 0, 0, 0, 255 },       { 255, 0, 0, 255 },   { 0, 255, 0, 255 },
    { 0, 0, 255, 255 },     { 255, 255, 0, 255 }, { 0, 255, 255, 255 },
    { 255, 0, 255, 255 },   { 255, 255, 255, 255 },
};

static Uint16 rgb565(Uint8 r, Uint8 g, Uint8 b)
{
    return (Uint16)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

/*
 * The frame as a PPM, so that a picture the machine says is right can also be
 * looked at.  P6 rather than P3: 900 KB against 3 MB, and every viewer reads
 * both.
 */
static void write_ppm(const char *path, const Uint16 *fb, int w, int h,
                      int stride_bytes)
{
    ag_handle_t f = ag_open(path, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (f < 0) {
        ag_printf("could not write %s\n", path);
        return;
    }

    char header[64];
    int n = snprintf(header, sizeof(header), "P6\n%d %d\n255\n", w, h);
    ag_write(f, header, (size_t)n);

    /* A row at a time: three bytes per pixel is 1.9 KB for 640, and a buffer
     * of the whole picture would be another megabyte of arena. */
    static unsigned char row[GAME_W * 3];
    for (int y = 0; y < h; ++y) {
        const Uint16 *src =
            (const Uint16 *)((const unsigned char *)fb + (size_t)y * stride_bytes);
        for (int x = 0; x < w; ++x) {
            const Uint16 p = src[x];
            /* Back out to 8 bits the way a display would: replicate the high
             * bits into the low ones, so white stays 255 rather than 248. */
            const unsigned r5 = (p >> 11) & 0x1Fu;
            const unsigned g6 = (p >> 5) & 0x3Fu;
            const unsigned b5 = p & 0x1Fu;
            row[x * 3 + 0] = (unsigned char)((r5 << 3) | (r5 >> 2));
            row[x * 3 + 1] = (unsigned char)((g6 << 2) | (g6 >> 4));
            row[x * 3 + 2] = (unsigned char)((b5 << 3) | (b5 >> 2));
        }
        ag_write(f, row, (size_t)w * 3u);
    }
    ag_close(f);
    ag_printf("wrote %s (%dx%d)\n", path, w, h);
}

extern "C" int ag_cxx_main(int argc, char **argv)
{
    const char *out = (argc > 1) ? argv[1] : "h:\\sdltest.ppm";

    ag_printf("SDLTEST - the SDL shim, without the engine\n\n");

    /* --- exactly what svga_init does, in order ---------------------------- */

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
    check("SDL_InitSubSystem(VIDEO)", SDL_InitSubSystem(SDL_INIT_VIDEO) == 0);

    SDL_Window *window = SDL_CreateWindow("SDLTEST", SDL_WINDOWPOS_UNDEFINED,
                                          SDL_WINDOWPOS_UNDEFINED, GAME_W,
                                          GAME_H, SDL_WINDOW_FULLSCREEN);
    check("SDL_CreateWindow", window != NULL);
    if (window == NULL) {
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, 0);
    check("SDL_CreateRenderer", renderer != NULL);

    SDL_Texture *texture =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888,
                          SDL_TEXTUREACCESS_STREAMING, GAME_W, GAME_H);
    check("SDL_CreateTexture", texture != NULL);

    Uint32 format = 0;
    check("SDL_QueryTexture", SDL_QueryTexture(texture, &format, NULL, NULL,
                                               NULL) == 0);
    check("texture is 16 bits", SDL_BITSPERPIXEL(format) == 16);

    SDL_Surface *present = SDL_CreateRGBSurfaceWithFormat(
        0, GAME_W, GAME_H, (int)SDL_BITSPERPIXEL(format), format);
    check("SDL_CreateRGBSurfaceWithFormat", present != NULL);

    SDL_Surface *game = SDL_CreateRGBSurface(0, GAME_W, GAME_H, 8, 0, 0, 0, 0);
    check("SDL_CreateRGBSurface(8 bit)", game != NULL);
    if (present == NULL || game == NULL) {
        return 1;
    }

    /*
     * The presenting surface should be the framebuffer itself.  If it is not -
     * a panel of another size, or the display refused - the picture still comes
     * out, through a copy in SDL_UpdateTexture, and this line says which of the
     * two happened.
     */
    ag_gfxinfo_t gi;
    const bool have_gfx = sdl_shim_display(&gi);
    const bool zero_copy = have_gfx && present->pixels == gi.fb;
    ag_printf("%-38s %s\n", "presenting surface is the panel",
              zero_copy ? "yes (no copy per frame)" : "no (copied per frame)");
    if (have_gfx && ((int)gi.width != GAME_W || (int)gi.height != GAME_H)) {
        ag_printf("panel is %ux%u, not %dx%d - put [display] width/height in "
                  "C:\\SYSTEM.CFG\n",
                  (unsigned)gi.width, (unsigned)gi.height, GAME_W, GAME_H);
    }

    /* --- a picture -------------------------------------------------------- */

    SDL_Color palette[256];
    memset(palette, 0, sizeof(palette));
    for (int i = 0; i < 8; ++i) {
        palette[i] = k_bars[i];
    }
    /* A ramp above the bars, so a palette written in two calls is exercised
     * the way the game's fades write it. */
    for (int i = 0; i < 64; ++i) {
        palette[64 + i].r = (Uint8)(i * 4);
        palette[64 + i].g = (Uint8)(i * 4);
        palette[64 + i].b = (Uint8)(i * 4);
        palette[64 + i].a = 255;
    }
    check("SDL_SetPaletteColors (bars)",
          SDL_SetPaletteColors(game->format->palette, palette, 0, 8) == 0);
    check("SDL_SetPaletteColors (ramp)",
          SDL_SetPaletteColors(game->format->palette, palette + 64, 64, 64) == 0);

    unsigned char *px = (unsigned char *)game->pixels;
    for (int y = 0; y < GAME_H; ++y) {
        unsigned char *row = px + (size_t)y * game->pitch;
        for (int x = 0; x < GAME_W; ++x) {
            unsigned char v;
            if (y < GAME_H / 2) {
                v = (unsigned char)(x * 8 / GAME_W); /* eight bars */
            } else {
                v = (unsigned char)(64 + x * 64 / GAME_W); /* the ramp */
            }
            if (x == y || x == GAME_W - 1 - y) {
                v = 7; /* white diagonals, to catch a wrong pitch */
            }
            if (x == 0 || y == 0 || x == GAME_W - 1 || y == GAME_H - 1) {
                v = 1; /* red border, to catch a wrong size */
            }
            row[x] = v;
        }
    }

    /* --- present, the way renderPresent does ------------------------------ */

    check("SDL_BlitSurface", SDL_BlitSurface(game, NULL, present, NULL) == 0);
    SDL_UpdateTexture(texture, NULL, present->pixels, present->pitch);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);

    /* --- read it back ----------------------------------------------------- */

    if (have_gfx && gi.fb != NULL) {
        const Uint16 *fb = (const Uint16 *)gi.fb;
        const int w = (int)gi.width < GAME_W ? (int)gi.width : GAME_W;
        const int h = (int)gi.height < GAME_H ? (int)gi.height : GAME_H;

        check("corner is border red",
              fb[0] == rgb565(255, 0, 0));
        check("bar 3 is blue",
              *(const Uint16 *)((const unsigned char *)fb +
                                (size_t)100 * gi.stride + 3 * 80 * 2) ==
                  rgb565(0, 0, 255));
        check("ramp is grey and rising",
              *(const Uint16 *)((const unsigned char *)fb +
                                (size_t)300 * gi.stride + 600 * 2) >
                  *(const Uint16 *)((const unsigned char *)fb +
                                    (size_t)300 * gi.stride + 100 * 2));

        uint32_t sum = 0;
        for (int y = 0; y < h; ++y) {
            const Uint16 *row = (const Uint16 *)((const unsigned char *)fb +
                                                 (size_t)y * gi.stride);
            for (int x = 0; x < w; ++x) {
                sum = sum * 31u + row[x];
            }
        }
        ag_printf("framebuffer checksum %08x over %dx%d\n", (unsigned)sum, w, h);

        write_ppm(out, fb, w, h, (int)gi.stride);
    }

    /* --- the formatter ---------------------------------------------------- */

    /*
     * Not SDL, and here anyway.  Fallout builds the name of a message file as
     * "pro_%.4s.msg" of "items", and a snprintf that parses the precision and
     * throws it away asks the archive for pro_items.msg - which is not in it.
     * The engine then reports that it cannot load its prototypes, and the
     * formatter is the last place anyone looks.
     */
    char fbuf[64];
    snprintf(fbuf, sizeof(fbuf), "pro_%.4s%s", "items", ".msg");
    check("snprintf honours %.4s", same(fbuf, "pro_item.msg"));
    snprintf(fbuf, sizeof(fbuf), "[%8s][%-8s]", "ab", "cd");
    check("snprintf pads and justifies", same(fbuf, "[      ab][cd      ]"));
    snprintf(fbuf, sizeof(fbuf), "%.2s%.0s%s", "abcdef", "ghi", "!");
    check("snprintf precision zero", same(fbuf, "ab!"));

    /* --- the resampler ---------------------------------------------------- */

    /*
     * Half rate in, so two output frames per input frame, and the one between
     * two inputs must be their average - that is what interpolating means and
     * it is the whole difference from repeating the sample.
     */
    SDL_AudioStream *stream =
        SDL_NewAudioStream(AUDIO_S16, 1, 11025, AUDIO_S16, 2, 22050);
    check("SDL_NewAudioStream", stream != NULL);
    if (stream != NULL) {
        static Sint16 in[64];
        for (int i = 0; i < 64; ++i) {
            in[i] = (Sint16)(i * 100);
        }
        for (int i = 0; i < 64; ++i) {
            SDL_AudioStreamPut(stream, &in[i], sizeof(in[i]));
        }

        static Sint16 out[512];
        const int got = SDL_AudioStreamGet(stream, out, (int)sizeof(out));
        const int frames = got / (2 * (int)sizeof(Sint16));
        /* 63 input intervals, two output frames each. */
        check("resampler doubles the rate", frames >= 124 && frames <= 128);
        check("resampler duplicates mono to stereo", out[0] == out[1]);
        check("resampler interpolates",
              out[2] == 50 && out[4] == 100 && out[6] == 150);

        SDL_FreeAudioStream(stream);
    }

    Sint16 mix[4] = { 1000, -1000, 32000, -32000 };
    const Sint16 add[4] = { 1000, -1000, 32000, -32000 };
    SDL_MixAudioFormat((Uint8 *)mix, (const Uint8 *)add, AUDIO_S16,
                       sizeof(mix), SDL_MIX_MAXVOLUME / 2);
    check("mixer adds at half volume", mix[0] == 1500 && mix[1] == -1500);
    check("mixer saturates", mix[2] == 32767 && mix[3] == -32768);

    /* --- the timer -------------------------------------------------------- */

    static int fired;
    fired = 0;
    struct T {
        static Uint32 tick(Uint32 interval, void *param)
        {
            (void)param;
            ++fired;
            return interval;
        }
    };
    SDL_TimerID timer = SDL_AddTimer(10, T::tick, NULL);
    check("SDL_AddTimer", timer != 0);
    for (int i = 0; i < 5; ++i) {
        ag_delay(15);
        SDL_RenderPresent(renderer); /* where timers are serviced */
    }
    check("timer fired", fired >= 3);
    check("SDL_RemoveTimer", SDL_RemoveTimer(timer) == SDL_TRUE);

    /* --- events ----------------------------------------------------------- */

    /* Nothing is pressed in an automated run, so what is checked is that the
     * queue drains and stays empty rather than what came out of it. */
    SDL_Event ev;
    int drained = 0;
    while (SDL_PollEvent(&ev) && drained < 100) {
        ++drained;
    }
    check("event queue drains", drained < 100);
    int mx = 0, my = 0;
    SDL_GetRelativeMouseState(&mx, &my);
    check("relative mouse state resets", (SDL_GetRelativeMouseState(&mx, &my),
                                          mx == 0 && my == 0));

    /* Leave the picture up for a moment when a window is open, then give the
     * console its display back - otherwise the shell prints into a screen
     * nobody is scanning out. */
    ag_delay(2000);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_FreeSurface(game);
    SDL_FreeSurface(present);
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);

    ag_printf("\n%d failure(s)\n", s_failures);
    return s_failures == 0 ? 0 : 1;
}
