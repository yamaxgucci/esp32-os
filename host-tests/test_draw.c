/*
 * ArgonOS - soft-draw geometry host tests.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <argon/draw.h>

#include "test.h"

#define W 32
#define H 24

static uint16_t s_fb[W * H];
static ag_draw_surf_t s_surf;

static void surf_clear(uint16_t c)
{
    for (int i = 0; i < W * H; i++) {
        s_fb[i] = c;
    }
}

static int count_color(uint16_t c)
{
    int n = 0;
    for (int i = 0; i < W * H; i++) {
        if (s_fb[i] == c) {
            n++;
        }
    }
    return n;
}

void run_draw_tests(void)
{
    printf("draw\n");

    memset(&s_surf, 0, sizeof(s_surf));
    s_surf.pix = s_fb;
    s_surf.w = W;
    s_surf.h = H;

    AG_CHECK(ag_draw_rgb_to_565(0x00FF0000u) == 0xF800);
    AG_CHECK(ag_draw_rgb_to_565(0x0000FF00u) == 0x07E0);
    AG_CHECK(ag_draw_rgb_to_565(0x000000FFu) == 0x001F);

    surf_clear(0);
    ag_draw_pixel(&s_surf, 3, 4, 0xFFFF);
    AG_CHECK(s_fb[4 * W + 3] == 0xFFFF);
    ag_draw_pixel(&s_surf, -1, 0, 0xFFFF);
    ag_draw_pixel(&s_surf, W, 0, 0xFFFF);
    AG_CHECK(count_color(0xFFFF) == 1);

    surf_clear(0);
    ag_draw_line(&s_surf, 0, 0, 10, 0, 0xF800);
    AG_CHECK(count_color(0xF800) == 11);

    surf_clear(0);
    ag_draw_line(&s_surf, 5, 0, 5, 7, 0x07E0);
    AG_CHECK(count_color(0x07E0) == 8);

    surf_clear(0);
    ag_draw_circle(&s_surf, 15, 12, 0, 0x001F);
    AG_CHECK(s_fb[12 * W + 15] == 0x001F);

    surf_clear(0);
    ag_draw_fill_circle(&s_surf, 15, 12, 4, 0xFFE0);
    AG_CHECK(count_color(0xFFE0) > 20);

    surf_clear(0);
    {
        const ag_point_t tri[3] = {{8, 2}, {20, 20}, {2, 20}};
        ag_draw_fill_convex(&s_surf, tri, 3, 0x07FF);
        AG_CHECK(count_color(0x07FF) > 30);
        ag_draw_stroke_convex(&s_surf, tri, 3, 0xF81F);
        AG_CHECK(count_color(0xF81F) >= 3);
    }

    surf_clear(0);
    ag_draw_fill_convex(&s_surf, NULL, 3, 0xFFFF);
    AG_CHECK(count_color(0xFFFF) == 0);
    {
        const ag_point_t bad[2] = {{1, 1}, {2, 2}};
        ag_draw_fill_convex(&s_surf, bad, 2, 0xFFFF);
        AG_CHECK(count_color(0xFFFF) == 0);
    }

    surf_clear(0);
    ag_draw_stroke_rect(&s_surf, 2, 2, 8, 6, 0xF800);
    AG_CHECK(s_fb[2 * W + 2] == 0xF800);
    AG_CHECK(s_fb[7 * W + 9] == 0xF800);

    surf_clear(0);
    s_surf.clip_x = 4;
    s_surf.clip_y = 4;
    s_surf.clip_w = 8;
    s_surf.clip_h = 8;
    ag_draw_fill_rect(&s_surf, 0, 0, W, H, 0x001F);
    AG_CHECK(count_color(0x001F) == 64);
    ag_draw_pixel(&s_surf, 0, 0, 0xFFFF);
    AG_CHECK(s_fb[0] == 0);
    s_surf.clip_w = 0;
    s_surf.clip_h = 0;

    surf_clear(0);
    ag_draw_fill_round_rect(&s_surf, 4, 4, 16, 12, 3, 0x07E0);
    AG_CHECK(count_color(0x07E0) > 80);

    /* Opaque + chroma-key blit (RGB565 LE, stride = w*2). */
    {
        static const uint16_t spr[2 * 2] = {0xF800, 0xF81F, 0x07E0, 0xF81F};
        surf_clear(0);
        ag_draw_blit(&s_surf, 1, 1, 2, 2, spr, 4, 0, 0, 0);
        AG_CHECK(s_fb[1 * W + 1] == 0xF800);
        AG_CHECK(s_fb[1 * W + 2] == 0xF81F);
        AG_CHECK(s_fb[2 * W + 1] == 0x07E0);
        AG_CHECK(s_fb[2 * W + 2] == 0xF81F);

        surf_clear(0xFFFF);
        ag_draw_blit(&s_surf, 1, 1, 2, 2, spr, 4, 0, 1, 0xF81F);
        AG_CHECK(s_fb[1 * W + 1] == 0xF800);
        AG_CHECK(s_fb[1 * W + 2] == 0xFFFF); /* keyed out */
        AG_CHECK(s_fb[2 * W + 1] == 0x07E0);
        AG_CHECK(s_fb[2 * W + 2] == 0xFFFF);
    }

    /* ARGB8888 blend: LE bytes B,G,R,A. */
    {
        uint32_t px;
        uint8_t *b = (uint8_t *)&px;
        b[0] = 0;
        b[1] = 0;
        b[2] = 255;
        b[3] = 255; /* opaque red */
        surf_clear(0);
        ag_draw_blit_argb8888(&s_surf, 2, 2, 1, 1, &px, 4);
        AG_CHECK(s_fb[2 * W + 2] == 0xF800);

        b[3] = 0; /* fully transparent */
        surf_clear(0xFFFF);
        ag_draw_blit_argb8888(&s_surf, 2, 2, 1, 1, &px, 4);
        AG_CHECK(s_fb[2 * W + 2] == 0xFFFF);

        b[2] = 255;
        b[3] = 128; /* 50% red over black → r8=128 → 565 0x8000 */
        surf_clear(0);
        ag_draw_blit_argb8888(&s_surf, 3, 3, 1, 1, &px, 4);
        AG_CHECK(s_fb[3 * W + 3] == 0x8000);
    }

    /* Transparent 8×16 glyph: left column on, rest off. */
    {
        uint8_t rows[16];
        int i;
        for (i = 0; i < 16; i++) {
            rows[i] = 0x01;
        }
        surf_clear(0xFFFF);
        ag_draw_glyph8x16(&s_surf, 0, 0, rows, 0xF800, 0x001F, 1);
        AG_CHECK(count_color(0xF800) == 16);
        AG_CHECK(s_fb[0] == 0xF800);
        AG_CHECK(s_fb[1] == 0xFFFF); /* off-bit left alone */
        surf_clear(0xFFFF);
        ag_draw_glyph8x16(&s_surf, 0, 0, rows, 0xF800, 0x001F, 0);
        AG_CHECK(count_color(0xF800) == 16);
        AG_CHECK(count_color(0x001F) == 8 * 16 - 16);
    }

    /* text_fit: ellipsis when the string is wider than max_w. */
    {
        uint8_t font[256][16];
        int i;
        memset(font, 0, sizeof(font));
        for (i = 0; i < 16; i++) {
            font[(unsigned char)'A'][i] = 0x01;
            font[(unsigned char)'.'][i] = 0x01;
            font[(unsigned char)'B'][i] = 0x01;
        }
        surf_clear(0);
        AG_CHECK(ag_draw_text8x16(&s_surf, 0, 0, font, "AA", 0xF800, 0, 1, -1) ==
                 16);
        AG_CHECK(count_color(0xF800) == 32);

        surf_clear(0);
        /* "AAAA" = 32px, max_w=24 → "..." only (nfit=0). */
        AG_CHECK(ag_draw_text8x16(&s_surf, 0, 0, font, "AAAA", 0xF800, 0, 1,
                                  24) == 24);
        AG_CHECK(count_color(0xF800) == 48);

        surf_clear(0);
        /* max_w=32 → one 'A' + "..." = 32px, 4 glyphs. */
        AG_CHECK(ag_draw_text8x16(&s_surf, 0, 0, font, "AAAA", 0xF800, 0, 1,
                                  32) == 32);
        AG_CHECK(count_color(0xF800) == 64);

        surf_clear(0);
        /* Second line is clipped to H=24 (8 visible rows of the glyph). */
        AG_CHECK(ag_draw_text8x16(&s_surf, 0, 0, font, "AB\nB", 0x07E0, 0, 1,
                                  -1) == 24);
        AG_CHECK(count_color(0x07E0) == 40);
    }
}
