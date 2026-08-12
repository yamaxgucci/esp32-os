/*
 * wetspot — Argon CC acceptance game for g2d (Wetspot-like).
 *
 * Inspired by Wetspot II (Angelo Mottola / Dmitry Smagin SDL port, GPL-2.0).
 * This is a Mini-C reimplementation of a small playable core, not a compile of
 * the upstream sources. Credits: Mottola, Smagin.
 *
 * HostFS: g2d_globals.h / g2d_impl.h beside this file (from apps/cc/lib/g2d).
 *
 *   run h:\cc.axe h:\wetspot.c h:\wetspot.axe
 *   run h:\wetspot.axe
 *
 * Pad: D-pad move, B1 (Z) wait/noop, Esc quit. Push soft blocks into enemies.
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
#define VIEW_H 176
#define TW 16
#define TH 16
#define ATLAS_W 8
#define MW 20
#define MH 11
#define KEY_RGB 16711935
#define MAX_E 8

char atlas[ATLAS_W * TW * TH * 2];
char map[MW * MH];
int ox;
int oy;
int px;
int py;
int lives;
int score;
int won;
int dead;
int running;
int tick;
int ex[MAX_E];
int ey[MAX_E];
int ealive[MAX_E];
int n_enemies;
int move_cool;

#include "g2d_impl.h"

#define C_KEY 63519
#define C_SAND0 44470
#define C_SAND1 35848
#define C_SAND2 52816
#define C_ROCK0 21130
#define C_ROCK1 31727
#define C_ROCK2 12684
#define C_WOOD0 27206
#define C_WOOD1 18920
#define C_WOOD2 42116
#define C_CRAB0 64064
#define C_CRAB1 64480
#define C_CRAB2 51200
#define C_FISH0 2047
#define C_FISH1 1471
#define C_FISH2 65504
#define C_X 65535
#define C_BLACK 0

void atlas_begin(void)
{
    g2d_tiles = atlas;
    g2d_atlas_w = ATLAS_W;
    g2d_tile_w = TW;
    g2d_tile_h = TH;
    g2d_atlas_stride = ATLAS_W * TW * 2;
}

void make_sand(int id)
{
    int x;
    int y;
    int n;
    g2d_fill_tile(atlas, ATLAS_W, TW, TH, id, C_SAND0);
    y = 0;
    while (y < TH) {
        x = 0;
        while (x < TW) {
            n = (x * 7 + y * 5) & 7;
            if (n < 2) {
                g2d_tpix(id, x, y, C_SAND1);
            }
            if (n == 4) {
                g2d_tpix(id, x, y, C_SAND2);
            }
            x = x + 1;
        }
        y = y + 1;
    }
}

void make_rock(int id)
{
    int x;
    int y;
    g2d_fill_tile(atlas, ATLAS_W, TW, TH, id, C_ROCK0);
    y = 0;
    while (y < TH) {
        x = 0;
        while (x < TW) {
            if (((x / 3) ^ (y / 3)) & 1) {
                g2d_tpix(id, x, y, C_ROCK1);
            }
            if (((x + y * 2) & 7) == 0) {
                g2d_tpix(id, x, y, C_ROCK2);
            }
            x = x + 1;
        }
        y = y + 1;
    }
    g2d_thline(id, 0, 15, 0, C_ROCK2);
    g2d_thline(id, 0, 15, 15, C_ROCK2);
    g2d_tvline(id, 0, 0, 15, C_ROCK2);
    g2d_tvline(id, 15, 0, 15, C_ROCK2);
}

void make_crate(int id)
{
    g2d_fill_tile(atlas, ATLAS_W, TW, TH, id, C_WOOD0);
    g2d_trect(id, 1, 1, 14, 14, C_WOOD1);
    g2d_thline(id, 0, 15, 0, C_WOOD2);
    g2d_thline(id, 0, 15, 15, C_WOOD2);
    g2d_tvline(id, 0, 0, 15, C_WOOD2);
    g2d_tvline(id, 15, 0, 15, C_WOOD2);
    g2d_thline(id, 2, 13, 5, C_WOOD0);
    g2d_thline(id, 2, 13, 10, C_WOOD0);
    g2d_tvline(id, 8, 2, 13, C_WOOD0);
    g2d_trect(id, 6, 6, 9, 9, C_WOOD2);
    g2d_tpix(id, 7, 7, C_SAND2);
}

void make_crab(int id)
{
    g2d_fill_tile(atlas, ATLAS_W, TW, TH, id, C_KEY);
    g2d_trect(id, 4, 6, 11, 11, C_CRAB0);
    g2d_trect(id, 5, 5, 10, 6, C_CRAB1);
    g2d_trect(id, 1, 7, 3, 9, C_CRAB0);
    g2d_trect(id, 12, 7, 14, 9, C_CRAB0);
    g2d_tpix(id, 1, 6, C_CRAB2);
    g2d_tpix(id, 14, 6, C_CRAB2);
    g2d_tpix(id, 3, 12, C_CRAB2);
    g2d_tpix(id, 5, 13, C_CRAB2);
    g2d_tpix(id, 10, 13, C_CRAB2);
    g2d_tpix(id, 12, 12, C_CRAB2);
    g2d_tpix(id, 6, 7, C_X);
    g2d_tpix(id, 9, 7, C_X);
    g2d_tpix(id, 6, 8, C_BLACK);
    g2d_tpix(id, 9, 8, C_BLACK);
}

void make_fish(int id)
{
    g2d_fill_tile(atlas, ATLAS_W, TW, TH, id, C_KEY);
    g2d_trect(id, 3, 5, 11, 10, C_FISH0);
    g2d_trect(id, 4, 4, 10, 5, C_FISH1);
    g2d_tpix(id, 12, 6, C_FISH0);
    g2d_tpix(id, 13, 7, C_FISH0);
    g2d_tpix(id, 14, 8, C_FISH1);
    g2d_tpix(id, 13, 9, C_FISH0);
    g2d_tpix(id, 12, 10, C_FISH0);
    g2d_tpix(id, 7, 3, C_FISH1);
    g2d_tpix(id, 8, 3, C_FISH1);
    g2d_tpix(id, 5, 6, C_X);
    g2d_tpix(id, 5, 7, C_BLACK);
    g2d_thline(id, 6, 10, 8, C_FISH2);
}

void make_crush(int id)
{
    g2d_fill_tile(atlas, ATLAS_W, TW, TH, id, C_KEY);
    g2d_thline(id, 3, 12, 3, C_X);
    g2d_thline(id, 3, 12, 12, C_X);
    g2d_tvline(id, 3, 3, 12, C_X);
    g2d_tvline(id, 12, 3, 12, C_X);
    g2d_tpix(id, 5, 5, C_X);
    g2d_tpix(id, 10, 10, C_X);
    g2d_tpix(id, 10, 5, C_X);
    g2d_tpix(id, 5, 10, C_X);
}

void build_atlas(void)
{
    atlas_begin();
    g2d_fill_tile(atlas, ATLAS_W, TW, TH, 0, C_BLACK);
    make_sand(1);
    make_rock(2);
    make_crate(3);
    make_crab(4);
    make_fish(5);
    make_crush(6);
}

void cell_set(int x, int y, int v)
{
    map[y * MW + x] = v;
}

int cell_get(int x, int y)
{
    if (x < 0 || y < 0 || x >= MW || y >= MH) {
        return 2;
    }
    return map[y * MW + x] & 255;
}

void build_level(void)
{
    int y;
    int x;
    y = 0;
    while (y < MH) {
        x = 0;
        while (x < MW) {
            if (x == 0 || y == 0 || x == MW - 1 || y == MH - 1) {
                cell_set(x, y, 2);
            } else {
                cell_set(x, y, 1);
            }
            x = x + 1;
        }
        y = y + 1;
    }
    /* soft blocks */
    cell_set(4, 3, 3);
    cell_set(5, 3, 3);
    cell_set(8, 5, 3);
    cell_set(9, 5, 3);
    cell_set(10, 5, 3);
    cell_set(14, 2, 3);
    cell_set(14, 3, 3);
    cell_set(6, 7, 3);
    cell_set(12, 8, 3);
    /* pillars */
    cell_set(7, 4, 2);
    cell_set(13, 6, 2);
    px = 2;
    py = 2;
    n_enemies = 4;
    ex[0] = 17;
    ey[0] = 2;
    ex[1] = 17;
    ey[1] = 8;
    ex[2] = 10;
    ey[2] = 3;
    ex[3] = 3;
    ey[3] = 8;
    {
        int i;
        i = 0;
        while (i < n_enemies) {
            ealive[i] = 1;
            i = i + 1;
        }
    }
}

