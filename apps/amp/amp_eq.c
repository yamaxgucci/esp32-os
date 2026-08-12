/*
 * Simple float biquad EQ (libc math via shim: sinf/cosf/powf).
 * SPDX-License-Identifier: Apache-2.0
 */
#include "amp_eq.h"

#include <string.h>

#include "math.h"

static const float k_freq[AMP_EQ_BANDS] = {
    60.f, 170.f, 310.f, 600.f, 1000.f, 3000.f, 6000.f, 12000.f, 14000.f, 16000.f
};

static float clampf(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static float db_to_lin(float db)
{
    return powf(10.f, db / 20.f);
}

static void peaking(float *b0, float *b1, float *b2, float *a1, float *a2,
                    float rate, float freq, float gain_db, float Q)
{
    float A = powf(db_to_lin(gain_db), 0.5f);
    float w0 = 2.f * 3.14159265f * freq / rate;
    float alpha = sinf(w0) / (2.f * Q);
    float cosw = cosf(w0);
    float a0 = 1.f + alpha / A;
    *b0 = (1.f + alpha * A) / a0;
    *b1 = (-2.f * cosw) / a0;
    *b2 = (1.f - alpha * A) / a0;
    *a1 = (-2.f * cosw) / a0;
    *a2 = (1.f - alpha / A) / a0;
}

static void lowshelf(float *b0, float *b1, float *b2, float *a1, float *a2,
                     float rate, float freq, float gain_db)
{
    float A = powf(db_to_lin(gain_db), 0.5f);
    float w0 = 2.f * 3.14159265f * freq / rate;
    float cosw = cosf(w0);
    float sinw = sinf(w0);
    float alpha = sinw / 2.f * 1.41421356f;
    float sa = 2.f * powf(A, 0.5f) * alpha;
    float a0 = (A + 1.f) + (A - 1.f) * cosw + sa;
    *b0 = (A * ((A + 1.f) - (A - 1.f) * cosw + sa)) / a0;
    *b1 = (2.f * A * ((A - 1.f) - (A + 1.f) * cosw)) / a0;
    *b2 = (A * ((A + 1.f) - (A - 1.f) * cosw - sa)) / a0;
    *a1 = (-2.f * ((A - 1.f) + (A + 1.f) * cosw)) / a0;
    *a2 = ((A + 1.f) + (A - 1.f) * cosw - sa) / a0;
}

void amp_eq_recalc(amp_eq_t *eq)
{
    int i;
    float rate;
    if (eq == NULL) {
        return;
    }
    rate = eq->rate ? (float)eq->rate : 22050.f;
    lowshelf(&eq->b0[0], &eq->b1[0], &eq->b2[0], &eq->a1[0], &eq->a2[0], rate,
             100.f, (float)eq->preamp);
    for (i = 0; i < AMP_EQ_BANDS; i++) {
        float f = k_freq[i];
        if (f > rate * 0.45f) {
            f = rate * 0.45f;
        }
        peaking(&eq->b0[i + 1], &eq->b1[i + 1], &eq->b2[i + 1], &eq->a1[i + 1],
                &eq->a2[i + 1], rate, f, (float)eq->gain[i], 1.2f);
    }
}

void amp_eq_init(amp_eq_t *eq, uint32_t rate)
{
    if (eq == NULL) {
        return;
    }
    memset(eq, 0, sizeof(*eq));
    eq->enabled = 1;
    eq->band_sel = 0;
    eq->rate = rate ? rate : 22050u;
    amp_eq_recalc(eq);
}

void amp_eq_set_rate(amp_eq_t *eq, uint32_t rate)
{
    if (eq == NULL) {
        return;
    }
    eq->rate = rate ? rate : 22050u;
    memset(eq->z1, 0, sizeof(eq->z1));
    memset(eq->z2, 0, sizeof(eq->z2));
    amp_eq_recalc(eq);
}

void amp_eq_reset(amp_eq_t *eq)
{
    int i;
    if (eq == NULL) {
        return;
    }
    eq->preamp = 0;
    for (i = 0; i < AMP_EQ_BANDS; i++) {
        eq->gain[i] = 0;
    }
    memset(eq->z1, 0, sizeof(eq->z1));
    memset(eq->z2, 0, sizeof(eq->z2));
    amp_eq_recalc(eq);
}

static float biquad(amp_eq_t *eq, int filt, int ch, float x)
{
    float y = eq->b0[filt] * x + eq->z1[ch][filt];
    eq->z1[ch][filt] = eq->b1[filt] * x - eq->a1[filt] * y + eq->z2[ch][filt];
    eq->z2[ch][filt] = eq->b2[filt] * x - eq->a2[filt] * y;
    return y;
}

static int eq_is_flat(const amp_eq_t *eq)
{
    int i;
    if (eq->preamp != 0) {
        return 0;
    }
    for (i = 0; i < AMP_EQ_BANDS; i++) {
        if (eq->gain[i] != 0) {
            return 0;
        }
    }
    return 1;
}

void amp_eq_process(amp_eq_t *eq, int16_t *stereo, int frames)
{
    int i, f, ch;
    float peaks[AMP_EQ_BANDS];
    int filter;
    if (eq == NULL || stereo == NULL || frames <= 0) {
        return;
    }
    /* 11 stereo float biquads/sample is the hot path — skip when flat. */
    filter = eq->enabled && !eq_is_flat(eq);
    for (i = 0; i < AMP_EQ_BANDS; i++) {
        peaks[i] = eq->spectrum[i] * 0.85f;
    }
    for (i = 0; i < frames; i++) {
        if (filter) {
            for (ch = 0; ch < 2; ch++) {
                float x = (float)stereo[i * 2 + ch];
                float y = x;
                for (f = 0; f < AMP_EQ_BANDS + 1; f++) {
                    y = biquad(eq, f, ch, y);
                }
                if (y > 32767.f) {
                    y = 32767.f;
                }
                if (y < -32768.f) {
                    y = -32768.f;
                }
                stereo[i * 2 + ch] = (int16_t)y;
            }
        }
        {
            float m = (float)((int)stereo[i * 2] + (int)stereo[i * 2 + 1]) * 0.5f;
            float a = (m < 0 ? -m : m) / 32768.f;
            int band = (i * AMP_EQ_BANDS) / frames;
            if (band >= AMP_EQ_BANDS) {
                band = AMP_EQ_BANDS - 1;
            }
            if (a > peaks[band]) {
                peaks[band] = a;
            }
        }
    }
    for (i = 0; i < AMP_EQ_BANDS; i++) {
        eq->spectrum[i] = clampf(peaks[i], 0.f, 1.f);
    }
}
