/*
 * ArgonOS lightweight integer FM synth (userspace).
 * Protocol-agnostic core; SMS maps YM2413 regs onto this API.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_FM_H
#define AG_FM_H

#include <stdint.h>

#define AG_FM_CHANNELS 9

typedef struct ag_fm_chan {
    uint32_t phase;      /* Q16 accumulator */
    uint32_t step;       /* Q16 phase increment per sample */
    uint16_t fnum;       /* last OPLL f-number */
    uint8_t  block;      /* last OPLL block */
    int32_t  env;        /* 0..65535 current level */
    int32_t  env_target;
    int32_t  env_rate;
    uint8_t  vol;        /* 0..15 attenuation (0 = loud) */
    uint8_t  inst;       /* 0..15 */
    uint8_t  key;
    uint8_t  sus;
    uint16_t mul;        /* frequency multiple x2 (2 = 1.0) */
    uint8_t  ar, dr, sl, rr;
} ag_fm_chan_t;

typedef struct ag_fm {
    ag_fm_chan_t ch[AG_FM_CHANNELS];
    uint32_t     clock;
    uint32_t     rate;
    uint8_t      patch[8];
    uint8_t      rhythm;
} ag_fm_t;

void ag_fm_init(ag_fm_t *fm, uint32_t clock, uint32_t sample_rate);
void ag_fm_reset(ag_fm_t *fm);
void ag_fm_set_patch(ag_fm_t *fm, const uint8_t patch[8]);
void ag_fm_set_rhythm(ag_fm_t *fm, uint8_t data);
void ag_fm_set_fnum(ag_fm_t *fm, int ch, uint16_t fnum, uint8_t block);
void ag_fm_set_key(ag_fm_t *fm, int ch, int on, int sus);
void ag_fm_set_inst_vol(ag_fm_t *fm, int ch, uint8_t inst, uint8_t vol);
void ag_fm_update(ag_fm_t *fm, int16_t *left, int16_t *right, int32_t samples);

#endif
