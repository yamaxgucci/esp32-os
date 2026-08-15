/*
 * Structural DX7-like 6-op FM renderer (fixed-point, polyphonic).
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_dx7.h"
#include "ag_dsp.h"

static const uint8_t k_alg_dest[AG_DX7_ALGS][AG_DX7_OPS] = {
    {1, 2, 3, 4, 5, 6}, {1, 2, 3, 4, 6, 6}, {1, 2, 3, 6, 5, 6},
    {1, 2, 3, 6, 5, 6}, {1, 2, 6, 4, 5, 6}, {1, 2, 6, 4, 5, 6},
    {1, 2, 6, 4, 6, 6}, {1, 6, 3, 4, 6, 6}, {1, 6, 3, 4, 6, 6},
    {1, 2, 6, 6, 5, 6}, {1, 2, 6, 6, 5, 6}, {1, 6, 6, 4, 5, 6},
    {1, 6, 6, 4, 5, 6}, {1, 2, 6, 4, 6, 6}, {1, 2, 6, 4, 6, 6},
    {1, 6, 3, 6, 6, 6}, {1, 6, 3, 6, 6, 6}, {1, 2, 6, 6, 6, 6},
    {1, 6, 3, 4, 5, 6}, {1, 6, 3, 6, 5, 6}, {1, 6, 6, 4, 6, 6},
    {1, 6, 6, 6, 5, 6}, {1, 6, 6, 6, 6, 6}, {1, 6, 3, 6, 6, 6},
    {1, 6, 6, 6, 6, 6}, {1, 6, 3, 4, 6, 6}, {1, 6, 3, 6, 6, 6},
    {1, 2, 6, 6, 6, 6}, {1, 6, 6, 6, 6, 6}, {1, 6, 6, 4, 6, 6},
    {1, 6, 6, 6, 6, 6}, {6, 6, 6, 6, 6, 6},
};


/*
 * DX7-ish rate → level units/sample. Quadratic was too soft at mid rates;
 * use a steeper curve closer to the chip's envelope timing feel.
 */
static int32_t rate_to_delta(uint8_t rate, uint32_t sample_rate)
{
    int r = ag_clampi((int)rate, 0, 99);
    int32_t d;
    if (r == 0) {
        return 0;
    }
    /* ~ r^2.2 in fixed point: r^2 * (64+r) / 64 */
    d = (r * r * (64 + r)) / 6400;
    if (d < 1) {
        d = 1;
    }
    if (sample_rate < 1u) {
        sample_rate = 22050u;
    }
    if (sample_rate != 22050u) {
        d = (d * 22050) / (int32_t)sample_rate;
    }
    if (d < 1) {
        d = 1;
    }
    return d;
}

/* Level 0..99 → linear amp 0..256 (~0.75 dB/step near top, DX7-like). */
static int32_t level_to_amp256(int level)
{
    int n = 99 - ag_clampi(level, 0, 99);
    int32_t amp = 256;
    while (n >= 8) {
        amp >>= 1;
        n -= 8;
    }
    if (n > 0) {
        amp = (amp * (8 - n) + (amp >> 1) * n) / 8;
    }
    return amp;
}

/* Keyboard level scale depth with curve 0=-lin 1=-exp 2=+exp 3=+lin */
static int32_t kbd_scale_delta(int dist, int depth, int curve)
{
    int32_t d;
    int ad;
    if (depth <= 0 || dist == 0) {
        return 0;
    }
    ad = dist < 0 ? -dist : dist;
    /* dist in semitones from break; depth 0..99 */
    if (curve == 1 || curve == 2) {
        /* exp-ish: x^2 / 64 */
        d = (ad * ad * depth) / (64 * 12);
    } else {
        d = (ad * depth) / 48;
    }
    if (curve == 0 || curve == 1) {
        return -d; /* attenuate away from centre side */
    }
    return d;
}

static void perf_reset(ag_dx7_perf_t *p)
{
    ag_dsp_zero(p, sizeof(*p));
    p->op_enable = 0x3fu;
    p->unison = 1;
    p->audition = 1; /* default: hear every MIDI note clearly */
}

static void eg_set_stage(ag_dx7_t *dx, ag_dx7_op_t *o, const ag_dx7_op_patch_t *p,
                         int stage)
{
    int32_t rate;
    if (stage < 0 || stage > 3) {
        o->eg_stage = 4;
        o->eg_target = 0;
        o->eg_rate = 0;
        return;
    }
    o->eg_stage = (uint8_t)stage;
    o->eg_target = (int32_t)p->level[stage] * 256;
    rate = rate_to_delta(p->rate[stage], dx->rate);
    o->eg_rate = rate;
    if (o->eg_rate < 1 && p->rate[stage] > 0) {
        o->eg_rate = 1;
    }
}

static void eg_set_stage_for_voice(ag_dx7_t *dx, ag_dx7_voice_t *v, int oi,
                                   int stage)
{
    ag_dx7_op_t *o = &v->op[oi];
    const ag_dx7_op_patch_t *p = &dx->patch.op[oi];
    int32_t rate;
    int rs;
    eg_set_stage(dx, o, p, stage);
    if (stage < 0 || stage > 3) {
        return;
    }
    rs = (int)p->kbd_rate_scale;
    if (rs > 0 && !dx->perf.audition) {
        int note = (int)v->note;
        /* Mild: higher keys a bit faster — was extreme and muted low notes. */
        int boost = ((note - 36) * rs) / 96;
        if (boost < 0) {
            boost = 0;
        }
        if (boost > 8) {
            boost = 8;
        }
        rate = o->eg_rate + (o->eg_rate * boost) / 32;
        if (rate < 1 && p->rate[stage] > 0) {
            rate = 1;
        }
        o->eg_rate = rate;
    }
    /*
     * Audition: short MIDI notes must speak. Force a usable attack so pads /
     * slow brass are not silent on eighths.
     */
    if (dx->perf.audition && stage == 0) {
        int32_t min_r = rate_to_delta(60, dx->rate);
        if (o->eg_rate < min_r) {
            o->eg_rate = min_r;
        }
    }
}

