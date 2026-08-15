/*
 * Fixed-point granular engine.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_grain.h"
#include "ag_dsp.h"

#define WIN_BITS 8
#define WIN_LEN  (1 << WIN_BITS)

static uint8_t s_hann[WIN_LEN];
static uint8_t s_tri[WIN_LEN];
static int     s_win_ready;


static void ensure_win(void)
{
    int i;
    if (s_win_ready) {
        return;
    }
    for (i = 0; i < WIN_LEN; i++) {
        /* Raised-cosine-ish Hann via quarter-sine polynomial. */
        int x = i;
        int32_t u, u2, u3, s;
        if (x > WIN_LEN / 2) {
            x = WIN_LEN - x;
        }
        u = ((int32_t)x * 32767) / (WIN_LEN / 2);
        if (u > 32767) {
            u = 32767;
        }
        u2 = (u * u) >> 15;
        u3 = (u2 * u) >> 15;
        s = (3 * u - u3) >> 1;
        if (s < 0) {
            s = 0;
        }
        if (s > 32767) {
            s = 32767;
        }
        s_hann[i] = (uint8_t)(s >> 7);
        /* Triangle */
        if (i < WIN_LEN / 2) {
            s_tri[i] = (uint8_t)((i * 255) / (WIN_LEN / 2));
        } else {
            s_tri[i] = (uint8_t)(((WIN_LEN - 1 - i) * 255) / (WIN_LEN / 2));
        }
    }
    s_win_ready = 1;
}

static uint32_t rng_next(ag_grain_t *g)
{
    return ag_dsp_rng(&g->rng);
}

/* Q16 step: (buf_rate / out_rate) * (hz(note+semis) / hz(C4)). */
static uint32_t pitch_step(ag_grain_t *g, int note, int semis, int cents_spr)
{
    int32_t note_hz = ag_dsp_note_hz_x100(note + semis);
    int32_t ref_hz = ag_dsp_note_hz_x100(60); /* C4 */
    uint32_t br = g->buf.rate ? g->buf.rate : g->rate;
    int64_t s;

    if (cents_spr > 0) {
        int32_t spr = (int32_t)(rng_next(g) % (uint32_t)(cents_spr * 2 + 1)) -
                      cents_spr;
        note_hz = note_hz + (note_hz * spr) / 1200;
        if (note_hz < 100) {
            note_hz = 100;
        }
    }
    if (ref_hz < 1) {
        ref_hz = 1;
    }
    if (g->rate < 1u) {
        return 65536u;
    }
    s = ((int64_t)br * (int64_t)note_hz * 65536ll) /
        ((int64_t)g->rate * (int64_t)ref_hz);
    if (s < 1) {
        s = 1;
    }
    if (s > 0x1000000ll) {
        s = 0x1000000ll;
    }
    return (uint32_t)s;
}

static uint8_t env_gain(uint8_t texture, uint32_t i, uint32_t len)
{
    uint32_t idx;
    uint8_t  box, tri, hann, a, b;
    if (len < 2u) {
        return 255;
    }
    idx = (i * (WIN_LEN - 1u)) / (len - 1u);
    if (idx >= WIN_LEN) {
        idx = WIN_LEN - 1u;
    }
    box = 255;
    tri = s_tri[idx];
    hann = s_hann[idx];
    if (texture < 64u) {
        /* box → triangle */
        a = box;
        b = tri;
        texture = (uint8_t)(texture * 2u);
    } else {
        /* triangle → hann */
        a = tri;
        b = hann;
        texture = (uint8_t)((texture - 64u) * 2u);
    }
    return (uint8_t)(((uint16_t)a * (255u - texture) + (uint16_t)b * texture) /
                     255u);
}

static uint32_t grain_len_samples(const ag_grain_t *g)
{
    /* size 0 → ~5ms, 127 → ~200ms */
    uint32_t ms = 5u + ((uint32_t)g->params.size * 195u) / 127u;
    uint32_t n = (g->rate * ms) / 1000u;
    if (n < 32u) {
        n = 32u;
    }
    return n;
}

