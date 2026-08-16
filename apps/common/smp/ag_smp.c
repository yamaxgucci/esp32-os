/*
 * Pitched sample player: linear interp, loop, steal, linear ADSR.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_smp.h"

void ag_smp_init(ag_smp_t *s, uint32_t out_rate)
{
    if (s == 0) {
        return;
    }
    ag_dsp_zero(s, sizeof(*s));
    s->rate = out_rate ? out_rate : 22050u;
    s->attack = 4;
    s->decay = 40;
    s->sustain = 100;
    s->release = 36;
    s->level = 127;
    s->zone.root = 60;
}

void ag_smp_set_zone(ag_smp_t *s, const ag_smp_zone_t *z)
{
    if (s == 0) {
        return;
    }
    if (z == 0) {
        ag_dsp_zero(&s->zone, sizeof(s->zone));
        return;
    }
    s->zone = *z;
    if (s->zone.root == 0u) {
        s->zone.root = 60;
    }
}

void ag_smp_set_adsr(ag_smp_t *s, uint8_t a, uint8_t d, uint8_t sus, uint8_t r)
{
    if (s == 0) {
        return;
    }
    s->attack = a;
    s->decay = d;
    s->sustain = sus;
    s->release = r;
}

uint32_t ag_smp_preset_frames(int preset, uint32_t rate)
{
    if (rate < 1u) {
        rate = 22050u;
    }
    if (preset == AG_SMP_ORGAN) {
        uint32_t c4 = rate / 262u;
        if (c4 < 8u) {
            c4 = 8u;
        }
        return c4 * 8u;
    }
    if (preset == AG_SMP_BASS) {
        return rate; /* 1 s */
    }
    return (rate * 5u) / 4u; /* piano */
}

static int32_t harm(uint32_t i, uint32_t period, int32_t amp)
{
    uint32_t ph;
    if (period < 2u) {
        period = 2u;
    }
    ph = (uint32_t)(((uint64_t)i << 32) / (uint64_t)period);
    return ((int32_t)ag_dsp_sin(ph) * amp) >> 15;
}

static void normalize(int16_t *dst, uint32_t n, int32_t peak)
{
    uint32_t i;
    int32_t  m = 1;
    for (i = 0; i < n; i++) {
        int32_t a = dst[i] < 0 ? -(int32_t)dst[i] : (int32_t)dst[i];
        if (a > m) {
            m = a;
        }
    }
    for (i = 0; i < n; i++) {
        dst[i] = (int16_t)(((int32_t)dst[i] * peak) / m);
    }
}

int ag_smp_fill_preset(int preset, int16_t *dst, uint32_t cap, uint32_t rate,
                       ag_smp_zone_t *out)
{
    uint32_t n = ag_smp_preset_frames(preset, rate);
    uint32_t i;
    uint32_t c4 = rate / 262u; /* ~C4 cycle */

    if (dst == 0 || cap < n || rate < 1u) {
        return -1;
    }
    if (c4 < 8u) {
        c4 = 8u;
    }
    if (preset < 0 || preset >= AG_SMP_NPRESETS) {
        preset = AG_SMP_ORGAN;
    }
    for (i = 0; i < n; i++) {
        int32_t acc = 0;
        if (preset == AG_SMP_ORGAN) {
            acc = harm(i, c4, 14000) + harm(i, c4 / 2u, 9000) +
                  harm(i, c4 / 3u, 6000) + harm(i, c4 / 4u, 4500) +
                  harm(i, c4 / 6u, 3000) + harm(i, c4 / 8u, 2000);
        } else if (preset == AG_SMP_BASS) {
            uint32_t per = rate / 131u; /* C3, laptop-audible */
            int32_t  env = (int32_t)((n - i) * 20000u / n);
            int32_t  click = 0;
            if (i < 80u) {
                click = (int32_t)(((80u - i) * 8000u) / 80u);
                if ((i & 1u) != 0u) {
                    click = -click;
                }
            }
            acc = harm(i, per, env) + harm(i, per / 2u, env / 2) +
                  harm(i, per / 3u, env / 3) + harm(i, per / 4u, env / 4) +
                  click;
        } else {
            int32_t env = (int32_t)((n - i) * 26000u / n);
            if (i < n / 8u) {
                env = 26000;
            }
            acc = harm(i, c4, env) + harm(i, c4 / 2u, env / 2) +
                  harm(i, c4 / 3u, env / 3) + harm(i, c4 / 4u, env / 4) +
                  harm(i, c4 / 5u, env / 6);
            if (i < 120u) {
                int32_t hammer = (int32_t)(((120u - i) * 12000u) / 120u);
                acc += ((int32_t)ag_dsp_sin((i * 19u) << 24) * hammer) >> 15;
            }
        }
        dst[i] = ag_sat16(acc);
    }
    normalize(dst, n, 24000);
    if (out != 0) {
        out->data = dst;
        out->frames = n;
        out->rate = rate;
        out->root = (preset == AG_SMP_BASS) ? 48u : 60u;
        if (preset == AG_SMP_ORGAN) {
            out->loop_start = 0;
            out->loop_end = n;
        } else {
            out->loop_start = 0;
            out->loop_end = 0;
        }
    }
    return 0;
}