static void eg_advance_op(ag_dx7_t *dx, ag_dx7_voice_t *v, int oi)
{
    ag_dx7_op_t *o = &v->op[oi];
    const ag_dx7_op_patch_t *p = &dx->patch.op[oi];
    int32_t d;

    if (o->eg_stage >= 4) {
        o->eg_level = 0;
        return;
    }
    if (o->eg_stage == 2 && o->eg_level == o->eg_target) {
        return;
    }
    d = o->eg_rate;
    if (d <= 0) {
        o->eg_level = o->eg_target;
        if (o->eg_stage < 2) {
            eg_set_stage_for_voice(dx, v, oi, o->eg_stage + 1);
        }
        return;
    }
    if (o->eg_level < o->eg_target) {
        o->eg_level += d;
        if (o->eg_level >= o->eg_target) {
            o->eg_level = o->eg_target;
            if (o->eg_stage < 2) {
                eg_set_stage_for_voice(dx, v, oi, o->eg_stage + 1);
            }
        }
    } else if (o->eg_level > o->eg_target) {
        o->eg_level -= d;
        if (o->eg_level <= o->eg_target) {
            o->eg_level = o->eg_target;
            if (o->eg_stage < 2) {
                eg_set_stage_for_voice(dx, v, oi, o->eg_stage + 1);
            } else if (o->eg_stage == 3 && o->eg_level <= 0) {
                o->eg_stage = 4;
                o->eg_level = 0;
            }
        }
    } else if (o->eg_stage < 2) {
        eg_set_stage_for_voice(dx, v, oi, o->eg_stage + 1);
    } else if (o->eg_stage == 3 && o->eg_level <= 0) {
        o->eg_stage = 4;
        o->eg_level = 0;
    }
    (void)p;
}

static void pitch_eg_advance(ag_dx7_t *dx, ag_dx7_voice_t *v)
{
    int32_t d;
    if (v->pitch_stage >= 4) {
        return;
    }
    if (v->pitch_stage == 2 && v->pitch_eg == v->pitch_target) {
        return;
    }
    d = v->pitch_rate;
    if (d <= 0) {
        v->pitch_eg = v->pitch_target;
        if (v->pitch_stage < 2) {
            v->pitch_stage++;
            v->pitch_target =
                (int32_t)dx->patch.pitch_level[v->pitch_stage] * 256;
            v->pitch_rate =
                rate_to_delta(dx->patch.pitch_rate[v->pitch_stage], dx->rate);
        }
        return;
    }
    if (v->pitch_eg < v->pitch_target) {
        v->pitch_eg += d;
        if (v->pitch_eg >= v->pitch_target) {
            v->pitch_eg = v->pitch_target;
            if (v->pitch_stage < 2) {
                v->pitch_stage++;
                v->pitch_target =
                    (int32_t)dx->patch.pitch_level[v->pitch_stage] * 256;
                v->pitch_rate = rate_to_delta(
                    dx->patch.pitch_rate[v->pitch_stage], dx->rate);
            }
        }
    } else if (v->pitch_eg > v->pitch_target) {
        v->pitch_eg -= d;
        if (v->pitch_eg <= v->pitch_target) {
            v->pitch_eg = v->pitch_target;
            if (v->pitch_stage < 2) {
                v->pitch_stage++;
                v->pitch_target =
                    (int32_t)dx->patch.pitch_level[v->pitch_stage] * 256;
                v->pitch_rate = rate_to_delta(
                    dx->patch.pitch_rate[v->pitch_stage], dx->rate);
            }
        }
    } else if (v->pitch_stage < 2) {
        v->pitch_stage++;
        v->pitch_target = (int32_t)dx->patch.pitch_level[v->pitch_stage] * 256;
        v->pitch_rate =
            rate_to_delta(dx->patch.pitch_rate[v->pitch_stage], dx->rate);
    }
}

static int32_t op_freq_hz_x100(const ag_dx7_t *dx, const ag_dx7_voice_t *v,
                               int oi)
{
    const ag_dx7_op_patch_t *p = &dx->patch.op[oi];
    int32_t base = v->base_hz_x100;
    int32_t pitch_mod = (v->pitch_eg / 256) - 50;
    int32_t hz;
    int det = (int)p->detune - 7;

    if (pitch_mod != 0) {
        base = base + (base * pitch_mod) / 100;
    }
    if (p->mode == 0) {
        int32_t coarse = p->coarse;
        int32_t fine = p->fine;
        int32_t ratio_x100 =
            (coarse == 0) ? (50 + fine) : (coarse * 100 + fine);
        hz = (base * ratio_x100) / 100;
    } else {
        int32_t c = ag_clampi((int)p->coarse, 0, 3);
        static const int32_t k_fix[4] = {100, 1000, 10000, 100000};
        hz = k_fix[c] + (int32_t)p->fine * (k_fix[c] / 100);
    }
    if (det != 0) {
        hz += (hz * det) / 200;
    }
    if (hz < 1) {
        hz = 1;
    }
    return hz;
}

static void refresh_steps(ag_dx7_t *dx, ag_dx7_voice_t *v)
{
    int i;
    int32_t lfo = 0;
    int32_t pmd = ((int32_t)dx->patch.lfo_pmd * (int32_t)dx->patch.lfo_pms);
    /* Mod wheel 0..127 scales pitch LFO depth. */
    pmd = (pmd * (64 + (int32_t)dx->perf.mod_wheel)) / 191;
    if (dx->lfo_delay_left == 0u) {
        lfo = ag_dsp_lfo_wave(dx->lfo_phase, dx->patch.lfo_wave);
    }
    for (i = 0; i < AG_DX7_OPS; i++) {
        int32_t hz = op_freq_hz_x100(dx, v, i);
        if (pmd > 0 && dx->lfo_delay_left == 0u) {
            hz += (hz * lfo / 32768) * pmd / (99 * 7);
        }
        /* Aftertouch → slight sharp pitch (performance). */
        if (dx->perf.aftertouch > 0u) {
            hz += (hz * (int32_t)dx->perf.aftertouch) / (127 * 64);
        }
        v->op[i].step = ag_dsp_hz_to_step(hz, dx->rate);
    }
}

