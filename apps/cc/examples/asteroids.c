/*
 * Asteroids — Argon CC demo (integer math, gfx draw builtins).
 *
 * Controls via HostFS PADPUSH (same as SMS — level state, chords work):
 *   Left/Right rotate, Up thrust, Z (b1) fire, Esc quit.
 *   Needs: argon run -Gfx -HostFs …  (with sms.cfg; wait for PADPUSH).
 * Sticky ag_key fallback only if pad is missing.
 *
 * Build on the guest:
 *   run t:\cc.axe t:\asteroids.c t:\asteroids.axe
 *   run t:\asteroids.axe
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */

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

int W;
int H;
int score;
int alive;
int sx;
int sy;
int sa;
int svx;
int svy;
int cool;
int sintab[32];
int costab[32];
int ax[8];
int ay[8];
int avx[8];
int avy[8];
int asz[8];
int bx[4];
int by[4];
int bvx[4];
int bvy[4];
int blife[4];

int wrap(int v, int lim)
{
    while (v < 0) {
        v = v + lim;
    }
    while (v >= lim) {
        v = v - lim;
    }
    return v;
}

int isin(int a)
{
    return sintab[wrap(a, 32)];
}

int icos(int a)
{
    return costab[wrap(a, 32)];
}

void init_tables(void)
{
    /* sin/cos for 32 steps, scale 256 (precomputed). */
    sintab[0] = 0;
    sintab[1] = 50;
    sintab[2] = 98;
    sintab[3] = 142;
    sintab[4] = 181;
    sintab[5] = 213;
    sintab[6] = 237;
    sintab[7] = 251;
    sintab[8] = 256;
    sintab[9] = 251;
    sintab[10] = 237;
    sintab[11] = 213;
    sintab[12] = 181;
    sintab[13] = 142;
    sintab[14] = 98;
    sintab[15] = 50;
    sintab[16] = 0;
    sintab[17] = -50;
    sintab[18] = -98;
    sintab[19] = -142;
    sintab[20] = -181;
    sintab[21] = -213;
    sintab[22] = -237;
    sintab[23] = -251;
    sintab[24] = -256;
    sintab[25] = -251;
    sintab[26] = -237;
    sintab[27] = -213;
    sintab[28] = -181;
    sintab[29] = -142;
    sintab[30] = -98;
    sintab[31] = -50;
    costab[0] = 256;
    costab[1] = 251;
    costab[2] = 237;
    costab[3] = 213;
    costab[4] = 181;
    costab[5] = 142;
    costab[6] = 98;
    costab[7] = 50;
    costab[8] = 0;
    costab[9] = -50;
    costab[10] = -98;
    costab[11] = -142;
    costab[12] = -181;
    costab[13] = -213;
    costab[14] = -237;
    costab[15] = -251;
    costab[16] = -256;
    costab[17] = -251;
    costab[18] = -237;
    costab[19] = -213;
    costab[20] = -181;
    costab[21] = -142;
    costab[22] = -98;
    costab[23] = -50;
    costab[24] = 0;
    costab[25] = 50;
    costab[26] = 98;
    costab[27] = 142;
    costab[28] = 181;
    costab[29] = 213;
    costab[30] = 237;
    costab[31] = 251;
}

int aradius(int sz)
{
    if (sz == 3) {
        return 28;
    }
    if (sz == 2) {
        return 18;
    }
    if (sz == 1) {
        return 10;
    }
    return 0;
}

void spawn_rock(int i, int x, int y, int sz)
{
    asz[i] = sz;
    ax[i] = x;
    ay[i] = y;
    avx[i] = (i * 3) % 5 - 2;
    avy[i] = (i * 5) % 5 - 2;
    if (avx[i] == 0) {
        avx[i] = 1;
    }
    if (avy[i] == 0) {
        avy[i] = -1;
    }
}

void reset_game(void)
{
    int i;
    score = 0;
    alive = 1;
    sx = W / 2;
    sy = H / 2;
    sa = 8;
    svx = 0;
    svy = 0;
    cool = 0;
    for (i = 0; i < 4; i = i + 1) {
        blife[i] = 0;
    }
    for (i = 0; i < 8; i = i + 1) {
        asz[i] = 0;
    }
    spawn_rock(0, 80, 60, 3);
    spawn_rock(1, W - 80, 80, 3);
    spawn_rock(2, 100, H - 70, 2);
    spawn_rock(3, W - 120, H - 90, 2);
}

void fire(void)
{
    int i;
    int spd;
    if (cool > 0) {
        return;
    }
    for (i = 0; i < 4; i = i + 1) {
        if (blife[i] == 0) {
            spd = 6;
            bx[i] = sx;
            by[i] = sy;
            bvx[i] = icos(sa) * spd / 256;
            bvy[i] = isin(sa) * spd / 256;
            blife[i] = 45;
            cool = 8;
            return;
        }
    }
}

