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

/*
 * sin(2*pi*idx/512), Q15, for idx in 0..511.  The index is a 512th of a turn
 * because the table is a 128-step quarter wave, and every twiddle a 512-point
 * transform needs lands on one of those steps exactly.
 */
static int32_t sin_512(int idx)
{
    const int quad = (idx >> 7) & 3;
    const int t = idx & 127;
    if (quad == 0) {
        return k_sin_q[t];
    }
    if (quad == 1) {
        return k_sin_q[128 - t];
    }
    if (quad == 2) {
        return -(int32_t)k_sin_q[t];
    }
    return -(int32_t)k_sin_q[128 - t];
}

/*
 * (a*wa + b*wb) >> 15.
 *
 * Written with a 64-bit intermediate on purpose, which looks like the
 * expensive way round on a 32-bit core and is not: the ESP32-S3 has MULL and
 * MULSH, so a 32x32 product costs two instructions, and SSAI/SRC pulls a
 * 32-bit window out of the pair in one more.  Tried and rejected: splitting
 * the operands at bit 15 to keep every product inside int32 - four multiplies
 * and four shifts where the compiler was already emitting three instructions.
 *
 * The 64-bit form is also the accurate one, because the sum happens before the
 * shift rather than after it.
 */
static int32_t mac_q15(int32_t a, int32_t wa, int32_t b, int32_t wb)
{
    return (int32_t)(((int64_t)a * wa + (int64_t)b * wb) >> 15);
}

/*
 * One twiddle, every block of the stage that uses it.
 *
 * A separate function, not an inner loop, and that is the whole point: with
 * the loop written inline the compiler ran out of registers at -Os and spilled
 * the twiddle and all four pointers to the stack on every butterfly - forty
 * instructions of address arithmetic around twenty of real work.  Xtensa gives
 * a called function a fresh register window, so here the eight live values fit
 * and the loop body is only the butterfly.  Six arguments, because that is how
 * many the window passes in registers; the step is derived from `half` rather
 * than passed for the same reason.
 */
#if defined(__GNUC__)
#define AG_FFT_NOINLINE __attribute__((noinline))
#else
#define AG_FFT_NOINLINE
#endif