static int32_t op_amp(const ag_dx7_t *dx, const ag_dx7_voice_t *v, int oi)
{
    const ag_dx7_op_t *o = &v->op[oi];
    const ag_dx7_op_patch_t *p = &dx->patch.op[oi];
    int32_t eg = o->eg_level / 256;
    int32_t lvl = o->level_scl;
    int32_t combined;
    int32_t amp;

    if (((dx->perf.op_enable >> oi) & 1u) == 0u) {
        return 0;
    }
    combined = (eg * lvl) / 99;
    if (dx->perf.audition) {
        /* Blend DX7-ish curve with linear so mid levels stay audible. */
        int32_t curve = level_to_amp256(combined);
        int32_t lin = (combined * 256) / 99;
        amp = (curve + lin) / 2;
        amp = (amp * 99) / 256;
    } else {
        amp = level_to_amp256(combined);
        amp = (amp * 99) / 256;
    }

    if (amp <= 0) {
        return 0;
    }
    if (dx->lfo_delay_left == 0u && p->amp_mod_sens > 0u &&
        dx->patch.lfo_amd > 0u) {
        int32_t lfo = ag_dsp_lfo_wave(dx->lfo_phase, dx->patch.lfo_wave);
        int32_t amd;
        int32_t amd_depth = (int32_t)dx->patch.lfo_amd;
        /* Aftertouch boosts AMD */
        amd_depth += ((int32_t)dx->perf.aftertouch * 40) / 127;
        if (lfo < 0) {
            lfo = -lfo;
        }
        amd = (amd_depth * (int32_t)p->amp_mod_sens * lfo) / (99 * 3 * 32768);
        amp = amp - (amp * amd) / 99;
        if (amp < 0) {
            amp = 0;
        }
    }
    return amp;
}

static int voice_dead(const ag_dx7_voice_t *v)
{
    int i;
    if (v->gate) {
        return 0;
    }
    for (i = 0; i < AG_DX7_OPS; i++) {
        if (v->op[i].eg_stage < 4 && v->op[i].eg_level > 0) {
            return 0;
        }
    }
    return 1;
}

static int32_t render_voice(ag_dx7_t *dx, ag_dx7_voice_t *v)
{
    int32_t mod[AG_DX7_OPS];
    int32_t out = 0;
    int i;
    uint8_t alg = dx->patch.algorithm;
    const uint8_t *dest;
    int32_t fb;

    if (!v->active) {
        return 0;
    }
    if (alg >= AG_DX7_ALGS) {
        alg = 0;
    }
    dest = k_alg_dest[alg];

    for (i = 0; i < AG_DX7_OPS; i++) {
        mod[i] = 0;
        eg_advance_op(dx, v, i);
    }
    pitch_eg_advance(dx, v);

    fb = v->fb_mem[0] + v->fb_mem[1];
    if (dx->patch.feedback > 0u) {
        mod[0] += (fb * (int32_t)dx->patch.feedback) << 10;
    }

    for (i = 0; i < AG_DX7_OPS; i++) {
        int32_t amp = op_amp(dx, v, i);
        int32_t s;
        uint8_t d;
        v->op[i].phase += v->op[i].step;
        s = (int32_t)ag_dsp_sin(v->op[i].phase + (uint32_t)mod[i]);
        s = (s * amp) / 99;
        v->op[i].out = s;
        d = dest[i];
        if (d >= 6u) {
            if (((dx->perf.carrier_mute >> i) & 1u) == 0u) {
                out += s;
            }
        } else {
            mod[d] += s << 16;
        }
    }

    v->fb_mem[0] = v->fb_mem[1];
    v->fb_mem[1] = v->op[0].out;

    /* Attack smoother; release uses EG. Final fade when EG is done. */
    if (v->gate) {
        if (v->out_gain < 256) {
            v->out_gain += 8;
            if (v->out_gain > 256) {
                v->out_gain = 256;
            }
        }
    } else if (voice_dead(v)) {
        if (v->out_gain > 0) {
            v->out_gain -= 8;
            if (v->out_gain < 0) {
                v->out_gain = 0;
            }
        }
    }
    out = (out * v->out_gain) / 256;

    /* Portamento glide toward target_hz */
    if (dx->perf.porta_on && dx->perf.porta_time > 0u &&
        v->base_hz_x100 != v->target_hz_x100) {
        int32_t diff = v->target_hz_x100 - v->base_hz_x100;
        int32_t step = diff / (8 + (int32_t)dx->perf.porta_time * 4);
        if (step == 0) {
            step = (diff > 0) ? 1 : -1;
        }
        v->base_hz_x100 += step;
        if ((diff > 0 && v->base_hz_x100 > v->target_hz_x100) ||
            (diff < 0 && v->base_hz_x100 < v->target_hz_x100)) {
            v->base_hz_x100 = v->target_hz_x100;
        }
    }

    if (voice_dead(v) && v->out_gain <= 0) {
        v->active = 0;
    }
    return out / 2;
}

static void apply_level_scaling(ag_dx7_t *dx, ag_dx7_voice_t *v)
{
    int i;
    int note = (int)v->note;
    int vel = (int)v->vel;
    for (i = 0; i < AG_DX7_OPS; i++) {
        const ag_dx7_op_patch_t *p = &dx->patch.op[i];
        int32_t lvl = p->out_level;
        int depth_l = (int)p->kbd_ldepth;
        int depth_r = (int)p->kbd_rdepth;
        /* Yamaha: break 39 ($27) = C3 = MIDI 48 → offset +9 (was wrongly +21). */
        int br = (int)p->kbd_break + 9;
        int dist = note - br;

        if (dx->perf.audition) {
            depth_l /= 2;
            depth_r /= 2;
        }
        if (dist < 0 && depth_l > 0) {
            lvl += kbd_scale_delta(dist, depth_l, (int)p->kbd_lcurve);
        } else if (dist > 0 && depth_r > 0) {
            lvl += kbd_scale_delta(dist, depth_r, (int)p->kbd_rcurve);
        }
        if (p->vel_sens > 0u) {
            /* Scale around vel=100 ≈ unity for typical MIDI files. */
            int vs = (int)p->vel_sens;
            lvl = (lvl * (64 + (vel * vs) / 7)) / (64 + (100 * vs) / 7);
        }
        lvl = ag_clampi((int)lvl, 0, 99);
        /* Don't let scaling fully silence an operator that has output level. */
        if (p->out_level > 0) {
            int floor = (int)p->out_level / (dx->perf.audition ? 3 : 6);
            if (floor < 1) {
                floor = 1;
            }
            if (lvl < floor) {
                lvl = floor;
            }
        }
        v->op[i].level_scl = lvl;
    }
}