void split_rock(int i)
{
    int sz;
    int x;
    int y;
    int j;
    sz = asz[i];
    x = ax[i];
    y = ay[i];
    asz[i] = 0;
    score = score + (4 - sz) * 10;
    if (sz <= 1) {
        return;
    }
    sz = sz - 1;
    spawn_rock(i, x, y, sz);
    for (j = 0; j < 8; j = j + 1) {
        if (asz[j] == 0) {
            spawn_rock(j, x + 6, y - 6, sz);
            return;
        }
    }
}

void update(void)
{
    int i;
    int j;
    int dx;
    int dy;
    int rr;
    int nose;

    /* ag_btn: 0=up 1=down 2=left 3=right 4=b1 5=b2 6=pause 7=quit */
    if (ag_btn(2)) {
        sa = wrap(sa - 1, 32);
    }
    if (ag_btn(3)) {
        sa = wrap(sa + 1, 32);
    }
    if (ag_btn(0)) {
        svx = svx + icos(sa) / 32;
        svy = svy + isin(sa) / 32;
    }
    if (ag_btn(4)) {
        fire();
    }
    if (cool > 0) {
        cool = cool - 1;
    }

    sx = wrap(sx + svx / 16, W);
    sy = wrap(sy + svy / 16, H);
    svx = svx * 15 / 16;
    svy = svy * 15 / 16;

    for (i = 0; i < 4; i = i + 1) {
        if (blife[i] > 0) {
            bx[i] = wrap(bx[i] + bvx[i], W);
            by[i] = wrap(by[i] + bvy[i], H);
            blife[i] = blife[i] - 1;
        }
    }

    for (i = 0; i < 8; i = i + 1) {
        if (asz[i] > 0) {
            ax[i] = wrap(ax[i] + avx[i], W);
            ay[i] = wrap(ay[i] + avy[i], H);
        }
    }

    for (i = 0; i < 4; i = i + 1) {
        if (blife[i] > 0) {
            for (j = 0; j < 8; j = j + 1) {
                if (asz[j] > 0) {
                    dx = bx[i] - ax[j];
                    dy = by[i] - ay[j];
                    rr = aradius(asz[j]);
                    if (dx * dx + dy * dy < rr * rr) {
                        blife[i] = 0;
                        split_rock(j);
                    }
                }
            }
        }
    }

    for (i = 0; i < 8; i = i + 1) {
        if (asz[i] > 0) {
            dx = sx - ax[i];
            dy = sy - ay[i];
            nose = aradius(asz[i]) + 8;
            if (dx * dx + dy * dy < nose * nose) {
                alive = 0;
            }
        }
    }
}

void draw_ship(void)
{
    int x0;
    int y0;
    int x1;
    int y1;
    int x2;
    int y2;
    x0 = sx + icos(sa) * 14 / 256;
    y0 = sy + isin(sa) * 14 / 256;
    x1 = sx + icos(sa + 10) * 10 / 256;
    y1 = sy + isin(sa + 10) * 10 / 256;
    x2 = sx + icos(sa + 22) * 10 / 256;
    y2 = sy + isin(sa + 22) * 10 / 256;
    ag_gfx_poly_begin();
    ag_gfx_poly_vertex(x0, y0);
    ag_gfx_poly_vertex(x1, y1);
    ag_gfx_poly_vertex(x2, y2);
    ag_gfx_poly_stroke(16777215);
}

void draw_world(void)
{
    int i;
    ag_gfx_clear(1052688);
    for (i = 0; i < 8; i = i + 1) {
        if (asz[i] > 0) {
            ag_gfx_circle(ax[i], ay[i], aradius(asz[i]), 13421772);
        }
    }
    for (i = 0; i < 4; i = i + 1) {
        if (blife[i] > 0) {
            ag_gfx_pixel(bx[i], by[i], 16776960);
        }
    }
    if (alive) {
        draw_ship();
    }
    ag_gfx_text(8, 8, "ASTEROIDS", 16777215, 1052688);
    if (!alive) {
        ag_gfx_text(W / 2 - 40, H / 2 - 8, "GAME OVER", 16711680, 1052688);
    }
    ag_gfx_flush(0, 0, W, H);
}

int rocks_left(void)
{
    int i;
    int n;
    n = 0;
    for (i = 0; i < 8; i = i + 1) {
        if (asz[i] > 0) {
            n = n + 1;
        }
    }
    return n;
}

int ag_main(void)
{
    int frames;
    int running;
    W = 640;
    H = 400;
    init_tables();
    ag_gfx_acquire();
    reset_game();
    frames = 0;
    running = 1;
    while (running && frames < 3600) {
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
            break;
        }
        if (alive) {
            update();
        }
        if (rocks_left() == 0) {
            reset_game();
        }
        draw_world();
        ag_delay(33);
        frames = frames + 1;
        if (!alive) {
            if (ag_btn(4)) {
                reset_game();
            }
        }
    }
    ag_gfx_release();
    return score;
}
