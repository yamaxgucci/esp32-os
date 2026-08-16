/*
 * ag_synth — VA / FM poly + control-rate modulation matrix.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_synth.h"

static const int32_t k_pmin[AG_SYNTH_P_N] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0,
    1, 0
};
static const int32_t k_pmax[AG_SYNTH_P_N] = {
    127, 127, 127, 127, 127, 127, 127, 5, 5, 127, 127, 127, 127, 127,
    127, 127, 127, 127, 127, 127, 4, 127, 8, 1, 4, 127, 127
};

static int32_t clamp_param(uint16_t id, int32_t v)
{
    if (id >= AG_SYNTH_P_N) {
        return 0;
    }
    return (int32_t)ag_clampi((int)v, (int)k_pmin[id], (int)k_pmax[id]);
}

void ag_synth_init(ag_synth_t *s, uint32_t rate)
{
    int i;
    if (s == 0) {
        return;
    }
    ag_dsp_zero(s, sizeof(*s));
    s->rate = rate ? rate : 22050u;
    s->base[AG_SYNTH_P_CUTOFF] = 80;
    s->base[AG_SYNTH_P_RESO] = 40;
    s->base[AG_SYNTH_P_OSC_MIX] = 64;
    s->base[AG_SYNTH_P_PWM] = 64;
    s->base[AG_SYNTH_P_DRIVE] = 0;
    s->base[AG_SYNTH_P_BIAS] = 64;
    s->base[AG_SYNTH_P_WAVE1] = AG_OSC_SAW;
    s->base[AG_SYNTH_P_WAVE2] = AG_OSC_SQR;
    s->base[AG_SYNTH_P_AMP_A] = 10;
    s->base[AG_SYNTH_P_AMP_D] = 30;
    s->base[AG_SYNTH_P_AMP_S] = 100;
    s->base[AG_SYNTH_P_AMP_R] = 40;
    s->base[AG_SYNTH_P_FILT_A] = 5;
    s->base[AG_SYNTH_P_FILT_D] = 50;
    s->base[AG_SYNTH_P_FILT_S] = 40;
    s->base[AG_SYNTH_P_FILT_R] = 40;
    s->base[AG_SYNTH_P_FILT_AMT] = 60;
    s->base[AG_SYNTH_P_LFO_RATE] = 40;
    s->base[AG_SYNTH_P_FM_INDEX] = 0; /* VA: no PM until set; FM path fills this */
    s->base[AG_SYNTH_P_FM_OPS] = 4;
    s->base[AG_SYNTH_P_ENGINE] = AG_SYNTH_VA;
    s->base[AG_SYNTH_P_OSC2_TUNE] = 64;
    s->base[AG_SYNTH_P_FM_INDEX2] = 70;
    s->lfo1.wave = AG_DSP_LFO_TRI;
    s->lfo2.wave = AG_DSP_LFO_SIN;
    ag_dsp_lfo_set_hz_x100(&s->lfo1, 40 * 8, s->rate); /* ~3.2 Hz */
    ag_dsp_lfo_set_hz_x100(&s->lfo2, 20 * 8, s->rate);
    s->ctrl_left = 0;
    for (i = 0; i < AG_SYNTH_VOICES; i++) {
        ag_fmx_reset(&s->voice[i].fmx);
        ag_fmx_set_n(&s->voice[i].fmx, 4);
        ag_fmx_algo_stack(&s->voice[i].fmx);
        s->voice[i].fmx.op[0].ratio_x2 = 2;
        s->voice[i].fmx.op[1].ratio_x2 = 2;
        s->voice[i].fmx.op[2].ratio_x2 = 4;
        s->voice[i].fmx.op[3].ratio_x2 = 2;
    }
}

void ag_synth_set(ag_synth_t *s, uint16_t param, int32_t value)
{
    if (s == 0 || param >= AG_SYNTH_P_N) {
        return;
    }
    s->base[param] = clamp_param(param, value);
    if (param == AG_SYNTH_P_LFO_RATE) {
        ag_dsp_lfo_set_hz_x100(&s->lfo1, s->base[param] * 8, s->rate);
    }
    if (param == AG_SYNTH_P_LFO_WAVE) {
        s->lfo1.wave = (uint8_t)s->base[param];
    }
    if (param == AG_SYNTH_P_ENGINE && s->base[param] == AG_SYNTH_FM &&
        s->base[AG_SYNTH_P_FM_INDEX] == 0) {
        s->base[AG_SYNTH_P_FM_INDEX] = 70;
    }
    if (param == AG_SYNTH_P_FM_OPS) {
        int i;
        for (i = 0; i < AG_SYNTH_VOICES; i++) {
            ag_fmx_set_n(&s->voice[i].fmx, (int)s->base[param]);
            ag_fmx_algo_stack(&s->voice[i].fmx);
        }
    }
}

