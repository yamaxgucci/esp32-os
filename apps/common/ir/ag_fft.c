/*
 * Fixed-point radix-2 complex FFT — int32, unnormalized, no libm.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_fft.h"

/* Quarter-wave sine, Q15: sin(pi/2 * i / 128) for i = 0..128. */
static const int16_t k_sin_q[129] = {
    0,     402,   804,   1205,  1607,  2009,  2410,  2811,  3211,  3611,
    4011,  4409,  4807,  5205,  5601,  5997,  6392,  6786,  7179,  7571,
    7961,  8351,  8739,  9126,  9511,  9895,  10278, 10659, 11038, 11416,
    11792, 12166, 12539, 12909, 13278, 13645, 14009, 14372, 14732, 15090,
    15446, 15799, 16150, 16499, 16845, 17189, 17530, 17868, 18204, 18537,
    18867, 19194, 19519, 19840, 20159, 20474, 20787, 21096, 21402, 21705,
    22004, 22301, 22594, 22883, 23169, 23452, 23731, 24006, 24278, 24546,
    24811, 25072, 25329, 25582, 25831, 26077, 26318, 26556, 26789, 27019,
    27244, 27466, 27683, 27896, 28105, 28309, 28510, 28706, 28897, 29085,
    29268, 29446, 29621, 29790, 29955, 30116, 30272, 30424, 30571, 30713,
    30851, 30984, 31113, 31236, 31356, 31470, 31580, 31684, 31785, 31880,
    31970, 32056, 32137, 32213, 32284, 32350, 32412, 32468, 32520, 32567,
    32609, 32646, 32678, 32705, 32727, 32744, 32756, 32764, 32767,
};

static int16_t sin_turn(int idx, int n)
{
    int full = (idx << 9) / n;
    int quad = (full >> 7) & 3;
    int t = full & 127;
    if (quad == 0) {
        return k_sin_q[t];
    }
    if (quad == 1) {
        return k_sin_q[128 - t];
    }
    if (quad == 2) {
        return (int16_t)(-k_sin_q[t]);
    }
    return (int16_t)(-k_sin_q[128 - t]);
}

static int16_t cos_turn(int idx, int n)
{
    return sin_turn(idx + (n >> 2), n);
}

static void bitrev(int32_t *re, int32_t *im, int n)
{
    int i, j = 0;
    for (i = 0; i < n; i++) {
        if (i < j) {
            int32_t tr = re[i], ti = im[i];
            re[i] = re[j];
            im[i] = im[j];
            re[j] = tr;
            im[j] = ti;
        }
        {
            int m = n >> 1;
            while (m >= 1 && j >= m) {
                j -= m;
                m >>= 1;
            }
            j += m;
        }
    }
}

int ag_fft_cplx_i32(int32_t *re, int32_t *im, int n, int forward)
{
    int len;
    if (re == 0 || im == 0 || n < 4 || n > AG_FFT_MAX_N ||
        (n & (n - 1)) != 0) {
        return -1;
    }

    bitrev(re, im, n);

    for (len = 2; len <= n; len <<= 1) {
        int half = len >> 1;
        int step;
        for (step = 0; step < n; step += len) {
            int k;
            for (k = 0; k < half; k++) {
                int i0 = step + k;
                int i1 = i0 + half;
                int16_t wr = cos_turn(k * (n / len), n);
                int16_t wi = sin_turn(k * (n / len), n);
                int32_t tr, ti;
                if (forward) {
                    wi = (int16_t)(-wi);
                }
                tr = (int32_t)(((int64_t)re[i1] * wr - (int64_t)im[i1] * wi) >>
                               15);
                ti = (int32_t)(((int64_t)re[i1] * wi + (int64_t)im[i1] * wr) >>
                               15);
                re[i1] = re[i0] - tr;
                im[i1] = im[i0] - ti;
                re[i0] = re[i0] + tr;
                im[i0] = im[i0] + ti;
            }
        }
    }
    return 0;
}