static void voice_clear(ag_dx7_voice_t *v)
{
    int i;
    ag_dsp_zero(v, sizeof(*v));
    for (i = 0; i < AG_DX7_OPS; i++) {
        v->op[i].eg_stage = 4;
    }
    v->pitch_eg = 50 * 256;
    v->pitch_stage = 4;
}

static int alloc_voice(ag_dx7_t *dx, uint8_t note)
{
    int i;
    int best = -1;
    uint32_t best_age = 0xffffffffu;

    /* Retrigger same MIDI note on an existing voice. */
    for (i = 0; i < AG_DX7_VOICES; i++) {
        if (dx->voice[i].active && dx->voice[i].note == note) {
            return i;
        }
    }
    for (i = 0; i < AG_DX7_VOICES; i++) {
        if (!dx->voice[i].active) {
            return i;
        }
    }
    /* Steal: prefer released, then oldest. */
    for (i = 0; i < AG_DX7_VOICES; i++) {
        ag_dx7_voice_t *v = &dx->voice[i];
        uint32_t score = v->age;
        if (!v->gate) {
            score = score / 2u; /* prefer ungated */
        }
        if (score < best_age) {
            best_age = score;
            best = i;
        }
    }
    return best < 0 ? 0 : best;
}

void ag_dx7_init(ag_dx7_t *dx, uint32_t sample_rate)
{
    ag_dsp_zero(dx, sizeof(*dx));
    dx->rate = sample_rate ? sample_rate : 22050u;
    perf_reset(&dx->perf);
    ag_dx7_load_patch(dx, ag_dx7_preset(0));
}

void ag_dx7_reset(ag_dx7_t *dx)
{
    int i;
    for (i = 0; i < AG_DX7_VOICES; i++) {
        voice_clear(&dx->voice[i]);
    }
    dx->lfo_phase = 0;
    dx->lfo_delay_left = 0;
    dx->age_seq = 0;
}

void ag_dx7_load_patch(ag_dx7_t *dx, const ag_dx7_patch_t *p)
{
    int32_t hz_x100;
    if (!p) {
        return;
    }
    ag_dsp_copy(&dx->patch, p, sizeof(*p));
    if (dx->patch.algorithm >= AG_DX7_ALGS) {
        dx->patch.algorithm = 0;
    }
    if (dx->patch.feedback > 7u) {
        dx->patch.feedback = 7;
    }
    hz_x100 = (int32_t)dx->patch.lfo_speed * 20;
    dx->lfo_step = ag_dsp_hz_to_step(hz_x100, dx->rate);
    dx->perf.op_enable = 0x3fu;
    dx->perf.carrier_mute = 0;
    ag_dx7_reset(dx);
}

void ag_dx7_set_algorithm(ag_dx7_t *dx, uint8_t alg)
{
    dx->patch.algorithm = (uint8_t)(alg % AG_DX7_ALGS);
}

void ag_dx7_set_feedback(ag_dx7_t *dx, uint8_t fb)
{
    dx->patch.feedback = (uint8_t)(fb > 7u ? 7u : fb);
}

void ag_dx7_set_op_level(ag_dx7_t *dx, int op, uint8_t level)
{
    int vi;
    if (op < 0 || op >= AG_DX7_OPS) {
        return;
    }
    dx->patch.op[op].out_level = (uint8_t)ag_clampi((int)level, 0, 99);
    for (vi = 0; vi < AG_DX7_VOICES; vi++) {
        if (dx->voice[vi].active) {
            apply_level_scaling(dx, &dx->voice[vi]);
        }
    }
}

void ag_dx7_set_op_enable(ag_dx7_t *dx, int op, int on)
{
    if (op < 0 || op >= AG_DX7_OPS) {
        return;
    }
    if (on) {
        dx->perf.op_enable |= (uint8_t)(1u << op);
    } else {
        dx->perf.op_enable &= (uint8_t)~(1u << op);
    }
}

void ag_dx7_toggle_op(ag_dx7_t *dx, int op)
{
    if (op < 0 || op >= AG_DX7_OPS) {
        return;
    }
    dx->perf.op_enable ^= (uint8_t)(1u << op);
}

void ag_dx7_mute_carriers(ag_dx7_t *dx, int mute)
{
    int i;
    uint8_t alg = dx->patch.algorithm;
    const uint8_t *dest;
    if (alg >= AG_DX7_ALGS) {
        alg = 0;
    }
    dest = k_alg_dest[alg];
    if (!mute) {
        dx->perf.carrier_mute = 0;
        return;
    }
    dx->perf.carrier_mute = 0;
    for (i = 0; i < AG_DX7_OPS; i++) {
        if (dest[i] >= 6u) {
            dx->perf.carrier_mute |= (uint8_t)(1u << i);
        }
    }
}

void ag_dx7_set_mod_wheel(ag_dx7_t *dx, uint8_t v)
{
    dx->perf.mod_wheel = (uint8_t)ag_clampi((int)v, 0, 127);
}

void ag_dx7_set_aftertouch(ag_dx7_t *dx, uint8_t v)
{
    dx->perf.aftertouch = (uint8_t)ag_clampi((int)v, 0, 127);
}

void ag_dx7_set_porta(ag_dx7_t *dx, int on, uint8_t time)
{
    dx->perf.porta_on = on ? 1u : 0u;
    dx->perf.porta_time = (uint8_t)ag_clampi((int)time, 0, 99);
}

void ag_dx7_set_unison(ag_dx7_t *dx, uint8_t voices, uint8_t detune)
{
    dx->perf.unison = (uint8_t)ag_clampi((int)voices, 1, 4);
    dx->perf.unison_detune = (uint8_t)ag_clampi((int)detune, 0, 99);
}

void ag_dx7_set_audition(ag_dx7_t *dx, int on)
{
    dx->perf.audition = on ? 1u : 0u;
}

