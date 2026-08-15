/*
 * ag_dsp — shared fixed-point primitives for ArgonOS audio engines.
 * No libm, no device I/O. SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_DSP_H
#define AG_DSP_H

#include <stdint.h>

#define AG_DSP_SIN_BITS    10
#define AG_DSP_SIN_LEN     (1 << AG_DSP_SIN_BITS)
#define AG_DSP_SIN_MASK    (AG_DSP_SIN_LEN - 1)
#define AG_DSP_PHASE_SHIFT (32 - AG_DSP_SIN_BITS)

#define AG_DSP_LFO_TRI   0
#define AG_DSP_LFO_SAWDN 1
#define AG_DSP_LFO_SAWUP 2
#define AG_DSP_LFO_SQR   3
#define AG_DSP_LFO_SIN   4

typedef struct ag_dsp_delay {
    int16_t *buf;
    uint32_t cap;
    uint32_t w;
} ag_dsp_delay_t;

typedef struct ag_dsp_adsr {
    uint8_t stage; /* 0A 1D 2S 3R 4off */
    int32_t level; /* 0..256 */
} ag_dsp_adsr_t;

typedef struct ag_dsp_lfo {
    uint32_t phase;
    uint32_t step;
    uint8_t  wave;
} ag_dsp_lfo_t;

static inline int16_t ag_sat16(int32_t x)
{
    if (x > 32767) {
        return 32767;
    }
    if (x < -32768) {
        return (int16_t)-32768;
    }
    return (int16_t)x;
}

static inline int ag_clampi(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static inline int32_t ag_dsp_mul_q15(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * (int64_t)b) >> 15);
}

/* mix 0..127 → dry*(127-mix)+wet*mix >> 7 */
static inline int32_t ag_dsp_mix127(int32_t dry, int32_t wet, unsigned mix)
{
    unsigned m = mix > 127u ? 127u : mix;
    return (dry * (int32_t)(127u - m) + wet * (int32_t)m) >> 7;
}

void     ag_dsp_zero(void *p, unsigned n);
void     ag_dsp_copy(void *dst, const void *src, unsigned n);

int16_t  ag_dsp_sin(uint32_t phase);
int32_t  ag_dsp_note_hz_x100(int note);
uint32_t ag_dsp_hz_to_step(int32_t hz_x100, uint32_t rate);

void     ag_dsp_delay_reset(ag_dsp_delay_t *d, int16_t *buf, uint32_t cap);
void     ag_dsp_delay_write(ag_dsp_delay_t *d, int16_t s);
int16_t  ag_dsp_delay_tap(const ag_dsp_delay_t *d, uint32_t delay);

/* First inactive slot, else oldest (largest age). active[i] != 0 if live. */
int      ag_dsp_voice_steal(const uint32_t *ages, const uint8_t *active, int n);

/* Linear ADSR 0..256; a/d/s/r are 0..127 like grain. Returns 1 if sounding. */
void     ag_dsp_adsr_on(ag_dsp_adsr_t *e);
void     ag_dsp_adsr_off(ag_dsp_adsr_t *e);
int      ag_dsp_adsr_tick(ag_dsp_adsr_t *e, uint8_t a, uint8_t d, uint8_t s,
                          uint8_t r, int gate);

int32_t  ag_dsp_lfo_wave(uint32_t phase, uint8_t wave); /* -32768..32767 */
void     ag_dsp_lfo_set_hz_x100(ag_dsp_lfo_t *l, int32_t hz_x100, uint32_t rate);
int32_t  ag_dsp_lfo_tick(ag_dsp_lfo_t *l);

uint32_t ag_dsp_rng(uint32_t *state);

#endif /* AG_DSP_H */