int enemy_at(int x, int y)
{
    int i;
    i = 0;
    while (i < n_enemies) {
        if (ealive[i]) {
            if (ex[i] == x) {
                if (ey[i] == y) {
                    return i;
                }
            }
        }
        i = i + 1;
    }
    return -1;
}

void crush_enemy(int x, int y)
{
    int i;
    i = enemy_at(x, y);
    if (i < 0) {
        return;
    }
    ealive[i] = 0;
    score = score + 100;
    cell_set(x, y, 6);
}

int try_push(int x, int y, int dx, int dy)
{
    int nx;
    int ny;
    int v;
    nx = x + dx;
    ny = y + dy;
    v = cell_get(nx, ny);
    if (v == 2) {
        return 0;
    }
    if (v == 3) {
        return 0;
    }
    if (enemy_at(nx, ny) >= 0) {
        crush_enemy(nx, ny);
        cell_set(x, y, 1);
        cell_set(nx, ny, 3);
        return 1;
    }
    if (v == 1 || v == 6 || v == 0) {
        cell_set(x, y, 1);
        cell_set(nx, ny, 3);
        return 1;
    }
    return 0;
}

void try_move(int dx, int dy)
{
    int nx;
    int ny;
    int v;
    if (dead || won) {
        return;
    }
    nx = px + dx;
    ny = py + dy;
    v = cell_get(nx, ny);
    if (v == 2) {
        return;
    }
    if (v == 3) {
        if (try_push(nx, ny, dx, dy) == 0) {
            return;
        }
    }
    if (enemy_at(nx, ny) >= 0) {
        dead = 1;
        lives = lives - 1;
        return;
    }
    px = nx;
    py = ny;
}

