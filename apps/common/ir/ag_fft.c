/*
 * Fixed-point radix-2 complex FFT — int32, unnormalized, no libm.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_fft.h"

/*
 * Quarter-wave sine, Q30: sin(pi/2 * i / 128) for i = 0..128.
 *
 * Q30 rather than the Q15 this held first, and it is the single largest thing
 * separating this engine from the convolution it claims to be.  A Q15 twiddle
 * is wrong by up to half a part in 32768, and that error is *multiplicative*:
 * it scales whatever passes through the rotation, so no amount of headroom in
 * the data words moves it.  Nine stages of it measured as a floor 68 dB under
 * the signal, which is 8 dB of the whole engine's error and 6 dB of its error
 * in the top octaves - where a cabinet's own output is 60 dB down and has
 * none to spare.
 *
 * It is free.  The multiply below was already 32x32 into a 64-bit
 * intermediate, so a wider coefficient costs no instructions, only 258 more
 * bytes of table.
 */
static const int32_t k_sin_q30[129] = {
    0, 13176464, 26350943, 39521455, 52686014, 65842639,
    78989349, 92124163, 105245103, 118350194, 131437462, 144504935,
    157550647, 170572633, 183568930, 196537583, 209476638, 222384147,
    235258165, 248096755, 260897982, 273659918, 286380643, 299058239,
    311690799, 324276419, 336813204, 349299266, 361732726, 374111709,
    386434353, 398698801, 410903207, 423045732, 435124548, 447137835,
    459083786, 470960600, 482766489, 494499676, 506158392, 517740883,
    529245404, 540670223, 552013618, 563273883, 574449320, 585538248,
    596538995, 607449906, 618269338, 628995660, 639627258, 650162530,
    660599890, 670937767, 681174602, 691308855, 701339000, 711263525,
    721080937, 730789757, 740388522, 749875788, 759250125, 768510122,
    777654384, 786681534, 795590213, 804379079, 813046808, 821592095,
    830013654, 838310216, 846480531, 854523370, 862437520, 870221790,
    877875009, 885396022, 892783698, 900036924, 907154608, 914135678,
    920979082, 927683790, 934248793, 940673101, 946955747, 953095785,
    959092290, 964944360, 970651112, 976211688, 981625251, 986890984,
    992008094, 996975812, 1001793390, 1006460100, 1010975242, 1015338134,
    1019548121, 1023604567, 1027506862, 1031254418, 1034846671, 1038283080,
    1041563127, 1044686319, 1047652185, 1050460278, 1053110176, 1055601479,
    1057933813, 1060106826, 1062120190, 1063973603, 1065666786, 1067199483,
    1068571464, 1069782521, 1070832474, 1071721163, 1072448455, 1073014240,
    1073418433, 1073660973, 1073741824,
};

/*
 * sin(2*pi*idx/512), Q30, for idx in 0..511.  The index is a 512th of a turn
 * because the table is a 128-step quarter wave, and every twiddle a 512-point
 * transform needs lands on one of those steps exactly.
 */
static int32_t sin_512(int idx)
{
    const int quad = (idx >> 7) & 3;
    const int t = idx & 127;
    if (quad == 0) {
        return k_sin_q30[t];
    }
    if (quad == 1) {
        return k_sin_q30[128 - t];
    }
    if (quad == 2) {
        return -k_sin_q30[t];
    }
    return -k_sin_q30[128 - t];
}

/*
 * (a*wa + b*wb) >> 30, for Q30 twiddles.
 *
 * Written with a 64-bit intermediate on purpose, which looks like the
 * expensive way round on a 32-bit core and is not: the ESP32-S3 has MULL and
 * MULSH, so a 32x32 product costs two instructions, and SSAI/SRC pulls a
 * 32-bit window out of the pair in one more.  Tried and rejected: splitting
 * the operands at bit 15 to keep every product inside int32 - four multiplies
 * and four shifts where the compiler was already emitting three instructions.
 *
 * The 64-bit form is also the accurate one, because the sum happens before the
 * shift rather than after it.  Nothing here overflows: the transform's inputs
 * are bounded to 2^30 by fft_headroom and the twiddles to 2^30, so the two
 * products together cannot pass 2^61.
 */
static int32_t mac_q30(int32_t a, int32_t wa, int32_t b, int32_t wb)
{
    return (int32_t)(((int64_t)a * wa + (int64_t)b * wb) >> 30);
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
        const int32_t tr = mac_q30(xr, wr, xi, -wi);
        const int32_t ti = mac_q30(xr, wi, xi, wr);
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
 * it is also the exact one - a tabulated one is never quite unity, and every
 * one of those butterflies used to lose that much.
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
    const int32_t xr = ar + mac_q30(br, wr, bi, wi);
    const int32_t xi = ai + mac_q30(bi, wr, br, -wi);

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

    re[k] = ar - mac_q30(ci, wr, cr, wi);
    im[k] = ai + mac_q30(cr, wr, ci, -wi);
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
