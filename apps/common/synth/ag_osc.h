/*
 * VA oscillators (PolyBLEP saw/square, LUT sine/tri, noise).
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
    AG_OSC_NOISE
};

typedef struct ag_osc {
    uint32_t phase;
    uint32_t step;
    uint8_t  wave;
    uint8_t  pwm; /* 0..127, 64 = 50% */
    uint32_t rng;
} ag_osc_t;

void    ag_osc_set_hz_x100(ag_osc_t *o, int32_t hz_x100, uint32_t rate);
int32_t ag_osc_tick(ag_osc_t *o); /* -32767..32767 */

#endif