static void voice_start(ag_dx7_t *dx, ag_dx7_voice_t *v, uint8_t note,
                        uint8_t vel, int32_t hz_x100, int32_t from_hz)
{
    int i;
    voice_clear(v);
    v->note = note;
    v->vel = vel ? vel : 100;
    v->gate = 1;
    v->active = 1;
    v->age = ++dx->age_seq;
    v->out_gain = 0;
    v->target_hz_x100 = hz_x100;
    if (dx->perf.porta_on && dx->perf.porta_time > 0u && from_hz > 0) {
        v->base_hz_x100 = from_hz;
    } else {
        v->base_hz_x100 = hz_x100;
    }
    apply_level_scaling(dx, v);
    for (i = 0; i < AG_DX7_OPS; i++) {
        if (dx->patch.osc_sync) {
            v->op[i].phase = 0;
        }
        v->op[i].eg_level = 0;
        eg_set_stage_for_voice(dx, v, i, 0);
    }
    v->pitch_stage = 0;
    v->pitch_eg = (int32_t)dx->patch.pitch_level[3] * 256;
    v->pitch_target = (int32_t)dx->patch.pitch_level[0] * 256;
    v->pitch_rate = rate_to_delta(dx->patch.pitch_rate[0], dx->rate);
    refresh_steps(dx, v);
}

static int alloc_free_voice(ag_dx7_t *dx)
{
    int i;
    int best = -1;
    uint32_t best_age = 0xffffffffu;
    for (i = 0; i < AG_DX7_VOICES; i++) {
        if (!dx->voice[i].active) {
            return i;
        }
    }
    for (i = 0; i < AG_DX7_VOICES; i++) {
        ag_dx7_voice_t *v = &dx->voice[i];
        uint32_t score = v->age;
        if (!v->gate) {
            score = score / 2u;
        }
        if (score < best_age) {
            best_age = score;
            best = i;
        }
    }
    return best < 0 ? 0 : best;
}

/* DX7 transpose: 24 = centre (C3). Sounding pitch = note + (transpose - 24). */
static int note_with_transpose(const ag_dx7_t *dx, int note)
{
    int tr = (int)dx->patch.transpose;
    if (tr < 0) {
        tr = 0;
    }
    if (tr > 48) {
        tr = 48;
    }
    return ag_clampi(note + (tr - 24), 0, 127);
}

void ag_dx7_note_on(ag_dx7_t *dx, uint8_t note, uint8_t vel)
{
    int u, nuni;
    int32_t hz, from_hz = 0;
    int i;
    int play;

    /* Keep MIDI note for note_off match; pitch uses patch transpose. */
    note = (uint8_t)ag_clampi((int)note, 0, 127);
    play = note_with_transpose(dx, (int)note);
    hz = ag_dsp_note_hz_x100(play);

    for (i = 0; i < AG_DX7_VOICES; i++) {
        if (dx->voice[i].active && dx->voice[i].gate) {
            from_hz = dx->voice[i].base_hz_x100;
            break;
        }
    }

    nuni = (int)dx->perf.unison;
    if (nuni < 1) {
        nuni = 1;
    }
    if (nuni > 4) {
        nuni = 4;
    }

    dx->lfo_delay_left = ((uint32_t)dx->patch.lfo_delay * dx->rate) / 50u;
    if (dx->patch.lfo_sync) {
        dx->lfo_phase = 0;
    }

    for (u = 0; u < nuni; u++) {
        int vi = (u == 0) ? alloc_voice(dx, note) : alloc_free_voice(dx);
        int32_t det = 0;
        int32_t hz_u;
        if (u > 0 && dx->perf.unison_detune > 0u) {
            /* Spread ±detune cents-ish around centre */
            det = ((int32_t)dx->perf.unison_detune * (u - (nuni - 1) / 2)) * hz /
                  (99 * 40);
        }
        hz_u = hz + det;
        if (hz_u < 1) {
            hz_u = 1;
        }
        voice_start(dx, &dx->voice[vi], note, vel, hz_u, from_hz);
    }
}

void ag_dx7_note_off(ag_dx7_t *dx, uint8_t note)
{
    int i;
    uint8_t n = (uint8_t)ag_clampi((int)note, 0, 127);

    for (i = 0; i < AG_DX7_VOICES; i++) {
        ag_dx7_voice_t *v = &dx->voice[i];
        int oi;
        if (!v->active || !v->gate || v->note != n) {
            continue;
        }
        v->gate = 0;
        for (oi = 0; oi < AG_DX7_OPS; oi++) {
            eg_set_stage_for_voice(dx, v, oi, 3);
        }
        v->pitch_stage = 3;
        v->pitch_target = (int32_t)dx->patch.pitch_level[3] * 256;
        v->pitch_rate = rate_to_delta(dx->patch.pitch_rate[3], dx->rate);
    }
}

void ag_dx7_note_off_all(ag_dx7_t *dx)
{
    int i;
    for (i = 0; i < AG_DX7_VOICES; i++) {
        ag_dx7_voice_t *v = &dx->voice[i];
        int oi;
        if (!v->active || !v->gate) {
            continue;
        }
        v->gate = 0;
        for (oi = 0; oi < AG_DX7_OPS; oi++) {
            eg_set_stage_for_voice(dx, v, oi, 3);
        }
        v->pitch_stage = 3;
        v->pitch_target = (int32_t)dx->patch.pitch_level[3] * 256;
        v->pitch_rate = rate_to_delta(dx->patch.pitch_rate[3], dx->rate);
    }
}

static uint8_t syx_clamp(uint8_t v, uint8_t maxv)
{
    return (v <= maxv) ? v : maxv;
}

