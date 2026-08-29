/*
 * See wavrate_core.h.  This is wavrate's filter, moved out of its main and
 * otherwise unchanged - same window, same taps, same cutoff - so that a
 * measurement made through either caller is the same measurement.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "wavrate_core.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TAPS_PER_PHASE 64

static double bessel_i0(double x)
{
    double s = 1.0, t = 1.0;
    int    i;
    for (i = 1; i < 80; i++) {
        t *= (x / (2.0 * i)) * (x / (2.0 * i));
        s += t;
        if (t < s * 1e-18) {
            break;
        }
    }
    return s;
}

static uint32_t gcd_u(uint32_t a, uint32_t b)
{
    while (b != 0) {
        const uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

double *wr_resample(const double *x, uint32_t n, uint32_t rate_in,
                    uint32_t rate_out, uint32_t *out_n, int verbose)
{
    uint32_t l, m, g, ntaps, i, on;
    double  *h, *y;
    double   fc, gain, sum = 0.0, peak = 0.0;
    int64_t  k;

    *out_n = 0;
    if (x == NULL || n == 0 || rate_in == 0 || rate_out == 0) {
        return NULL;
    }
    if (rate_in == rate_out) {
        y = (double *)malloc(sizeof(double) * n);
        if (y == NULL) {
            return NULL;
        }
        for (i = 0; i < n; i++) {
            y[i] = x[i];
        }
        *out_n = n;
        if (verbose) {
            printf("  resample: already %u Hz, %u frames\n", rate_in, n);
        }
        return y;
    }

    g = gcd_u(rate_in, rate_out);
    l = rate_out / g;
    m = rate_in / g;
    ntaps = l * TAPS_PER_PHASE;
    if ((ntaps & 1u) == 0u) {
        ntaps++;
    }

    /* Cut off below both Nyquists, in the L-times-upsampled domain. */
    fc = 0.47 / (double)(l > m ? l : m);
    gain = (double)l;
    h = (double *)malloc((size_t)ntaps * sizeof(double));
    if (h == NULL) {
        return NULL;
    }
    for (i = 0; i < ntaps; i++) {
        const double t = (double)i - (double)(ntaps - 1) / 2.0;
        const double s = (t == 0.0) ? 2.0 * fc
                                    : sin(2.0 * M_PI * fc * t) / (M_PI * t);
        const double r = (2.0 * i) / (double)(ntaps - 1) - 1.0;
        const double w = bessel_i0(12.0 * sqrt(1.0 - r * r)) / bessel_i0(12.0);
        h[i] = s * w;
        sum += h[i];
    }
    for (i = 0; i < ntaps; i++) {
        h[i] *= gain / sum;
    }

    on = (uint32_t)(((uint64_t)n * l) / m);
    y = (double *)calloc(on ? on : 1u, sizeof(double));
    if (y == NULL) {
        free(h);
        return NULL;
    }
    /*
     * Output sample j sits at input position j*m/l.  Only every l-th tap of the
     * prototype lines up with a real input sample, so the inner loop walks one
     * phase of it - sixty-four multiplies, not ten thousand.
     */
    for (i = 0; i < on; i++) {
        const uint64_t pos = (uint64_t)i * m + (ntaps - 1u) / 2u;
        const int64_t  base = (int64_t)(pos / l);
        const uint32_t phase = (uint32_t)(pos % l);
        double         acc = 0.0;
        for (k = 0; phase + (uint64_t)k * l < ntaps; k++) {
            const int64_t idx = base - k;
            if (idx < 0) {
                break;
            }
            if (idx < (int64_t)n) {
                acc += h[phase + (uint64_t)k * l] * x[idx];
            }
        }
        y[i] = acc;
        if (fabs(acc) > peak) {
            peak = fabs(acc);
        }
    }
    free(h);
    *out_n = on;
    if (verbose) {
        printf("  resample: %u Hz -> %u Hz (%u/%u), %u taps, %u -> %u frames,"
               " peak %.3f\n", rate_in, rate_out, l, m, ntaps, n, on, peak);
    }
    return y;
}

float *wr_resample_f(const float *x, uint32_t n, uint32_t rate_in,
                     uint32_t rate_out, uint32_t *out_n, int verbose)
{
    double  *xd = (double *)malloc(sizeof(double) * (n ? n : 1u));
    double  *yd;
    float   *y;
    uint32_t i;

    if (xd == NULL) {
        *out_n = 0;
        return NULL;
    }
    for (i = 0; i < n; i++) {
        xd[i] = (double)x[i];
    }
    yd = wr_resample(xd, n, rate_in, rate_out, out_n, verbose);
    free(xd);
    if (yd == NULL) {
        return NULL;
    }
    y = (float *)malloc(sizeof(float) * (*out_n ? *out_n : 1u));
    if (y == NULL) {
        free(yd);
        *out_n = 0;
        return NULL;
    }
    for (i = 0; i < *out_n; i++) {
        y[i] = (float)yd[i];
    }
    free(yd);
    return y;
}
