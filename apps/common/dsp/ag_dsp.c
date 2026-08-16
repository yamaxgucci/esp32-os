/*
 * ag_dsp — shared fixed-point primitives.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_dsp.h"

static int16_t s_sin[AG_DSP_SIN_LEN];
static int     s_sin_ready;

void ag_dsp_zero(void *p, unsigned n)
{
    unsigned char *d = (unsigned char *)p;
    while (n--) {
        *d++ = 0;
    }
}

void ag_dsp_copy(void *dst, const void *src, unsigned n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) {
        *d++ = *s++;
    }
}

static void ensure_sin(void)
{
    int i;
    if (s_sin_ready) {
        return;
    }
    for (i = 0; i < AG_DSP_SIN_LEN; i++) {
        int     x = i & AG_DSP_SIN_MASK;
        int     neg = 0;
        int32_t u, u2, u3, s;
        if (x >= AG_DSP_SIN_LEN / 2) {
            x -= AG_DSP_SIN_LEN / 2;
            neg = 1;
        }
        if (x >= AG_DSP_SIN_LEN / 4) {
            x = AG_DSP_SIN_LEN / 2 - x;
        }
        u = ((int32_t)x * 32767) / (AG_DSP_SIN_LEN / 4);
        if (u > 32767) {
            u = 32767;
        }
        u2 = (u * u) >> 15;
        u3 = (u2 * u) >> 15;
        s = (3 * u - u3) >> 1;
        if (s > 32767) {
            s = 32767;
        }
        if (s < 0) {
            s = 0;
        }
        s_sin[i] = (int16_t)(neg ? -s : s);
    }
    s_sin_ready = 1;
}

int16_t ag_dsp_sin(uint32_t phase)
{
    ensure_sin();
    return s_sin[(phase >> AG_DSP_PHASE_SHIFT) & AG_DSP_SIN_MASK];
}

int32_t ag_dsp_note_hz_x100(int note)
{
    static const int32_t k_a4 = 44000;
    static const int32_t k_ratio[12] = {
        10000, 10595, 11225, 11892, 12599, 13348,
        14142, 14983, 15874, 16818, 17817, 18877
    };
    int     n = ag_clampi(note, 0, 127);
    int     d = n - 69;
    int     octs = d / 12;
    int     rem = d % 12;
    int32_t hz;
    if (rem < 0) {
        rem += 12;
        octs -= 1;
    }
    hz = (k_a4 * k_ratio[rem]) / 10000;
    while (octs > 0) {
        hz *= 2;
        octs--;
    }
    while (octs < 0) {
        hz /= 2;
        octs++;
    }
    if (hz < 100) {
        hz = 100;
    }
    return hz;
}

uint32_t ag_dsp_hz_to_step(int32_t hz_x100, uint32_t rate)
{
    uint64_t num;
    if (rate < 1u) {
        rate = 22050u;
    }
    if (hz_x100 < 1) {
        hz_x100 = 1;
    }
    num = (uint64_t)(uint32_t)hz_x100 << 32;
    num /= 100ull * (uint64_t)rate;
    if (num > 0xffffffffull) {
        num = 0xffffffffull;
    }
    return (uint32_t)num;
}

void ag_dsp_delay_reset(ag_dsp_delay_t *d, int16_t *buf, uint32_t cap)
{
    uint32_t i;
    if (d == 0) {
        return;
    }
    d->buf = buf;
    d->cap = cap;
    d->w = 0;
    if (buf != 0) {
        for (i = 0; i < cap; i++) {
            buf[i] = 0;
        }
    }
}

void ag_dsp_delay_write(ag_dsp_delay_t *d, int16_t s)
{
    if (d == 0 || d->buf == 0 || d->cap == 0u) {
        return;
    }
    d->buf[d->w] = s;
    d->w++;
    if (d->w >= d->cap) {
        d->w = 0;
    }
}

int16_t ag_dsp_delay_tap(const ag_dsp_delay_t *d, uint32_t delay)
{
    uint32_t idx;
    if (d == 0 || d->buf == 0 || d->cap == 0u) {
        return 0;
    }
    if (delay >= d->cap) {
        delay = d->cap - 1u;
    }
    idx = (d->w + d->cap - delay) % d->cap;
    return d->buf[idx];
}

int ag_dsp_voice_steal(const uint32_t *born, const uint8_t *active, int n)
{
    int      i;
    int      oldest_i = 0;
    uint32_t oldest = 0xffffffffu;
    if (born == 0 || active == 0 || n <= 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (!active[i]) {
            return i;
        }
        if (born[i] < oldest) {
            oldest = born[i];
            oldest_i = i;
        }
    }
    return oldest_i;
}

int32_t ag_dsp_adsr_seg_ms(uint8_t p, int32_t lo_ms, int32_t hi_ms)
{
    const int32_t v = (int32_t)(p > 127u ? 127u : p);
    /* Quadratic: the short end, where the ear is picky, gets the resolution. */
    return lo_ms + (int32_t)(((int64_t)(hi_ms - lo_ms) * v * v) / (127 * 127));
}

