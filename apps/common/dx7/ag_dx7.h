/*
 * ag_dx7 — structural DX7-like 6-op FM (userspace, fixed-point).
 * Topology matches the instrument (6 ops, 32 algs, EG, feedback, LFO).
 * Accuracy work is incremental toward bit-exact / SysEx; not ROM-complete yet.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_DX7_H
#define AG_DX7_H

#include <stdint.h>

#define AG_DX7_OPS 6
#define AG_DX7_ALGS 32
#define AG_DX7_VOICES 8
#define AG_DX7_NAME_LEN 11

/* Per-operator patch (DX7-shaped fields; ranges match the instrument). */
typedef struct ag_dx7_op_patch {
    uint8_t rate[4];   /* R1..R4, 0..99 */
    uint8_t level[4];  /* L1..L4, 0..99 */
    uint8_t kbd_break; /* 0..99 */
    uint8_t kbd_ldepth;
    uint8_t kbd_rdepth;
    uint8_t kbd_lcurve; /* 0..3 : -lin -exp +exp +lin */
    uint8_t kbd_rcurve;
    uint8_t kbd_rate_scale; /* 0..7 */
    uint8_t amp_mod_sens;   /* 0..3 */
    uint8_t vel_sens;       /* 0..7 */
    uint8_t out_level;      /* output level 0..99 */
    uint8_t mode;           /* 0=ratio, 1=fixed */
    uint8_t coarse;         /* 0..31 */
    uint8_t fine;           /* 0..99 */
    uint8_t detune;         /* 0..14 (7 = centre) */
} ag_dx7_op_patch_t;

typedef struct ag_dx7_patch {
    ag_dx7_op_patch_t op[AG_DX7_OPS]; /* OP1..OP6 */
    uint8_t pitch_rate[4];
    uint8_t pitch_level[4];
    uint8_t algorithm; /* 0..31 */
    uint8_t feedback;  /* 0..7 */
    uint8_t osc_sync;
    uint8_t lfo_speed; /* 0..99 */
    uint8_t lfo_delay;
    uint8_t lfo_pmd;
    uint8_t lfo_amd;
    uint8_t lfo_sync;
    uint8_t lfo_wave; /* 0 triangle, 1 saw down, 2 saw up, 3 square, 4 sine */
    uint8_t lfo_pms;  /* 0..7 */
    uint8_t transpose; /* 0..48, 24 = C3 (SysEx field; keyboard may ignore) */
    char    name[AG_DX7_NAME_LEN];
} ag_dx7_patch_t;

/* Live performance state (not part of patch dump). */
typedef struct ag_dx7_perf {
    uint8_t op_enable;     /* bits 0..5 = OP1..OP6, 1 = on (default 0x3f) */
    uint8_t carrier_mute;  /* bits 0..5: mute that op when it is a carrier */
    uint8_t mod_wheel;     /* 0..127 → scales LFO pitch mod */
    uint8_t aftertouch;    /* 0..127 → light amp / LFO boost */
    uint8_t breath;        /* 0..127 reserved CC2 */
    uint8_t foot;          /* 0..127 reserved CC4 */
    uint8_t porta_on;      /* 0/1 */
    uint8_t porta_time;    /* 0..99 (0 = off/instant) */
    uint8_t unison;        /* 1..4 stacked voices (1 = off) */
    uint8_t unison_detune; /* 0..99 */
    uint8_t audition;      /* 1 = stable preview: faster attack, softer kbd scl */
} ag_dx7_perf_t;

typedef struct ag_dx7_op {
    uint32_t phase;
    uint32_t step;
    int32_t  eg_level; /* 0..99*256 */
    int32_t  eg_target;
    int32_t  eg_rate;
    uint8_t  eg_stage; /* 0..3 active, 4 off */
    int32_t  out;
    int32_t  level_scl;
} ag_dx7_op_t;

