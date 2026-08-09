/*
 * ArgonOS lightweight integer FM-ish synth.
 * Not chip-accurate; cheap waves + envelopes for .AXE apps.  No float, no uint64.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_fm.h"

static const uint8_t k_preset_wave[16] = {
    0, 0, 1, 2, 0, 3, 1, 2, 0, 3, 1, 0, 2, 3, 1, 0
};
static const uint8_t k_preset_ar[16] = {
    15, 14, 13, 12, 14, 10, 12, 11, 13, 9, 14, 12, 10, 11, 13, 15
};
static const uint8_t k_preset_dr[16] = {
    4, 5, 6, 7, 4, 8, 5, 6, 4, 9, 5, 6, 7, 5, 4, 3
};
static const uint8_t k_preset_sl[16] = {
    4, 5, 6, 4, 3, 8, 5, 6, 4, 10, 5, 6, 7, 5, 4, 2
};
static const uint8_t k_preset_rr[16] = {
    6, 7, 8, 6, 5, 10, 7, 8, 6, 11, 7, 8, 9, 7, 6, 5
};
static const uint8_t k_preset_mul[16] = {
    2, 2, 4, 2, 6, 2, 4, 8, 2, 3, 4, 2, 6, 4, 2, 2
};

static void mem_zero(void *p, unsigned n)
{
    unsigned char *d = (unsigned char *)p;
    while (n--) {
        *d++ = 0;
    }
}

static int32_t rate_from_nibble(uint8_t n, int attack)
{
    static const int32_t k_att[16] = {
        40, 60, 90, 140, 220, 350, 550, 850,
        1300, 2000, 3200, 5000, 8000, 12000, 20000, 40000
    };
    static const int32_t k_dec[16] = {
        8, 12, 18, 28, 42, 65, 100, 150,
        230, 350, 550, 850, 1300, 2000, 3200, 5000
    };
    if (n > 15u) {
        n = 15;
    }
    return attack ? k_att[n] : k_dec[n];
}

static void apply_inst(ag_fm_t *fm, ag_fm_chan_t *c)
{
    if (c->inst == 0) {
        uint8_t ml = (uint8_t)(fm->patch[0] & 0x0f);
        c->ar = (uint8_t)((fm->patch[4] >> 4) & 0x0f);
        c->dr = (uint8_t)(fm->patch[4] & 0x0f);
        c->sl = (uint8_t)((fm->patch[6] >> 4) & 0x0f);
        c->rr = (uint8_t)(fm->patch[6] & 0x0f);
        c->mul = (uint16_t)((ml == 0) ? 1u : (uint16_t)ml) * 2u;
    } else {
        uint8_t i = (uint8_t)(c->inst & 0x0f);
        c->ar = k_preset_ar[i];
        c->dr = k_preset_dr[i];
        c->sl = k_preset_sl[i];
        c->rr = k_preset_rr[i];
        c->mul = k_preset_mul[i];
    }
}

static void refresh_step(ag_fm_t *fm, int ch)
{
    ag_fm_chan_t *c;
    uint32_t      tuned;
    uint32_t      clk_div;
    uint32_t      step;
    if (ch < 0 || ch >= AG_FM_CHANNELS) {
        return;
    }
    c = &fm->ch[ch];
    /* (fnum << block) * (clock/72) / rate << 13, then * mul/2 — all 32-bit */
    tuned = ((uint32_t)c->fnum << c->block);
    clk_div = fm->clock / 72u;
    if (clk_div == 0) {
        clk_div = 1;
    }
    step = tuned * clk_div;
    step /= fm->rate ? fm->rate : 1u;
    step <<= 13;
    step = (step / 2u) * (uint32_t)c->mul;
    c->step = step;
}

void ag_fm_init(ag_fm_t *fm, uint32_t clock, uint32_t sample_rate)
{
    mem_zero(fm, sizeof(*fm));
    fm->clock = clock ? clock : 3579545u;
    fm->rate = sample_rate ? sample_rate : 22050u;
    ag_fm_reset(fm);
}

void ag_fm_reset(ag_fm_t *fm)
{
    int i;
    for (i = 0; i < AG_FM_CHANNELS; i++) {
        mem_zero(&fm->ch[i], sizeof(fm->ch[i]));
        fm->ch[i].mul = 2;
        fm->ch[i].vol = 15;
        fm->ch[i].ar = 12;
        fm->ch[i].dr = 6;
        fm->ch[i].sl = 4;
        fm->ch[i].rr = 7;
    }
    mem_zero(fm->patch, sizeof(fm->patch));
    fm->rhythm = 0;
}

