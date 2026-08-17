/*
 * ag_mathf - single-precision transcendentals for circuit simulation.
 *
 * The libc shim in apps/common/libc returns zero from expf/logf/powf: it exists
 * so that vendored code links, not so that it computes.  A device equation that
 * calls it gets a diode which never conducts, and it fails silently - the
 * signal just stays clean.  These are the real thing, written for the ESP32-S3
 * FPU: no tables, no branches on the hot path beyond range clamping, accuracy
 * around 1e-7 relative, which is far below the noise floor of 24-bit audio.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_MATHF_H
#define AG_MATHF_H

/* e^x.  Saturates to 0 below -87 and to ~3.4e38 above 88 (float range). */
float ag_expf(float x);

/* ln(x).  x <= 0 returns -88.0f rather than trapping: a Newton iteration that
 * wandered negative must come back, not stop the audio thread. */
float ag_logf(float x);

/* x^y for x > 0.  x <= 0 returns 0. */
float ag_powf(float x, float y);

/* sqrt(x) via the FPU reciprocal-square-root sequence; x <= 0 returns 0. */
float ag_sqrtf(float x);

/* tanh(x), used by the fast triode/JFET soft-knee paths. */
float ag_tanhf(float x);

#endif /* AG_MATHF_H */
