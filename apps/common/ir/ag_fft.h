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

#endif /* AG_FFT_H */
