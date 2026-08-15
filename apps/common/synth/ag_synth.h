/*
 * ag_synth — VA / N-op FM voice + modulation matrix.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_SYNTH_H
#define AG_SYNTH_H

#include <stdint.h>

#include "ag_dist.h"
#include "ag_dsp.h"
#include "ag_filt.h"
#include "ag_fmx.h"
#include "ag_osc.h"

#define AG_SYNTH_VOICES    8
#define AG_SYNTH_MOD_SLOTS 8
#define AG_SYNTH_CTRL      16 /* samples per control tick */

enum {
    AG_SYNTH_VA = 0,
    AG_SYNTH_FM = 1
};

enum {
    AG_SYNTH_P_CUTOFF = 0,
    AG_SYNTH_P_RESO,
    AG_SYNTH_P_OSC_MIX,
    AG_SYNTH_P_PWM,
    AG_SYNTH_P_DRIVE,
    AG_SYNTH_P_BIAS,
    AG_SYNTH_P_SAG,
    AG_SYNTH_P_WAVE1,
    AG_SYNTH_P_WAVE2,
    AG_SYNTH_P_NOISE,
    AG_SYNTH_P_AMP_A,
    AG_SYNTH_P_AMP_D,
    AG_SYNTH_P_AMP_S,
    AG_SYNTH_P_AMP_R,
    AG_SYNTH_P_FILT_A,
    AG_SYNTH_P_FILT_D,
    AG_SYNTH_P_FILT_S,
    AG_SYNTH_P_FILT_R,
    AG_SYNTH_P_FILT_AMT,
    AG_SYNTH_P_LFO_RATE,
    AG_SYNTH_P_LFO_WAVE,
    AG_SYNTH_P_FM_INDEX,
    AG_SYNTH_P_FM_OPS,
    AG_SYNTH_P_ENGINE,
    AG_SYNTH_P_DIST_MODEL,
    AG_SYNTH_P_N
};

enum {
    AG_SYNTH_SRC_LFO1 = 0,
    AG_SYNTH_SRC_LFO2,
    AG_SYNTH_SRC_FEG,
    AG_SYNTH_SRC_AEG,
    AG_SYNTH_SRC_VEL,
    AG_SYNTH_SRC_NOTE,
    AG_SYNTH_SRC_MW,
    AG_SYNTH_SRC_AT,
    AG_SYNTH_SRC_N
};

typedef struct ag_synth_mod {
    uint8_t  src;
    uint16_t dest;
    int16_t  depth; /* bipolar, 256 ≈ ±full */
    uint8_t  used;
} ag_synth_mod_t;

typedef struct ag_synth_voice {
    uint8_t       note;
    uint8_t       vel;
    uint8_t       gate;
    uint8_t       active;
    uint32_t      age;
    ag_osc_t      osc1;
    ag_osc_t      osc2;
    ag_osc_t      noise;
    ag_filt_t     filt;
    ag_dist_t     dist;
    ag_fmx_t      fmx;
    ag_dsp_adsr_t amp;
    ag_dsp_adsr_t feg;
} ag_synth_voice_t;

typedef struct ag_synth {
    uint32_t         rate;
    int32_t          base[AG_SYNTH_P_N];
    ag_synth_mod_t   mod[AG_SYNTH_MOD_SLOTS];
    ag_synth_voice_t voice[AG_SYNTH_VOICES];
    ag_dsp_lfo_t     lfo1;
    ag_dsp_lfo_t     lfo2;
    uint8_t          mw;
    uint8_t          at;
    uint32_t         age_seq;
    uint32_t         ctrl_left;
} ag_synth_t;

void ag_synth_init(ag_synth_t *s, uint32_t rate);
void ag_synth_set(ag_synth_t *s, uint16_t param, int32_t value);
int32_t ag_synth_get(const ag_synth_t *s, uint16_t param);
int  ag_synth_mod_bind(ag_synth_t *s, uint8_t src, uint16_t dest, int16_t depth);
void ag_synth_mod_clear(ag_synth_t *s);

void ag_synth_note_on(ag_synth_t *s, uint8_t note, uint8_t vel);
void ag_synth_note_off(ag_synth_t *s, uint8_t note);
void ag_synth_all_notes_off(ag_synth_t *s);
void ag_synth_set_mw(ag_synth_t *s, uint8_t v);
void ag_synth_set_at(ag_synth_t *s, uint8_t v);

void ag_synth_render(ag_synth_t *s, int16_t *stereo, int32_t frames);

#endif