void ag_synth_set_wavetable(ag_synth_t *s, const int16_t *data, uint32_t frames)
{
    int i;
    if (s == 0) {
        return;
    }
    s->wt = (data != 0 && frames >= 2u) ? data : 0;
    s->wt_n = s->wt ? frames : 0u;
    for (i = 0; i < AG_SYNTH_VOICES; i++) {
        ag_osc_set_table(&s->voice[i].osc1, s->wt, s->wt_n);
        ag_osc_set_table(&s->voice[i].osc2, s->wt, s->wt_n);
    }
}

int32_t ag_synth_get(const ag_synth_t *s, uint16_t param)
{
    if (s == 0 || param >= AG_SYNTH_P_N) {
        return 0;
    }
    return s->base[param];
}

int ag_synth_mod_bind(ag_synth_t *s, uint8_t src, uint16_t dest, int16_t depth)
{
    int i;
    if (s == 0 || src >= AG_SYNTH_SRC_N || dest >= AG_SYNTH_P_N) {
        return -1;
    }
    for (i = 0; i < AG_SYNTH_MOD_SLOTS; i++) {
        if (!s->mod[i].used ||
            (s->mod[i].src == src && s->mod[i].dest == dest)) {
            s->mod[i].src = src;
            s->mod[i].dest = dest;
            s->mod[i].depth = depth;
            s->mod[i].used = 1;
            return i;
        }
    }
    return -1;
}

void ag_synth_mod_clear(ag_synth_t *s)
{
    if (s == 0) {
        return;
    }
    ag_dsp_zero(s->mod, sizeof(s->mod));
}

void ag_synth_set_mw(ag_synth_t *s, uint8_t v)
{
    if (s) {
        s->mw = v;
    }
}

void ag_synth_set_at(ag_synth_t *s, uint8_t v)
{
    if (s) {
        s->at = v;
    }
}

static int steal_slot(ag_synth_t *s)
{
    uint32_t ages[AG_SYNTH_VOICES];
    uint8_t  act[AG_SYNTH_VOICES];
    int      i;
    for (i = 0; i < AG_SYNTH_VOICES; i++) {
        ages[i] = s->voice[i].age;
        act[i] = s->voice[i].active;
    }
    return ag_dsp_voice_steal(ages, act, AG_SYNTH_VOICES);
}

void ag_synth_note_on(ag_synth_t *s, uint8_t note, uint8_t vel)
{
    int              slot;
    ag_synth_voice_t *v;
    int32_t          hz;
    if (s == 0) {
        return;
    }
    if (vel == 0u) {
        ag_synth_note_off(s, note);
        return;
    }
    slot = steal_slot(s);
    v = &s->voice[slot];
    v->note = note;
    v->vel = vel;
    v->gate = 1;
    v->active = 1;
    v->age = ++s->age_seq;
    hz = ag_dsp_note_hz_x100((int)note);
    ag_osc_set_hz_x100(&v->osc1, hz, s->rate);
    {
        int32_t tune = s->base[AG_SYNTH_P_OSC2_TUNE];
        int32_t hz2 = (hz * (tune > 0 ? tune : 64)) / 64;
        ag_osc_set_hz_x100(&v->osc2, hz2, s->rate);
    }
    v->osc1.wave = (uint8_t)s->base[AG_SYNTH_P_WAVE1];
    v->osc2.wave = (uint8_t)s->base[AG_SYNTH_P_WAVE2];
    v->osc1.pwm = (uint8_t)s->base[AG_SYNTH_P_PWM];
    v->osc2.pwm = (uint8_t)s->base[AG_SYNTH_P_PWM];
    ag_osc_set_table(&v->osc1, s->wt, s->wt_n);
    ag_osc_set_table(&v->osc2, s->wt, s->wt_n);
    v->noise.wave = AG_OSC_NOISE;
    ag_filt_reset(&v->filt);
    ag_dist_reset(&v->dist);
    ag_dsp_adsr_on(&v->amp);
    ag_dsp_adsr_on(&v->feg);
    ag_fmx_set_n(&v->fmx, (int)s->base[AG_SYNTH_P_FM_OPS]);
    ag_fmx_algo_stack(&v->fmx);
    {
        int n = (int)v->fmx.n_ops;
        if (n >= 2) {
            v->fmx.route[n - 2][n - 1] = (uint8_t)s->base[AG_SYNTH_P_FM_INDEX];
        }
        if (n >= 3) {
            v->fmx.route[0][1] = (uint8_t)s->base[AG_SYNTH_P_FM_INDEX2];
        }
    }
    ag_fmx_set_hz(&v->fmx, hz, s->rate);
    ag_fmx_note_on(&v->fmx);
    s->ctrl_left = 0;
}

