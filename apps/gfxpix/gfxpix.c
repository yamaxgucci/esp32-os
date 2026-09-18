/*
 * ArgonOS - the smallest thing that puts pixels on a panel.
 *
 * GFXDEMO exercises everything the soft renderer has, at coordinates chosen
 * for a 640x400 surface.  That makes it a poor first test on a board whose
 * surface is 160x120: when it stops, the question "did the pixels reach the
 * glass" is tangled up with a dozen clipping paths it also just walked.
 *
 * Two modes, and both exist because of how a previous one was ambiguous.
 *
 *   gfxpix [seconds]   a circle, a diagonal and four corner squares, held.
 *
 *                      The circle is the point: a text mode cannot draw one.
 *                      Anybody glancing at the board can say "graphics" or
 *                      "characters" without knowing anything else, and half a
 *                      circle says half the frame arrived.  The corners are
 *                      each a different colour, so a mirrored or rotated
 *                      surface shows up as the wrong corner being red.
 *
 *   A red dot goes round the circle, one step of sixteen per frame, because a
 *   still picture cannot tell a working link from a frozen one - which is
 *   exactly what came back from the phone the first time.  It is also the
 *   frame rate, readable from across the room: a lap a second is sixteen
 *   frames a second.
 *
 *   The picture is handed over again twice a second for as long as it is
 *   held, which is not decoration: a screen at the end of a wire keeps
 *   nothing, so a picture drawn once belongs to whoever happened to be
 *   watching.  A phone picked up afterwards would see an empty canvas.
 *
 *   gfxpix own [s]     the same picture, but 160x144 in the application's own
 *                      memory, handed to the panel sixteen rows at a time
 *                      (gfx->present).  What an emulator has to do, and what
 *                      the system's surface cannot do for it: the shape is
 *                      wrong and the memory is wanted elsewhere.
 *
 *                      Taken by itself, without the word, on a display that
 *                      has no surface at all - a board whose framebuffer would
 *                      be 150 KB of a 207 KB heap.  There the first mode draws
 *                      into nothing and says it drew; this one is the only one
 *                      that puts a picture anywhere.
 *
 *   gfxpix cycle [n]   the whole surface, one flat colour at a time, changing
 *                      every second, n times.  For telling "there was no
 *                      picture" apart from "the panel went black" and from "the
 *                      console text stayed" - three different faults that look
 *                      alike at a glance.
 *
 * Every step prints, and the flushes are timed, which says whether the pixels
 * were sent without anybody watching the board: a whole 320x240 frame over SPI
 * is tens of milliseconds and a flush that reached no driver is tens of
 * microseconds.  Careful with the converse - equal time does *not* mean the
 * frame landed where it should.  Three hundred small SPI transactions cost the
 * same as three hundred large ones, and a whole frame once went into a single
 * 8x8 character cell in exactly the same 55 ms as a correct one.
 *
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32-elf-gcc \
 *       --include sdk/include -o build/apps/GFXPIX.AXE apps/gfxpix/gfxpix.c
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/keys.h>

/*
 * Sixteen kilobytes of stack, and the reason is not this program.
 *
 * On a board with no framebuffer the screen at the end of the wire is
 * fed from inside the drawing call: PHONE.SYS encodes the band and
 * pushes it through the socket on the caller's task, which is this
 * one.  Eight kilobytes - what an application gets by default - is not
 * enough for a band encoder plus lwIP, and the board resets:
 *
 *   ***ERROR*** A stack overflow in task gfxpix.axe has been detected
 *
 * followed, on the next run, by the allocator asserting on a heap the
 * overflow had already walked into.  Three resets in seventeen runs,
 * one cause.
 *
 * Anything that draws while a phone is attached wants the same.
 */
AG_APP_SIZED("GFXPIX", "1.3", "argon", AG_AXE_NEEDS_GFX, 16 * 1024,
             4 * 1024);

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

static void show_flat(uint16_t w, uint16_t h, uint32_t rgb, const char *name)
{
    const ag_time_t t0 = ag_micros();
    ag_gfx_clear(rgb);
    ag_gfx_flush(0, 0, w, h);
    const ag_time_t t1 = ag_micros();
    ag_printf("%s in %u us\n", name, (unsigned)(t1 - t0));
}

