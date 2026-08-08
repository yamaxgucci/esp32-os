/*
 * ArgonOS - framebuffer → text console preview.
 *
 * Reads soft d:\fb0 (RGB565) without acquiring gfx, so it can run beside
 * SMS.AXE (`run /b`).  Each cell is CP437 ▀ (fg=top, bg=bottom).
 *
 *   run /b a:\fbcon.axe
 *   run /b a:\sms.axe a:\rambo.sms 999999
 *
 * Esc / Q quits.
 * Optional: fbcon.axe [pixel_step]   1=finest (slower), 4=default, 8=chunky
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/keys.h>

AG_APP_SIZED("FBCON", "1.0", "argon", 0, 8 * 1024, 256 * 1024);

static const uint8_t k_pal[16][3] = {
    {0x00, 0x00, 0x00}, {0x00, 0x00, 0xAA}, {0x00, 0xAA, 0x00}, {0x00, 0xAA, 0xAA},
    {0xAA, 0x00, 0x00}, {0xAA, 0x00, 0xAA}, {0xAA, 0x55, 0x00}, {0xAA, 0xAA, 0xAA},
    {0x55, 0x55, 0x55}, {0x55, 0x55, 0xFF}, {0x55, 0xFF, 0x55}, {0x55, 0xFF, 0xFF},
    {0xFF, 0x55, 0x55}, {0xFF, 0x55, 0xFF}, {0xFF, 0xFF, 0x55}, {0xFF, 0xFF, 0xFF},
};

#define HALF_BLOCK ((char)0xDF)

static uint8_t nearest_ansi(uint8_t r, uint8_t g, uint8_t b)
{
    int best = 0;
    int best_d = 1 << 30;
    for (int i = 0; i < 16; i++) {
        const int dr = (int)r - (int)k_pal[i][0];
        const int dg = (int)g - (int)k_pal[i][1];
        const int db = (int)b - (int)k_pal[i][2];
        const int d = dr * dr + dg * dg + db * db;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return (uint8_t)best;
}

static void rgb565_to_rgb(uint16_t p, uint8_t *r, uint8_t *g, uint8_t *b)
{
    const unsigned R = (p >> 11) & 0x1F;
    const unsigned G = (p >> 5) & 0x3F;
    const unsigned B = p & 0x1F;
    *r = (uint8_t)((R * 255u) / 31u);
    *g = (uint8_t)((G * 255u) / 63u);
    *b = (uint8_t)((B * 255u) / 31u);
}

static int parse_u(const char *s, int def)
{
    if (s == NULL || s[0] < '0' || s[0] > '9') {
        return def;
    }
    int v = 0;
    for (const char *p = s; *p >= '0' && *p <= '9'; p++) {
        v = v * 10 + (*p - '0');
    }
    return v > 0 ? v : def;
}

int ag_main(int argc, char **argv)
{
    int step = 4;
    if (argc > 1) {
        step = parse_u(argv[1], 4);
        if (step < 1) {
            step = 1;
        }
        if (step > 16) {
            step = 16;
        }
    }

    const ag_handle_t h = ag_open("d:\\fb0", AG_O_RDONLY);
    if (h < 0) {
        ag_printf("fbcon: cannot open d:\\fb0 (%s)\n", ag_strerror(h));
        return 1;
    }

    const int64_t bytes = ag_seek(h, 0, AG_SEEK_END);
    ag_seek(h, 0, AG_SEEK_SET);
    if (bytes < 2) {
        ag_printf("fbcon: empty framebuffer\n");
        ag_close(h);
        return 1;
    }

    /* Infer geometry from size; prefer console-sized 640×400, then 320×240. */
    unsigned fw = 640;
    unsigned fh = 400;
    if ((uint64_t)bytes == 640ull * 400ull * 2ull) {
        fw = 640;
        fh = 400;
    } else if ((uint64_t)bytes == 320ull * 240ull * 2ull) {
        fw = 320;
        fh = 240;
    } else if ((bytes % (640u * 2u)) == 0u) {
        fw = 640;
        fh = (unsigned)((uint64_t)bytes / (640ull * 2ull));
    } else {
        fw = 320;
        fh = (unsigned)((uint64_t)bytes / (320ull * 2ull));
        if (fh == 0) {
            fh = 1;
        }
    }

    uint16_t *fb = (uint16_t *)ag_malloc((size_t)bytes);
    if (fb == NULL) {
        ag_printf("fbcon: oom\n");
        ag_close(h);
        return 1;
    }

    ag_coninfo_t ci;
    ag_coninfo(&ci);
    const unsigned max_cols = ci.cols ? ci.cols : 80;
    const unsigned max_rows = ci.rows ? ci.rows : 25;

    /* One cell = step x (2*step) source pixels (half-block = 2 vertical samples). */
    unsigned cols = fw / (unsigned)step;
    unsigned rows = fh / (2u * (unsigned)step);
    if (cols == 0) {
        cols = 1;
    }
    if (rows == 0) {
        rows = 1;
    }
    if (cols > max_cols) {
        cols = max_cols;
    }
    if (rows > max_rows) {
        rows = max_rows;
    }

    /*
     * Keyboard events only reach the foreground process.  So:
     *   run /b sms.axe rambo.sms 999999
     *   run fbcon.axe              ← without /b, so Esc/Q work here
     * From the shell while fbcon is background: kill <pid>
     */
    ag_cursor(false);
    ag_cls();
    ag_printf("fbcon: %ux%u fb -> %ux%u cells (step %d). Esc/Q quit.\n",
              fw, fh, cols, rows, step);
    ag_printf("tip: start SMS with /b, then fbcon WITHOUT /b\n");
    ag_delay(800);
    ag_cls();

    for (;;) {
        ag_event_t ev;
        while (ag_poll_event(&ev, 0)) {
            if (ev.type != AG_EV_KEY_DOWN) {
                continue;
            }
            const uint16_t kc = ev.key.keycode;
            const uint32_t uc = ev.key.unicode;
            if (kc == AG_KEY_ESC || kc == AG_KEY_Q || uc == 0x1b ||
                uc == 'q' || uc == 'Q') {
                ag_free(fb);
                ag_close(h);
                ag_cursor(true);
                ag_cls();
                return 0;
            }
        }

        ag_seek(h, 0, AG_SEEK_SET);
        size_t got = 0;
        while (got < (size_t)bytes) {
            const int32_t n =
                ag_read(h, (uint8_t *)fb + got, (size_t)bytes - got);
            if (n <= 0) {
                break;
            }
            got += (size_t)n;
        }
        if (got < (size_t)bytes) {
            ag_delay(50);
            continue;
        }

        for (unsigned cy = 0; cy < rows; cy++) {
            for (unsigned cx = 0; cx < cols; cx++) {
                const unsigned x = cx * (unsigned)step + (unsigned)step / 2u;
                const unsigned y0 = cy * 2u * (unsigned)step + (unsigned)step / 2u;
                const unsigned y1 = y0 + (unsigned)step;
                uint16_t p0 = fb[(y0 < fh ? y0 : fh - 1) * fw + (x < fw ? x : fw - 1)];
                uint16_t p1 = fb[(y1 < fh ? y1 : fh - 1) * fw + (x < fw ? x : fw - 1)];
                uint8_t r0, g0, b0, r1, g1, b1;
                rgb565_to_rgb(p0, &r0, &g0, &b0);
                rgb565_to_rgb(p1, &r1, &g1, &b1);
                ag_poke((uint16_t)cx, (uint16_t)cy, HALF_BLOCK,
                        AG_ATTR(nearest_ansi(r0, g0, b0), nearest_ansi(r1, g1, b1)));
            }
        }

        ag_delay(33);
    }
}