static uint32_t pitch_step(const ag_smp_t *s, int note)
{
    int32_t  note_hz = ag_dsp_note_hz_x100(note);
    int32_t  root_hz = ag_dsp_note_hz_x100((int)s->zone.root);
    uint32_t sr = s->zone.rate ? s->zone.rate : s->rate;
    int64_t  st;

    if (root_hz < 1) {
        root_hz = 1;
    }
    if (s->rate < 1u) {
        return 65536u;
    }
    st = ((int64_t)sr * (int64_t)note_hz * 65536ll) /
         ((int64_t)s->rate * (int64_t)root_hz);
    if (st < 1) {
        st = 1;
    }
    if (st > 0x1000000ll) {
        st = 0x1000000ll;
    }
    return (uint32_t)st;
}

void ag_smp_note_on(ag_smp_t *s, uint8_t note, uint8_t vel)
{
    uint32_t ages[AG_SMP_VOICES];
    uint8_t  active[AG_SMP_VOICES];
    int      i, slot;
    ag_smp_voice_t *v;

    if (s == 0 || s->zone.data == 0 || s->zone.frames < 2u) {
        return;
    }
    for (i = 0; i < AG_SMP_VOICES; i++) {
        if (s->voice[i].active && s->voice[i].note == note) {
            ag_smp_note_off(s, note);
            break;
        }
    }
    for (i = 0; i < AG_SMP_VOICES; i++) {
        ages[i] = s->voice[i].age;
        active[i] = s->voice[i].active;
    }
    slot = ag_dsp_voice_steal(ages, active, AG_SMP_VOICES);
    v = &s->voice[slot];
    ag_dsp_zero(v, sizeof(*v));
    v->note = note;
    v->vel = vel ? vel : 1u;
    v->gate = 1;
    v->active = 1;
    v->pos = 0;
    v->step = pitch_step(s, (int)note);
    v->age = ++s->age_seq;
    ag_dsp_adsr_set_rate(&v->amp, s->rate); /* ticked per sample below */
    ag_dsp_adsr_on(&v->amp);
}

void ag_smp_note_off(ag_smp_t *s, uint8_t note)
{
    int i;
    if (s == 0) {
        return;
    }
    for (i = 0; i < AG_SMP_VOICES; i++) {
        if (s->voice[i].active && s->voice[i].note == note) {
            s->voice[i].gate = 0;
            ag_dsp_adsr_off(&s->voice[i].amp);
        }
    }
}

void ag_smp_all_notes_off(ag_smp_t *s)
{
    int i;
    if (s == 0) {
        return;
    }
    for (i = 0; i < AG_SMP_VOICES; i++) {
        s->voice[i].gate = 0;
        ag_dsp_adsr_off(&s->voice[i].amp);
    }
}

static int32_t tap(const ag_smp_zone_t *z, uint32_t pos)
{
    uint32_t idx = pos >> 16;
    uint32_t frac = pos & 0xffffu;
    int32_t  a, b;
    uint32_t n = z->frames;

    if (idx >= n) {
        return 0;
    }
    a = z->data[idx];
    if (idx + 1u < n) {
        b = z->data[idx + 1u];
    } else if (z->loop_end > z->loop_start && z->loop_end <= n) {
        b = z->data[z->loop_start];
    } else {
        b = 0;
    }
    return a + (int32_t)(((int64_t)(b - a) * (int64_t)frac) >> 16);
}

static int advance(ag_smp_voice_t *v, const ag_smp_zone_t *z)
{
    uint32_t next = v->pos + v->step;
    uint32_t end = z->frames << 16;

    if (z->loop_end > z->loop_start && z->loop_end <= z->frames) {
        uint32_t ls = z->loop_start << 16;
        uint32_t le = z->loop_end << 16;
        if (next >= le && le > ls) {
            next = ls + (next - le) % (le - ls);
        }
        v->pos = next;
        return 1;
    }
    if (next >= end) {
        v->pos = end;
        return 0;
    }
    v->pos = next;
    return 1;
}

void ag_smp_render(ag_smp_t *s, int16_t *stereo, int32_t frames)
{
    int32_t i, vi;

    if (s == 0 || stereo == 0 || frames <= 0) {
        return;
    }
    for (i = 0; i < frames; i++) {
        int32_t acc = 0;
        for (vi = 0; vi < AG_SMP_VOICES; vi++) {
            ag_smp_voice_t *v = &s->voice[vi];
            int32_t samp, live;
            if (!v->active) {
                continue;
            }
            live = ag_dsp_adsr_tick(&v->amp, s->attack, s->decay, s->sustain,
                                    s->release, v->gate);
            if (!live || s->zone.data == 0) {
                v->active = 0;
                continue;
            }
            samp = tap(&s->zone, v->pos);
            samp = (samp * v->amp.level) >> 8;
            samp = (samp * (int32_t)v->vel) >> 7;
            samp = (samp * (int32_t)s->level) >> 7;
            acc += samp;
            if (!advance(v, &s->zone)) {
                v->gate = 0;
                ag_dsp_adsr_off(&v->amp);
                if (v->amp.level <= 0) {
                    v->active = 0;
                }
            }
        }
        stereo[i * 2] = ag_sat16(acc);
        stereo[i * 2 + 1] = ag_sat16(acc);
    }
}