static void draw_scene(uint16_t w, uint16_t h)
{
    const uint16_t qw = (uint16_t)(w / 5);
    const uint16_t qh = (uint16_t)(h / 5);

    ag_gfx_clear(0x00000080u); /* navy, so an untouched panel is obvious */

    /* The whole point of this picture: characters cannot be round. */
    {
        const uint16_t r = (uint16_t)(((w < h) ? h : w) / 2u - 2u);
        const uint16_t rr = (uint16_t)(((w < h) ? w : h) / 2u - 2u);
        ag_gfx_fill_circle((int16_t)(w / 2), (int16_t)(h / 2),
                           (rr < r) ? rr : r, 0x00E0C040u);
        ag_gfx_circle((int16_t)(w / 2), (int16_t)(h / 2),
                      (uint16_t)(((rr < r) ? rr : r) - 3u), 0x00202020u);
    }

    /* Two diagonals: a broken window or a wrong stride bends them. */
    ag_gfx_line(0, 0, (int16_t)(w - 1), (int16_t)(h - 1), 0x00FFFFFFu);
    ag_gfx_line(0, (int16_t)(h - 1), (int16_t)(w - 1), 0, 0x00FFFFFFu);

    /* One corner each, so a mirrored or rotated surface shows up as such. */
    ag_gfx_fill_rect(0, 0, qw, qh, 0x00FF0000u);
    ag_gfx_fill_rect((int16_t)(w - qw), 0, qw, qh, 0x0000FF00u);
    ag_gfx_fill_rect(0, (int16_t)(h - qh), qw, qh, 0x00FFFF00u);
    ag_gfx_fill_rect((int16_t)(w - qw), (int16_t)(h - qh), qw, qh, 0x00FFFFFFu);
}

/*
 * The other way round: pixels the application owns, handed straight to the
 * panel (gfx->present, ABI 0.31).
 *
 * A Game Boy screen is 160x144 and the system's surface on this board is
 * 160x120 - the wrong shape, and 37 KB that an emulator would rather spend on a
 * cartridge.  So this draws the same kind of picture into a buffer of its own,
 * sixteen rows at a time, which is exactly the shape an emulator's scanline
 * renderer produces.
 *
 * Sixteen rows and not one: a present costs a window command and a transfer
 * whatever its height, and 144 of those per frame is most of a frame's time
 * spent on overhead.  Nine is nothing.
 */
#define OWN_W 160
#define OWN_H 144
#define OWN_BAND 16

static uint16_t s_band[OWN_W * OWN_BAND];

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xf8u) << 8) | ((g & 0xfcu) << 3) | (b >> 3));
}

/*
 * Where the spoke is pointing.  Advanced once per frame by the caller, so a
 * still picture and a stopped link look different from across the room - the
 * one thing a test picture has to be able to say and the one thing the first
 * version of this could not.
 */
static int s_phase;

/* Sixteen positions round the circle, as (dx, dy) in sixteenths. */
static const signed char k_spoke[16][2] = {
    {16, 0},  {15, 6},   {11, 11},  {6, 15},   {0, 16},   {-6, 15},
    {-11, 11},{-15, 6},  {-16, 0},  {-15, -6}, {-11, -11},{-6, -15},
    {0, -16}, {6, -15},  {11, -11}, {15, -6},
};

static int own_picture(void)
{
    /* A circle, two diagonals and a border - the same test as the framebuffer
     * path, so the two can be compared by eye - and a spoke that moves. */
    const int cx = OWN_W / 2, cy = OWN_H / 2;
    const int r = (OWN_W < OWN_H ? OWN_W : OWN_H) / 2 - 2;
    const int sx = cx + (k_spoke[s_phase & 15][0] * (r - 6)) / 16;
    const int sy = cy + (k_spoke[s_phase & 15][1] * (r - 6)) / 16;

    for (int y0 = 0; y0 < OWN_H; y0 += OWN_BAND) {
        const int rows = (OWN_H - y0 < OWN_BAND) ? (OWN_H - y0) : OWN_BAND;

        for (int row = 0; row < rows; row++) {
            const int y = y0 + row;
            for (int x = 0; x < OWN_W; x++) {
                const int dx = x - cx, dy = y - cy;
                uint16_t  c;

                if (x == 0 || y == 0 || x == OWN_W - 1 || y == OWN_H - 1) {
                    c = rgb565(255, 255, 255);
                } else if (x * OWN_H == y * OWN_W ||
                           (OWN_W - 1 - x) * OWN_H == y * OWN_W) {
                    c = rgb565(255, 255, 255);
                } else if ((x - sx) * (x - sx) + (y - sy) * (y - sy) <= 36) {
                    /* The spoke's head: six pixels across, and the only thing
                     * in this picture that is ever in a different place. */
                    c = rgb565(255, 40, 40);
                } else if (dx * dx + dy * dy <= r * r) {
                    c = rgb565(224, 192, 64);
                } else {
                    c = rgb565(0, 0, 128);
                }
                s_band[row * OWN_W + x] = c;
            }
        }

        const ag_blit_t b = {
            .px = s_band,
            .stride = OWN_W * sizeof(uint16_t),
            .surf_w = OWN_W,
            .surf_h = OWN_H,
            .x = 0,
            .y = (uint16_t)y0,
            .w = OWN_W,
            .h = (uint16_t)rows,
        };
        const ag_err_t err = ag_gfx_present(&b);
        if (err != AG_OK) {
            ag_printf("present at y=%d: %s\n", y0, ag_strerror(err));
            return 1;
        }
    }
    return 0;
}

