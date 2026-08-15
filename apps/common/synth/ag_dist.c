/*
 * FET / tube waveshaper: polynomial LUT, no tanhf.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_dist.h"

#include "ag_dsp.h"

#define LUT_N 256

static int16_t s_jfet[LUT_N];
static int16_t s_tube[LUT_N];
static int     s_ready;

/* Rational tanh-ish: x*(27+x^2)/(27+9x^2), x in Q15, |x|<3. */
static int32_t tanh_q15(int32_t x)
{
    int32_t x2, num, den;
    int     neg = 0;
    if (x < 0) {
        x = -x;
        neg = 1;
    }
    if (x > 3 * 32767) {
        x = 3 * 32767;
    }
    x2 = (int32_t)(((int64_t)x * x) >> 15);
    num = (27 * 32767 + x2);
    den = (27 * 32767 + 9 * x2);
    if (den < 1) {
        den = 1;
    }
    x = (int32_t)(((int64_t)x * num) / den);
    if (x > 32767) {
        x = 32767;
    }
    return neg ? -x : x;
}

static void ensure_lut(void)
{
    int i;
    if (s_ready) {
        return;
    }
    for (i = 0; i < LUT_N; i++) {
        int32_t x = ((i - 128) * 32767) / 128;
        int32_t ax = x < 0 ? -x : x;
        int32_t x2 = (int32_t)(((int64_t)x * ax) >> 15); /* x*|x| */
        int32_t tu;
        /* JFET: odd, symmetric soft clip. */
        s_jfet[i] = (int16_t)ag_clampi((int)tanh_q15((x * 3) / 2), -32767, 32767);
        /* Tube: even harmonics via quadratic term, no DC pedestal. */
        tu = x + ((x2 * 22) >> 7);
        s_tube[i] = (int16_t)ag_clampi((int)tanh_q15((tu * 5) / 4), -32767, 32767);
    }
    s_ready = 1;
}

static int16_t lut_lookup(const int16_t *tab, int32_t x)
{
    int32_t u, i, f, a, b;
    if (x > 32767) {
        x = 32767;
    }
    if (x < -32767) {
        x = -32767;
    }
    u = x + 32767;
    i = u >> 8;
    f = u & 255;
    if (i > 254) {
        i = 254;
        f = 255;
    }
    a = tab[i];
    b = tab[i + 1];
    return (int16_t)(a + (((b - a) * f) >> 8));
}

void ag_dist_reset(ag_dist_t *d)
{
    if (d == 0) {
        return;
    }
    d->hp_z = 0;
    d->lp_z = 0;
    d->env = 0;
}

int32_t ag_dist_tick(ag_dist_t *d, int32_t in)
{
    int32_t x, y, gain, bias, hp, env_t;
    ensure_lut();
    if (d == 0) {
        return in;
    }

    /* Input coupling HP (~80–150 Hz @ 22 kHz). */
    hp = in - d->hp_z;
    d->hp_z += (hp * 6) >> 7;

    /* drive 0 ≈ unity into the soft knee; 64 ≈ 3×; 127 ≈ 5×. */
    gain = 128 + ((int32_t)d->drive * 4);
    x = (hp * gain) >> 7;

    bias = ((int32_t)d->bias - 64) * 80;
    env_t = hp < 0 ? -hp : hp;
    d->env += ((env_t - d->env) * 2) >> 7;
    bias -= (d->env * (int32_t)d->sag) >> 10;
    x += bias;
    if (x > 32767) {
        x = 32767;
    }
    if (x < -32767) {
        x = -32767;
    }

    switch (d->model) {
    case AG_DIST_TUBE:
        y = lut_lookup(s_tube, x);
        break;
    case AG_DIST_CLIP:
        y = ag_sat16(x * 2);
        break;
    case AG_DIST_FOLD:
        while (x > 32767 || x < -32767) {
            if (x > 32767) {
                x = 65534 - x;
            }
            if (x < -32767) {
                x = -65534 - x;
            }
        }
        y = x;
        break;
    case AG_DIST_CRUSH:
        y = (x >> 4) << 4;
        break;
    default:
        y = lut_lookup(s_jfet, x);
        break;
    }

    /* Mild post LP — keeps pick attack, knocks the worst alias. */
    d->lp_z += ((y - d->lp_z) * 108) >> 7;
    return d->lp_z;
}

void ag_dist_process(ag_dist_t *d, int16_t *stereo, int32_t frames)
{
    int32_t i;
    if (d == 0 || stereo == 0 || frames <= 0) {
        return;
    }
    for (i = 0; i < frames; i++) {
        int32_t m = ((int32_t)stereo[i * 2] + (int32_t)stereo[i * 2 + 1]) >> 1;
        int32_t y = ag_dist_tick(d, m);
        stereo[i * 2] = ag_sat16(y);
        stereo[i * 2 + 1] = ag_sat16(y);
    }
}