void enemy_step(int i)
{
    int dx;
    int dy;
    int nx;
    int ny;
    int v;
    int dir;
    if (ealive[i] == 0) {
        return;
    }
    dir = (tick + i * 3) & 3;
    dx = 0;
    dy = 0;
    if (dir == 0) {
        dy = -1;
    }
    if (dir == 1) {
        dy = 1;
    }
    if (dir == 2) {
        dx = -1;
    }
    if (dir == 3) {
        dx = 1;
    }
    nx = ex[i] + dx;
    ny = ey[i] + dy;
    v = cell_get(nx, ny);
    if (v != 1) {
        if (v != 6) {
            return;
        }
    }
    if (enemy_at(nx, ny) >= 0) {
        return;
    }
    if (nx == px) {
        if (ny == py) {
            dead = 1;
            lives = lives - 1;
            return;
        }
    }
    ex[i] = nx;
    ey[i] = ny;
}

void enemies_think(void)
{
    int i;
    int left;
    i = 0;
    while (i < n_enemies) {
        enemy_step(i);
        i = i + 1;
    }
    left = 0;
    i = 0;
    while (i < n_enemies) {
        if (ealive[i]) {
            left = left + 1;
        }
        i = i + 1;
    }
    if (left == 0) {
        won = 1;
    }
}

void draw_world(void)
{
    int y;
    int x;
    int v;
    int i;
    ag_gfx_fill_rect(ox, oy, VIEW_W, VIEW_H, 0);
    y = 0;
    while (y < MH) {
        x = 0;
        while (x < MW) {
            v = cell_get(x, y);
            if (v == 6) {
                g2d_draw_tile(1, ox + x * TW, oy + y * TH);
                g2d_draw_sprite(6, ox + x * TW, oy + y * TH);
            } else {
                g2d_draw_tile(v, ox + x * TW, oy + y * TH);
            }
            x = x + 1;
        }
        y = y + 1;
    }
    i = 0;
    while (i < n_enemies) {
        if (ealive[i]) {
            g2d_draw_sprite(5, ox + ex[i] * TW, oy + ey[i] * TH);
        }
        i = i + 1;
    }
    if (dead == 0) {
        g2d_draw_sprite(4, ox + px * TW, oy + py * TH);
    }
    ag_gfx_text(ox + 8, oy + VIEW_H - 8, "WETSPOT CC", 16777215, 0);
    if (won) {
        ag_gfx_text(ox + 100, oy + 80, "YOU WIN", 65280, 0);
    }
    if (dead) {
        ag_gfx_text(ox + 100, oy + 80, "OUCH", 16711680, 0);
    }
    g2d_present();
}

int ag_main(void)
{
    ox = (640 - VIEW_W) / 2;
    oy = (400 - VIEW_H) / 2;
    lives = 3;
    score = 0;
    won = 0;
    dead = 0;
    running = 1;
    tick = 0;
    move_cool = 0;

    build_atlas();
    build_level();

    ag_gfx_acquire();
    ag_gfx_clear(0);
    g2d_init(ox, oy, VIEW_W, VIEW_H);
    g2d_tileset(atlas, ATLAS_W, TW, TH);
    g2d_tilemap(map, MW, MH);
    g2d_set_key(KEY_RGB);
    g2d_set_scroll(0, 0);

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
        if (move_cool > 0) {
            move_cool = move_cool - 1;
        } else {
            if (dead) {
                if (lives > 0) {
                    if (ag_btn(4)) {
                        dead = 0;
                        px = 2;
                        py = 2;
                        move_cool = 10;
                    }
                }
            } else {
                if (ag_btn(2)) {
                    try_move(-1, 0);
                    move_cool = 8;
                }
                if (ag_btn(3)) {
                    try_move(1, 0);
                    move_cool = 8;
                }
                if (ag_btn(0)) {
                    try_move(0, -1);
                    move_cool = 8;
                }
                if (ag_btn(1)) {
                    try_move(0, 1);
                    move_cool = 8;
                }
            }
        }
        tick = tick + 1;
        if ((tick & 15) == 0) {
            if (dead == 0) {
                if (won == 0) {
                    enemies_think();
                }
            }
        }
        draw_world();
        ag_delay(16);
    }

    ag_gfx_release();
    return score;
}
