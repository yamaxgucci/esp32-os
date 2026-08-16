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

/*
 * Linear ADSR.  Segment lengths are milliseconds, not ticks, because the
 * same envelope is driven per sample by the synths and once per block by
 * ag_grain: a per-tick increment would make an attack 256 times shorter in
 * one caller than the other, and would drift with the block size.
 */
#define AG_DSP_ADSR_FULL (256 << 16) /* acc at level 256 */

typedef struct ag_dsp_adsr {
    uint8_t  stage; /* 0A 1D 2S 3R 4off */
    int32_t  level; /* 0..256 */
    int32_t  acc;   /* level << 16 */
    uint32_t rate;  /* ticks per second; 0 = 22050 */
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

/*
 * First inactive slot, else the oldest voice.  born[] is the note-on sequence
 * number the caller stamped (`++seq`), so the oldest voice is the *smallest*
 * one — reading it the other way round steals the note just played.
 */
int      ag_dsp_voice_steal(const uint32_t *born, const uint8_t *active, int n);

/*
 * Linear ADSR, level 0..256; a/d/s/r are 0..127.  A/D/R map to milliseconds
 * (attack 1..3000, decay 2..4000, release 3..5000), S is the level.  Tell the
 * envelope how often it will be ticked before using it, then tick it once per
 * sample, or once per block with n = frames in that block.  Returns 1 while
 * the voice is still sounding.
 */
void     ag_dsp_adsr_set_rate(ag_dsp_adsr_t *e, uint32_t ticks_per_sec);
void     ag_dsp_adsr_on(ag_dsp_adsr_t *e);
void     ag_dsp_adsr_off(ag_dsp_adsr_t *e);
int      ag_dsp_adsr_tick_n(ag_dsp_adsr_t *e, uint8_t a, uint8_t d, uint8_t s,
                            uint8_t r, int gate, uint32_t n);
int      ag_dsp_adsr_tick(ag_dsp_adsr_t *e, uint8_t a, uint8_t d, uint8_t s,
                          uint8_t r, int gate);
/* Segment length in ms for a 0..127 knob — the curve the envelope uses. */
int32_t  ag_dsp_adsr_seg_ms(uint8_t p, int32_t lo_ms, int32_t hi_ms);

int32_t  ag_dsp_lfo_wave(uint32_t phase, uint8_t wave); /* -32768..32767 */
void     ag_dsp_lfo_set_hz_x100(ag_dsp_lfo_t *l, int32_t hz_x100, uint32_t rate);
int32_t  ag_dsp_lfo_tick(ag_dsp_lfo_t *l);

uint32_t ag_dsp_rng(uint32_t *state);

#endif /* AG_DSP_H */