void ag_dx7_unpack_packed(const uint8_t packed[AG_DX7_PACKED_VOICE],
                          uint8_t unpack[AG_DX7_SYX_VOICE])
{
    int op;
    if (!packed || !unpack) {
        return;
    }
    ag_dsp_zero(unpack, AG_DX7_SYX_VOICE);
    /* Packed layout: OP6..OP1 × 17 bytes (Dexed Cartridge::unpackProgram). */
    for (op = 0; op < AG_DX7_OPS; op++) {
        const uint8_t *b = packed + op * 17;
        uint8_t *u = unpack + op * 21;
        int i;
        for (i = 0; i < 11; i++) {
            u[i] = syx_clamp((uint8_t)(b[i] & 0x7fu), 99);
        }
        {
            uint8_t lr = (uint8_t)(b[11] & 0x0fu);
            u[11] = (uint8_t)(lr & 3u);
            u[12] = (uint8_t)((lr >> 2) & 3u);
        }
        {
            uint8_t det_rs = (uint8_t)(b[12] & 0x7fu);
            u[13] = (uint8_t)(det_rs & 7u);
            u[20] = syx_clamp((uint8_t)((det_rs >> 3) & 0x0fu), 14);
        }
        {
            uint8_t kvs_ams = (uint8_t)(b[13] & 0x1fu);
            u[14] = (uint8_t)(kvs_ams & 3u);
            u[15] = (uint8_t)((kvs_ams >> 2) & 7u);
        }
        u[16] = syx_clamp((uint8_t)(b[14] & 0x7fu), 99);
        {
            uint8_t fc_mode = (uint8_t)(b[15] & 0x3fu);
            u[17] = (uint8_t)(fc_mode & 1u);
            u[18] = (uint8_t)((fc_mode >> 1) & 31u);
        }
        u[19] = syx_clamp((uint8_t)(b[16] & 0x7fu), 99);
    }
    for (op = 0; op < 8; op++) {
        unpack[126 + op] = syx_clamp((uint8_t)(packed[102 + op] & 0x7fu), 99);
    }
    unpack[134] = syx_clamp((uint8_t)(packed[110] & 0x1fu), 31);
    {
        uint8_t oks_fb = (uint8_t)(packed[111] & 0x0fu);
        unpack[135] = (uint8_t)(oks_fb & 7u);
        unpack[136] = (uint8_t)(oks_fb >> 3);
    }
    unpack[137] = (uint8_t)(packed[112] & 0x7fu);
    unpack[138] = (uint8_t)(packed[113] & 0x7fu);
    unpack[139] = (uint8_t)(packed[114] & 0x7fu);
    unpack[140] = (uint8_t)(packed[115] & 0x7fu);
    {
        uint8_t lpms = (uint8_t)(packed[116] & 0x7fu);
        unpack[141] = (uint8_t)(lpms & 1u);
        unpack[142] = (uint8_t)((lpms >> 1) & 7u);
        unpack[143] = (uint8_t)((lpms >> 4) & 7u);
    }
    unpack[144] = syx_clamp((uint8_t)(packed[117] & 0x7fu), 48);
    for (op = 0; op < 10; op++) {
        unpack[145 + op] = (uint8_t)(packed[118 + op] & 0x7fu);
    }
}

int ag_dx7_patch_from_sysex(ag_dx7_patch_t *out, const uint8_t *data)
{
    int oi;
    int base;
    if (!out || !data) {
        return -1;
    }
    ag_dsp_zero(out, sizeof(*out));
    /* SysEx order OP6..OP1 → engine op[5]..op[0] */
    for (oi = 0; oi < AG_DX7_OPS; oi++) {
        ag_dx7_op_patch_t *o = &out->op[AG_DX7_OPS - 1 - oi];
        base = oi * 21;
        o->rate[0] = syx_clamp(data[base + 0], 99);
        o->rate[1] = syx_clamp(data[base + 1], 99);
        o->rate[2] = syx_clamp(data[base + 2], 99);
        o->rate[3] = syx_clamp(data[base + 3], 99);
        o->level[0] = syx_clamp(data[base + 4], 99);
        o->level[1] = syx_clamp(data[base + 5], 99);
        o->level[2] = syx_clamp(data[base + 6], 99);
        o->level[3] = syx_clamp(data[base + 7], 99);
        o->kbd_break = syx_clamp(data[base + 8], 99);
        o->kbd_ldepth = syx_clamp(data[base + 9], 99);
        o->kbd_rdepth = syx_clamp(data[base + 10], 99);
        o->kbd_lcurve = (uint8_t)(data[base + 11] & 3u);
        o->kbd_rcurve = (uint8_t)(data[base + 12] & 3u);
        o->kbd_rate_scale = (uint8_t)(data[base + 13] & 7u);
        o->amp_mod_sens = (uint8_t)(data[base + 14] & 3u);
        o->vel_sens = (uint8_t)(data[base + 15] & 7u);
        o->out_level = syx_clamp(data[base + 16], 99);
        o->mode = (uint8_t)(data[base + 17] & 1u);
        o->coarse = (uint8_t)(data[base + 18] & 31u);
        o->fine = syx_clamp(data[base + 19], 99);
        o->detune = syx_clamp((uint8_t)(data[base + 20] & 0x0fu), 14);
    }
    out->pitch_rate[0] = syx_clamp(data[126], 99);
    out->pitch_rate[1] = syx_clamp(data[127], 99);
    out->pitch_rate[2] = syx_clamp(data[128], 99);
    out->pitch_rate[3] = syx_clamp(data[129], 99);
    out->pitch_level[0] = syx_clamp(data[130], 99);
    out->pitch_level[1] = syx_clamp(data[131], 99);
    out->pitch_level[2] = syx_clamp(data[132], 99);
    out->pitch_level[3] = syx_clamp(data[133], 99);
    out->algorithm = (uint8_t)(data[134] % AG_DX7_ALGS);
    out->feedback = (uint8_t)(data[135] & 7u);
    out->osc_sync = (uint8_t)(data[136] & 1u);
    out->lfo_speed = syx_clamp(data[137], 99);
    out->lfo_delay = syx_clamp(data[138], 99);
    out->lfo_pmd = syx_clamp(data[139], 99);
    out->lfo_amd = syx_clamp(data[140], 99);
    out->lfo_sync = (uint8_t)(data[141] & 1u);
    out->lfo_wave = (uint8_t)(data[142] % 6u);
    out->lfo_pms = (uint8_t)(data[143] & 7u);
    out->transpose = syx_clamp(data[144], 48);
    for (oi = 0; oi < 10; oi++) {
        char c = (char)(data[145 + oi] & 0x7fu);
        out->name[oi] = (c >= 32 && c < 127) ? c : ' ';
    }
    out->name[10] = '\0';
    return 0;
}

int ag_dx7_load_sysex_voice(ag_dx7_t *dx, const uint8_t *data, int len)
{
    ag_dx7_patch_t p;
    if (!dx || !data || len < AG_DX7_SYX_VOICE) {
        return -1;
    }
    if (ag_dx7_patch_from_sysex(&p, data) != 0) {
        return -1;
    }
    ag_dx7_load_patch(dx, &p);
    return 0;
}