void ag_synth_note_off(ag_synth_t *s, uint8_t note)
{
    int i;
    if (s == 0) {
        return;
    }
    for (i = 0; i < AG_SYNTH_VOICES; i++) {
        if (s->voice[i].active && s->voice[i].note == note) {
            s->voice[i].gate = 0;
            ag_dsp_adsr_off(&s->voice[i].amp);
            ag_dsp_adsr_off(&s->voice[i].feg);
            ag_fmx_note_off(&s->voice[i].fmx);
        }
    }
}

void ag_synth_all_notes_off(ag_synth_t *s)
{
    int i;
    if (s == 0) {
        return;
    }
    for (i = 0; i < AG_SYNTH_VOICES; i++) {
        s->voice[i].gate = 0;
        ag_dsp_adsr_off(&s->voice[i].amp);
        ag_dsp_adsr_off(&s->voice[i].feg);
        ag_fmx_note_off(&s->voice[i].fmx);
    }
}

static int32_t src_value(const ag_synth_t *s, const ag_synth_voice_t *v,
                         uint8_t src, int32_t lfo1, int32_t lfo2)
{
    switch (src) {
    case AG_SYNTH_SRC_LFO1:
        return lfo1;
    case AG_SYNTH_SRC_LFO2:
        return lfo2;
    case AG_SYNTH_SRC_FEG:
        return (v->feg.level * 32767) / 256;
    case AG_SYNTH_SRC_AEG:
        return (v->amp.level * 32767) / 256;
    case AG_SYNTH_SRC_VEL:
        return ((int32_t)v->vel * 32767) / 127;
    case AG_SYNTH_SRC_NOTE:
        return ((int32_t)v->note * 32767) / 127;
    case AG_SYNTH_SRC_MW:
        return ((int32_t)s->mw * 32767) / 127;
    case AG_SYNTH_SRC_AT:
        return ((int32_t)s->at * 32767) / 127;
    default:
        return 0;
    }
}

static void apply_mods(ag_synth_t *s, ag_synth_voice_t *v, int32_t *eff,
                       int32_t lfo1, int32_t lfo2)
{
    int i;
    for (i = 0; i < AG_SYNTH_P_N; i++) {
        eff[i] = s->base[i];
    }
    for (i = 0; i < AG_SYNTH_MOD_SLOTS; i++) {
        int32_t add;
        uint16_t dest;
        if (!s->mod[i].used) {
            continue;
        }
        dest = s->mod[i].dest;
        add = (src_value(s, v, s->mod[i].src, lfo1, lfo2) * (int32_t)s->mod[i].depth) >>
              16;
        eff[dest] = clamp_param(dest, eff[dest] + add);
    }
}

