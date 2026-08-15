/*
 * ArgonOS lightweight integer FM-ish synth.
 * SMS: cheap waves + linear envelopes.  MD OPN: 4-op + sin/TL + simple EG.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_fm.h"
#include "ag_dsp.h"

#define AG_FM_RAW_INST 0xffu

#define AG_SIN_BITS 10
#define AG_SIN_MASK ((1 << AG_SIN_BITS) - 1)
#define AG_TL_RES_LEN 256
#define AG_TL_TAB_LEN (13 * 2 * AG_TL_RES_LEN)
#define AG_ENV_QUIET (AG_TL_TAB_LEN >> 3)
#define AG_MAX_ATT 1023

#define AG_EG_ATT 4
#define AG_EG_DEC 3
#define AG_EG_SUS 2
#define AG_EG_REL 1
#define AG_EG_OFF 0

/* SLOT array indices (same as ym2612.c). */
#define OP_M1 0
#define OP_M2 1
#define OP_C1 2
#define OP_C2 3

#include "ag_fm_ym_tables.inc"

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

/* Per-sample EG steps at ~22050 Hz (simplified vs chip rate/3). */
static const uint16_t k_eg_att[32] = {
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 3, 4, 5, 6,
    8, 10, 14, 18, 24, 32, 42, 56, 76, 100, 140, 200, 300, 450, 700, 1024
};
static const uint16_t k_eg_dec[32] = {
    0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2,
    3, 3, 4, 5, 6, 8, 10, 13, 16, 20, 26, 32, 40, 52, 64, 80
};


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
    if (c->inst == AG_FM_RAW_INST) {
        return;
    }
    if (c->inst == 0) {
        uint8_t ml = (uint8_t)(fm->patch[0] & 0x0f);
        c->ar = (uint8_t)((fm->patch[4] >> 4) & 0x0f);
        c->dr = (uint8_t)(fm->patch[4] & 0x0f);
        c->sl = (uint8_t)((fm->patch[6] >> 4) & 0x0f);
        c->rr = (uint8_t)(fm->patch[6] & 0x0f);
        c->mul = (uint16_t)((ml == 0) ? 1u : (uint16_t)ml) * 2u;
        c->mod_mul = 0;
        c->fb = 0;
    } else {
        uint8_t i = (uint8_t)(c->inst & 0x0f);
        c->ar = k_preset_ar[i];
        c->dr = k_preset_dr[i];
        c->sl = k_preset_sl[i];
        c->rr = k_preset_rr[i];
        c->mul = k_preset_mul[i];
        c->mod_mul = 0;
        c->fb = 0;
    }
}

static uint32_t step_from(ag_fm_t *fm, uint16_t fnum, uint8_t block,
                          uint16_t mul_x2)
{
    uint32_t tuned;
    uint32_t clk_div;
    uint32_t rate;
    uint32_t base;
    uint32_t step;
    uint32_t div = fm->clk_div ? fm->clk_div : AG_FM_CLKDIV_OPLL;

    clk_div = fm->clock / div;
    if (clk_div == 0u) {
        clk_div = 1u;
    }
    rate = fm->rate ? fm->rate : 1u;
    if (mul_x2 == 0u) {
        mul_x2 = 1u;
    }

    if (div >= AG_FM_CLKDIV_OPN) {
        /*
         * OPN with 20-bit phase (sin idx = phase>>10), ear-matched to MDYM:
         * previous 24-bit path used base·mul_x2·4; scale by 2^(20-24).
         */
        if (block == 0u) {
            tuned = (uint32_t)fnum >> 1;
        } else {
            tuned = (uint32_t)fnum << (block - 1u);
        }
        base = (tuned / rate) * clk_div + ((tuned % rate) * clk_div) / rate;
        if (mul_x2 > 1u && base > (0xffffffffu / (uint32_t)mul_x2)) {
            return 0xffffffffu;
        }
        return (base * (uint32_t)mul_x2) / 4u;
    }

    /* OPLL / SMS: fnum·2^block, wave idx = phase>>16. */
    tuned = ((uint32_t)fnum << block);
    base = (tuned / rate) * clk_div + ((tuned % rate) * clk_div) / rate;
    step = (base / 2u) * (uint32_t)mul_x2;
    if (step > (0xffffffffu >> 13)) {
        step = 0xffffffffu >> 13;
    }
    return step << 13;
}

