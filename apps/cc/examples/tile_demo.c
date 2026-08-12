/*
 * tile_demo — Argon CC smoke for g2d (textured tiles + sprite).
 *
 * HostFS: g2d_globals.h / g2d_impl.h beside this file (apps/cc/lib/g2d).
 *
 *   run h:\cc.axe h:\tile_demo.c h:\tile_demo.axe
 *   run h:\tile_demo.axe
 *
 * Pad: Left/Right scroll, Up/Down move sprite, Esc quit.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "g2d_globals.h"

#define AG_EV_FOCUS_GAINED 12
#define AG_EV_FOCUS_LOST   13
#define AG_EV_QUIT         14

struct ag_ev {
    int type;
    int pad;
    int ts0;
    int ts1;
    int u0;
    int u1;
    int u2;
    int u3;
    int u4;
    int u5;
};

#define VIEW_W 320
#define VIEW_H 200
#define TW 16
#define TH 16
#define ATLAS_W 8
#define MAP_W 40
#define MAP_H 20
#define KEY_RGB 16711935

#define C_KEY 63519
#define C_GRASS0 8416
#define C_GRASS1 14727
#define C_GRASS2 2016
#define C_DIRT0 18920
#define C_DIRT1 27206
#define C_DIRT2 14726
#define C_WATER0 1471
#define C_WATER1 2588
#define C_WATER2 1243
#define C_BRICK0 41120
#define C_BRICK1 32912
#define C_MORTAR 48631
#define C_STONE0 16912
#define C_STONE1 31727
#define C_STONE2 8456
#define C_WOOD0 27206
#define C_WOOD1 18920
#define C_WOOD2 42116
#define C_LEAF 9984
#define C_TRUNK 18920

char atlas[ATLAS_W * TW * TH * 2];
char map[MAP_W * MAP_H];
int ox;
int oy;
int px;
int py;
int running;

#include "g2d_impl.h"

void atlas_begin(void)
{
    g2d_tiles = atlas;
    g2d_atlas_w = ATLAS_W;
    g2d_tile_w = TW;
    g2d_tile_h = TH;
    g2d_atlas_stride = ATLAS_W * TW * 2;
}

void make_grass(int id)
{
    int x;
    int y;
    int n;
    g2d_fill_tile(atlas, ATLAS_W, TW, TH, id, C_GRASS0);
    y = 0;
    while (y < TH) {
        x = 0;
        while (x < TW) {
            n = (x * 3 + y * 7 + id * 11) & 7;
            if (n < 2) {
                g2d_tpix(id, x, y, C_GRASS1);
            }
            if (n == 3) {
                g2d_tpix(id, x, y, C_GRASS2);
            }
            if (((x + y) & 3) == 0) {
                g2d_tpix(id, x, y, C_GRASS1);
            }
            x = x + 1;
        }
        y = y + 1;
    }
}

void make_dirt(int id)
{
    int x;
    int y;
    int n;
    g2d_fill_tile(atlas, ATLAS_W, TW, TH, id, C_DIRT0);
    y = 0;
    while (y < TH) {
        x = 0;
        while (x < TW) {
            n = (x * 5 + y * 3) & 7;
            if (n < 2) {
                g2d_tpix(id, x, y, C_DIRT1);
            }
            if (n == 5) {
                g2d_tpix(id, x, y, C_DIRT2);
            }
            x = x + 1;
        }
        y = y + 1;
    }
}

void make_water(int id)
{
    int x;
    int y;
    g2d_fill_tile(atlas, ATLAS_W, TW, TH, id, C_WATER0);
    y = 0;
    while (y < TH) {
        x = 0;
        while (x < TW) {
            if (((x + y / 2) & 3) == 0) {
                g2d_tpix(id, x, y, C_WATER1);
            }
            if (((x * 2 + y) & 7) == 1) {
                g2d_tpix(id, x, y, C_WATER2);
            }
            x = x + 1;
        }
        y = y + 1;
    }
    g2d_thline(id, 2, 6, 3, C_WATER1);
    g2d_thline(id, 9, 13, 9, C_WATER1);
}

void make_brick(int id)
{
    int y;
    int shift;
    g2d_fill_tile(atlas, ATLAS_W, TW, TH, id, C_BRICK0);
    y = 0;
    while (y < TH) {
        g2d_thline(id, 0, 15, y, C_MORTAR);
        shift = 0;
        if ((y / 4) & 1) {
            shift = 4;
        }
        g2d_tvline(id, (0 + shift) & 15, y + 1, y + 3, C_MORTAR);
        g2d_tvline(id, (8 + shift) & 15, y + 1, y + 3, C_MORTAR);
        g2d_trect(id, 1, y + 1, 3, y + 2, C_BRICK1);
        y = y + 4;
    }
}

void make_stone(int id)
{
    int x;
    int y;
    g2d_fill_tile(atlas, ATLAS_W, TW, TH, id, C_STONE0);
    y = 0;
    while (y < TH) {
        x = 0;
        while (x < TW) {
            if (((x / 4) + (y / 4)) & 1) {
                g2d_tpix(id, x, y, C_STONE1);
            }
            if (((x + y * 3) & 15) == 0) {
                g2d_tpix(id, x, y, C_STONE2);
            }
            x = x + 1;
        }
        y = y + 1;
    }
    g2d_thline(id, 0, 15, 0, C_STONE2);
    g2d_tvline(id, 0, 0, 15, C_STONE2);
}

void make_crate(int id)
{
    g2d_fill_tile(atlas, ATLAS_W, TW, TH, id, C_WOOD0);
    g2d_trect(id, 1, 1, 14, 14, C_WOOD1);
    g2d_thline(id, 1, 14, 1, C_WOOD2);
    g2d_thline(id, 1, 14, 14, C_WOOD2);
    g2d_tvline(id, 1, 1, 14, C_WOOD2);
    g2d_tvline(id, 14, 1, 14, C_WOOD2);
    g2d_thline(id, 2, 13, 5, C_WOOD0);
    g2d_thline(id, 2, 13, 10, C_WOOD0);
    g2d_tvline(id, 8, 2, 13, C_WOOD0);
    g2d_tpix(id, 4, 7, C_WOOD2);
    g2d_tpix(id, 11, 8, C_WOOD2);
}

void make_tree(int id)
{
    g2d_fill_tile(atlas, ATLAS_W, TW, TH, id, C_KEY);
    g2d_trect(id, 7, 10, 8, 15, C_TRUNK);
    g2d_trect(id, 3, 2, 12, 9, C_LEAF);
    g2d_trect(id, 5, 0, 10, 3, C_LEAF);
    g2d_tpix(id, 4, 4, C_GRASS2);
    g2d_tpix(id, 10, 6, C_GRASS2);
}

void make_hero(int id)
{
    g2d_fill_tile(atlas, ATLAS_W, TW, TH, id, C_KEY);
    g2d_trect(id, 5, 3, 10, 7, 64480);
    g2d_trect(id, 4, 8, 11, 13, 2047);
    g2d_trect(id, 3, 8, 4, 12, 128);
    g2d_trect(id, 11, 8, 12, 12, 128);
    g2d_tpix(id, 6, 5, 0);
    g2d_tpix(id, 9, 5, 0);
    g2d_thline(id, 6, 9, 2, 65504);
    g2d_trect(id, 5, 14, 6, 15, C_STONE2);
    g2d_trect(id, 9, 14, 10, 15, C_STONE2);
}

void build_atlas(void)
{
    atlas_begin();
    make_grass(0);
    make_dirt(1);
    make_water(2);
    make_brick(3);
    make_stone(4);
    make_crate(5);
    make_tree(6);
    make_hero(7);
}

void build_map(void)
{
    int y;
    int x;
    int v;
    y = 0;
    while (y < MAP_H) {
        x = 0;
        while (x < MAP_W) {
            v = 0;
            if (y >= 8) {
                if (y <= 11) {
                    v = 1;
                }
            }
            if (y == 14 || y == 15) {
                v = 2;
            }
            if (x == 0 || y == 0 || x == MAP_W - 1 || y == MAP_H - 1) {
                v = 3;
            }
            if (x >= 18 && x <= 22 && y >= 3 && y <= 6) {
                v = 4;
            }
            if ((x == 6 && y == 5) || (x == 12 && y == 9) || (x == 28 && y == 4)) {
                v = 5;
            }
            if ((x == 4 && y == 3) || (x == 15 && y == 6) || (x == 30 && y == 8)) {
                v = 6;
            }
            map[y * MAP_W + x] = v;
            x = x + 1;
        }
        y = y + 1;
    }
}

void frame(void)
{
    ag_gfx_fill_rect(ox, oy, VIEW_W, VIEW_H, 0);
    g2d_set_scroll(px, 0);
    g2d_map_draw();
    g2d_sprite_world(7, px + 48, py);
    ag_gfx_text(ox + 8, oy + 8, "TILE DEMO", 16777215, 0);
    g2d_present();
}

int ag_main(void)
{
    ox = (640 - VIEW_W) / 2;
    oy = (400 - VIEW_H) / 2;
    px = 0;
    py = 96;
    running = 1;

    build_atlas();
    build_map();

    ag_gfx_acquire();
    ag_gfx_clear(0);
    g2d_init(ox, oy, VIEW_W, VIEW_H);
    g2d_tileset(atlas, ATLAS_W, TW, TH);
    g2d_tilemap(map, MAP_W, MAP_H);
    g2d_set_key(KEY_RGB);

    while (running) {
        struct ag_ev ev;
        while (ag_poll_event(&ev, 0)) {
            if (ev.type == AG_EV_FOCUS_GAINED) {
                ag_gfx_acquire();
            } else if (ev.type == AG_EV_QUIT) {
                if (ag_focused()) {
                    running = 0;
                }
            }
        }
        if (running == 0) {
            break;
        }
        if (!ag_focused()) {
            ag_heartbeat();
            ag_delay(50);
            continue;
        }
        if (ag_btn(7)) {
            running = 0;
        }
        if (ag_btn(2)) {
            px = px - 2;
        }
        if (ag_btn(3)) {
            px = px + 2;
        }
        if (ag_btn(0)) {
            py = py - 2;
        }
        if (ag_btn(1)) {
            py = py + 2;
        }
        if (px < 0) {
            px = 0;
        }
        if (py < 0) {
            py = 0;
        }
        if (py > VIEW_H - TH) {
            py = VIEW_H - TH;
        }
        frame();
        ag_delay(16);
    }

    ag_gfx_release();
    return 0;
}
