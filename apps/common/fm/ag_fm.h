/*
 * ArgonOS lightweight integer FM synth (userspace).
 * SMS: OPLL-style presets + triangle waves.
 * Mega Drive: OPN 4-op + sin/TL tables + simplified EG (not a chip emu).
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_FM_H
#define AG_FM_H

#include <stdint.h>

#define AG_FM_CHANNELS 9
#define AG_FM_OPS 4

/* Phase-generator clock divides: OPLL uses 72, OPN/OPNA (YM2612) uses 144. */
#define AG_FM_CLKDIV_OPLL 72u
#define AG_FM_CLKDIV_OPN 144u

typedef struct ag_fm_op {
    uint32_t phase;
    uint32_t step;
    int16_t  eg;       /* 0..1023 attenuation (0 = loud) */
    uint8_t  eg_state; /* see ag_fm.c */
    uint8_t  key;
    uint16_t mul;      /* frequency multiple x2 (2 = 1.0) */
    uint8_t  tl;       /* 0..127 */
    uint8_t  ar;       /* 0..31 */
    uint8_t  dr;       /* 0..31 */
    uint8_t  sl;       /* 0..15 */
    uint8_t  rr;       /* 0..15 */
} ag_fm_op_t;

typedef struct ag_fm_chan {
    uint16_t fnum; /* up to 11 bits (OPN); OPLL uses 9 */
    uint8_t  block;
    uint8_t  key;  /* SMS / any-op keyed */
    uint8_t  sus;
    uint8_t  inst; /* 0..15; 0xFF = OPN raw 4-op */

    /* SMS / triangle path (ignored when inst == raw OPN) */
    uint32_t phase;
    uint32_t step;
    uint32_t mod_phase;
    uint32_t mod_step;
    int32_t  env;
    int32_t  env_target;
    int32_t  env_rate;
    uint8_t  vol;
    uint16_t mul;
    uint16_t mod_mul;
    uint8_t  fb;
    uint8_t  mod_tl;
    uint8_t  ar, dr, sl, rr;

    /* OPN 4-op (SLOT order: 0=M1, 1=M2, 2=C1, 3=C2) */
    ag_fm_op_t op[AG_FM_OPS];
    uint8_t    alg;
    int32_t    op1_out[2];
    int32_t    mem;
} ag_fm_chan_t;

typedef struct ag_fm {
    ag_fm_chan_t ch[AG_FM_CHANNELS];
    uint32_t     clock;
    uint32_t     rate;
    uint32_t     clk_div;
    uint8_t      patch[8];
    uint8_t      rhythm;
} ag_fm_t;

void ag_fm_init(ag_fm_t *fm, uint32_t clock, uint32_t sample_rate);
void ag_fm_set_clk_div(ag_fm_t *fm, uint32_t div);
void ag_fm_reset(ag_fm_t *fm);
void ag_fm_set_patch(ag_fm_t *fm, const uint8_t patch[8]);
void ag_fm_set_rhythm(ag_fm_t *fm, uint8_t data);
void ag_fm_set_fnum(ag_fm_t *fm, int ch, uint16_t fnum, uint8_t block);
void ag_fm_set_key(ag_fm_t *fm, int ch, int on, int sus);
void ag_fm_set_inst_vol(ag_fm_t *fm, int ch, uint8_t inst, uint8_t vol);

/* OPN front-end (Mega Drive). */
void ag_fm_set_opn_op(ag_fm_t *fm, int ch, int slot, uint16_t mul_x2, uint8_t tl,
                      uint8_t ar, uint8_t dr, uint8_t sl, uint8_t rr);
void ag_fm_set_alg(ag_fm_t *fm, int ch, uint8_t alg, uint8_t fb);
/* bits: 0=SLOT1/M1, 1=SLOT2/C1, 2=SLOT3/M2, 3=SLOT4/C2 (YM2612 $28 order). */
void ag_fm_set_opn_key(ag_fm_t *fm, int ch, uint8_t op_mask);

void ag_fm_update(ag_fm_t *fm, int16_t *left, int16_t *right, int32_t samples);

#endif