static uint32_t spawn_interval(const ag_grain_t *g)
{
    /* density 0 → rare (~4Hz), 64 → idle-ish, 127 → dense (~80Hz overlap) */
    uint8_t d = g->params.density;
    uint32_t hz;
    if (d < 4u) {
        d = 4u;
    }
    hz = 2u + ((uint32_t)d * 78u) / 127u;
    return g->rate / hz;
}

static ag_grain_grain_t *alloc_grain(ag_grain_t *g)
{
    int i;
    int best = -1;
    uint32_t best_i = 0;
    for (i = 0; i < AG_GRAIN_POOL; i++) {
        if (!g->grain[i].active) {
            return &g->grain[i];
        }
        if (g->grain[i].i >= best_i) {
            best_i = g->grain[i].i;
            best = i;
        }
    }
    if (best >= 0) {
        g->grain[best].active = 0;
        return &g->grain[best];
    }
    return 0;
}

static void spawn_grain(ag_grain_t *g, int vi)
{
    ag_grain_grain_t *gr;
    ag_grain_voice_t *v = &g->voice[vi];
    uint32_t pos0, spray, frames, start;
    int semis;
    uint8_t pan;

    if (g->buf.data == 0 || g->buf.frames < 16u) {
        return;
    }
    gr = alloc_grain(g);
    if (gr == 0) {
        return;
    }

    frames = g->buf.frames;
    pos0 = ((uint32_t)g->params.position * (frames - 1u)) / 127u;
    spray = ((uint32_t)g->params.spray * frames) / 127u;
    if (spray > 0u) {
        uint32_t r = rng_next(g) % (spray * 2u + 1u);
        int32_t s = (int32_t)pos0 + (int32_t)r - (int32_t)spray;
        if (s < 0) {
            s = 0;
        }
        if ((uint32_t)s >= frames) {
            s = (int32_t)(frames - 1u);
        }
        start = (uint32_t)s;
    } else {
        start = pos0;
    }

    semis = (int)g->params.pitch;
    gr->active = 1;
    gr->voice = (uint8_t)vi;
    gr->reverse = (uint8_t)((rng_next(g) & 127u) < g->params.reverse);
    gr->pos = start << 16;
    gr->step = pitch_step(g, (int)v->note, semis, (int)g->params.pitch_spr);
    gr->len = grain_len_samples(g);
    gr->i = 0;
    gr->amp = ((int32_t)v->vel * (int32_t)g->params.level) / 127;
    if (gr->amp > 256) {
        gr->amp = 256;
    }

    pan = 64u;
    if (g->params.pan_spr) {
        int32_t d =
            (int32_t)(rng_next(g) % (uint32_t)(g->params.pan_spr * 2u + 1u)) -
            (int32_t)g->params.pan_spr;
        pan = (uint8_t)ag_clampi(64 + d, 0, 128);
    }
    gr->pan_l = (uint8_t)(128u - pan);
    gr->pan_r = pan;

    if (gr->reverse && start > 0u) {
        /* start from end of window toward start */
        uint32_t span = (gr->len * gr->step) >> 16;
        if (span >= frames) {
            span = frames - 1u;
        }
        if (start + span >= frames) {
            gr->pos = (frames - 1u) << 16;
        } else {
            gr->pos = (start + span) << 16;
        }
    }
}

static void eg_tick(ag_grain_t *g, ag_grain_voice_t *v)
{
    ag_dsp_adsr_t e;
    e.stage = v->eg_stage;
    e.level = v->eg;
    if (!ag_dsp_adsr_tick(&e, g->params.attack, g->params.decay,
                          g->params.sustain, g->params.release, v->gate)) {
        v->active = 0;
    }
    v->eg_stage = e.stage;
    v->eg = e.level;
}

void ag_grain_init(ag_grain_t *g, uint32_t out_rate)
{
    ag_dsp_zero(g, sizeof(*g));
    g->rate = out_rate ? out_rate : 22050u;
    g->rng = 0xC0FFEEu ^ out_rate;
    ensure_win();
    ag_grain_set_defaults(g);
}

