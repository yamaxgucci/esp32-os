/*
 * Fixed-point radix-2 complex FFT — int32, unnormalized, no libm.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_FFT_H
#define AG_FFT_H

#include <stdint.h>

#define AG_FFT_MAX_N 512

/*
 * In-place complex FFT. n = power of two, 4..AG_FFT_MAX_N.
 * Unnormalized: IDFT(DFT(x)) = n * x. Twiddles are Q15.
 * forward=1 → DFT, forward=0 → inverse (conjugated twiddles).
 */
int ag_fft_cplx_i32(int32_t *re, int32_t *im, int n, int forward);

/*
 * The same two transforms for a real signal, at a bit under half the cost.
 *
 * n real samples are n/2 complex ones if the even samples are read as the real
 * part and the odd as the imaginary, so a transform of half the length does
 * almost all of the work and one pass over the bins separates the answer back
 * out.  512 points is 2304 butterflies; 256 is 1024.
 *
 * Deliberately the same convention and the same scale as the complex pair, so
 * a caller's own shift bookkeeping cannot tell which one it called:
 *
 *   fwd: re[0..n-1] holds the real signal on entry and all n bins of its
 *        (conjugate-symmetric) transform on return.  im is written throughout
 *        and need not be zeroed first.
 *   inv: re/im hold bins 0..n/2 of a conjugate-symmetric spectrum - the upper
 *        bins are not read - and re[0..n-1] holds n times the real signal on
 *        return.  im is scratch.
 *
 * n = power of two, 8..AG_FFT_MAX_N.
 */
int ag_fft_real_fwd(int32_t *re, int32_t *im, int n);
int ag_fft_real_inv(int32_t *re, int32_t *im, int n);

#endif /* AG_FFT_H */
