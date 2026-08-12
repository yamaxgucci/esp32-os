/*
 * Harbor Quest — top-down RPG for Argon CC + g2d (Phantasy Star-like).
 *
 * HostFS next to this file: g2d_globals.h, g2d_impl.h, hq_data.h, atlas.bin
 *
 *   run h:\cc.axe h:\harbor.c h:\harbr.axe
 *   run h:\harbr.axe
 *
 * Keyboard (sms.cfg / HostFS PADPUSH — focus the SDL window):
 *   Arrows move, Z or Enter talk/confirm, X cancel/run, Esc quit.
 */
#pragma appstack 24576
#pragma appheap 2097152

#include "g2d_globals.h"
#include "hq_data.h"

#include "g2d_impl.h"

/* ag_event_t layout (ILP32): type @0, then pad+ts+union — keep ≥40 bytes. */
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

/* sms.cfg: pad0.b1=Z, pad0.b2=X, pad0.start=ENTER, pad0.quit=ESC */
int btn_ok(void)
{
    if (ag_btn(4)) {
        return 1;
    }
    if (ag_btn(9)) {
        return 1;
    }
    return 0;
}

int btn_back(void)
{
    if (ag_btn(5)) {
        return 1;
    }
    return 0;
}

void sav_put(int v)
{
    savbuf[sav_off] = v & 255;
    savbuf[sav_off + 1] = (v >> 8) & 255;
    savbuf[sav_off + 2] = (v >> 16) & 255;
    savbuf[sav_off + 3] = (v >> 24) & 255;
    sav_off = sav_off + 4;
}

int sav_get(void)
{
    int v;
    v = savbuf[sav_off] & 255;
    v = v | ((savbuf[sav_off + 1] & 255) << 8);
    v = v | ((savbuf[sav_off + 2] & 255) << 16);
    v = v | ((savbuf[sav_off + 3] & 255) << 24);
    sav_off = sav_off + 4;
    return v;
}

