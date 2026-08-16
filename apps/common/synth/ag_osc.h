/*
 * VA oscillators (PolyBLEP saw/square, LUT sine/tri, noise, custom table).
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_OSC_H
#define AG_OSC_H

#include <stdint.h>

enum {
    AG_OSC_SAW = 0,
    AG_OSC_SQR,
    AG_OSC_TRI,
    AG_OSC_SIN,
    AG_OSC_NOISE,
    AG_OSC_WT /* custom single-cycle table (from WAV or filled in RAM) */
};

typedef struct ag_osc {
    uint32_t       phase;
    uint32_t       step;
    uint8_t        wave;
    uint8_t        pwm; /* 0..127, 64 = 50% */
    uint32_t       rng;
    const int16_t *wt;
    uint32_t       wt_n;
} ag_osc_t;

void    ag_osc_set_hz_x100(ag_osc_t *o, int32_t hz_x100, uint32_t rate);
void    ag_osc_set_table(ag_osc_t *o, const int16_t *data, uint32_t frames);

/*
 * Build one period into dst[cap] from a longer PCM (a WAV note).
 * root = MIDI note of the recording. Returns frames written (>= 2) or 0.
 */
uint32_t ag_osc_cycle_from_pcm(int16_t *dst, uint32_t cap, const int16_t *pcm,
                               uint32_t frames, uint32_t src_rate, int root);

int32_t ag_osc_tick(ag_osc_t *o); /* -32767..32767 */

#endif
