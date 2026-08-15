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
    s->level = 110;
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
        return 2048u;
    }
    if (preset == AG_SMP_BASS) {
        return (rate * 3u) / 4u;
    }
    return (rate * 5u) / 4u; /* piano */
}

static int32_t harm(uint32_t i, uint32_t period, int32_t amp)
{
    uint32_t ph = (i * 65536u) / (period ? period : 1u);
    return ((int32_t)ag_dsp_sin(ph << 16) * amp) >> 15;
}

int ag_smp_fill_preset(int preset, int16_t *dst, uint32_t cap, uint32_t rate,
                       ag_smp_zone_t *out)
{
    uint32_t n = ag_smp_preset_frames(preset, rate);
    uint32_t i;

    if (dst == 0 || cap < n || rate < 1u) {
        return -1;
    }
    if (preset < 0 || preset >= AG_SMP_NPRESETS) {
        preset = AG_SMP_ORGAN;
    }
    for (i = 0; i < n; i++) {
        int32_t acc = 0;
        if (preset == AG_SMP_ORGAN) {
            /* Looped drawbar-ish: 1 + 2 + 3 + 4 + 6 */
            acc = harm(i, n, 18000) + harm(i, n / 2u, 9000) +
                  harm(i, n / 3u, 5000) + harm(i, n / 4u, 3500) +
                  harm(i, n / 6u, 2500);
        } else if (preset == AG_SMP_BASS) {
            uint32_t per = rate / 82u; /* ~E2 */
            int32_t env = (int32_t)((n - i) * 28000u / n);
            acc = harm(i, per, env) + harm(i, per / 2u, env / 3);
        } else {
            uint32_t per = rate / 261u; /* ~C4 */
            int32_t env = (int32_t)((n - i) * (n - i) / n);
            env = (env * 24000) / (int32_t)n;
            acc = harm(i, per, env) + harm(i, (per * 2u) / 3u, env / 4) +
                  harm(i, per / 2u, env / 5);
        }
        dst[i] = ag_sat16(acc);
    }
    if (out != 0) {
        out->data = dst;
        out->frames = n;
        out->rate = rate;
        out->root = (preset == AG_SMP_BASS) ? 40u : 60u;
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
