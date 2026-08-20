/*
 * ag_biq - see ag_biq.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_biq.h"

#include "ag_mathf.h"

#define TWO_PI 6.28318531f
#define PI_    3.14159265f
#define HALFPI 1.57079633f

/*
 * Sine and cosine, which ag_mathf does not have because nothing in the circuit
 * solver needed them.  They live here rather than there because they are only
 * ever called when a coefficient changes - a knob turn, a preset load - so
 * their cost is irrelevant and their accuracy is not: a 150 Hz high pass at
 * 22 kHz needs 1 - cos(w0), which is 9e-4, and taking that difference from a
 * cosine good to only four digits leaves nothing.
 *
 * Range reduced to [-pi/2, pi/2] and then Taylor to the term that puts the
 * truncation error below 5e-7, which is under the resolution of the float the
 * coefficient is stored in.
 */
void ag_sincosf(float x, float *sn, float *cs)
{
    float k = x * (1.0f / TWO_PI);
    int   ki = (int)(k >= 0.0f ? k + 0.5f : k - 0.5f);
    float y = x - (float)ki * TWO_PI; /* now in [-pi, pi] */
    float sc = 1.0f;
    float y2;

    if (y > HALFPI) {
        y = PI_ - y;
        sc = -1.0f;
    } else if (y < -HALFPI) {
        y = -PI_ - y;
        sc = -1.0f;
    }
    y2 = y * y;
    *sn = y * (1.0f + y2 * (-1.66666672e-1f +
                            y2 * (8.33333333e-3f +
                                  y2 * (-1.98412698e-4f + y2 * 2.75573192e-6f))));
    *cs = sc * (1.0f + y2 * (-0.5f +
                             y2 * (4.16666667e-2f +
                                   y2 * (-1.38888889e-3f +
                                         y2 * (2.48015873e-5f -
                                               y2 * 2.75573192e-7f)))));
}

static float db_to_lin(float db) { return ag_expf(db * 0.115129255f); }

/* Whatever the caller asked for, brought inside the band this sample rate has.
 * Returned rather than silently applied: a 12 kHz cut at a 22.05 kHz rate is a
 * question about the sample rate, and it should be printed, not swallowed. */
static float clamp_f0(float fs, float f0)
{
    const float hi = fs * 0.45f;
    const float lo = 0.05f;
    if (f0 > hi) {
        return hi;
    }
    if (f0 < lo) {
        return lo;
    }
    return f0;
}

void ag_biq_bypass(ag_biq_t *s)
{
    if (s == 0) {
        return;
    }
    s->b0 = 1.0f;
    s->b1 = 0.0f;
    s->b2 = 0.0f;
    s->a1 = 0.0f;
    s->a2 = 0.0f;
    ag_biq_reset(s);
}

void ag_biq_reset(ag_biq_t *s)
{
    if (s == 0) {
        return;
    }
    s->x1 = 0.0f;
    s->x2 = 0.0f;
    s->y1 = 0.0f;
    s->y2 = 0.0f;
}

float ag_biq_hp1(ag_biq_t *s, float fs, float f0)
{
    float f, sn, cs, k, d;

    if (s == 0 || fs <= 0.0f) {
        return 0.0f;
    }
    f = clamp_f0(fs, f0);
    /* Prewarped, so the corner lands where it was asked for and not where the
     * bilinear transform would have put it. */
    ag_sincosf(PI_ * f / fs, &sn, &cs);
    k = sn / cs; /* tan(w0/2) */
    d = 1.0f / (1.0f + k);
    s->b0 = d;
    s->b1 = -d;
    s->b2 = 0.0f;
    s->a1 = (k - 1.0f) * d;
    s->a2 = 0.0f;
    ag_biq_reset(s);
    return f;
}