static int32_t render_voice(ag_synth_t *s, ag_synth_voice_t *v, int32_t *eff)
{
    int32_t mix, out;
    int     live;
    live = ag_dsp_adsr_tick(&v->amp, (uint8_t)eff[AG_SYNTH_P_AMP_A],
                            (uint8_t)eff[AG_SYNTH_P_AMP_D],
                            (uint8_t)eff[AG_SYNTH_P_AMP_S],
                            (uint8_t)eff[AG_SYNTH_P_AMP_R], v->gate);
    (void)ag_dsp_adsr_tick(&v->feg, (uint8_t)eff[AG_SYNTH_P_FILT_A],
                           (uint8_t)eff[AG_SYNTH_P_FILT_D],
                           (uint8_t)eff[AG_SYNTH_P_FILT_S],
                           (uint8_t)eff[AG_SYNTH_P_FILT_R], v->gate);
    if (!live) {
        v->active = 0;
        return 0;
    }
    if (eff[AG_SYNTH_P_ENGINE] == AG_SYNTH_FM) {
        int n = (int)v->fmx.n_ops;
        if (n >= 2) {
            v->fmx.route[n - 2][n - 1] = (uint8_t)eff[AG_SYNTH_P_FM_INDEX];
        }
        if (n >= 3) {
            v->fmx.route[0][1] = (uint8_t)eff[AG_SYNTH_P_FM_INDEX2];
        }
        mix = ag_fmx_tick(&v->fmx, (uint8_t)eff[AG_SYNTH_P_AMP_A],
                          (uint8_t)eff[AG_SYNTH_P_AMP_D],
                          (uint8_t)eff[AG_SYNTH_P_AMP_S],
                          (uint8_t)eff[AG_SYNTH_P_AMP_R], v->gate);
    } else {
        int32_t a, b, n, m, hz, tune;
        v->osc1.wave = (uint8_t)eff[AG_SYNTH_P_WAVE1];
        v->osc2.wave = (uint8_t)eff[AG_SYNTH_P_WAVE2];
        v->osc1.pwm = (uint8_t)eff[AG_SYNTH_P_PWM];
        v->osc2.pwm = (uint8_t)eff[AG_SYNTH_P_PWM];
        if (v->osc1.wt != s->wt) {
            ag_osc_set_table(&v->osc1, s->wt, s->wt_n);
            ag_osc_set_table(&v->osc2, s->wt, s->wt_n);
        }
        hz = ag_dsp_note_hz_x100((int)v->note);
        tune = eff[AG_SYNTH_P_OSC2_TUNE];
        ag_osc_set_hz_x100(&v->osc2, (hz * (tune > 0 ? tune : 64)) / 64, s->rate);
        b = ag_osc_tick(&v->osc2);
        if (eff[AG_SYNTH_P_FM_INDEX] > 0) {
            int32_t pm = (b * (int32_t)eff[AG_SYNTH_P_FM_INDEX]) >> 6;
            a = ag_osc_tick_pm(&v->osc1, pm);
        } else {
            a = ag_osc_tick(&v->osc1);
        }
        n = 0;
        if (eff[AG_SYNTH_P_NOISE] > 0) {
            n = (ag_osc_tick(&v->noise) * (int32_t)eff[AG_SYNTH_P_NOISE]) >> 7;
        }
        m = (int32_t)eff[AG_SYNTH_P_OSC_MIX];
        mix = ag_dsp_mix127(a, b, (unsigned)m) + n;
    }
    {
        int32_t cut = eff[AG_SYNTH_P_CUTOFF] +
                      ((v->feg.level * eff[AG_SYNTH_P_FILT_AMT]) >> 8);
        v->filt.cutoff = (uint8_t)ag_clampi((int)cut, 1, 127);
        v->filt.reso = (uint8_t)eff[AG_SYNTH_P_RESO];
        mix = ag_filt_tick(&v->filt, mix);
    }
    if (eff[AG_SYNTH_P_DRIVE] > 0) {
        v->dist.model = (uint8_t)eff[AG_SYNTH_P_DIST_MODEL];
        v->dist.drive = (uint8_t)eff[AG_SYNTH_P_DRIVE];
        v->dist.bias = (uint8_t)eff[AG_SYNTH_P_BIAS];
        v->dist.sag = (uint8_t)eff[AG_SYNTH_P_SAG];
        mix = ag_dist_tick(&v->dist, mix);
    }
    out = (mix * v->amp.level * (int32_t)v->vel) >> 15;
    return out;
}

void ag_synth_render(ag_synth_t *s, int16_t *stereo, int32_t frames)
{
    int32_t i, vi;
    int32_t lfo1 = 0, lfo2 = 0;
    int32_t eff[AG_SYNTH_VOICES][AG_SYNTH_P_N];
    if (s == 0 || stereo == 0 || frames <= 0) {
        return;
    }
    ag_dsp_zero(eff, sizeof(eff));
    for (vi = 0; vi < AG_SYNTH_VOICES; vi++) {
        int p;
        for (p = 0; p < AG_SYNTH_P_N; p++) {
            eff[vi][p] = s->base[p];
        }
    }
    for (i = 0; i < frames; i++) {
        int32_t acc = 0;
        if (s->ctrl_left == 0u) {
            s->ctrl_left = AG_SYNTH_CTRL;
            lfo1 = ag_dsp_lfo_tick(&s->lfo1);
            lfo2 = ag_dsp_lfo_tick(&s->lfo2);
            for (vi = 0; vi < AG_SYNTH_VOICES; vi++) {
                if (s->voice[vi].active) {
                    apply_mods(s, &s->voice[vi], eff[vi], lfo1, lfo2);
                }
            }
        }
        s->ctrl_left--;
        for (vi = 0; vi < AG_SYNTH_VOICES; vi++) {
            if (s->voice[vi].active) {
                acc += render_voice(s, &s->voice[vi], eff[vi]);
            }
        }
        acc >>= 2;
        stereo[i * 2] = ag_sat16(acc);
        stereo[i * 2 + 1] = ag_sat16(acc);
    }
}