void ag_fm_set_patch(ag_fm_t *fm, const uint8_t patch[8])
{
    int i;
    for (i = 0; i < 8; i++) {
        fm->patch[i] = patch[i];
    }
    for (i = 0; i < AG_FM_CHANNELS; i++) {
        if (fm->ch[i].inst == 0) {
            apply_inst(fm, &fm->ch[i]);
            refresh_step(fm, i);
        }
    }
}

void ag_fm_set_rhythm(ag_fm_t *fm, uint8_t data)
{
    fm->rhythm = data;
}

void ag_fm_set_fnum(ag_fm_t *fm, int ch, uint16_t fnum, uint8_t block)
{
    ag_fm_chan_t *c;
    if (ch < 0 || ch >= AG_FM_CHANNELS) {
        return;
    }
    c = &fm->ch[ch];
    if (block > 7u) {
        block = 7;
    }
    c->fnum = (uint16_t)(fnum & 0x1ffu);
    c->block = block;
    refresh_step(fm, ch);
}

void ag_fm_set_key(ag_fm_t *fm, int ch, int on, int sus)
{
    ag_fm_chan_t *c;
    if (ch < 0 || ch >= AG_FM_CHANNELS) {
        return;
    }
    c = &fm->ch[ch];
    c->sus = sus ? 1u : 0u;
    if (on) {
        if (!c->key) {
            c->env = 0;
            c->phase = 0;
        }
        c->key = 1;
        c->env_target = 65535;
        c->env_rate = rate_from_nibble(c->ar, 1);
    } else {
        c->key = 0;
        c->env_target = 0;
        c->env_rate = rate_from_nibble(c->sus ? (uint8_t)(c->rr / 2u + 1u) : c->rr, 0);
    }
}

void ag_fm_set_inst_vol(ag_fm_t *fm, int ch, uint8_t inst, uint8_t vol)
{
    ag_fm_chan_t *c;
    if (ch < 0 || ch >= AG_FM_CHANNELS) {
        return;
    }
    c = &fm->ch[ch];
    c->inst = (uint8_t)(inst & 0x0f);
    c->vol = (uint8_t)(vol & 0x0f);
    apply_inst(fm, c);
    refresh_step(fm, ch);
}

static int16_t wave_sample(uint8_t wave, uint32_t phase)
{
    uint32_t idx = (phase >> 16) & 0xffu;
    int32_t  s;
    switch (wave) {
    case 1:
        if (idx & 0x80u) {
            return 0;
        }
        s = (int32_t)idx - 64;
        return (int16_t)(s * 400);
    case 2:
        return (int16_t)(((int32_t)idx - 128) * 200);
    case 3:
        return (idx & 0x80u) ? (int16_t)12000 : (int16_t)-12000;
    default:
        if (idx < 64u) {
            s = (int32_t)idx;
        } else if (idx < 192u) {
            s = 128 - (int32_t)idx;
        } else {
            s = (int32_t)idx - 256;
        }
        return (int16_t)(s * 280);
    }
}

void ag_fm_update(ag_fm_t *fm, int16_t *left, int16_t *right, int32_t samples)
{
    int32_t i, ch;
    if (!left || !right || samples <= 0) {
        return;
    }
    for (i = 0; i < samples; i++) {
        int32_t mix = 0;
        for (ch = 0; ch < AG_FM_CHANNELS; ch++) {
            ag_fm_chan_t *c = &fm->ch[ch];
            int32_t       amp;
            int16_t       w;
            uint8_t       wave;

            if (c->env < c->env_target) {
                c->env += c->env_rate;
                if (c->env > c->env_target) {
                    c->env = c->env_target;
                }
            } else if (c->env > c->env_target) {
                c->env -= c->env_rate;
                if (c->env < c->env_target) {
                    c->env = c->env_target;
                }
            } else if (c->key && c->env_target == 65535) {
                int32_t sus_lvl = (15 - (int32_t)c->sl) * (65535 / 15);
                if (sus_lvl < 0) {
                    sus_lvl = 0;
                }
                c->env_target = sus_lvl;
                c->env_rate = rate_from_nibble(c->dr, 0);
            }

            if (c->env <= 0 || c->step == 0) {
                continue;
            }
            c->phase += c->step;
            wave = (c->inst == 0) ? (uint8_t)((fm->patch[3] >> 3) & 3u)
                                  : k_preset_wave[c->inst & 0x0f];
            w = wave_sample(wave, c->phase);
            amp = ((int32_t)c->env * (15 - (int32_t)c->vol)) / 15;
            mix += ((int32_t)w * amp) / 65536;
        }
        if (mix > 32767) {
            mix = 32767;
        }
        if (mix < -32768) {
            mix = -32768;
        }
        left[i] = (int16_t)mix;
        right[i] = (int16_t)mix;
    }
}