int ag_dx7_load_packed_voice(ag_dx7_t *dx, const uint8_t *packed128)
{
    uint8_t u[AG_DX7_SYX_VOICE];
    if (!dx || !packed128) {
        return -1;
    }
    ag_dx7_unpack_packed(packed128, u);
    return ag_dx7_load_sysex_voice(dx, u, AG_DX7_SYX_VOICE);
}

static void pack_155_to_128(const uint8_t *src, uint8_t *bulk)
{
    int op;
    for (op = 0; op < AG_DX7_OPS; op++) {
        int pp = op * 17;
        int up = op * 21;
        int i;
        for (i = 0; i < 11; i++) {
            bulk[pp + i] = src[up + i];
        }
        bulk[pp + 11] =
            (uint8_t)((src[up + 11] & 3u) | ((src[up + 12] & 3u) << 2));
        bulk[pp + 12] =
            (uint8_t)((src[up + 13] & 7u) | ((src[up + 20] & 0x0fu) << 3));
        bulk[pp + 13] =
            (uint8_t)((src[up + 14] & 3u) | ((src[up + 15] & 7u) << 2));
        bulk[pp + 14] = src[up + 16];
        bulk[pp + 15] =
            (uint8_t)((src[up + 17] & 1u) | ((src[up + 18] & 31u) << 1));
        bulk[pp + 16] = src[up + 19];
    }
    ag_dsp_copy(bulk + 102, src + 126, 9);
    bulk[111] = (uint8_t)((src[135] & 7u) | ((src[136] & 1u) << 3));
    ag_dsp_copy(bulk + 112, src + 137, 4);
    bulk[116] = (uint8_t)((src[141] & 1u) | ((src[142] & 7u) << 1) |
                          ((src[143] & 7u) << 4));
    bulk[117] = src[144];
    ag_dsp_copy(bulk + 118, src + 145, 10);
}

int ag_dx7_syx_parse_bank(const uint8_t *data, int len,
                          uint8_t packed[][AG_DX7_PACKED_VOICE], int max_voices)
{
    int n = 0;
    int i;
    if (!data || !packed || max_voices < 1 || len < 128) {
        return -1;
    }

    /* Yamaha 32-voice bulk: F0 43 0n 09 20 00 + 4096 + checksum F7 */
    for (i = 0; i + 6 < len; i++) {
        if (data[i] == 0xf0u && data[i + 1] == 0x43u &&
            (data[i + 3] == 0x09u) && i + 6 + 4096 <= len) {
            const uint8_t *body = data + i + 6;
            int v;
            for (v = 0; v < AG_DX7_BANK_VOICES && n < max_voices; v++) {
                ag_dsp_copy(packed[n], body + v * 128, 128);
                n++;
            }
            return n;
        }
    }

    /* Single voice: F0 43 0n 00 01 1B + 155 + checksum F7 */
    for (i = 0; i + 6 < len; i++) {
        if (data[i] == 0xf0u && data[i + 1] == 0x43u &&
            (data[i + 3] == 0x00u) && i + 6 + 155 <= len) {
            uint8_t u[AG_DX7_SYX_VOICE];
            ag_dsp_copy(u, data + i + 6, AG_DX7_SYX_VOICE);
            pack_155_to_128(u, packed[0]);
            return 1;
        }
    }

    /* Raw 4096 packed bank */
    if (len >= 4096) {
        int v;
        for (v = 0; v < AG_DX7_BANK_VOICES && n < max_voices; v++) {
            ag_dsp_copy(packed[n], data + v * 128, 128);
            n++;
        }
        return n;
    }

    /* Raw 155 edit buffer */
    if (len >= AG_DX7_SYX_VOICE) {
        pack_155_to_128(data, packed[0]);
        return 1;
    }

    /* Raw single packed voice */
    if (len >= AG_DX7_PACKED_VOICE) {
        ag_dsp_copy(packed[0], data, AG_DX7_PACKED_VOICE);
        return 1;
    }
    return -1;
}

int ag_dx7_active_voices(const ag_dx7_t *dx)
{
    int i, n = 0;
    for (i = 0; i < AG_DX7_VOICES; i++) {
        if (dx->voice[i].active) {
            n++;
        }
    }
    return n;
}

void ag_dx7_render(ag_dx7_t *dx, int16_t *pcm, int32_t frames)
{
    int32_t i;
    static uint32_t tick;
    if (!pcm || frames <= 0) {
        return;
    }
    for (i = 0; i < frames; i++) {
        int32_t mix = 0;
        int vi;
        if (dx->lfo_delay_left > 0u) {
            dx->lfo_delay_left--;
        }
        dx->lfo_phase += dx->lfo_step;
        for (vi = 0; vi < AG_DX7_VOICES; vi++) {
            if (dx->voice[vi].active) {
                mix += render_voice(dx, &dx->voice[vi]);
            }
        }
        if ((++tick & 31u) == 0u) {
            for (vi = 0; vi < AG_DX7_VOICES; vi++) {
                if (dx->voice[vi].active) {
                    refresh_steps(dx, &dx->voice[vi]);
                }
            }
        }
        /* Soft headroom for poly mix */
        mix = mix / 2;
        if (mix > 32767) {
            mix = 32767;
        } else if (mix < -32768) {
            mix = -32768;
        }
        pcm[i * 2] = (int16_t)mix;
        pcm[i * 2 + 1] = (int16_t)mix;
    }
}

/* ---- presets ------------------------------------------------------------ */

static void op_init(ag_dx7_op_patch_t *o, uint8_t lvl, uint8_t coarse,
                    uint8_t fine)
{
    ag_dsp_zero(o, sizeof(*o));
    o->rate[0] = 80;
    o->rate[1] = 40;
    o->rate[2] = 30;
    o->rate[3] = 50;
    o->level[0] = 99; /* L1 */
    o->level[1] = 85; /* L2 */
    o->level[2] = 75; /* L3 sustain — must be >0 for held notes */
    o->level[3] = 0;  /* L4 */
    o->out_level = lvl;
    o->coarse = coarse;
    o->fine = fine;
    o->detune = 7;
}

