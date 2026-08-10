/*
 * ag_fx — lite fixed-point master-bus FX (delay → chorus → reverb).
 * No libm. Tuned for ~22 kHz stereo s16 chunks on ESP32.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_FX_H
#define AG_FX_H

#include <stdint.h>

#define AG_FX_DELAY  (1u << 0)
#define AG_FX_CHORUS (1u << 1)
#define AG_FX_REVERB (1u << 2)
#define AG_FX_ALL    (AG_FX_DELAY | AG_FX_CHORUS | AG_FX_REVERB)

typedef struct ag_fx ag_fx_t;

/* Opaque size for stack/static embedding; buffers live in heap after init. */
struct ag_fx {
    uint32_t rate;
    uint32_t enable;     /* AG_FX_* bits */
    uint8_t  ready;

    /* Delay: ms, feedback 0..127, mix 0..127 */
    uint16_t delay_ms;
    uint8_t  delay_fb;
    uint8_t  delay_mix;

    /* Chorus: LFO rate/depth 0..127, mix 0..127 */
    uint8_t  chorus_rate;
    uint8_t  chorus_depth;
    uint8_t  chorus_mix;

    /* Reverb: room/damp/wet 0..127 */
    uint8_t  rev_room;
    uint8_t  rev_damp;
    uint8_t  rev_wet;

    /* Master wet after chain (0 = dry passthrough of chain output blend) */
    uint8_t  master_wet;

    /* Internal (set by ag_fx_init) */
    int16_t *delay_buf;
    uint32_t delay_cap; /* samples */
    uint32_t delay_w;

    int16_t *chorus_buf;
    uint32_t chorus_cap;
    uint32_t chorus_w;
    uint32_t chorus_phase; /* 0..65535 triangle */

    int16_t *comb_buf[8]; /* 4 L + 4 R */
    uint16_t comb_len[4];
    uint16_t comb_w[4];
    int32_t  comb_filter[8]; /* one-pole damp state */

    int16_t *ap_buf[4]; /* 2 L + 2 R */
    uint16_t ap_len[2];
    uint16_t ap_w[2];
};

/* Allocate delay/chorus/reverb buffers (~25 KB @ 22 kHz). Returns 0 ok. */
int ag_fx_init(ag_fx_t *fx, uint32_t rate);

void ag_fx_reset(ag_fx_t *fx);
void ag_fx_free(ag_fx_t *fx);

void ag_fx_set_enable(ag_fx_t *fx, unsigned mask);
void ag_fx_set_defaults(ag_fx_t *fx);

/* delay_ms 1..370; fb/mix/rate/depth/room/damp/wet/master 0..127 */
void ag_fx_set_params(ag_fx_t *fx, uint16_t delay_ms, uint8_t delay_fb,
                      uint8_t delay_mix, uint8_t chorus_rate,
                      uint8_t chorus_depth, uint8_t chorus_mix,
                      uint8_t rev_room, uint8_t rev_damp, uint8_t rev_wet,
                      uint8_t master_wet);

/* In-place stereo interleaved s16. */
void ag_fx_process(ag_fx_t *fx, int16_t *stereo_io, int32_t frames);

#endif /* AG_FX_H */