static void refresh_step_sms(ag_fm_t *fm, int ch)
{
    ag_fm_chan_t *c = &fm->ch[ch];
    c->step = step_from(fm, c->fnum, c->block, c->mul);
    if (c->mod_mul != 0u) {
        c->mod_step = step_from(fm, c->fnum, c->block, c->mod_mul);
    } else {
        c->mod_step = 0;
    }
}

static void refresh_step_opn(ag_fm_t *fm, int ch)
{
    ag_fm_chan_t *c = &fm->ch[ch];
    int s;
    for (s = 0; s < AG_FM_OPS; s++) {
        c->op[s].step = step_from(fm, c->fnum, c->block, c->op[s].mul);
    }
}

static void refresh_step(ag_fm_t *fm, int ch)
{
    if (ch < 0 || ch >= AG_FM_CHANNELS) {
        return;
    }
    if (fm->ch[ch].inst == AG_FM_RAW_INST) {
        refresh_step_opn(fm, ch);
    } else {
        refresh_step_sms(fm, ch);
    }
}

void ag_fm_init(ag_fm_t *fm, uint32_t clock, uint32_t sample_rate)
{
    ag_dsp_zero(fm, sizeof(*fm));
    fm->clock = clock ? clock : 3579545u;
    fm->rate = sample_rate ? sample_rate : 22050u;
    fm->clk_div = AG_FM_CLKDIV_OPLL;
    ag_fm_reset(fm);
}

void ag_fm_set_clk_div(ag_fm_t *fm, uint32_t div)
{
    int i;
    fm->clk_div = div ? div : AG_FM_CLKDIV_OPLL;
    for (i = 0; i < AG_FM_CHANNELS; i++) {
        refresh_step(fm, i);
    }
}

void ag_fm_reset(ag_fm_t *fm)
{
    int i, s;
    for (i = 0; i < AG_FM_CHANNELS; i++) {
        ag_dsp_zero(&fm->ch[i], sizeof(fm->ch[i]));
        fm->ch[i].mul = 2;
        fm->ch[i].vol = 15;
        fm->ch[i].ar = 12;
        fm->ch[i].dr = 6;
        fm->ch[i].sl = 4;
        fm->ch[i].rr = 7;
        for (s = 0; s < AG_FM_OPS; s++) {
            fm->ch[i].op[s].mul = 2;
            fm->ch[i].op[s].tl = 0x7f;
            fm->ch[i].op[s].ar = 31;
            fm->ch[i].op[s].dr = 0;
            fm->ch[i].op[s].sl = 0;
            fm->ch[i].op[s].rr = 15;
            fm->ch[i].op[s].eg = AG_MAX_ATT;
            fm->ch[i].op[s].eg_state = AG_EG_OFF;
        }
    }
    ag_dsp_zero(fm->patch, sizeof(fm->patch));
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
    c->fnum = (uint16_t)(fnum & 0x7ffu);
    c->block = block;
    refresh_step(fm, ch);
}

static void opn_key_on(ag_fm_op_t *o)
{
    if (!o->key) {
        o->phase = 0;
        o->eg = AG_MAX_ATT;
    }
    o->key = 1;
    o->eg_state = AG_EG_ATT;
}

static void opn_key_off(ag_fm_op_t *o)
{
    if (o->key) {
        o->key = 0;
        if (o->eg_state != AG_EG_OFF) {
            o->eg_state = AG_EG_REL;
        }
    }
}

