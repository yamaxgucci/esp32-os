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

    /* Nearest scale + tile of a 2×2 RGB565 checker. */
    {
        static const uint16_t tex[2 * 2] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
        surf_clear(0);
        ag_draw_blit_scaled(&s_surf, 0, 0, 4, 4, tex, 4, 0, 0, 2, 2);
        AG_CHECK(s_fb[0] == 0xF800);
        AG_CHECK(s_fb[1] == 0xF800);
        AG_CHECK(s_fb[2] == 0x07E0);
        AG_CHECK(s_fb[W] == 0xF800);
        AG_CHECK(s_fb[2 * W + 0] == 0x001F);
        AG_CHECK(s_fb[3 * W + 3] == 0xFFFF);

        surf_clear(0);
        ag_draw_blit_tiled(&s_surf, 0, 0, 4, 2, tex, 4, 0, 0, 2, 2);
        AG_CHECK(s_fb[0] == 0xF800);
        AG_CHECK(s_fb[2] == 0xF800);
        AG_CHECK(s_fb[1] == 0x07E0);
        AG_CHECK(s_fb[3] == 0x07E0);
        AG_CHECK(s_fb[W + 0] == 0x001F);
        AG_CHECK(s_fb[W + 1] == 0xFFFF);
    }

    /* Textured triangle: UVs cover the 2×2 tex over a 4×4 right triangle. */
    {
        static const uint16_t tex[2 * 2] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
        const ag_draw_texvert_t tri[3] = {
            {0, 0, 0, 0},
            {4, 0, 1, 0},
            {0, 4, 0, 1},
        };
        surf_clear(0);
        ag_draw_fill_convex_tex(&s_surf, tri, 3, tex, 4, 0, 0, 2, 2);
        AG_CHECK(s_fb[0] == 0xF800);
        AG_CHECK(count_color(0xF800) > 0);
        AG_CHECK(count_color(0x07E0) + count_color(0x001F) + count_color(0xFFFF) >
                 0);
    }

    /* Axis-aligned quad (fan of 2 triangles) must match a 2×2 nearest scale. */
    {
        static const uint16_t tex[2 * 2] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
        const ag_draw_texvert_t quad[4] = {
            {0, 0, 0, 0},
            {4, 0, 2, 0},
            {4, 4, 2, 2},
            {0, 4, 0, 2},
        };
        surf_clear(0);
        ag_draw_fill_convex_tex(&s_surf, quad, 4, tex, 4, 0, 0, 2, 2);
        AG_CHECK(s_fb[0] == 0xF800);
        AG_CHECK(s_fb[1] == 0xF800);
        AG_CHECK(s_fb[2] == 0x07E0);
        AG_CHECK(s_fb[3] == 0x07E0);
        AG_CHECK(s_fb[3 * W] == 0x001F);
        AG_CHECK(s_fb[3 * W + 3] == 0xFFFF);
    }

    /* Kite (unequal halves): bilinear UV covers the interior, no empty waist. */
    {
        static const uint16_t tex[2 * 2] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
        const ag_draw_texvert_t kite[4] = {
            {2, 10, 0, 1},
            {10, 2, 1, 0},
            {18, 10, 2, 1},
            {10, 20, 1, 2},
        };
        surf_clear(0);
        ag_draw_fill_convex_tex(&s_surf, kite, 4, tex, 4, 0, 0, 2, 2);
        AG_CHECK(s_fb[10 * W + 2] != 0);
        AG_CHECK(s_fb[2 * W + 10] != 0);
        AG_CHECK(s_fb[10 * W + 18] != 0);
        AG_CHECK(s_fb[19 * W + 10] != 0);
        AG_CHECK(s_fb[10 * W + 10] != 0);
        AG_CHECK(s_fb[9 * W + 10] != 0);
        AG_CHECK(s_fb[11 * W + 10] != 0);
        AG_CHECK(count_color(0xF800) + count_color(0x07E0) + count_color(0x001F) +
                     count_color(0xFFFF) >
                 20);
    }

    /*
     * A scene drawn in strips of three rows (row0/rows, the band renderer a
     * display with no framebuffer uses) is the same scene drawn whole, pixel
     * for pixel - and a strip never writes outside its own rows, whatever the
     * primitive or the clip.
     */
    {
        static uint16_t whole[W * H];
        static uint16_t strip[W * 3 + 2 * W]; /* + a guard row each side */
        static const uint16_t tex[2 * 2] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
        uint8_t font[256][16];
        memset(font, 0, sizeof(font));
        for (int i = 0; i < 16; i++) {
            font[(unsigned char)'A'][i] = (uint8_t)(0x81u | (1u << (i % 8)));
        }
        const ag_point_t tri[3] = {{8, 2}, {28, 20}, {2, 22}};

        for (int pass = 0; pass < 2; pass++) {
            const int strips = (pass == 0) ? 1 : (H + 2) / 3;
            for (int k = 0; k < strips; k++) {
                ag_draw_surf_t s;
                memset(&s, 0, sizeof(s));
                s.w = W;
                s.h = H;
                int rows = H;
                if (pass == 0) {
                    s.pix = whole;
                    memset(whole, 0, sizeof(whole));
                } else {
                    rows = (H - k * 3 < 3) ? H - k * 3 : 3;
                    s.pix = strip + W; /* the guard row in front */
                    s.row0 = (uint16_t)(k * 3);
                    s.rows = (uint16_t)rows;
                    for (int i = 0; i < (int)(sizeof(strip) / 2); i++) {
                        strip[i] = 0xBEEF;
                    }
                    memset(strip + W, 0, (size_t)W * (size_t)rows * 2u);
                }
                ag_draw_fill_rect(&s, 1, 1, 30, 22, 0x1111);
                ag_draw_fill_round_rect(&s, 3, 3, 20, 14, 4, 0x07E0);
                ag_draw_fill_circle(&s, 16, 12, 7, 0xF800);
                ag_draw_circle(&s, 16, 12, 9, 0xFFFF);
                ag_draw_line(&s, 0, 23, 31, 0, 0x001F);
                ag_draw_fill_convex(&s, tri, 3, 0x07FF);
                ag_draw_stroke_rect(&s, 5, 5, 10, 10, 0xF81F);
                ag_draw_blit_scaled(&s, 20, 10, 8, 8, tex, 4, 0, 0, 2, 2);
                (void)ag_draw_text8x16(&s, 2, 4, font, "AA", 0xFFE0, 0, 1, -1);
                s.clip_x = 10;
                s.clip_y = 6;
                s.clip_w = 12;
                s.clip_h = 9;
                ag_draw_fill_rect(&s, 0, 0, W, H, 0x4208);
                if (pass == 1) {
                    /* Nothing outside the strip's own rows. */
                    for (int i = 0; i < W; i++) {
                        AG_CHECK(strip[i] == 0xBEEF);
                        AG_CHECK(strip[W + rows * W + i] == 0xBEEF);
                    }
                    AG_CHECK(memcmp(strip + W, whole + k * 3 * W,
                                    (size_t)W * (size_t)rows * 2u) == 0);
                }
            }
        }
    }
}
