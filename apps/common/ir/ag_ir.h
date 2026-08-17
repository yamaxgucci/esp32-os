/*
 * Uniform partitioned convolution (overlap-add FFT) for IR FX.
 * Target: up to 1 s mono IR @ 22 kHz on ESP32-S3. No libm.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_IR_H
#define AG_IR_H

#include <stdint.h>

#define AG_IR_BLOCK  256u
#define AG_IR_FFT    512u

/*
 * The longest impulse response that will be loaded; anything longer is
 * truncated.  A cap on memory, not on arithmetic: the two spectra are two
 * kilobytes per 256 taps, so a second at 22.05 kHz is 87 partitions and
 * 348 KB, and they live in PSRAM.  Per-sample cost grows with it too, but
 * gently - one complex multiply-accumulate per bin per partition, 24
 * instructions per partition per sample.
 */
#define AG_IR_MAX_MS 1000u

/*
 * The synthetic reverbs are 500 ms whatever the cap is.  Tied to the cap they
 * would change character every time it moved, and "hall" is a description of a
 * sound rather than of how much memory happened to be allowed.
 */
#define AG_IR_PRESET_MS 500u

/* Marks a slot of the input spectrum delay line that has never been written. */
#define AG_IR_EMPTY (-127)

/*
 * Fully wet.  128 rather than 127 because the mix divides by 128: stopping a
 * notch short leaves 1/128 of the dry signal - inaudible behind a reverb, and
 * the end of a cabinet, which exists to remove the top end that this leak
 * puts straight back.
 */
#define AG_IR_WET_MAX 128u

/*
 * The mix a reverb wants, and the only thing the leak above is harmless
 * behind.  Not a general default: everything that loads an impulse picks its
 * own wet from what the impulse is (ag_ir_load_preset, ag_ir_load), because
 * one number cannot be right for both a tail you sit behind the dry signal
 * and a cabinet that exists to remove part of it.
 */
#define AG_IR_WET_REVERB 100u

typedef struct ag_ir {
    uint32_t rate;
    uint32_t ir_frames;
    uint32_t parts;
    uint8_t  ready;
    uint8_t  bypass;
    uint8_t  wet;  /* 0..AG_IR_WET_MAX; AG_IR_WET_MAX is fully wet */
    uint8_t  gain; /* 0..127; 64 ≈ unity after IR normalize */

    int16_t *H; /* parts * FFT * 2 (re,im), scaled down by 2^h_shift */
    int16_t *X; /* input spectrum delay line */
    /*
     * Block floating point: how far each stored input block had to come down
     * to fit int16, one entry per slot of X.  A fixed shift has to assume a
     * full-scale block, so a quiet one keeps only the top few bits of its
     * spectrum and the convolution hisses at a level that does not follow the
     * signal - loud enough to hear the moment a guitar note decays.
     */
    int8_t  *x_sh;
    uint32_t x_pos;
    /*
     * How far H had to be shifted right to fit int16, picked from the IR that
     * was actually loaded and undone again on the output.  A fixed shift
     * cannot work for both a 20 ms cabinet and a 500 ms tail: one overflows
     * the bins, the other leaves them near zero and the convolution turns
     * into rounding noise.
     */
    uint8_t  h_shift;
    /*
     * How far one partition's product is shifted down as it goes into the
     * sum.  Set from the partition count, not fixed at 15: a Q15 multiply
     * leaves the accumulator running at 2^15 when it could run at 2^30, and
     * every bit left unused there comes back as a step in the output.
     */
    uint8_t  p_shift;
    /*
     * How far the impulse response was lifted before its own transform, so
     * the output shift can put it back.  Picked from the taps, since a 20 ms
     * cabinet and a 500 ms tail have nothing like the same room above them.
     */
    uint8_t  h_pre;

    /*
     * int32, not int16: this is the second half of the overlap-add, and a
     * cabinet's low end puts more into the tail than into the block that
     * carried it.  Saturating it clipped the part of the output nothing else
     * clipped.
     */
    int32_t overlap[AG_IR_BLOCK];
} ag_ir_t;

int ag_ir_init(ag_ir_t *ir, uint32_t rate);
void ag_ir_free(ag_ir_t *ir);
void ag_ir_reset(ag_ir_t *ir);

/*
 * Mono IR; resampled to ir->rate, truncated to AG_IR_MAX_MS.
 * Sets wet to AG_IR_WET_MAX on success: an impulse response that came from a
 * file is far more often a cabinet than a room, and a cabinet is the case a
 * dry leak ruins.  Call ag_ir_set_wet afterwards for a mix.
 */
int ag_ir_load(ag_ir_t *ir, const int16_t *mono, uint32_t frames, uint32_t src_rate);

/*
 * 0=room, 1=hall (~500 ms), 2=spring, 3=cab dark, 4=cab bright.
 * Sets wet on success: AG_IR_WET_REVERB for 0-2, AG_IR_WET_MAX for 3-4.  Both
 * ways round, so switching a cabinet back to a hall gets the hall's mix back.
 * Call ag_ir_set_wet afterwards to override.
 *
 * 3 and 4 are not measured cabinets - one large first tap and 20 ms of
 * low-passed noise, flat within 2 dB to 10 kHz.  Judge a distortion model
 * through assets/audio/guitar-di, not through these.
 */
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