void ag_grain_reset(ag_grain_t *g)
{
    uint32_t rate = g->rate;
    ag_grain_buf_t buf = g->buf;
    ag_grain_params_t p = g->params;
    ag_dsp_zero(g, sizeof(*g));
    g->rate = rate;
    g->buf = buf;
    g->params = p;
    g->rng = 0xC0FFEEu ^ rate;
}

void ag_grain_set_defaults(ag_grain_t *g)
{
    g->params.position = 40;
    g->params.size = 48;
    g->params.density = 72;
    g->params.spray = 16;
    g->params.pitch = 0;
    g->params.pitch_spr = 8;
    g->params.texture = 96;
    g->params.pan_spr = 40;
    g->params.reverse = 10;
    g->params.level = 100;
    g->params.attack = 20;
    g->params.decay = 40;
    g->params.sustain = 100;
    g->params.release = 50;
}

void ag_grain_buf_clear(ag_grain_t *g)
{
    /* Caller frees owned sample memory; engine only drops the view. */
    ag_dsp_zero(&g->buf, sizeof(g->buf));
}

int ag_grain_buf_set(ag_grain_t *g, int16_t *data, uint32_t frames,
                     uint32_t rate, int owned)
{
    g->buf.data = data;
    g->buf.frames = frames;
    g->buf.capacity = frames;
    g->buf.rate = rate ? rate : g->rate;
    g->buf.owned = owned ? 1u : 0u;
    g->buf.freeze = 0;
    return 0;
}

uint32_t ag_grain_buf_append(ag_grain_t *g, const int16_t *src, uint32_t frames)
{
    uint32_t space, n, i;
    if (g->buf.freeze || g->buf.data == 0 || src == 0 || frames == 0u) {
        return 0;
    }
    if (g->buf.frames >= g->buf.capacity) {
        /* ring: shift left by frames or wrap — simple drop-oldest */
        uint32_t drop = frames;
        if (drop > g->buf.frames) {
            drop = g->buf.frames;
        }
        for (i = 0; i + drop < g->buf.frames; i++) {
            g->buf.data[i] = g->buf.data[i + drop];
        }
        g->buf.frames -= drop;
    }
    space = g->buf.capacity - g->buf.frames;
    n = frames < space ? frames : space;
    for (i = 0; i < n; i++) {
        g->buf.data[g->buf.frames + i] = src[i];
    }
    g->buf.frames += n;
    return n;
}

void ag_grain_freeze(ag_grain_t *g, int on)
{
    g->buf.freeze = on ? 1u : 0u;
}

void ag_grain_note_on(ag_grain_t *g, uint8_t note, uint8_t vel)
{
    int i, slot = -1;
    uint32_t oldest = 0;
    int oldest_i = 0;

    if (vel == 0u) {
        ag_grain_note_off(g, note);
        return;
    }
    for (i = 0; i < AG_GRAIN_VOICES; i++) {
        if (g->voice[i].active && g->voice[i].note == note &&
            g->voice[i].gate) {
            slot = i;
            break;
        }
        if (!g->voice[i].active) {
            slot = i;
            break;
        }
        if (g->voice[i].age >= oldest) {
            oldest = g->voice[i].age;
            oldest_i = i;
        }
    }
    if (slot < 0) {
        slot = oldest_i;
    }
    g->age_seq++;
    g->voice[slot].note = note;
    g->voice[slot].vel = vel;
    g->voice[slot].gate = 1;
    g->voice[slot].active = 1;
    g->voice[slot].eg_stage = 0;
    g->voice[slot].eg = 0;
    g->voice[slot].spawn_left = 0;
    g->voice[slot].age = g->age_seq;
    spawn_grain(g, slot);
}

void ag_grain_note_off(ag_grain_t *g, uint8_t note)
{
    int i;
    for (i = 0; i < AG_GRAIN_VOICES; i++) {
        if (g->voice[i].active && g->voice[i].note == note) {
            g->voice[i].gate = 0;
            if (g->voice[i].eg_stage < 3u) {
                g->voice[i].eg_stage = 3;
            }
        }
    }
}

void ag_grain_all_notes_off(ag_grain_t *g)
{
    int i;
    for (i = 0; i < AG_GRAIN_VOICES; i++) {
        g->voice[i].gate = 0;
        g->voice[i].eg_stage = 3;
    }
}

