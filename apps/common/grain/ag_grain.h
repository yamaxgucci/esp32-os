/*
 * ag_grain — fixed-point granular synth engine (userspace).
 * Clouds-shaped params: position, size, density, texture, freeze.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_GRAIN_H
#define AG_GRAIN_H

#include <stdint.h>

#define AG_GRAIN_VOICES 8
#define AG_GRAIN_POOL   48
#define AG_GRAIN_VIZ    48

typedef struct ag_grain_params {
    uint8_t position; /* 0..127 playhead in buffer */
    uint8_t size;     /* 0..127 grain duration */
    uint8_t density;  /* 0..127 spawn rate */
    uint8_t spray;    /* 0..127 position random */
    int8_t  pitch;    /* -48..+48 semitone offset vs note */
    uint8_t pitch_spr;/* 0..127 pitch random (cents-ish) */
    uint8_t texture;  /* 0..127 box → triangle → Hann */
    uint8_t pan_spr;  /* 0..127 stereo spray */
    uint8_t reverse;  /* 0..127 reverse chance */
    uint8_t level;    /* 0..127 master */
    uint8_t attack;   /* ADSR 0..127 */
    uint8_t decay;
    uint8_t sustain;
    uint8_t release;
} ag_grain_params_t;

typedef struct ag_grain_buf {
    int16_t *data;     /* mono s16, owned by caller or heap */
    uint32_t frames;
    uint32_t capacity;
    uint32_t rate;     /* sample rate of data[] */
    uint8_t  freeze;   /* 1 = ignore appends (Clouds freeze) */
    uint8_t  owned;    /* 1 = free data on clear/replace */
} ag_grain_buf_t;

typedef struct ag_grain_viz {
    uint16_t x_q15; /* position in buffer 0..32767 */
    uint8_t  y;     /* 0..255 amp/pan hint */
    uint8_t  a;     /* 0..255 envelope alpha */
} ag_grain_viz_t;

typedef struct ag_grain_grain {
    uint8_t  active;
    uint8_t  voice;
    uint8_t  reverse;
    uint8_t  pan_l; /* 0..128 */
    uint8_t  pan_r;
    uint32_t pos;   /* Q16 sample index in buffer */
    uint32_t step;  /* Q16 increment per output sample */
    uint32_t len;   /* grain length in output samples */
    uint32_t i;     /* progress */
    int32_t  amp;   /* 0..256 */
} ag_grain_grain_t;

typedef struct ag_grain_voice {
    uint8_t  note;
    uint8_t  vel;
    uint8_t  gate;
    uint8_t  active;
    uint8_t  eg_stage; /* 0A 1D 2S 3R 4off */
    int32_t  eg;       /* 0..256 */
    uint32_t spawn_left;
    uint32_t age;
} ag_grain_voice_t;

typedef struct ag_grain {
    ag_grain_params_t params;
    ag_grain_buf_t    buf;
    ag_grain_voice_t  voice[AG_GRAIN_VOICES];
    ag_grain_grain_t  grain[AG_GRAIN_POOL];
    ag_grain_viz_t    viz[AG_GRAIN_VIZ];
    uint8_t           viz_n;
    uint32_t          rate; /* output sample rate */
    uint32_t          rng;
    uint32_t          age_seq;
    uint32_t          active_grains;
} ag_grain_t;

void ag_grain_init(ag_grain_t *g, uint32_t out_rate);
void ag_grain_reset(ag_grain_t *g);

/* Defaults tuned for pads / textures. */
void ag_grain_set_defaults(ag_grain_t *g);

void ag_grain_note_on(ag_grain_t *g, uint8_t note, uint8_t vel);
void ag_grain_note_off(ag_grain_t *g, uint8_t note);
void ag_grain_all_notes_off(ag_grain_t *g);

/* Buffer: take ownership of data if owned=1 (ag_malloc). */
void ag_grain_buf_clear(ag_grain_t *g);
int  ag_grain_buf_set(ag_grain_t *g, int16_t *data, uint32_t frames,
                      uint32_t rate, int owned);
/* Append mono s16 for future mic capture; no-op when freeze. Returns frames written. */
uint32_t ag_grain_buf_append(ag_grain_t *g, const int16_t *src, uint32_t frames);
void     ag_grain_freeze(ag_grain_t *g, int on);

/* Stereo interleaved s16 out. */
void ag_grain_render(ag_grain_t *g, int16_t *stereo, int32_t frames);

/* Snapshot active grains for UI (call after render). */
void ag_grain_viz_update(ag_grain_t *g);

#endif /* AG_GRAIN_H */
