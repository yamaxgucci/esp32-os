/*
 * Circuit-inspired distortion: JFET / 12AX7-ish LUT + pre/de EQ + sag.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_DIST_H
#define AG_DIST_H

#include <stdint.h>

enum {
    AG_DIST_JFET = 0,
    AG_DIST_TUBE,
    AG_DIST_CLIP,
    AG_DIST_FOLD,
    AG_DIST_CRUSH
};

typedef struct ag_dist {
    uint8_t  model;
    uint8_t  drive; /* 0..127 */
    uint8_t  bias;  /* 0..127, 64 = centre */
    uint8_t  sag;   /* 0..127 */
    int32_t  hp_z;
    int32_t  lp_z;
    int32_t  env;
} ag_dist_t;

void    ag_dist_reset(ag_dist_t *d);
int32_t ag_dist_tick(ag_dist_t *d, int32_t in);
void    ag_dist_process(ag_dist_t *d, int16_t *stereo, int32_t frames);

#endif