static void set_name(ag_dx7_patch_t *p, const char *s)
{
    int i;
    for (i = 0; i < AG_DX7_NAME_LEN - 1 && s[i]; i++) {
        p->name[i] = s[i];
    }
    p->name[i] = '\0';
}

static ag_dx7_patch_t make_init(void)
{
    ag_dx7_patch_t p;
    int i;
    ag_dsp_zero(&p, sizeof(p));
    for (i = 0; i < AG_DX7_OPS; i++) {
        op_init(&p.op[i], 0, 1, 0);
    }
    /* Single carrier sine — clearest smoke test */
    op_init(&p.op[5], 99, 1, 0);
    p.op[5].rate[0] = 90;
    p.op[5].rate[1] = 0;
    p.op[5].rate[2] = 0;
    p.op[5].level[0] = 99;
    p.op[5].level[1] = 99;
    p.op[5].level[2] = 99;
    p.op[5].rate[3] = 55;
    p.pitch_rate[0] = p.pitch_rate[1] = p.pitch_rate[2] = p.pitch_rate[3] = 99;
    p.pitch_level[0] = p.pitch_level[1] = p.pitch_level[2] = p.pitch_level[3] =
        50;
    p.algorithm = 31;
    p.feedback = 0;
    p.lfo_speed = 35;
    p.lfo_wave = 0;
    p.transpose = 24;
    set_name(&p, "INIT");
    return p;
}

static ag_dx7_patch_t make_epiano(void)
{
    ag_dx7_patch_t p = make_init();
    p.algorithm = 4;
    p.feedback = 4;
    op_init(&p.op[0], 65, 1, 0);
    p.op[0].rate[3] = 60;
    op_init(&p.op[1], 80, 14, 0);
    p.op[1].rate[3] = 70;
    p.op[1].level[2] = 0; /* modulator dies -> tine decay */
    op_init(&p.op[2], 70, 1, 0);
    op_init(&p.op[3], 99, 1, 0);
    p.op[3].level[2] = 70;
    p.op[3].rate[3] = 45;
    op_init(&p.op[4], 60, 1, 0);
    op_init(&p.op[5], 95, 1, 0);
    p.op[5].level[2] = 65;
    p.op[5].rate[3] = 45;
    set_name(&p, "E.PIANO");
    return p;
}

static ag_dx7_patch_t make_brass(void)
{
    ag_dx7_patch_t p = make_init();
    p.algorithm = 18;
    p.feedback = 3;
    op_init(&p.op[0], 75, 1, 0);
    p.op[0].rate[0] = 55;
    op_init(&p.op[1], 85, 1, 0);
    op_init(&p.op[2], 99, 1, 0);
    p.op[2].rate[0] = 60;
    p.op[2].level[2] = 80;
    op_init(&p.op[3], 0, 1, 0);
    op_init(&p.op[4], 0, 1, 0);
    op_init(&p.op[5], 0, 1, 0);
    p.lfo_pmd = 15;
    p.lfo_pms = 2;
    p.lfo_speed = 25;
    set_name(&p, "BRASS");
    return p;
}

static ag_dx7_patch_t make_bell(void)
{
    ag_dx7_patch_t p = make_init();
    int i;
    p.algorithm = 0;
    p.feedback = 0;
    op_init(&p.op[0], 90, 3, 0);
    op_init(&p.op[1], 80, 7, 0);
    op_init(&p.op[2], 70, 11, 0);
    op_init(&p.op[3], 60, 14, 0);
    op_init(&p.op[4], 50, 1, 0);
    op_init(&p.op[5], 99, 1, 0);
    for (i = 0; i < AG_DX7_OPS; i++) {
        p.op[i].rate[0] = 99;
        p.op[i].rate[1] = 35;
        p.op[i].rate[2] = 25;
        p.op[i].rate[3] = 30;
        p.op[i].level[1] = 40;
        p.op[i].level[2] = 0; /* decay to silence (bell) */
    }
    set_name(&p, "BELL");
    return p;
}

static ag_dx7_patch_t make_pad(void)
{
    ag_dx7_patch_t p = make_init();
    int i;
    p.algorithm = 31;
    p.feedback = 1;
    op_init(&p.op[0], 55, 1, 0);
    p.op[0].detune = 5;
    op_init(&p.op[1], 55, 1, 5);
    p.op[1].detune = 9;
    op_init(&p.op[2], 50, 2, 0);
    op_init(&p.op[3], 50, 2, 10);
    op_init(&p.op[4], 45, 3, 0);
    op_init(&p.op[5], 65, 1, 0);
    for (i = 0; i < AG_DX7_OPS; i++) {
        p.op[i].rate[0] = 35;
        p.op[i].rate[1] = 20;
        p.op[i].rate[2] = 15;
        p.op[i].rate[3] = 25;
        p.op[i].level[1] = 80;
        p.op[i].level[2] = 70;
    }
    p.lfo_amd = 20;
    p.op[5].amp_mod_sens = 2;
    p.lfo_speed = 18;
    set_name(&p, "PAD");
    return p;
}

static ag_dx7_patch_t make_bassoon(void)
{
    ag_dx7_patch_t p = make_init();
    p.algorithm = 15;
    p.feedback = 5;
    op_init(&p.op[0], 80, 1, 0);
    op_init(&p.op[1], 85, 2, 0);
    op_init(&p.op[2], 99, 1, 0);
    p.op[2].level[2] = 85;
    op_init(&p.op[3], 65, 3, 0);
    op_init(&p.op[4], 75, 1, 0);
    op_init(&p.op[5], 99, 1, 0);
    p.op[5].level[2] = 80;
    set_name(&p, "BASSOON");
    return p;
}

const ag_dx7_patch_t *ag_dx7_preset(int index)
{
    static int ready;
    static ag_dx7_patch_t presets[AG_DX7_NPRESETS];
    if (!ready) {
        presets[0] = make_init();
        presets[1] = make_epiano();
        presets[2] = make_brass();
        presets[3] = make_bell();
        presets[4] = make_pad();
        presets[5] = make_bassoon();
        ready = 1;
    }
    if (index < 0 || index >= AG_DX7_NPRESETS) {
        index = 0;
    }
    return &presets[index];
}