typedef struct ag_dx7_voice {
    ag_dx7_op_t op[AG_DX7_OPS];
    uint8_t     note;
    uint8_t     vel;
    uint8_t     gate;
    uint8_t     active; /* sounding (gate or release tail) */
    int32_t     fb_mem[2];
    int32_t     pitch_eg;
    int32_t     pitch_target;
    int32_t     pitch_rate;
    uint8_t     pitch_stage;
    int32_t     base_hz_x100;   /* current (may glide) */
    int32_t     target_hz_x100; /* porta destination */
    int32_t     out_gain;       /* 0..256 note smoother */
    uint32_t    age;
} ag_dx7_voice_t;

typedef struct ag_dx7 {
    ag_dx7_patch_t patch;
    ag_dx7_perf_t  perf;
    ag_dx7_voice_t voice[AG_DX7_VOICES];
    uint32_t       rate;
    uint32_t       lfo_phase;
    uint32_t       lfo_step;
    uint32_t       lfo_delay_left;
    uint32_t       age_seq;
} ag_dx7_t;

void ag_dx7_init(ag_dx7_t *dx, uint32_t sample_rate);
void ag_dx7_reset(ag_dx7_t *dx);
void ag_dx7_load_patch(ag_dx7_t *dx, const ag_dx7_patch_t *p);
void ag_dx7_set_algorithm(ag_dx7_t *dx, uint8_t alg);
void ag_dx7_set_feedback(ag_dx7_t *dx, uint8_t fb);
void ag_dx7_set_op_level(ag_dx7_t *dx, int op, uint8_t level);

void ag_dx7_set_op_enable(ag_dx7_t *dx, int op, int on);
void ag_dx7_toggle_op(ag_dx7_t *dx, int op);
void ag_dx7_mute_carriers(ag_dx7_t *dx, int mute); /* 1=mute all carriers */
void ag_dx7_set_mod_wheel(ag_dx7_t *dx, uint8_t v);
void ag_dx7_set_aftertouch(ag_dx7_t *dx, uint8_t v);
void ag_dx7_set_porta(ag_dx7_t *dx, int on, uint8_t time);
void ag_dx7_set_unison(ag_dx7_t *dx, uint8_t voices, uint8_t detune);
void ag_dx7_set_audition(ag_dx7_t *dx, int on);

void ag_dx7_note_on(ag_dx7_t *dx, uint8_t note, uint8_t vel);
void ag_dx7_note_off(ag_dx7_t *dx, uint8_t note);
void ag_dx7_note_off_all(ag_dx7_t *dx);
int  ag_dx7_active_voices(const ag_dx7_t *dx);

#define AG_DX7_SYX_VOICE 155
#define AG_DX7_PACKED_VOICE 128
#define AG_DX7_BANK_VOICES 32

/* 128-byte packed cart voice → 155-byte edit buffer (Dexed-compatible). */
void ag_dx7_unpack_packed(const uint8_t packed[AG_DX7_PACKED_VOICE],
                          uint8_t unpack[AG_DX7_SYX_VOICE]);
/* 155-byte edit buffer → patch struct. */
int ag_dx7_patch_from_sysex(ag_dx7_patch_t *out, const uint8_t *v155);
/* Load 155-byte voice body into engine. */
int ag_dx7_load_sysex_voice(ag_dx7_t *dx, const uint8_t *data, int len);
/* Load packed 128-byte cart voice. */
int ag_dx7_load_packed_voice(ag_dx7_t *dx, const uint8_t *packed128);

/*
 * Parse a .syx blob (with or without F0..F7).
 * Fills up to AG_DX7_BANK_VOICES packed slots; returns voice count or <0.
 */
int ag_dx7_syx_parse_bank(const uint8_t *data, int len,
                          uint8_t packed[][AG_DX7_PACKED_VOICE], int max_voices);

/* Interleaved stereo s16 (L=R). */
void ag_dx7_render(ag_dx7_t *dx, int16_t *pcm, int32_t frames);

/* Built-in presets: 0 INIT, 1 E.PIANO, 2 BRASS, 3 BELL, 4 PAD, 5 BASSOON */
#define AG_DX7_NPRESETS 6
const ag_dx7_patch_t *ag_dx7_preset(int index);

#endif
