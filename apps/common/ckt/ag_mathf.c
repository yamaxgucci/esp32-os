/*
 * ag_mathf - single-precision transcendentals, no libm, no tables.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_mathf.h"

#include <stdint.h>

typedef union {
    float    f;
    int32_t  i;
    uint32_t u;
} fbits_t;

/*
 * e^x = 2^(x*log2 e) = 2^k * 2^f with f in [-0.5, 0.5].
 *
 * 2^f is the Taylor series in (f ln2) through the sixth term; on that interval
 * the tail is below 2e-8, which is under one ulp of the result.  2^k is built
 * by writing the exponent field directly - the only place this file needs to
 * know what a float looks like.
 */
float ag_expf(float x)
{
    fbits_t two_k;
    float   t, f, p;
    int     k;

    if (x > 88.0f) {
        return 3.4028235e38f;
    }
    if (x < -87.0f) {
        return 0.0f;
    }

    t = x * 1.44269504088896f; /* log2(e) */
    k = (int)(t >= 0.0f ? t + 0.5f : t - 0.5f);
    f = t - (float)k;

    p = 1.5253e-4f;                /* (ln2)^6/720 */
    p = p * f + 1.3333558e-3f;     /* (ln2)^5/120 */
    p = p * f + 9.6181291e-3f;     /* (ln2)^4/24  */
    p = p * f + 5.5504109e-2f;     /* (ln2)^3/6   */
    p = p * f + 2.4022651e-1f;     /* (ln2)^2/2   */
    p = p * f + 6.9314718e-1f;     /* ln2         */
    p = p * f + 1.0f;

    two_k.i = (k + 127) << 23;
    return p * two_k.f;
}

/*
 * ln(x): split off the exponent, then the odd atanh series on the mantissa.
 *
 * Folding the mantissa into [1/sqrt2, sqrt2) keeps |s| = |(m-1)/(m+1)| under
 * 0.1716, so five odd terms are worth ~1e-8.  The one divide is the price of
 * that fast convergence and is still cheaper than a longer polynomial in m.
 */
float ag_logf(float x)
{
    fbits_t v;
    float   m, s, s2, p;
    int     e;

    if (x <= 0.0f) {
        return -88.0f;
    }

    v.f = x;
    e = (int)((v.u >> 23) & 0xffu) - 127;
    v.u = (v.u & 0x007fffffu) | (127u << 23);
    m = v.f; /* [1, 2) */
    if (m > 1.41421356f) {
        m *= 0.5f;
        e++;
    }

    s = (m - 1.0f) / (m + 1.0f);
    s2 = s * s;
    p = 1.0f / 9.0f;
    p = p * s2 + 1.0f / 7.0f;
    p = p * s2 + 1.0f / 5.0f;
    p = p * s2 + 1.0f / 3.0f;
    p = p * s2 + 1.0f;

    return 2.0f * s * p + (float)e * 6.93147181e-1f;
}

float ag_powf(float x, float y)
{
    if (x <= 0.0f) {
        return 0.0f;
    }
    return ag_expf(y * ag_logf(x));
}

/*
 * sqrt(x) = x * rsqrt(x), Newton on the reciprocal square root.
 *
 * The obvious form, r = (r + x/r)/2, needs a division per step, and a float
 * division on this FPU measured 37 instructions - more than the whole rest of
 * the function.  Newton on 1/sqrt(x) has no division in it at all: three
 * multiplies and a subtract per step.  Two steps from the bit-hack seed land
 * within about 1e-7 relative, and the whole thing costs less than one divide.
 */
float ag_sqrtf(float x)
{
    fbits_t v;
    float   y, hx;

    if (x <= 0.0f) {
        return 0.0f;
    }

    hx = 0.5f * x;
    v.f = x;
    v.i = 0x5f3759df - (v.i >> 1); /* seed for 1/sqrt(x) */
    y = v.f;
    y = y * (1.5f - hx * y * y);
    y = y * (1.5f - hx * y * y);
    y = y * (1.5f - hx * y * y);
    return x * y;
}

/*
 * tanh(x) = 1 - 2/(e^2x + 1), with the series near zero where that form loses
 * precision to cancellation, and saturation past +-9 where it is 1.0 in float.
 */
float ag_tanhf(float x)
{
    float ax = x < 0.0f ? -x : x;
    float y;

    if (ax > 9.0f) {
        return x < 0.0f ? -1.0f : 1.0f;
    }
    if (ax < 1.0e-3f) {
        return x - x * x * x * (1.0f / 3.0f);
    }
    y = 1.0f - 2.0f / (ag_expf(2.0f * ax) + 1.0f);
    return x < 0.0f ? -y : y;
}