/* Level units to add per tick so a full sweep takes ms. */
static int32_t seg_step(int32_t ms, uint32_t rate)
{
    int64_t st;
    if (ms < 1) {
        ms = 1;
    }
    if (rate < 1u) {
        rate = 22050u;
    }
    st = ((int64_t)AG_DSP_ADSR_FULL * 1000) / ((int64_t)ms * (int64_t)rate);
    if (st < 1) {
        st = 1; /* never stall: a segment always finishes */
    }
    return (int32_t)st;
}

static int32_t seg_delta(int32_t ms, uint32_t rate, uint32_t n)
{
    const int64_t d = (int64_t)seg_step(ms, rate) * (int64_t)n;
    return d > (int64_t)AG_DSP_ADSR_FULL ? AG_DSP_ADSR_FULL : (int32_t)d;
}

void ag_dsp_adsr_set_rate(ag_dsp_adsr_t *e, uint32_t ticks_per_sec)
{
    if (e == 0) {
        return;
    }
    e->rate = ticks_per_sec ? ticks_per_sec : 22050u;
}

void ag_dsp_adsr_on(ag_dsp_adsr_t *e)
{
    if (e == 0) {
        return;
    }
    e->stage = 0;
    e->level = 0;
    e->acc = 0;
}

void ag_dsp_adsr_off(ag_dsp_adsr_t *e)
{
    if (e == 0) {
        return;
    }
    if (e->stage < 3u) {
        e->stage = 3;
    }
}

int ag_dsp_adsr_tick_n(ag_dsp_adsr_t *e, uint8_t a, uint8_t d, uint8_t s,
                       uint8_t r, int gate, uint32_t n)
{
    uint32_t rate;
    int32_t  target;
    if (e == 0) {
        return 0;
    }
    if (n == 0u) {
        return e->stage < 4u ? 1 : 0;
    }
    rate = e->rate ? e->rate : 22050u;
    switch (e->stage) {
    case 0:
        e->acc += seg_delta(ag_dsp_adsr_seg_ms(a, 1, 3000), rate, n);
        if (e->acc >= AG_DSP_ADSR_FULL) {
            e->acc = AG_DSP_ADSR_FULL;
            e->stage = 1;
        }
        break;
    case 1:
        target = (int32_t)(((int64_t)s * AG_DSP_ADSR_FULL) / 127);
        if (e->acc > target) {
            e->acc -= seg_delta(ag_dsp_adsr_seg_ms(d, 2, 4000), rate, n);
            if (e->acc <= target) {
                e->acc = target;
                e->stage = 2;
            }
        } else {
            e->acc = target;
            e->stage = 2;
        }
        break;
    case 2:
        if (!gate) {
            e->stage = 3;
        }
        break;
    case 3:
        e->acc -= seg_delta(ag_dsp_adsr_seg_ms(r, 3, 5000), rate, n);
        if (e->acc <= 0) {
            e->acc = 0;
            e->stage = 4;
        }
        break;
    default:
        e->acc = 0;
        e->level = 0;
        e->stage = 4;
        return 0;
    }
    e->level = e->acc >> 16;
    return e->stage < 4u ? 1 : 0;
}

int ag_dsp_adsr_tick(ag_dsp_adsr_t *e, uint8_t a, uint8_t d, uint8_t s,
                     uint8_t r, int gate)
{
    return ag_dsp_adsr_tick_n(e, a, d, s, r, gate, 1u);
}

int32_t ag_dsp_lfo_wave(uint32_t phase, uint8_t wave)
{
    int32_t v;
    switch (wave) {
    case AG_DSP_LFO_SAWDN:
        v = 32767 - (int32_t)((phase >> 16) & 0xffff);
        break;
    case AG_DSP_LFO_SAWUP:
        v = (int32_t)((phase >> 16) & 0xffff) - 32768;
        break;
    case AG_DSP_LFO_SQR:
        v = (phase & 0x80000000u) ? 32767 : -32768;
        break;
    case AG_DSP_LFO_SIN:
        v = ag_dsp_sin(phase);
        break;
    default: { /* triangle: same 16 bits of phase as the ramps above, or it
                * runs at four times the rate the caller asked for. */
        uint32_t x = (phase >> 16) & 0xffff;
        if (x < 32768u) {
            v = (int32_t)x - 16384;
        } else {
            v = 49151 - (int32_t)x;
        }
        v *= 2;
        break;
    }
    }
    return v;
}

void ag_dsp_lfo_set_hz_x100(ag_dsp_lfo_t *l, int32_t hz_x100, uint32_t rate)
{
    if (l == 0) {
        return;
    }
    l->step = ag_dsp_hz_to_step(hz_x100, rate);
}

int32_t ag_dsp_lfo_tick(ag_dsp_lfo_t *l)
{
    int32_t v;
    if (l == 0) {
        return 0;
    }
    v = ag_dsp_lfo_wave(l->phase, l->wave);
    l->phase += l->step;
    return v;
}

uint32_t ag_dsp_rng(uint32_t *state)
{
    uint32_t x;
    if (state == 0) {
        return 0;
    }
    x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if (x == 0u) {
        x = 0xA5A5A5A5u;
    }
    *state = x;
    return x;
}