float ag_biq_shelf1(ag_biq_t *s, float fs, float f_zero, float db)
{
    float g, fz, fp, c, wz, wp, d;

    if (s == 0 || fs <= 0.0f) {
        return 0.0f;
    }
    if (f_zero <= 0.0f || db > -0.01f) {
        ag_biq_bypass(s);
        return 0.0f;
    }
    g = db_to_lin(db); /* < 1: how much less gain below the corner */
    fz = clamp_f0(fs, f_zero);
    fp = clamp_f0(fs, fz / g);

    /*
     * Plain bilinear, no prewarping, and that is deliberate: prewarping a
     * shelf moves its two corners by different amounts and tilts the shelf.
     * Both corners here are under a kilohertz against a 22 kHz rate, where the
     * bilinear frequency error is under a tenth of a percent.
     */
    c = 2.0f * fs;
    wz = TWO_PI * fz;
    wp = TWO_PI * fp;
    d = 1.0f / (c + wp);
    s->b0 = (c + wz) * d;
    s->b1 = (wz - c) * d;
    s->b2 = 0.0f;
    s->a1 = (wp - c) * d;
    s->a2 = 0.0f;
    ag_biq_reset(s);
    return fz;
}

float ag_biq_lp2(ag_biq_t *s, float fs, float f0, float q)
{
    float f, w0, sn, cs, alpha, a0i;

    if (s == 0 || fs <= 0.0f) {
        return 0.0f;
    }
    if (q < 0.05f) {
        q = 0.05f;
    }
    f = clamp_f0(fs, f0);
    w0 = TWO_PI * f / fs;
    ag_sincosf(w0, &sn, &cs);
    alpha = sn / (2.0f * q);
    a0i = 1.0f / (1.0f + alpha);
    s->b0 = (1.0f - cs) * 0.5f * a0i;
    s->b1 = (1.0f - cs) * a0i;
    s->b2 = s->b0;
    s->a1 = -2.0f * cs * a0i;
    s->a2 = (1.0f - alpha) * a0i;
    ag_biq_reset(s);
    return f;
}

float ag_biq_peak(ag_biq_t *s, float fs, float f0, float db, float q)
{
    float f, w0, sn, cs, alpha, a, a0i;

    if (s == 0 || fs <= 0.0f) {
        return 0.0f;
    }
    if (db > -0.01f && db < 0.01f) {
        ag_biq_bypass(s);
        return 0.0f;
    }
    if (q < 0.05f) {
        q = 0.05f;
    }
    f = clamp_f0(fs, f0);
    w0 = TWO_PI * f / fs;
    ag_sincosf(w0, &sn, &cs);
    alpha = sn / (2.0f * q);
    a = db_to_lin(db * 0.5f); /* 10^(db/40) */
    a0i = 1.0f / (1.0f + alpha / a);
    s->b0 = (1.0f + alpha * a) * a0i;
    s->b1 = -2.0f * cs * a0i;
    s->b2 = (1.0f - alpha * a) * a0i;
    s->a1 = s->b1;
    s->a2 = (1.0f - alpha / a) * a0i;
    ag_biq_reset(s);
    return f;
}

float ag_biq_bandpass(ag_biq_t *s, float fs, float f0, float bw)
{
    float f, w0, sn, cs, alpha, a0i, x;

    if (s == 0 || fs <= 0.0f) {
        return 0.0f;
    }
    if (bw < 0.05f) {
        bw = 0.05f;
    }
    f = clamp_f0(fs, f0);
    w0 = TWO_PI * f / fs;
    ag_sincosf(w0, &sn, &cs);
    /*
     * alpha = sin(w0) * sinh( (ln2 / 2) * bw * w0 / sin(w0) ), and sinh is
     * written out as (e^x - e^-x)/2 rather than added to ag_mathf for one caller.
     */
    x = 0.34657359f * bw * (sn > 1.0e-6f ? w0 / sn : 1.0f);
    alpha = sn * 0.5f * (ag_expf(x) - ag_expf(-x));
    a0i = 1.0f / (1.0f + alpha);
    s->b0 = alpha * a0i;
    s->b1 = 0.0f;
    s->b2 = -alpha * a0i;
    s->a1 = -2.0f * cs * a0i;
    s->a2 = (1.0f - alpha) * a0i;
    ag_biq_reset(s);
    return f;
}

float ag_biq_tick(ag_biq_t *s, float x)
{
    float y;
    if (s == 0) {
        return x;
    }
    y = s->b0 * x + s->b1 * s->x1 + s->b2 * s->x2 - s->a1 * s->y1 -
        s->a2 * s->y2;
    s->x2 = s->x1;
    s->x1 = x;
    s->y2 = s->y1;
    s->y1 = y;
    return y;
}

void ag_biq_chain_init(ag_biq_chain_t *c)
{
    int i;
    if (c == 0) {
        return;
    }
    c->n = 0;
    for (i = 0; i < AG_BIQ_MAX; i++) {
        ag_biq_bypass(&c->s[i]);
    }
}

