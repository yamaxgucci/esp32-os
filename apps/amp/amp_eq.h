/*
 * 10-band + preamp EQ and simple spectrum peaks for AMP.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AMP_EQ_H
#define AMP_EQ_H

#include <stdint.h>

enum { AMP_EQ_BANDS = 10 };

typedef struct amp_eq {
    int      enabled;
    int      band_sel;                 /* 0=preamp, 1..10 bands */
    int8_t   preamp;                   /* -12..+12 dB */
    int8_t   gain[AMP_EQ_BANDS];       /* -12..+12 dB */
    /* biquad state: preamp shelf + 10 peaking, stereo */
    float    z1[2][AMP_EQ_BANDS + 1];
    float    z2[2][AMP_EQ_BANDS + 1];
    float    b0[AMP_EQ_BANDS + 1], b1[AMP_EQ_BANDS + 1], b2[AMP_EQ_BANDS + 1];
    float    a1[AMP_EQ_BANDS + 1], a2[AMP_EQ_BANDS + 1];
    uint32_t rate;
    float    spectrum[AMP_EQ_BANDS];   /* 0..1 display */
} amp_eq_t;

void amp_eq_init(amp_eq_t *eq, uint32_t rate);
void amp_eq_set_rate(amp_eq_t *eq, uint32_t rate);
void amp_eq_recalc(amp_eq_t *eq);
void amp_eq_reset(amp_eq_t *eq);
void amp_eq_process(amp_eq_t *eq, int16_t *stereo, int frames);

#endif /* AMP_EQ_H */
