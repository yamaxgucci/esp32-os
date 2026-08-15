/*
 * N-operator FM with a free routing matrix (not DX7).
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_FMX_H
#define AG_FMX_H

#include <stdint.h>

#include "ag_dsp.h"

#define AG_FMX_MAX_OPS 8

typedef struct ag_fmx_op {
    uint32_t      phase;
    uint32_t      step;
    ag_dsp_adsr_t eg;
    uint8_t       ratio_x2; /* 2 = 1.0 */
    uint8_t       level;    /* 0..127 */
    int32_t       out;
} ag_fmx_op_t;

typedef struct ag_fmx {
    uint8_t    n_ops; /* 2..AG_FMX_MAX_OPS */
    uint8_t    fb;    /* 0..127 on op 0 */
    uint8_t    route[AG_FMX_MAX_OPS][AG_FMX_MAX_OPS]; /* src→dst 0..127 */
    uint8_t    carrier; /* bit i = op i is a carrier */
    ag_fmx_op_t op[AG_FMX_MAX_OPS];
    int32_t    fb_mem;
} ag_fmx_t;

void    ag_fmx_reset(ag_fmx_t *f);
void    ag_fmx_set_n(ag_fmx_t *f, int n);
void    ag_fmx_algo_stack(ag_fmx_t *f); /* op0→op1→… carriers last */
void    ag_fmx_set_hz(ag_fmx_t *f, int32_t hz_x100, uint32_t rate);
void    ag_fmx_note_on(ag_fmx_t *f);
void    ag_fmx_note_off(ag_fmx_t *f);
int32_t ag_fmx_tick(ag_fmx_t *f, uint8_t a, uint8_t d, uint8_t s, uint8_t r,
                    int gate);

#endif