AG_FFT_NOINLINE static void bfly(int32_t *ar, int32_t *ai, int half, int count,
                                 int32_t wr, int32_t wi)
{
    const int step = half << 1;
    int32_t  *br = ar + half, *bi = ai + half;

    while (count-- > 0) {
        const int32_t xr = *br, xi = *bi;
        const int32_t tr = mac_q15(xr, wr, xi, -wi);
        const int32_t ti = mac_q15(xr, wi, xi, wr);
        *br = *ar - tr;
        *bi = *ai - ti;
        *ar += tr;
        *ai += ti;
        ar += step;
        ai += step;
        br += step;
        bi += step;
    }
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

/*
 * Loop order matters here.  Written the textbook way - blocks outside, k
 * inside - the twiddle depends on the inner index and has to be produced for
 * every butterfly, which for a 512-point transform is 2304 pairs of table
 * lookups where 511 would do.  With k outside, each twiddle is computed once
 * and reused by every block of that stage.
 *
 * And k = 0 is worth its own loop: the twiddle there is exactly one, so the
 * butterfly is four adds.  That is 511 of the 2304 butterflies for free, and
 * it is also more accurate than what was here before, because a Q15 one is
 * 32767/32768 and every one of those butterflies was quietly losing 0.003%.
 */
int ag_fft_cplx_i32(int32_t *re, int32_t *im, int n, int forward)
{
    int len, lg;

    if (re == 0 || im == 0 || n < 4 || n > AG_FFT_MAX_N ||
        (n & (n - 1)) != 0) {
        return -1;
    }

    bitrev(re, im, n);

    for (len = 2, lg = 1; len <= n; len <<= 1, lg++) {
        const int half = len >> 1;
        const int count = n / len;
        int       k, i0;

        /* k = 0: the twiddle is exactly one, so this is four adds. */
        for (i0 = 0; i0 < n; i0 += len) {
            const int     i1 = i0 + half;
            const int32_t tr = re[i1], ti = im[i1];
            re[i1] = re[i0] - tr;
            im[i1] = im[i0] - ti;
            re[i0] = re[i0] + tr;
            im[i0] = im[i0] + ti;
        }

        for (k = 1; k < half; k++) {
            /*
             * The twiddle index: k turns out of len, expressed in 512ths.
             * The old code got here through (k * (n / len) << 9) / n, which is
             * the same number with a division in it.
             */
            const int     idx = lg <= 9 ? (k << (9 - lg)) : (k >> (lg - 9));
            const int32_t wr = sin_512((idx + 128) & 511);
            const int32_t wi = forward ? -sin_512(idx) : sin_512(idx);

            bfly(re + k, im + k, half, count, wr, wi);
        }
    }
    return 0;
}

/* ----------------------------------------------------------- real input --- */

/* 512ths of a turn per unit of k, for a transform of length n. */
static int turn_shift(int n)
{
    int lg = 0;
    while ((1 << lg) < n) {
        lg++;
    }
    return 9 - lg;
}

/*
 * One bin of the forward separation.
 *
 * Z is the transform of the packed half-length signal.  Writing A for the
 * transform of the even samples and B for the odd ones,
 *
 *   2A[k] = Z[k] + conj(Z[m-k]),   2B[k] = -i (Z[k] - conj(Z[m-k]))
 *   X[k]  = A[k] + W_n^k B[k]
 *
 * so A and B are formed with their halving done first and the rotation applied
 * to them.  Halving here rather than telling the caller about a factor of two
 * is the whole reason the caller needs no changes - and it costs half a unit in
 * the last place, where a factor of two would cost a whole bit of the lift the
 * caller spent care on.
 *
 * The halving is also what keeps A and B inside int32, and that matters more
 * than it looks: with them held as int64 the two rotations became 64-by-64
 * multiplies, which the compiler turns into three each, and the separation
 * cost as much as the transform it was meant to shorten.  Halved first, every
 * product is 32-by-32 - two instructions on this core.
 */
static void sep_bin(int32_t *re, int32_t *im, int n, int k, int32_t wr,
                    int32_t wi, int32_t zr1, int32_t zi1, int32_t zr2,
                    int32_t zi2)
{
    const int32_t ar = (int32_t)(((int64_t)zr1 + zr2 + 1) >> 1);
    const int32_t ai = (int32_t)(((int64_t)zi1 - zi2 + 1) >> 1);
    const int32_t br = (int32_t)(((int64_t)zi1 + zi2 + 1) >> 1);
    const int32_t bi = (int32_t)(((int64_t)zr2 - zr1 + 1) >> 1);
    const int32_t xr = ar + mac_q15(br, wr, bi, wi);
    const int32_t xi = ai + mac_q15(bi, wr, br, -wi);

    re[k] = xr;
    im[k] = xi;
    re[n - k] = xr;
    im[n - k] = -xi;
}

/*
 * Bins k and m-k together, because they need the same two Z bins and because
 * their twiddles are the same two numbers: W_n^(m-k) is -cos and +sin where
 * W_n^k is +cos and +sin, so one pair of table lookups does both.  Reading
 * both Z bins before writing anything is what makes it safe in place.
 */
static void sep_pair(int32_t *re, int32_t *im, int n, int k, int32_t wr,
                     int32_t wi)
{
    const int     k2 = (n >> 1) - k;
    const int32_t zr1 = re[k], zi1 = im[k];
    const int32_t zr2 = re[k2], zi2 = im[k2];

    sep_bin(re, im, n, k, wr, wi, zr1, zi1, zr2, zi2);
    if (k2 != k) {
        sep_bin(re, im, n, k2, -wr, wi, zr2, zi2, zr1, zi1);
    }
}

int ag_fft_real_fwd(int32_t *re, int32_t *im, int n)
{
    const int m = n >> 1;
    int       sh, j, k;

    if (re == 0 || im == 0 || n < 8 || n > AG_FFT_MAX_N ||
        (n & (n - 1)) != 0) {
        return -1;
    }
    sh = turn_shift(n);

    /*
     * Pack in place: z[j] = x[2j] + i x[2j+1].  Ascending is safe because slot
     * j is written only after slots 2j and 2j+1 have been read, and no later
     * iteration reads a slot that low.
     */
    for (j = 0; j < m; j++) {
        const int32_t a = re[j * 2], b = re[j * 2 + 1];
        re[j] = a;
        im[j] = b;
    }
    if (ag_fft_cplx_i32(re, im, m, 1) != 0) {
        return -1;
    }

    /*
     * Separate, in place, in pairs.  Bin k needs Z[k] and Z[m-k], and so does
     * bin m-k, so the two are done together and the slots they overwrite are
     * exactly the two just read.  The mirrored halves land in the upper bins,
     * which hold nothing yet.
     */
    {
        const int32_t zr = re[0], zi = im[0];
        /* DC and Nyquist are both real, and both come out of Z[0]: the sum of
         * the even samples plus or minus the sum of the odd ones. */
        re[0] = zr + zi;
        im[0] = 0;
        re[m] = zr - zi;
        im[m] = 0;
    }
    for (k = 1; k <= (m >> 1); k++) {
        const int idx = (k << sh) & 511;
        sep_pair(re, im, n, k, sin_512((idx + 128) & 511), sin_512(idx));
    }
    return 0;
}

/*
 * One bin of the inverse separation - the same identity read backwards:
 *
 *   2A[k] = X[k] + conj(X[m-k]),  2B[k] = W_n^-k (X[k] - conj(X[m-k]))
 *   2Z[k] = 2A[k] + i 2B[k]
 *
 * Twice Z is what is wanted here rather than a nuisance: the half-length
 * inverse is unnormalized and multiplies by m, so 2Z comes out as n times the
 * signal - exactly the scale the full-length inverse would have produced.
 */
static void sep_inv_bin(int32_t *re, int32_t *im, int k, int32_t wr, int32_t wi,
                        int32_t xr1, int32_t xi1, int32_t xr2, int32_t xi2)
{
    /* No halving on this side, and none needed: the caller's bound leaves a
     * bit of room precisely so that these sums fit. */
    const int32_t ar = xr1 + xr2, ai = xi1 - xi2;
    const int32_t cr = xr1 - xr2, ci = xi1 + xi2;

    re[k] = ar - mac_q15(ci, wr, cr, wi);
    im[k] = ai + mac_q15(cr, wr, ci, -wi);
}

static void sep_inv_pair(int32_t *re, int32_t *im, int n, int k, int32_t wr,
                         int32_t wi)
{
    const int     k2 = (n >> 1) - k;
    const int32_t xr1 = re[k], xi1 = im[k];
    const int32_t xr2 = re[k2], xi2 = im[k2];

    sep_inv_bin(re, im, k, wr, wi, xr1, xi1, xr2, xi2);
    if (k2 != k) {
        sep_inv_bin(re, im, k2, -wr, wi, xr2, xi2, xr1, xi1);
    }
}

int ag_fft_real_inv(int32_t *re, int32_t *im, int n)
{
    const int m = n >> 1;
    int       sh, j, k;

    if (re == 0 || im == 0 || n < 8 || n > AG_FFT_MAX_N ||
        (n & (n - 1)) != 0) {
        return -1;
    }
    sh = turn_shift(n);

    {
        const int32_t x0 = re[0], xm = re[m];
        re[0] = x0 + xm;
        im[0] = x0 - xm;
    }
    for (k = 1; k <= (m >> 1); k++) {
        const int idx = (k << sh) & 511;
        sep_inv_pair(re, im, n, k, sin_512((idx + 128) & 511), sin_512(idx));
    }
    if (ag_fft_cplx_i32(re, im, m, 0) != 0) {
        return -1;
    }
    /* Unpack, descending so that slot 2j is written after slot j is read. */
    for (j = m - 1; j >= 0; j--) {
        const int32_t a = re[j], b = im[j];
        re[j * 2] = a;
        re[j * 2 + 1] = b;
    }
    return 0;
}
