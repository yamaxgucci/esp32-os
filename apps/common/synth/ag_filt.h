/*
 * State-variable filter (LP/HP/BP/notch), fixed-point.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_FILT_H
#define AG_FILT_H

#include <stdint.h>

enum {
    AG_FILT_LP = 0,
    AG_FILT_HP,
    AG_FILT_BP,
    AG_FILT_NOTCH
};

typedef struct ag_filt {
    int32_t low;
    int32_t band;
    uint8_t cutoff; /* 0..127 */
    uint8_t reso;   /* 0..127 */
    uint8_t mode;
} ag_filt_t;

void    ag_filt_reset(ag_filt_t *f);
int32_t ag_filt_tick(ag_filt_t *f, int32_t in); /* in/out ~Q15 */

#endif
