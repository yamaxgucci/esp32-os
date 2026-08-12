/*
 * Uniform partitioned convolution (overlap-add FFT) for IR FX.
 * Target: up to ~500 ms mono IR @ 22 kHz on ESP32-S3. No libm.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_IR_H
#define AG_IR_H

#include <stdint.h>

#define AG_IR_BLOCK  256u
#define AG_IR_FFT    512u
#define AG_IR_MAX_MS 500u

typedef struct ag_ir {
    uint32_t rate;
    uint32_t ir_frames;
    uint32_t parts;
    uint8_t  ready;
    uint8_t  bypass;
    uint8_t  wet;  /* 0..127 */
    uint8_t  gain; /* 0..127; 64 ≈ unity after IR normalize */

    int16_t *H; /* parts * FFT * 2 (re,im) Q15-scaled */
    int16_t *X; /* input spectrum delay line */
    uint32_t x_pos;

    int16_t overlap[AG_IR_BLOCK];
} ag_ir_t;

int ag_ir_init(ag_ir_t *ir, uint32_t rate);
void ag_ir_free(ag_ir_t *ir);
void ag_ir_reset(ag_ir_t *ir);

/* Mono IR; resampled to ir->rate, truncated to AG_IR_MAX_MS. */
int ag_ir_load(ag_ir_t *ir, const int16_t *mono, uint32_t frames, uint32_t src_rate);

/* 0=room, 1=hall (~500 ms), 2=spring. */
int ag_ir_load_preset(ag_ir_t *ir, int preset);

void ag_ir_set_wet(ag_ir_t *ir, uint8_t wet);
void ag_ir_set_gain(ag_ir_t *ir, uint8_t gain);
void ag_ir_set_bypass(ag_ir_t *ir, int on);

/*
 * One block: mono_in[AG_IR_BLOCK] → stereo interleaved out[AG_IR_BLOCK*2].
 * Wet/dry mix applied; bypass copies dry to L/R.
 */
void ag_ir_process_block(ag_ir_t *ir, const int16_t *mono_in, int16_t *stereo_out);

#endif /* AG_IR_H */
