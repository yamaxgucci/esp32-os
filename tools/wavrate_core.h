/*
 * The resampler behind wavrate, on its own so that more than one tool can use
 * it.
 *
 * It was inside wavrate's main, which was fine while the only thing that needed
 * to change a sample rate was a file on the way to or from a NAM capture.  The
 * tone matcher does it in memory instead - the take goes up to 48 kHz, through
 * the capture, and back down, without a 16-bit file in the middle - so the
 * filter had to come out of the program that wrote files.
 *
 * One implementation, so the -64 dB round trip recorded in
 * apps/common/tube/README.md keeps meaning what it says for both paths.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef WAVRATE_CORE_H
#define WAVRATE_CORE_H

#include <stdint.h>

/*
 * Polyphase sinc, Kaiser beta 12, 64 taps per phase, cut below both Nyquists.
 * At 44.1 <-> 48 kHz that is 160 phases of 64 - a ten thousand tap prototype
 * with a stopband under -110 dB.
 *
 * Returns a malloc'd buffer of *out_n samples, or NULL.  `verbose` prints the
 * line wavrate has always printed.  The result is *not* peak-limited: a
 * resampler can overshoot, and whether that matters is the caller's business -
 * wavrate scales before it quantises, the matcher stays in float and does not.
 */
double *wr_resample(const double *x, uint32_t n, uint32_t rate_in,
                    uint32_t rate_out, uint32_t *out_n, int verbose);

/* The same, for callers whose signal is float. */
float *wr_resample_f(const float *x, uint32_t n, uint32_t rate_in,
                     uint32_t rate_out, uint32_t *out_n, int verbose);

#endif /* WAVRATE_CORE_H */