void ag_biq_chain_reset(ag_biq_chain_t *c)
{
    int i;
    if (c == 0) {
        return;
    }
    for (i = 0; i < AG_BIQ_MAX; i++) {
        ag_biq_reset(&c->s[i]);
    }
}

ag_biq_t *ag_biq_chain_push(ag_biq_chain_t *c)
{
    if (c == 0 || c->n >= AG_BIQ_MAX) {
        return 0;
    }
    return &c->s[c->n++];
}

/*
 * The same arithmetic as ag_biq_tick, in the same order, written out here
 * instead of called.
 *
 * A biquad is five multiplies, four adds and four state moves; measured through
 * the call it cost forty instructions a section, so two thirds of it was the
 * call and the pointer arithmetic around it.  F2 runs three of these at four
 * times the sample rate and the two voicing banks another seven, so it is
 * twelve to nineteen sections a sample and the overhead was worth about three
 * percent of a core.  Bit for bit identical output - this is a call being
 * removed, not a formula being changed.
 */
float ag_biq_chain_tick(ag_biq_chain_t *c, float x)
{
    int i, n;
    if (c == 0) {
        return x;
    }
    n = c->n;
    for (i = 0; i < n; i++) {
        ag_biq_t   *s = &c->s[i];
        const float y = s->b0 * x + s->b1 * s->x1 + s->b2 * s->x2 -
                        s->a1 * s->y1 - s->a2 * s->y2;
        s->x2 = s->x1;
        s->x1 = x;
        s->y2 = s->y1;
        s->y1 = y;
        x = y;
    }
    return x;
}

float ag_biq_mag_db(const ag_biq_t *s, float f, float fs)
{
    float w, s1, c1, s2, c2, nre, nim, dre, dim, num, den;

    if (s == 0 || fs <= 0.0f) {
        return 0.0f;
    }
    w = TWO_PI * f / fs;
    ag_sincosf(w, &s1, &c1);
    ag_sincosf(2.0f * w, &s2, &c2);
    nre = s->b0 + s->b1 * c1 + s->b2 * c2;
    nim = -(s->b1 * s1 + s->b2 * s2);
    dre = 1.0f + s->a1 * c1 + s->a2 * c2;
    dim = -(s->a1 * s1 + s->a2 * s2);
    num = nre * nre + nim * nim;
    den = dre * dre + dim * dim;
    if (num < 1.0e-30f) {
        return -300.0f;
    }
    if (den < 1.0e-30f) {
        return 300.0f;
    }
    /* 10*log10(num/den), num and den being squared magnitudes already. */
    return 4.34294482f * ag_logf(num / den);
}

float ag_biq_chain_mag_db(const ag_biq_chain_t *c, float f, float fs)
{
    float acc = 0.0f;
    int   i;
    if (c == 0) {
        return 0.0f;
    }
    for (i = 0; i < c->n; i++) {
        acc += ag_biq_mag_db(&c->s[i], f, fs);
    }
    return acc;
}

float ag_biq_hshelf1(ag_biq_t *s, float fs, float f0, float db)
{
    float g, w1, w2, c, d, fc;

    if (s == 0 || fs <= 0.0f) {
        return 0.0f;
    }
    if (f0 <= 0.0f || (db > -0.01f && db < 0.01f)) {
        ag_biq_bypass(s);
        return 0.0f;
    }
    g = db_to_lin(db);
    if (g <= 0.0f) {
        ag_biq_bypass(s);
        return 0.0f;
    }
    fc = clamp_f0(fs, f0);
    /*
     * Zero and pole placed symmetrically about fc in the log domain, so that the
     * transition is centred where it was asked for: H(0) = 1, H(inf) = g, and
     * sqrt(w1*w2) = w0.  One real pole, which is the whole point - see the
     * header.
     */
    w1 = TWO_PI * fc / ag_sqrtf(g);
    w2 = TWO_PI * fc * ag_sqrtf(g);
    c = 2.0f * fs;
    d = 1.0f / (c + w2);
    s->b0 = g * (c + w1) * d;
    s->b1 = g * (w1 - c) * d;
    s->b2 = 0.0f;
    s->a1 = (w2 - c) * d;
    s->a2 = 0.0f;
    ag_biq_reset(s);
    return fc;
}