int save_game(void)
{
    int h;
    int n;
    sav_off = 0;
    sav_put(SAVE_MAGIC);
    sav_put(SAVE_VER);
    sav_put(qflags);
    sav_put(gold);
    sav_put(potions);
    sav_put(has_sword);
    sav_put(map_id);
    sav_put(px);
    sav_put(py);
    sav_put(pdir);
    sav_put(hero_hp);
    sav_put(hero_mhp);
    sav_put(hero_atk);
    sav_put(hero_def);
    sav_put(hero_lvl);
    sav_put(hero_exp);
    sav_put(ban_in);
    sav_put(ban_hp);
    sav_put(ban_mhp);
    sav_put(ban_atk);
    sav_put(ban_def);
    h = ag_open(SAVE_PATH, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (h < 0) {
        return 0;
    }
    n = ag_write(h, savbuf, sav_off);
    ag_close(h);
    if (n != sav_off) {
        return 0;
    }
    save_ok = 1;
    need_save = 0;
    return 1;
}

int clamp(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

void draw_num(int x, int y, int v, int fg)
{
    int n;
    int i;
    int t;
    int neg;
    neg = 0;
    if (v < 0) {
        neg = 1;
        v = 0 - v;
    }
    if (v == 0) {
        numbuf[0] = '0';
        numbuf[1] = 0;
        ag_gfx_text(x, y, numbuf, fg, 0);
        return;
    }
    n = 0;
    while (v > 0) {
        if (n >= 10) {
            break;
        }
        numbuf[n] = '0' + (v % 10);
        v = v / 10;
        n = n + 1;
    }
    if (neg) {
        numbuf[n] = '-';
        n = n + 1;
    }
    i = 0;
    while (i < n / 2) {
        t = numbuf[i];
        numbuf[i] = numbuf[n - 1 - i];
        numbuf[n - 1 - i] = t;
        i = i + 1;
    }
    numbuf[n] = 0;
    ag_gfx_text(x, y, numbuf, fg, 0);
}

int tile_at(int tx, int ty)
{
    if (tx < 0 || ty < 0 || tx >= map_w || ty >= map_h) {
        return T_VOID;
    }
    return map_ptr[ty * map_w + tx] & 255;
}

int tile_solid(int t)
{
    if (t == T_WALL || t == T_WATER || t == T_DEEP || t == T_TREE) {
        return 1;
    }
    if (t == T_ROCK || t == T_CWALL || t == T_ROOF || t == T_COUNTER) {
        return 1;
    }
    if (t == T_VOID || t == T_FENCE || t == T_WINDOW || t == T_BED) {
        return 1;
    }
    return 0;
}

void map_fill(int h, int t)
{
    int i;
    i = 0;
    while (i < mw_w * h) {
        mw_map[i] = t;
        i = i + 1;
    }
}

void map_rect(int x0, int y0, int x1, int y1, int t)
{
    int x;
    int y;
    y = y0;
    while (y < y1) {
        x = x0;
        while (x < x1) {
            if (x >= 0 && y >= 0 && x < mw_w) {
                mw_map[y * mw_w + x] = t;
            }
            x = x + 1;
        }
        y = y + 1;
    }
}

void map_set(int x, int y, int t)
{
    mw_map[y * mw_w + x] = t;
}

void build_town(void)
{
    int x;
    int y;
    mw_map = map_town;
    mw_w = TOWN_W;
    map_fill(TOWN_H, T_SAND);
    map_rect(0, 0, TOWN_W, 8, T_DEEP);
    map_rect(0, 8, TOWN_W, 11, T_WATER);
    map_rect(14, 8, 20, 12, T_PIER);
    map_set(16, 7, T_SAIL);
    map_rect(2, 12, TOWN_W - 2, TOWN_H - 2, T_GRASS);
    map_rect(16, 12, 20, TOWN_H - 1, T_ROAD);
    map_rect(2, 18, TOWN_W - 2, 20, T_ROAD);
    map_rect(4, 14, 10, 19, T_WALL);
    map_rect(5, 15, 9, 18, T_FLOOR);
    map_set(7, 18, T_DOOR);
    map_set(6, 15, T_BED);
    map_set(8, 15, T_WINDOW);
    map_rect(4, 13, 10, 14, T_ROOF);
    map_rect(22, 14, 30, 19, T_WALL);
    map_rect(23, 15, 29, 18, T_FLOOR);
    map_set(25, 18, T_DOOR);
    map_rect(23, 16, 28, 17, T_COUNTER);
    map_rect(22, 13, 30, 14, T_ROOF);
    map_rect(10, 21, 18, 26, T_WALL);
    map_rect(11, 22, 17, 25, T_FLOOR);
    map_set(14, 25, T_DOOR);
    map_set(12, 22, T_BED);
    map_rect(10, 20, 18, 21, T_ROOF);
    y = 12;
    while (y < TOWN_H - 1) {
        x = 2;
        while (x < TOWN_W - 2) {
            if (map_town[y * TOWN_W + x] == T_GRASS) {
                if (((x * 5 + y * 3) & 15) == 0) {
                    map_set(x, y, T_TREE);
                }
                if (((x * 7 + y) & 15) == 3) {
                    map_set(x, y, T_FLOWER);
                }
            }
            x = x + 1;
        }
        y = y + 1;
    }
    map_set(18, TOWN_H - 2, T_SIGN);
    map_set(18, TOWN_H - 1, T_ROAD);
}

void build_field(void)
{
    int x;
    int y;
    int n;
    mw_map = map_field;
    mw_w = FIELD_W;
    map_fill(FIELD_H, T_GRASS);
    y = 0;
    while (y < FIELD_H) {
        x = 0;
        while (x < FIELD_W) {
            n = (x * 3 + y * 5) & 7;
            if (n == 0) {
                map_set(x, y, T_GRASS2);
            }
            if (n == 2) {
                map_set(x, y, T_BUSH);
            }
            if (n == 4 && (x & 3) == 0) {
                map_set(x, y, T_TREE);
            }
            if (n == 6 && y > 4) {
                map_set(x, y, T_ROCK);
            }
            x = x + 1;
        }
        y = y + 1;
    }
    map_rect(22, 0, 26, FIELD_H, T_ROAD);
    map_rect(0, 14, FIELD_W, 17, T_PATH);
    map_rect(0, 0, FIELD_W, 3, T_WATER);
    map_rect(20, 12, 28, 15, T_BRIDGE);
    map_rect(30, 4, 36, 9, T_ROCK);
    map_set(33, 8, T_STAIRS);
    map_set(24, FIELD_H - 1, T_SIGN);
    map_set(24, 0, T_SIGN);
}

void build_gray(void)
{
    int x;
    int y;
    mw_map = map_gray;
    mw_w = GRAY_W;
    map_fill(GRAY_H, T_GRASS);
    map_rect(0, GRAY_H - 3, GRAY_W, GRAY_H, T_ROAD);
    map_rect(12, 0, 16, GRAY_H, T_ROAD);
    map_rect(4, 4, 12, 10, T_WALL);
    map_rect(5, 5, 11, 9, T_FLOOR);
    map_set(8, 9, T_DOOR);
    map_rect(4, 3, 12, 4, T_ROOF);
    map_rect(16, 6, 24, 12, T_WALL);
    map_rect(17, 7, 23, 11, T_FLOOR);
    map_set(20, 11, T_DOOR);
    map_set(18, 8, T_CHEST);
    map_rect(16, 5, 24, 6, T_ROOF);
    y = 2;
    while (y < GRAY_H - 3) {
        x = 1;
        while (x < GRAY_W - 1) {
            if (map_gray[y * GRAY_W + x] == T_GRASS && ((x + y) & 7) == 0) {
                map_set(x, y, T_TREE);
            }
            x = x + 1;
        }
        y = y + 1;
    }
    map_set(14, GRAY_H - 1, T_SIGN);
}

void build_cave(void)
{
    int x;
    int y;
    mw_map = map_cave;
    mw_w = CAVE_W;
    map_fill(CAVE_H, T_CWALL);
    map_rect(2, 2, CAVE_W - 2, CAVE_H - 2, T_CFLOOR);
    map_rect(8, 2, 10, 14, T_CWALL);
    map_rect(14, 8, 28, 10, T_CWALL);
    map_rect(20, 10, 22, 20, T_CWALL);
    map_set(9, 6, T_CFLOOR);
    map_set(18, 9, T_CFLOOR);
    map_set(21, 15, T_CFLOOR);
    map_set(4, CAVE_H - 3, T_STAIRS);
    map_set(25, 4, T_CHEST);
    map_set(26, 18, T_TORCH);
    y = 3;
    while (y < CAVE_H - 3) {
        x = 3;
        while (x < CAVE_W - 3) {
            if (map_cave[y * CAVE_W + x] == T_CFLOOR && ((x * y) & 31) == 0) {
                map_set(x, y, T_ROCK);
            }
            x = x + 1;
        }
        y = y + 1;
    }
}

void npc_clear(void)
{
    npc_n = 0;
}

/* rad: key NPCs ~2, filler townsfolk ~6, 0 = rooted. */
void npc_add(int x, int y, int sp, int dlg, int rad)
{
    if (npc_n >= NPC_MAX) {
        return;
    }
    npc_x[npc_n] = x;
    npc_y[npc_n] = y;
    npc_hx[npc_n] = x;
    npc_hy[npc_n] = y;
    npc_rad[npc_n] = rad;
    npc_cool[npc_n] = 20 + ((npc_n * 11) & 31);
    npc_sp[npc_n] = sp;
    npc_dlg[npc_n] = dlg;
    npc_frame[npc_n] = 0;
    npc_n = npc_n + 1;
}

int npc_cell_free(int nx, int ny, int self)
{
    int j;
    if (tile_solid(tile_at(nx, ny))) {
        return 0;
    }
    if (nx == px && ny == py) {
        return 0;
    }
    j = 0;
    while (j < npc_n) {
        if (j != self && npc_x[j] == nx && npc_y[j] == ny) {
            return 0;
        }
        j = j + 1;
    }
    return 1;
}

void npc_step(int i, int dx, int dy)
{
    int nx;
    int ny;
    int ax;
    int ay;
    nx = npc_x[i] + dx;
    ny = npc_y[i] + dy;
    ax = nx - npc_hx[i];
    ay = ny - npc_hy[i];
    if (ax < 0) {
        ax = 0 - ax;
    }
    if (ay < 0) {
        ay = 0 - ay;
    }
    if (ax > npc_rad[i] || ay > npc_rad[i]) {
        return;
    }
    if (!npc_cell_free(nx, ny, i)) {
        return;
    }
    npc_x[i] = nx;
    npc_y[i] = ny;
    npc_frame[i] = 1 - npc_frame[i];
}

void npc_wander(void)
{
    int i;
    int r;
    int dx;
    int dy;
    i = 0;
    while (i < npc_n) {
        if (npc_cool[i] > 0) {
            npc_cool[i] = npc_cool[i] - 1;
        } else if (npc_rad[i] > 0) {
            r = (tick * 17 + i * 31 + steps * 3) & 7;
            dx = 0;
            dy = 0;
            if (r == 0) {
                dy = -1;
            } else if (r == 1) {
                dy = 1;
            } else if (r == 2) {
                dx = -1;
            } else if (r == 3) {
                dx = 1;
            }
            if (dx != 0 || dy != 0) {
                npc_step(i, dx, dy);
            }
            /* Key (small rad): pause longer. Wanderers move more often. */
            if (npc_rad[i] <= 2) {
                npc_cool[i] = 40 + ((tick + i * 7) & 31);
            } else {
                npc_cool[i] = 18 + ((tick + i * 5) & 15);
            }
        }
        i = i + 1;
    }
}

void use_map(int id, int x, int y)
{
    map_id = id;
    if (id == MAP_TOWN) {
        map_ptr = map_town;
        map_w = TOWN_W;
        map_h = TOWN_H;
        npc_clear();
        /* Key NPCs: small leash. Villagers: wide roam. */
        npc_add(9, 19, SP_LINA, 1, 2);
        npc_add(25, 17, SP_SHOP, 2, 2);
        npc_add(14, 23, SP_INN, 3, 2);
        npc_add(20, 19, SP_VILL, 4, 6);
    } else if (id == MAP_FIELD) {
        map_ptr = map_field;
        map_w = FIELD_W;
        map_h = FIELD_H;
        npc_clear();
        npc_add(10, 16, SP_VILL, 5, 6);
    } else if (id == MAP_GRAY) {
        map_ptr = map_gray;
        map_w = GRAY_W;
        map_h = GRAY_H;
        npc_clear();
        npc_add(8, 7, SP_ELDER, 6, 2);
        if ((qflags & QF_BAN_JOIN) == 0) {
            npc_add(20, 9, SP_BAN, 7, 2);
        }
        npc_add(6, 14, SP_VILL, 8, 6);
    } else {
        map_ptr = map_cave;
        map_w = CAVE_W;
        map_h = CAVE_H;
        npc_clear();
        if ((qflags & QF_CAVE) == 0) {
            npc_add(26, 17, SP_BOSS, 9, 1);
        }
    }
    g2d_tilemap(map_ptr, map_w, map_h);
    px = x;
    py = y;
    steps = 0;
}

void dlg_begin(void)
{
    dlg_i = 0;
    dlg_n = 0;
    dlg0 = 0;
    dlg1 = 0;
    dlg2 = 0;
    dlg3 = 0;
    dlg4 = 0;
    dlg5 = 0;
    dlg6 = 0;
    dlg7 = 0;
}

void dlg_add(char *s)
{
    if (dlg_n == 0) {
        dlg0 = s;
    } else if (dlg_n == 1) {
        dlg1 = s;
    } else if (dlg_n == 2) {
        dlg2 = s;
    } else if (dlg_n == 3) {
        dlg3 = s;
    } else if (dlg_n == 4) {
        dlg4 = s;
    } else if (dlg_n == 5) {
        dlg5 = s;
    } else if (dlg_n == 6) {
        dlg6 = s;
    } else if (dlg_n == 7) {
        dlg7 = s;
    } else {
        return;
    }
    dlg_n = dlg_n + 1;
}

int dlg_get(int i)
{
    if (i == 0) {
        return dlg0;
    }
    if (i == 1) {
        return dlg1;
    }
    if (i == 2) {
        return dlg2;
    }
    if (i == 3) {
        return dlg3;
    }
    if (i == 4) {
        return dlg4;
    }
    if (i == 5) {
        return dlg5;
    }
    if (i == 6) {
        return dlg6;
    }
    if (i == 7) {
        return dlg7;
    }
    return 0;
}

void dlg_start_id(int id)
{
    dlg_begin();
    if (id == 1) {
        if ((qflags & QF_DONE) != 0) {
            dlg_add("Lina: You came back...");
            dlg_add("Lina: I knew the harbor wind");
            dlg_add("would bring you home.");
            dlg_add("--- Chapter 1 Complete ---");
        } else if ((qflags & QF_CAVE) != 0) {
            dlg_add("Lina: The cave is quiet now.");
            dlg_add("Lina: Rest, then tell me of");
            dlg_add("the road beyond Grayfen.");
            qflags = qflags | QF_DONE;
            dlg_add("--- Chapter 1 Complete ---");
        } else if ((qflags & QF_LINA) == 0) {
            dlg_add("Lina: Roy... must you leave");
            dlg_add("Minato Harbor so soon?");
            dlg_add("Lina: Buy a blade. Find a");
            dlg_add("guide. Prove you can return.");
            qflags = qflags | QF_LINA;
        } else {
            dlg_add("Lina: North road to Grayfen.");
            dlg_add("Lina: Come home to me.");
        }
    } else if (id == 2) {
        if ((qflags & QF_SWORD) == 0) {
            dlg_add("Smith: A traveler needs steel.");
            dlg_add("Smith: Short sword - 20 gold.");
            dlg_add("Z/Enter buy   X leave");
            dlg_i = 0;
            state = ST_SHOP;
            return;
        }
        dlg_add("Smith: That blade will bite.");
        dlg_add("Smith: Bandits fear honest iron.");
    } else if (id == 3) {
        dlg_add("Inn: Bed and broth - 10 gold.");
        dlg_add("Z/Enter rest   X leave");
        dlg_i = 0;
        state = ST_INN;
        return;
    } else if (id == 4) {
        dlg_add("Sailor: Storms took three ships");
        dlg_add("last moon. Stick to the road.");
        dlg_add("Sailor: Grayfen elder knows the");
        dlg_add("cave where bandits nest.");
    } else if (id == 5) {
        dlg_add("Wanderer: Slimes on the path.");
        dlg_add("Wanderer: Stairs in the rocks");
        dlg_add("mark the bandit cave.");
    } else if (id == 6) {
        dlg_add("Elder: Welcome to Grayfen.");
        dlg_add("Elder: Bandits stole our stores.");
        dlg_add("Elder: Ask Ban by the storehouse.");
        dlg_add("Elder: Clear their cave, please.");
    } else if (id == 7) {
        if ((qflags & QF_BAN_JOIN) != 0) {
            dlg_add("Ban: I am with you, Roy.");
        } else if ((qflags & QF_SWORD) == 0) {
            dlg_add("Ban: You look unarmed.");
            dlg_add("Ban: Buy a sword in Minato.");
            qflags = qflags | QF_BAN_MET;
        } else {
            dlg_add("Ban: Name's Ban. I hit hard.");
            dlg_add("Ban: Cave north of the field.");
            dlg_add("Ban: I'll watch your back.");
            qflags = qflags | QF_BAN_MET;
            qflags = qflags | QF_BAN_JOIN;
            ban_in = 1;
            ban_hp = ban_mhp;
            dlg_add("*** Ban joined the party! ***");
        }
    } else if (id == 8) {
        dlg_add("Villager: Since the raids, we");
        dlg_add("sleep with doors barred.");
    } else if (id == 9) {
        dlg_add("Cave Chief: Another soft harbor");
        dlg_add("boy? Your gold is mine!");
        state = ST_BATTLE;
        /* battle started from talk */
        return;
    } else {
        dlg_add("...");
    }
    if (dlg_n > 0) {
        state = ST_DIALOG;
        dlg_i = 0;
    }
}

int load_atlas_path(char *path)
{
    int h;
    int n;
    int got;
    h = ag_open(path, 0);
    if (h < 0) {
        return 0;
    }
    /* flags: AG_O_RDONLY == 0 */
    n = 0;
    while (n < ATLAS_BYTES) {
        got = ag_read(h, atlas + n, ATLAS_BYTES - n);
        if (got <= 0) {
            break;
        }
        n = n + got;
    }
    ag_close(h);
    if (n == ATLAS_BYTES) {
        return 1;
    }
    return 0;
}

void paint_tile_fallback(int id, int c)
{
    g2d_fill_tile(atlas, ATLAS_W, TW, TH, id, c);
}

void make_atlas_fallback(void)
{
    int i;
    i = 0;
    while (i < ATLAS_N) {
        paint_tile_fallback(i, 63519);
        i = i + 1;
    }
    paint_tile_fallback(T_GRASS, 8416);
    paint_tile_fallback(T_GRASS2, 14727);
    paint_tile_fallback(T_DIRT, 18920);
    paint_tile_fallback(T_SAND, 27369);
    paint_tile_fallback(T_WATER, 1471);
    paint_tile_fallback(T_ROAD, 21070);
    paint_tile_fallback(T_WALL, 16912);
    paint_tile_fallback(T_FLOOR, 23153);
    paint_tile_fallback(T_DOOR, 18920);
    paint_tile_fallback(T_TREE, 9984);
    paint_tile_fallback(T_ROCK, 21130);
    paint_tile_fallback(T_BUSH, 8416);
    paint_tile_fallback(T_CARPET, 40960);
    paint_tile_fallback(T_COUNTER, 21070);
    paint_tile_fallback(T_CWALL, 10570);
    paint_tile_fallback(T_CFLOOR, 8484);
    paint_tile_fallback(T_ROOF, 40960);
    paint_tile_fallback(T_STAIRS, 16912);
    paint_tile_fallback(T_BRIDGE, 18920);
    paint_tile_fallback(T_DEEP, 659);
    paint_tile_fallback(T_PIER, 21070);
    paint_tile_fallback(T_VOID, 0);
    /* crude hero */
    paint_tile_fallback(SP_HERO0, 63519);
    g2d_trect(SP_HERO0, 5, 2, 6, 5, 30356);
    g2d_trect(SP_HERO0, 4, 6, 8, 6, 1376);
    g2d_trect(SP_HERO0 + 1, 5, 2, 6, 5, 30356);
    g2d_trect(SP_HERO0 + 1, 4, 6, 8, 6, 1376);
    i = 2;
    while (i < 8) {
        paint_tile_fallback(SP_HERO0 + i, 63519);
        g2d_trect(SP_HERO0 + i, 5, 2, 6, 5, 30356);
        g2d_trect(SP_HERO0 + i, 4, 6, 8, 6, 1376);
        i = i + 1;
    }
    paint_tile_fallback(SP_LINA, 63519);
    g2d_trect(SP_LINA, 5, 2, 6, 5, 30356);
    g2d_trect(SP_LINA, 4, 6, 8, 6, 51200);
    paint_tile_fallback(SP_BAN, 63519);
    g2d_trect(SP_BAN, 5, 2, 6, 5, 25136);
    g2d_trect(SP_BAN, 4, 6, 8, 6, 8464);
    paint_tile_fallback(SP_SHOP, 63519);
    g2d_trect(SP_SHOP, 4, 4, 8, 8, 21070);
    paint_tile_fallback(SP_SLIME, 63519);
    g2d_trect(SP_SLIME, 3, 6, 10, 8, 14727);
    paint_tile_fallback(SP_BANDIT, 63519);
    g2d_trect(SP_BANDIT, 4, 4, 8, 10, 40960);
    paint_tile_fallback(SP_BOSS, 63519);
    g2d_trect(SP_BOSS, 2, 2, 12, 12, 40960);
}

int hero_sprite(void)
{
    int base;
    base = SP_HERO0 + pdir * 2;
    if (panim > 0) {
        return base + pframe;
    }
    return base;
}

void camera_follow(void)
{
    int sx;
    int sy;
    sx = px * TW + 8 - VIEW_W / 2;
    sy = py * TH + 8 - VIEW_H / 2;
    sx = clamp(sx, 0, map_w * TW - VIEW_W);
    sy = clamp(sy, 0, map_h * TH - VIEW_H);
    if (map_w * TW < VIEW_W) {
        sx = 0;
    }
    if (map_h * TH < VIEW_H) {
        sy = 0;
    }
    g2d_set_scroll(sx, sy);
}

void draw_hud(void)
{
    ag_gfx_text(ox + 4, oy + 4, "Roy HP", 16777215, 0);
    draw_num(ox + 60, oy + 4, hero_hp, 65280);
    if (ban_in) {
        ag_gfx_text(ox + 100, oy + 4, "Ban", 16777215, 0);
        draw_num(ox + 132, oy + 4, ban_hp, 65280);
    }
    ag_gfx_text(ox + 200, oy + 4, "G", 16776960, 0);
    draw_num(ox + 216, oy + 4, gold, 16776960);
}

void draw_field(void)
{
    int i;
    int sp;
    ag_gfx_fill_rect(ox, oy, VIEW_W, VIEW_H, 0);
    camera_follow();
    g2d_map_draw();
    i = 0;
    while (i < npc_n) {
        sp = npc_sp[i];
        if (sp == SP_LINA || sp == SP_BAN) {
            sp = sp + npc_frame[i];
        } else if ((tick & 16) != 0) {
            if (sp == SP_SLIME || sp == SP_BANDIT || sp == SP_BAT ||
                sp == SP_BOSS) {
                sp = sp + 1;
            }
        }
        g2d_sprite_world(sp, npc_x[i] * TW, npc_y[i] * TH);
        i = i + 1;
    }
    g2d_sprite_world(hero_sprite(), px * TW, py * TH);
    draw_hud();
    if (map_id == MAP_TOWN) {
        ag_gfx_text(ox + 4, oy + VIEW_H - 12, "Minato  Z:talk", 16777215, 0);
    } else if (map_id == MAP_FIELD) {
        ag_gfx_text(ox + 4, oy + VIEW_H - 12, "Road  Z:talk", 16777215, 0);
    } else if (map_id == MAP_GRAY) {
        ag_gfx_text(ox + 4, oy + VIEW_H - 12, "Grayfen  Z:talk", 16777215, 0);
    } else {
        ag_gfx_text(ox + 4, oy + VIEW_H - 12, "Cave  Z:talk", 16777215, 0);
    }
}

void draw_dialog(void)
{
    int a;
    int b;
    draw_field();
    ag_gfx_fill_rect(ox + 8, oy + VIEW_H - 56, VIEW_W - 16, 48, 32);
    a = dlg_get(dlg_i);
    b = dlg_get(dlg_i + 1);
    if (a != 0) {
        ag_gfx_text(ox + 16, oy + VIEW_H - 48, a, 16777215, 0);
    }
    if (b != 0) {
        ag_gfx_text(ox + 16, oy + VIEW_H - 32, b, 16777215, 0);
    }
    ag_gfx_text(ox + VIEW_W - 96, oy + VIEW_H - 16, "Z next", 12632256, 0);
}

void battle_set_msg(char *a, char *b)
{
    bat_a = a;
    bat_b = b;
}

void battle_start_wild(void)
{
    int roll;
    bat_boss = 0;
    bat_sel = 0;
    bat_turn = 0;
    roll = (steps + tick) & 3;
    if (map_id == MAP_CAVE) {
        bat_n = 1;
        if (roll < 2) {
            bat_sp[0] = SP_BANDIT;
            bat_hp[0] = 18;
            bat_mhp[0] = 18;
            bat_atk[0] = 7;
            bat_def[0] = 2;
            bat_exp[0] = 12;
            bat_gold[0] = 8;
            battle_set_msg("Bandit draws a knife!", 0);
        } else {
            bat_sp[0] = SP_BAT;
            bat_hp[0] = 10;
            bat_mhp[0] = 10;
            bat_atk[0] = 5;
            bat_def[0] = 1;
            bat_exp[0] = 8;
            bat_gold[0] = 4;
            battle_set_msg("A cave bat swoops!", 0);
        }
    } else {
        bat_n = 1;
        bat_sp[0] = SP_SLIME;
        bat_hp[0] = 8 + roll;
        bat_mhp[0] = bat_hp[0];
        bat_atk[0] = 4;
        bat_def[0] = 1;
        bat_exp[0] = 6;
        bat_gold[0] = 3;
        battle_set_msg("A slime wobbles forth!", 0);
    }
    if (roll == 0 && map_id == MAP_FIELD) {
        bat_n = 2;
        bat_sp[1] = SP_SLIME;
        bat_hp[1] = 6;
        bat_mhp[1] = 6;
        bat_atk[1] = 3;
        bat_def[1] = 0;
        bat_exp[1] = 4;
        bat_gold[1] = 2;
        battle_set_msg("Two slimes block the road!", 0);
    }
    state = ST_BATTLE;
}

void battle_start_boss(void)
{
    bat_boss = 1;
    bat_n = 1;
    bat_sel = 0;
    bat_turn = 0;
    bat_sp[0] = SP_BOSS;
    bat_hp[0] = 40;
    bat_mhp[0] = 40;
    bat_atk[0] = 10;
    bat_def[0] = 3;
    bat_exp[0] = 40;
    bat_gold[0] = 50;
    battle_set_msg("Cave Chief bars the way!", 0);
    state = ST_BATTLE;
}

int dmg_roll(int atk, int def)
{
    int d;
    d = atk - def;
    if (d < 1) {
        d = 1;
    }
    d = d + ((tick + steps) & 2);
    return d;
}

void battle_victory(void)
{
    int i;
    int eg;
    int gg;
    eg = 0;
    gg = 0;
    i = 0;
    while (i < bat_n) {
        eg = eg + bat_exp[i];
        gg = gg + bat_gold[i];
        i = i + 1;
    }
    hero_exp = hero_exp + eg;
    gold = gold + gg;
    if (hero_exp >= hero_lvl * 20) {
        hero_exp = hero_exp - hero_lvl * 20;
        hero_lvl = hero_lvl + 1;
        hero_mhp = hero_mhp + 4;
        hero_hp = hero_mhp;
        hero_atk = hero_atk + 1;
        hero_def = hero_def + 1;
        battle_set_msg("Victory! Level up!", 0);
    } else {
        battle_set_msg("Victory!", 0);
    }
    if (bat_boss) {
        qflags = qflags | QF_CAVE;
        npc_clear();
        battle_set_msg("Cave Chief falls!", "The stolen goods are free.");
    }
    enc_cool = 40;
    save_game();
}

void battle_enemy_turn(void)
{
    int i;
    int d;
    int tgt;
    i = 0;
    while (i < bat_n) {
        if (bat_hp[i] > 0) {
            tgt = 0;
            if (ban_in && ban_hp > 0 && ((tick + i) & 1) == 0) {
                tgt = 1;
            }
            if (tgt) {
                d = dmg_roll(bat_atk[i], ban_def);
            } else {
                d = dmg_roll(bat_atk[i], hero_def);
            }
            if (tgt) {
                ban_hp = ban_hp - d;
                if (ban_hp < 0) {
                    ban_hp = 0;
                }
                battle_set_msg("Enemy hits Ban!", 0);
            } else {
                hero_hp = hero_hp - d;
                if (hero_hp < 0) {
                    hero_hp = 0;
                }
                battle_set_msg("Enemy hits Roy!", 0);
            }
        }
        i = i + 1;
    }
    if (hero_hp <= 0) {
        state = ST_DEAD;
    }
}

void battle_fight(void)
{
    int d;
    int i;
    int left;
    if (bat_sel < 0 || bat_sel >= bat_n || bat_hp[bat_sel] <= 0) {
        bat_sel = 0;
        while (bat_sel < bat_n && bat_hp[bat_sel] <= 0) {
            bat_sel = bat_sel + 1;
        }
    }
    if (bat_sel >= bat_n) {
        return;
    }
    d = hero_atk;
    if (has_sword) {
        d = d + 3;
    }
    d = dmg_roll(d, bat_def[bat_sel]);
    bat_hp[bat_sel] = bat_hp[bat_sel] - d;
    battle_set_msg("Roy attacks!", 0);
    if (bat_hp[bat_sel] <= 0) {
        bat_hp[bat_sel] = 0;
    }
    if (ban_in && ban_hp > 0) {
        i = 0;
        while (i < bat_n && bat_hp[i] <= 0) {
            i = i + 1;
        }
        if (i < bat_n) {
            d = dmg_roll(ban_atk, bat_def[i]);
            bat_hp[i] = bat_hp[i] - d;
            if (bat_hp[i] < 0) {
                bat_hp[i] = 0;
            }
        }
    }
    left = 0;
    i = 0;
    while (i < bat_n) {
        if (bat_hp[i] > 0) {
            left = left + 1;
        }
        i = i + 1;
    }
    if (left == 0) {
        battle_victory();
        bat_turn = 2;
        return;
    }
    battle_enemy_turn();
}

void draw_battle(void)
{
    int i;
    int x;
    ag_gfx_fill_rect(ox, oy, VIEW_W, VIEW_H, 32);
    ag_gfx_text(ox + 8, oy + 8, "BATTLE", 16776960, 0);
    i = 0;
    while (i < bat_n) {
        x = ox + 200;
        if (bat_hp[i] > 0) {
            g2d_draw_sprite(bat_sp[i] + ((tick >> 3) & 1), x, oy + 40 + i * 40);
            ag_gfx_text(x - 70, oy + 48 + i * 40, "HP", 65280, 0);
            draw_num(x - 40, oy + 48 + i * 40, bat_hp[i], 65280);
            if (i == bat_sel) {
                ag_gfx_text(x - 90, oy + 48 + i * 40, ">", 16776960, 0);
            }
        } else {
            ag_gfx_text(x - 40, oy + 48 + i * 40, "Down", 12632256, 0);
        }
        i = i + 1;
    }
    g2d_draw_sprite(SP_HERO0, ox + 40, oy + 80);
    ag_gfx_text(ox + 40, oy + 100, "Roy", 16777215, 0);
    draw_num(ox + 40, oy + 112, hero_hp, 65280);
    if (ban_in) {
        g2d_draw_sprite(SP_BAN, ox + 80, oy + 80);
        ag_gfx_text(ox + 80, oy + 100, "Ban", 16777215, 0);
        draw_num(ox + 80, oy + 112, ban_hp, 65280);
    }
    ag_gfx_fill_rect(ox + 8, oy + VIEW_H - 64, VIEW_W - 16, 56, 16);
    if (bat_turn == 2) {
        if (bat_a != 0) {
            ag_gfx_text(ox + 16, oy + VIEW_H - 56, bat_a, 16777215, 0);
        }
        if (bat_b != 0) {
            ag_gfx_text(ox + 16, oy + VIEW_H - 40, bat_b, 16777215, 0);
        }
        ag_gfx_text(ox + 16, oy + VIEW_H - 20, "Z continue", 12632256, 0);
    } else {
        ag_gfx_text(ox + 16, oy + VIEW_H - 56, "Z Fight   X Run", 16777215, 0);
        ag_gfx_text(ox + 16, oy + VIEW_H - 40, "Arrows: target", 12632256, 0);
        if (bat_a != 0) {
            ag_gfx_text(ox + 16, oy + VIEW_H - 24, bat_a, 16776960, 0);
        }
    }
}

void try_warp(void)
{
    int t;
    int from;
    from = map_id;
    t = tile_at(px, py);
    if (map_id == MAP_TOWN && py >= TOWN_H - 1) {
        use_map(MAP_FIELD, 24, FIELD_H - 2);
    } else if (map_id == MAP_FIELD && py >= FIELD_H - 1) {
        use_map(MAP_TOWN, 18, TOWN_H - 2);
    } else if (map_id == MAP_FIELD && py <= 0) {
        use_map(MAP_GRAY, 14, GRAY_H - 2);
    } else if (map_id == MAP_GRAY && py >= GRAY_H - 1) {
        use_map(MAP_FIELD, 24, 1);
    } else if (map_id == MAP_FIELD && t == T_STAIRS) {
        use_map(MAP_CAVE, 4, CAVE_H - 4);
    } else if (map_id == MAP_CAVE && t == T_STAIRS) {
        use_map(MAP_FIELD, 33, 9);
    }
    if (map_id != from) {
        save_game();
    }
}

int npc_near(int i)
{
    int dx;
    int dy;
    dx = npc_x[i] - px;
    dy = npc_y[i] - py;
    if (dx < 0) {
        dx = 0 - dx;
    }
    if (dy < 0) {
        dy = 0 - dy;
    }
    /* Same tile or orthogonally adjacent. */
    if (dx + dy <= 1) {
        return 1;
    }
    return 0;
}

void try_talk(void)
{
    int i;
    int tx;
    int ty;
    i = 0;
    while (i < npc_n) {
        if (npc_near(i)) {
            if (npc_dlg[i] == 9) {
                dlg_start_id(9);
                if (state == ST_BATTLE) {
                    battle_start_boss();
                }
                return;
            }
            dlg_start_id(npc_dlg[i]);
            return;
        }
        i = i + 1;
    }
    /* Sign in facing direction or underfoot. */
    tx = px;
    ty = py;
    if (tile_at(tx, ty) != T_SIGN) {
        if (pdir == 0) {
            ty = ty + 1;
        } else if (pdir == 1) {
            ty = ty - 1;
        } else if (pdir == 2) {
            tx = tx - 1;
        } else {
            tx = tx + 1;
        }
    }
    if (tile_at(tx, ty) == T_SIGN) {
        dlg_begin();
        if (map_id == MAP_TOWN) {
            dlg_add("Sign: North Road -> Grayfen");
        } else if (map_id == MAP_FIELD) {
            dlg_add("Sign: Bandit Cave in the rocks");
        } else {
            dlg_add("Sign: Grayfen Village");
        }
        state = ST_DIALOG;
        dlg_i = 0;
        return;
    }
    dlg_begin();
    dlg_add("Nothing to talk to.");
    dlg_add("(Stand next to someone, Z)");
    state = ST_DIALOG;
    dlg_i = 0;
}

void try_move(int dx, int dy)
{
    int nx;
    int ny;
    int t;
    int i;
    if (move_cool > 0) {
        return;
    }
    if (dy > 0) {
        pdir = 0;
    } else if (dy < 0) {
        pdir = 1;
    } else if (dx < 0) {
        pdir = 2;
    } else if (dx > 0) {
        pdir = 3;
    }
    nx = px + dx;
    ny = py + dy;
    t = tile_at(nx, ny);
    if (tile_solid(t)) {
        return;
    }
    i = 0;
    while (i < npc_n) {
        if (npc_x[i] == nx && npc_y[i] == ny) {
            return;
        }
        i = i + 1;
    }
    px = nx;
    py = ny;
    move_cool = 8;
    panim = 10;
    pframe = 1 - pframe;
    steps = steps + 1;
    try_warp();
    if (state != ST_FIELD) {
        return;
    }
    if (enc_cool > 0) {
        enc_cool = enc_cool - 1;
        return;
    }
    if (map_id == MAP_FIELD || map_id == MAP_CAVE) {
        if ((steps % 12) == 0 && ((px * 7 + py * 3 + tick) & 3) == 0) {
            battle_start_wild();
        }
    }
}

void update_field(void)
{
    if (ag_btn(7)) {
        running = 0;
        return;
    }
    if (ag_btn(0)) {
        try_move(0, -1);
    } else if (ag_btn(1)) {
        try_move(0, 1);
    } else if (ag_btn(2)) {
        try_move(-1, 0);
    } else if (ag_btn(3)) {
        try_move(1, 0);
    }
    if (btn_ok()) {
        if (move_cool == 0) {
            try_talk();
            move_cool = 12;
        }
    }
    if (move_cool > 0) {
        move_cool = move_cool - 1;
    }
    if (panim > 0) {
        panim = panim - 1;
    }
    npc_wander();
}

void update_dialog(void)
{
    if (btn_ok() && move_cool == 0) {
        dlg_i = dlg_i + 2;
        move_cool = 12;
        if (dlg_i >= dlg_n) {
            save_game();
            if ((qflags & QF_DONE) != 0) {
                state = ST_WIN;
            } else {
                state = ST_FIELD;
            }
        }
    }
    if (btn_back()) {
        state = ST_FIELD;
    }
    if (move_cool > 0) {
        move_cool = move_cool - 1;
    }
}

void update_shop(void)
{
    if (btn_ok() && move_cool == 0) {
        move_cool = 12;
        if (gold >= 20 && (qflags & QF_SWORD) == 0) {
            gold = gold - 20;
            has_sword = 1;
            qflags = qflags | QF_SWORD;
            hero_atk = hero_atk + 2;
            dlg_begin();
            dlg_add("Bought a short sword!");
            dlg_add("Attack rises.");
            save_game();
            state = ST_DIALOG;
            dlg_i = 0;
            return;
        }
        dlg_begin();
        if ((qflags & QF_SWORD) != 0) {
            dlg_add("You already have a sword.");
        } else {
            dlg_add("Need 20 gold.");
        }
        state = ST_DIALOG;
        dlg_i = 0;
    }
    if (btn_back()) {
        state = ST_FIELD;
    }
    if (move_cool > 0) {
        move_cool = move_cool - 1;
    }
}

void update_inn(void)
{
    if (btn_ok() && move_cool == 0) {
        move_cool = 12;
        if (gold >= 10) {
            gold = gold - 10;
            hero_hp = hero_mhp;
            if (ban_in) {
                ban_hp = ban_mhp;
            }
            dlg_begin();
            dlg_add("You sleep until dawn.");
            dlg_add("HP restored.");
            save_game();
            state = ST_DIALOG;
            dlg_i = 0;
            return;
        }
        dlg_begin();
        dlg_add("Need 10 gold.");
        state = ST_DIALOG;
        dlg_i = 0;
    }
    if (btn_back()) {
        state = ST_FIELD;
    }
    if (move_cool > 0) {
        move_cool = move_cool - 1;
    }
}

void update_battle(void)
{
    if (bat_turn == 2) {
        if (btn_ok() && move_cool == 0) {
            move_cool = 12;
            state = ST_FIELD;
        }
        if (move_cool > 0) {
            move_cool = move_cool - 1;
        }
        return;
    }
    if (ag_btn(0) && move_cool == 0) {
        bat_sel = bat_sel - 1;
        if (bat_sel < 0) {
            bat_sel = bat_n - 1;
        }
        move_cool = 8;
    }
    if (ag_btn(1) && move_cool == 0) {
        bat_sel = bat_sel + 1;
        if (bat_sel >= bat_n) {
            bat_sel = 0;
        }
        move_cool = 8;
    }
    if (btn_ok() && move_cool == 0) {
        move_cool = 14;
        battle_fight();
    }
    if (btn_back() && move_cool == 0) {
        move_cool = 14;
        if (bat_boss) {
            battle_set_msg("Cannot run!", 0);
        } else if (((tick) & 1) == 0) {
            battle_set_msg("Got away!", 0);
            bat_turn = 2;
            enc_cool = 20;
        } else {
            battle_set_msg("Could not escape!", 0);
            battle_enemy_turn();
        }
    }
    if (move_cool > 0) {
        move_cool = move_cool - 1;
    }
}

void draw_title(void)
{
    ag_gfx_fill_rect(ox, oy, VIEW_W, VIEW_H, 16);
    ag_gfx_text(ox + 248, oy + 120, "HARBOR QUEST", 16776960, 0);
    ag_gfx_text(ox + 220, oy + 150, "A short road from Minato", 16777215, 0);
    ag_gfx_text(ox + 180, oy + 200, "Arrows move   Z/Enter talk", 65280, 0);
    ag_gfx_text(ox + 210, oy + 220, "X cancel   Esc quit", 65280, 0);
    if (save_ok) {
        ag_gfx_text(ox + 160, oy + 280, "Z/Enter continue save", 16777215, 0);
        ag_gfx_text(ox + 200, oy + 300, "X new game", 16777215, 0);
    } else {
        ag_gfx_text(ox + 200, oy + 280, "Z/Enter start", 16777215, 0);
    }
    ag_gfx_text(ox + 180, oy + 340, "Focus the SDL window", 12632256, 0);
}

void draw_win(void)
{
    ag_gfx_fill_rect(ox, oy, VIEW_W, VIEW_H, 32);
    ag_gfx_text(ox + 80, oy + 70, "CHAPTER 1 CLEAR", 16776960, 0);
    ag_gfx_text(ox + 40, oy + 100, "Roy returns to Lina's smile.", 16777215, 0);
    ag_gfx_text(ox + 60, oy + 130, "Thanks for playing.", 12632256, 0);
    ag_gfx_text(ox + 70, oy + 160, "Z or Esc to quit", 65280, 0);
}

void draw_dead(void)
{
    ag_gfx_fill_rect(ox, oy, VIEW_W, VIEW_H, 4194304);
    ag_gfx_text(ox + 110, oy + 80, "DEFEATED", 16777215, 0);
    ag_gfx_text(ox + 50, oy + 110, "Z revive in Minato", 16777215, 0);
}

void party_init(void)
{
    hero_lvl = 1;
    hero_mhp = 30;
    hero_hp = 30;
    hero_atk = 5;
    hero_def = 2;
    hero_exp = 0;
    ban_mhp = 28;
    ban_hp = 28;
    ban_atk = 6;
    ban_def = 3;
    ban_in = 0;
    gold = 35;
    potions = 1;
    has_sword = 0;
    qflags = 0;
    pdir = 0;
    pframe = 0;
}

int load_game(void)
{
    int h;
    int n;
    int magic;
    int ver;
    int mid;
    int x;
    int y;
    h = ag_open(SAVE_PATH, AG_O_RDONLY);
    if (h < 0) {
        return 0;
    }
    n = ag_read(h, savbuf, 128);
    ag_close(h);
    if (n < 84) {
        return 0;
    }
    sav_off = 0;
    magic = sav_get();
    ver = sav_get();
    if (magic != SAVE_MAGIC || ver != SAVE_VER) {
        return 0;
    }
    qflags = sav_get();
    gold = sav_get();
    potions = sav_get();
    has_sword = sav_get();
    mid = sav_get();
    x = sav_get();
    y = sav_get();
    pdir = sav_get();
    hero_hp = sav_get();
    hero_mhp = sav_get();
    hero_atk = sav_get();
    hero_def = sav_get();
    hero_lvl = sav_get();
    hero_exp = sav_get();
    ban_in = sav_get();
    ban_hp = sav_get();
    ban_mhp = sav_get();
    ban_atk = sav_get();
    ban_def = sav_get();
    if (mid < 0 || mid > MAP_CAVE) {
        mid = MAP_TOWN;
        x = 8;
        y = 19;
    }
    use_map(mid, x, y);
    save_ok = 1;
    return 1;
}

int probe_save(void)
{
    int h;
    h = ag_open(SAVE_PATH, AG_O_RDONLY);
    if (h < 0) {
        return 0;
    }
    ag_close(h);
    return 1;
}

int ag_main(void)
{
    name_roy = "Roy";
    name_lina = "Lina";
    name_ban = "Ban";

    build_town();
    build_field();
    build_gray();
    build_cave();
    party_init();
    save_ok = probe_save();

    ag_gfx_acquire();
    /* Full soft framebuffer — no letterboxed console “hole”. */
    ox = 0;
    oy = 0;
    g2d_init(ox, oy, VIEW_W, VIEW_H);
    g2d_set_key(KEY_RGB);

    atlas_ok = 0;
    if (load_atlas_path("h:\\atlas.bin")) {
        atlas_ok = 1;
    } else if (load_atlas_path("atlas.bin")) {
        atlas_ok = 1;
    }
    g2d_tileset(atlas, ATLAS_W, TW, TH);
    if (!atlas_ok) {
        make_atlas_fallback();
    }

    use_map(MAP_TOWN, 8, 19);
    state = ST_TITLE;
    running = 1;
    tick = 0;
    move_cool = 0;
    enc_cool = 0;

    while (running) {
        struct ag_ev ev;

        while (ag_poll_event(&ev, 0)) {
            if (ev.type == AG_EV_FOCUS_GAINED) {
                /* Kernel force-releases gfx on focus loss; reclaim it. */
                ag_gfx_acquire();
            } else if (ev.type == AG_EV_QUIT) {
                /* Shell Ctrl+C while unfocused is for the slot, not us. */
                if (ag_focused()) {
                    running = 0;
                }
            }
        }
        if (!running) {
            break;
        }
        if (!ag_focused()) {
            ag_heartbeat();
            ag_delay(50);
            continue;
        }

        tick = tick + 1;
        if (state == ST_TITLE) {
            draw_title();
            if (btn_ok() && move_cool == 0) {
                move_cool = 16;
                if (save_ok && load_game()) {
                    state = ST_FIELD;
                } else {
                    party_init();
                    use_map(MAP_TOWN, 8, 19);
                    state = ST_FIELD;
                }
            }
            if (btn_back() && move_cool == 0) {
                move_cool = 16;
                party_init();
                use_map(MAP_TOWN, 8, 19);
                state = ST_FIELD;
            }
            if (ag_btn(7)) {
                running = 0;
            }
        } else if (state == ST_FIELD) {
            update_field();
            draw_field();
        } else if (state == ST_DIALOG) {
            update_dialog();
            draw_dialog();
        } else if (state == ST_SHOP) {
            update_shop();
            draw_dialog();
        } else if (state == ST_INN) {
            update_inn();
            draw_dialog();
        } else if (state == ST_BATTLE) {
            update_battle();
            draw_battle();
        } else if (state == ST_WIN) {
            draw_win();
            if (btn_ok() || ag_btn(7)) {
                save_game();
                running = 0;
            }
        } else if (state == ST_DEAD) {
            draw_dead();
            if (btn_ok()) {
                hero_hp = hero_mhp;
                if (ban_in) {
                    ban_hp = ban_mhp;
                }
                use_map(MAP_TOWN, 8, 19);
                save_game();
                state = ST_FIELD;
                move_cool = 16;
            }
        }
        if (move_cool > 0 && state == ST_TITLE) {
            move_cool = move_cool - 1;
        }
        g2d_present();
        ag_delay(16);
    }

    if (state != ST_TITLE) {
        save_game();
    }
    ag_gfx_release();
    return 0;
}