void ag_fm_set_key(ag_fm_t *fm, int ch, int on, int sus)
{
    ag_fm_chan_t *c;
    if (ch < 0 || ch >= AG_FM_CHANNELS) {
        return;
    }
    c = &fm->ch[ch];
    c->sus = sus ? 1u : 0u;
    if (c->inst == AG_FM_RAW_INST) {
        ag_fm_set_opn_key(fm, ch, on ? 0x0fu : 0u);
        return;
    }
    if (on) {
        if (!c->key) {
            c->env = 0;
            c->phase = 0;
            c->mod_phase = 0;
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

void ag_fm_set_opn_key(ag_fm_t *fm, int ch, uint8_t op_mask)
{
    ag_fm_chan_t *c;
    /* YM $28 bits → SLOT array: bit0 M1, bit1 C1, bit2 M2, bit3 C2 */
    static const int k_slot[4] = {OP_M1, OP_C1, OP_M2, OP_C2};
    int i;

    if (ch < 0 || ch >= AG_FM_CHANNELS) {
        return;
    }
    c = &fm->ch[ch];
    c->inst = AG_FM_RAW_INST;
    for (i = 0; i < 4; i++) {
        ag_fm_op_t *o = &c->op[k_slot[i]];
        if (op_mask & (1u << i)) {
            opn_key_on(o);
        } else {
            opn_key_off(o);
        }
    }
    c->key = (op_mask != 0u) ? 1u : 0u;
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

void ag_fm_set_opn_op(ag_fm_t *fm, int ch, int slot, uint16_t mul_x2, uint8_t tl,
                      uint8_t ar, uint8_t dr, uint8_t sl, uint8_t rr)
{
    ag_fm_op_t *o;
    if (ch < 0 || ch >= AG_FM_CHANNELS || slot < 0 || slot >= AG_FM_OPS) {
        return;
    }
    fm->ch[ch].inst = AG_FM_RAW_INST;
    o = &fm->ch[ch].op[slot];
    o->mul = mul_x2 ? mul_x2 : 1u;
    o->tl = (uint8_t)(tl & 0x7fu);
    o->ar = (uint8_t)(ar & 0x1fu);
    o->dr = (uint8_t)(dr & 0x1fu);
    o->sl = (uint8_t)(sl & 0x0fu);
    o->rr = (uint8_t)(rr & 0x0fu);
    refresh_step_opn(fm, ch);
}

void ag_fm_set_alg(ag_fm_t *fm, int ch, uint8_t alg, uint8_t fb)
{
    if (ch < 0 || ch >= AG_FM_CHANNELS) {
        return;
    }
    fm->ch[ch].inst = AG_FM_RAW_INST;
    fm->ch[ch].alg = (uint8_t)(alg & 7u);
    fm->ch[ch].fb = (uint8_t)(fb & 7u);
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

static int32_t op_calc(uint32_t phase, unsigned env, int32_t pm)
{
    uint32_t p = (env << 3) +
                 ag_fm_sin_tab[((phase >> AG_SIN_BITS) + ((uint32_t)pm >> 1)) &
                               AG_SIN_MASK];
    if (p >= AG_TL_TAB_LEN) {
        return 0;
    }
    return ag_fm_tl_tab[p];
}

static int32_t op_calc1(uint32_t phase, unsigned env, int32_t pm)
{
    uint32_t p = (env << 3) +
                 ag_fm_sin_tab[((phase + (uint32_t)pm) >> AG_SIN_BITS) & AG_SIN_MASK];
    if (p >= AG_TL_TAB_LEN) {
        return 0;
    }
    return ag_fm_tl_tab[p];
}

static unsigned env_out(const ag_fm_op_t *o)
{
    unsigned e = (unsigned)o->eg + ((unsigned)o->tl << 3);
    if (e > AG_MAX_ATT) {
        e = AG_MAX_ATT;
    }
    return e;
}

static void eg_advance(ag_fm_op_t *o)
{
    int sl_level;
    int rate;

    switch (o->eg_state) {
    case AG_EG_ATT:
        rate = (int)k_eg_att[o->ar & 31u];
        if (rate <= 0) {
            break;
        }
        o->eg = (int16_t)(o->eg - rate);
        if (o->eg <= 0) {
            o->eg = 0;
            o->eg_state = AG_EG_DEC;
        }
        break;
    case AG_EG_DEC:
        rate = (int)k_eg_dec[o->dr & 31u];
        sl_level = (o->sl == 15u) ? AG_MAX_ATT : ((int)o->sl << 5);
        o->eg = (int16_t)(o->eg + rate);
        if (o->eg >= sl_level) {
            o->eg = (int16_t)sl_level;
            o->eg_state = AG_EG_SUS;
        }
        break;
    case AG_EG_SUS:
        /* No D2R in v1 — hold level (percussive patches use REL). */
        break;
    case AG_EG_REL:
        rate = (int)k_eg_dec[((o->rr << 1) + 1u) & 31u];
        o->eg = (int16_t)(o->eg + rate);
        if (o->eg >= AG_MAX_ATT) {
            o->eg = AG_MAX_ATT;
            o->eg_state = AG_EG_OFF;
        }
        break;
    default:
        break;
    }
}

static int32_t render_opn_chan(ag_fm_chan_t *c)
{
    int32_t m2 = 0, c1 = 0, c2 = 0, mem = 0, out = 0;
    int32_t *om1;
    int32_t *oc1;
    int32_t *om2;
    int32_t *memc;
    int32_t *carrier = &out;
    unsigned eg;
    int32_t fb_in;
    int s;

    for (s = 0; s < AG_FM_OPS; s++) {
        eg_advance(&c->op[s]);
    }

    switch (c->alg & 7u) {
    case 0:
        om1 = &c1;
        oc1 = &mem;
        om2 = &c2;
        memc = &m2;
        break;
    case 1:
        om1 = &mem;
        oc1 = &mem;
        om2 = &c2;
        memc = &m2;
        break;
    case 2:
        om1 = &c2;
        oc1 = &mem;
        om2 = &c2;
        memc = &m2;
        break;
    case 3:
        om1 = &c1;
        oc1 = &mem;
        om2 = &c2;
        memc = &c2;
        break;
    case 4:
        om1 = &c1;
        oc1 = carrier;
        om2 = &c2;
        memc = &mem;
        break;
    case 5:
        om1 = 0;
        oc1 = carrier;
        om2 = carrier;
        memc = &m2;
        break;
    case 6:
        om1 = &c1;
        oc1 = carrier;
        om2 = carrier;
        memc = &mem;
        break;
    default: /* 7 */
        om1 = carrier;
        oc1 = carrier;
        om2 = carrier;
        memc = &mem;
        break;
    }

    *memc = c->mem;

    fb_in = c->op1_out[0] + c->op1_out[1];
    c->op1_out[0] = c->op1_out[1];
    if (!om1) {
        /* ALG5: M1 feeds C1, M2, C2 */
        mem = c1 = c2 = c->op1_out[0];
    } else {
        *om1 += c->op1_out[0];
    }

    c->op1_out[1] = 0;
    eg = env_out(&c->op[OP_M1]);
    if (eg < AG_ENV_QUIET) {
        if (c->fb == 0u) {
            fb_in = 0;
        }
        c->op1_out[1] = op_calc1(c->op[OP_M1].phase, eg, fb_in << c->fb);
    }

    eg = env_out(&c->op[OP_M2]);
    if (eg < AG_ENV_QUIET) {
        *om2 += op_calc(c->op[OP_M2].phase, eg, m2);
    }

    eg = env_out(&c->op[OP_C1]);
    if (eg < AG_ENV_QUIET) {
        *oc1 += op_calc(c->op[OP_C1].phase, eg, c1);
    }

    eg = env_out(&c->op[OP_C2]);
    if (eg < AG_ENV_QUIET) {
        *carrier += op_calc(c->op[OP_C2].phase, eg, c2);
    }

    c->mem = mem;

    for (s = 0; s < AG_FM_OPS; s++) {
        c->op[s].phase += c->op[s].step;
    }

    if (out > 8191) {
        out = 8191;
    } else if (out < -8192) {
        out = -8192;
    }
    return out;
}

static int32_t render_sms_chan(ag_fm_t *fm, ag_fm_chan_t *c)
{
    int32_t  amp;
    int16_t  w;
    uint8_t  wave;
    uint32_t phase;

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
        return 0;
    }

    phase = c->phase;
    if (c->mod_step != 0u && (c->fb != 0u || c->mod_mul != 0u)) {
        int16_t mod_w;
        int32_t mod_amp;
        int32_t fm_mod;
        c->mod_phase += c->mod_step;
        mod_w = wave_sample(0, c->mod_phase);
        mod_amp = (127 - (int32_t)c->mod_tl);
        if (mod_amp < 0) {
            mod_amp = 0;
        }
        fm_mod = ((int32_t)mod_w * mod_amp) >> 7;
        if (c->fb > 0u) {
            fm_mod = fm_mod << (c->fb > 4u ? 4 : (int)c->fb);
        } else {
            fm_mod = fm_mod >> 2;
        }
        phase += (uint32_t)fm_mod;
    }

    c->phase += c->step;
    if (c->inst == 0) {
        wave = (uint8_t)((fm->patch[3] >> 3) & 3u);
    } else {
        wave = k_preset_wave[c->inst & 0x0f];
    }
    w = wave_sample(wave, phase);
    amp = ((int32_t)c->env * (15 - (int32_t)c->vol)) / 15;
    return ((int32_t)w * amp) / 65536;
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
            if (c->inst == AG_FM_RAW_INST) {
                mix += render_opn_chan(c);
            } else {
                mix += render_sms_chan(fm, c);
            }
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
