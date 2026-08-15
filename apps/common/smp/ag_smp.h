/*
 * ag_smp — pitched sample / ROM-player voice (fixed-point).
 * One zone: buffer + root note + optional loop. Not granular (see ag_grain).
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_SMP_H
#define AG_SMP_H

#include <stdint.h>

#include "ag_dsp.h"

#define AG_SMP_VOICES 8

enum {
    AG_SMP_ORGAN = 0,
    AG_SMP_PIANO,
    AG_SMP_BASS,
    AG_SMP_NPRESETS
};

typedef struct ag_smp_zone {
    const int16_t *data;
    uint32_t       frames;
    uint32_t       rate;       /* of data[] */
    uint32_t       loop_start; /* samples; ignored if loop_end <= loop_start */
    uint32_t       loop_end;
    uint8_t        root;       /* MIDI note of the recording */
} ag_smp_zone_t;

typedef struct ag_smp_voice {
    uint8_t       note;
    uint8_t       vel;
    uint8_t       gate;
    uint8_t       active;
    uint32_t      age;
    uint32_t      pos; /* Q16 index into zone */
    uint32_t      step;
    ag_dsp_adsr_t amp;
} ag_smp_voice_t;

typedef struct ag_smp {
    uint32_t       rate;
    ag_smp_zone_t  zone;
    ag_smp_voice_t voice[AG_SMP_VOICES];
    uint8_t        attack, decay, sustain, release;
    uint8_t        level; /* 0..127 */
    uint32_t       age_seq;
} ag_smp_t;

void ag_smp_init(ag_smp_t *s, uint32_t out_rate);
void ag_smp_set_zone(ag_smp_t *s, const ag_smp_zone_t *z);
void ag_smp_set_adsr(ag_smp_t *s, uint8_t a, uint8_t d, uint8_t sus, uint8_t r);

/* Fill caller buffer with a synthetic ROM. Returns 0 and writes zone. */
uint32_t ag_smp_preset_frames(int preset, uint32_t rate);
int      ag_smp_fill_preset(int preset, int16_t *dst, uint32_t cap, uint32_t rate,
                            ag_smp_zone_t *out);

void ag_smp_note_on(ag_smp_t *s, uint8_t note, uint8_t vel);
void ag_smp_note_off(ag_smp_t *s, uint8_t note);
void ag_smp_all_notes_off(ag_smp_t *s);
void ag_smp_render(ag_smp_t *s, int16_t *stereo, int32_t frames);

#endif