int ag_main(int argc, char **argv)
{
    ag_gfxinfo_t info;
    uint32_t     hold_s = 0;
    int          cycle = 0;

    int own = 0;

    if (argc >= 2 && argv[1] != NULL) {
        if (argv[1][0] == 'c' || argv[1][0] == 'C') {
            cycle = 1;
            hold_s = (argc >= 3) ? number(argv[2]) : 60u;
        } else if (argv[1][0] == 'o' || argv[1][0] == 'O') {
            own = 1;
            hold_s = (argc >= 3) ? number(argv[2]) : 30u;
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

    /*
     * A display with no surface draws nothing, whatever is asked of it.
     *
     * fb == NULL is the kernel saying "there is no framebuffer here; hand me
     * pixels with gfx->present" (ag_gfxinfo_t), which is the CYD: 320x240 in
     * RGB565 is 150 KB of a 207 KB heap.  Drawing into it is not an error
     * anywhere - every primitive checks and returns - so the run reports a
     * circle, holds the screen, and shows a blank one.  Measured exactly that
     * way while looking for why a phone only ever showed text.
     *
     * So this takes the road that works instead of the one that was asked
     * for, and says which.  `own` on the command line still forces it.
     */
    if (!own && info.fb == NULL) {
        own = 1;
        ag_printf("no surface on this display (fb is NULL); "
                  "drawing into my own memory and handing it over\n");
    }

    if (own) {
        if (!AG_HAS(ag_api()->gfx, present)) {
            ag_printf("this kernel has no gfx->present (ABI < 0.31)\n");
            ag_gfx_release();
            return 1;
        }
        const ag_time_t t0 = ag_micros();
        int             bad = own_picture();
        const ag_time_t t1 = ag_micros();
        ag_printf("2 own %dx%d in %u us%s\n", OWN_W, OWN_H,
                  (unsigned)(t1 - t0), bad ? " (failed)" : "");
        /*
         * And again, twice a second, for as long as it is held.  See the note
         * at the top of this file: a screen on the other end of a wire keeps
         * nothing, so a picture sent once belongs only to whoever was watching
         * then.  Repeating it is what makes a phone picked up afterwards show
         * the circle, and what gives the link's encoder a second chance at the
         * frame it was not yet ready for.
         */
        /*
         * With no number, until somebody stops it.  A picture drawn once
         * and let go is a flash - reported from a phone as "it flickered
         * and went" - and on the glass the console takes the screen back
         * the moment this releases it.  The command without arguments is
         * the first one anybody types, so it is the one that has to
         * behave.
         */
        uint32_t   left = hold_s * 1000u;
        const bool forever = (hold_s == 0u);
        for (;;) {
            if (!forever && left == 0u) {
                break;
            }
            const uint32_t nap = (forever || left > 500u) ? 500u : left;
            ag_delay(nap);
            if (!forever) {
                left -= nap;
            }

            /*
             * Both ways round.  This used to be asked only when no number was
             * given, and `run gfxpix.axe 20` then sat out its twenty seconds
             * through Esc, through Ctrl+C and through `stop`: the key arrived
             * and the flag was set, and nobody in here looked at either.  An
             * application holding the screen and deaf to the stop key looks
             * exactly like a hung one - which is the state this program exists
             * to let somebody rule out.
             */
            ag_event_t ev;
            bool       done = false;
            while (ag_poll_event(&ev, 0)) {
                if (ev.type == AG_EV_QUIT || ev.type == AG_EV_KEY_DOWN) {
                    done = true;
                }
            }
            if (done || ag_interrupted()) {
                break;
            }

            s_phase++;
            if (own_picture() != 0) {
                bad = 1;
            }
        }
        ag_gfx_release();
        if (forever || left != 0u) {
            ag_printf("3 held until stopped\n");
        } else {
            ag_printf("3 held %u s\n", (unsigned)hold_s);
        }
        return bad;
    }

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

    draw_scene(w, h);
    ag_printf("2 drawn\n");

    {
        const ag_time_t t0 = ag_micros();
        ag_gfx_flush(0, 0, w, h);
        const ag_time_t t1 = ag_micros();
        ag_printf("3 flushed in %u us\n", (unsigned)(t1 - t0));
    }

    /*
     * Nothing is printed while it is held.  Printing scrolls the console, and
     * although the panel is not repainted while an application owns it, the
     * scroll is waiting to appear the moment it lets go - which is what made
     * the last run look like "the bottom half is green".
     */
    if (hold_s != 0) {
        ag_delay(hold_s * 1000u);
        ag_gfx_release();
        ag_printf("4 held %u s\n", (unsigned)hold_s);
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