void ag_grain_render(ag_grain_t *g, int16_t *stereo, int32_t frames)
{
    int32_t f, gi, vi;
    ensure_win();
    if (stereo == 0 || frames <= 0) {
        return;
    }
    for (f = 0; f < frames * 2; f++) {
        stereo[f] = 0;
    }

    /* voice EG + spawn */
    for (vi = 0; vi < AG_GRAIN_VOICES; vi++) {
        ag_grain_voice_t *v = &g->voice[vi];
        if (!v->active) {
            continue;
        }
        eg_tick(g, v);
        if (!v->active) {
            continue;
        }
        if (v->gate || v->eg_stage < 3u) {
            uint32_t left = (uint32_t)frames;
            while (left > 0u) {
                if (v->spawn_left == 0u) {
                    spawn_grain(g, vi);
                    v->spawn_left = spawn_interval(g);
                    if (v->spawn_left == 0u) {
                        v->spawn_left = 1u;
                    }
                }
                if (v->spawn_left > left) {
                    v->spawn_left -= left;
                    left = 0u;
                } else {
                    left -= v->spawn_left;
                    v->spawn_left = 0u;
                }
            }
        }
    }

    g->active_grains = 0;
    for (gi = 0; gi < AG_GRAIN_POOL; gi++) {
        ag_grain_grain_t *gr = &g->grain[gi];
        ag_grain_voice_t *v;
        if (!gr->active) {
            continue;
        }
        v = &g->voice[gr->voice];
        if (!v->active) {
            gr->active = 0;
            continue;
        }
        g->active_grains++;
        for (f = 0; f < frames; f++) {
            uint32_t idx;
            int16_t  smp;
            uint8_t  egain;
            int32_t  env, out, l, r;

            if (gr->i >= gr->len) {
                gr->active = 0;
                break;
            }
            idx = gr->pos >> 16;
            if (idx >= g->buf.frames) {
                gr->active = 0;
                break;
            }
            smp = g->buf.data[idx];
            egain = env_gain(g->params.texture, gr->i, gr->len);
            env = ((int32_t)egain * gr->amp * v->eg) >> 16; /* ~0..256 */
            out = ((int32_t)smp * env) >> 8;
            l = (out * (int32_t)gr->pan_l) >> 7;
            r = (out * (int32_t)gr->pan_r) >> 7;
            {
                int32_t al = (int32_t)stereo[f * 2] + l;
                int32_t ar = (int32_t)stereo[f * 2 + 1] + r;
                if (al > 32767) {
                    al = 32767;
                }
                if (al < -32768) {
                    al = -32768;
                }
                if (ar > 32767) {
                    ar = 32767;
                }
                if (ar < -32768) {
                    ar = -32768;
                }
                stereo[f * 2] = (int16_t)al;
                stereo[f * 2 + 1] = (int16_t)ar;
            }
            if (gr->reverse) {
                if (gr->pos > gr->step) {
                    gr->pos -= gr->step;
                } else {
                    gr->active = 0;
                    break;
                }
            } else {
                gr->pos += gr->step;
                if ((gr->pos >> 16) >= g->buf.frames) {
                    gr->active = 0;
                    break;
                }
            }
            gr->i++;
        }
    }
}

void ag_grain_viz_update(ag_grain_t *g)
{
    int i;
    uint8_t n = 0;
    for (i = 0; i < AG_GRAIN_POOL && n < AG_GRAIN_VIZ; i++) {
        const ag_grain_grain_t *gr = &g->grain[i];
        uint32_t idx;
        uint8_t  a;
        if (!gr->active || g->buf.frames == 0u) {
            continue;
        }
        idx = gr->pos >> 16;
        if (idx >= g->buf.frames) {
            continue;
        }
        a = env_gain(g->params.texture, gr->i, gr->len);
        g->viz[n].x_q15 =
            (uint16_t)((idx * 32767u) / (g->buf.frames > 1u ? g->buf.frames - 1u
                                                            : 1u));
        g->viz[n].y = (uint8_t)(gr->pan_r * 2u);
        g->viz[n].a = a;
        n++;
    }
    g->viz_n = n;
}
