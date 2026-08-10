/*
 * ag_fx — fixed-point delay → chorus → reverb (no libm).
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_fx.h"

#include <string.h>

#include <argon/argon.h>

/* ~370 ms mono @ 22.05 kHz */
#define FX_DELAY_MS_MAX 370u
/* Chorus delay line ~30 ms */
#define FX_CHORUS_MS 30u
/* Freeverb-ish comb base lengths @ 44100; scaled to rate */
static const uint16_t k_comb_base[4] = {1116u, 1188u, 1277u, 1356u};
static const uint16_t k_ap_base[2] = {556u, 441u};

static int16_t sat16(int32_t x)
{
    if (x > 32767) {
        return 32767;
    }
    if (x < -32768) {
        return -32768;
    }
    return (int16_t)x;
}

static uint32_t scale_len(uint32_t base441, uint32_t rate)
{
    uint32_t n = (base441 * rate + 22050u) / 44100u;
    if (n < 4u) {
        n = 4u;
    }
    return n;
}

void ag_fx_set_defaults(ag_fx_t *fx)
{
    if (fx == NULL) {
        return;
    }
    fx->enable = AG_FX_ALL;
    fx->delay_ms = 280u;
    fx->delay_fb = 48u;
    fx->delay_mix = 40u;
    fx->chorus_rate = 40u;
    fx->chorus_depth = 70u;
    fx->chorus_mix = 36u;
    fx->rev_room = 70u;
    fx->rev_damp = 50u;
    fx->rev_wet = 45u;
    fx->master_wet = 90u;
}

int ag_fx_init(ag_fx_t *fx, uint32_t rate)
{
    uint32_t i;
    uint32_t dcap, ccap;

    if (fx == NULL || rate < 8000u) {
        return -1;
    }
    memset(fx, 0, sizeof(*fx));
    fx->rate = rate;
    ag_fx_set_defaults(fx);

    dcap = (rate * FX_DELAY_MS_MAX) / 1000u + 8u;
    ccap = (rate * FX_CHORUS_MS) / 1000u + 8u;
    fx->delay_buf = (int16_t *)ag_malloc(sizeof(int16_t) * dcap);
    fx->chorus_buf = (int16_t *)ag_malloc(sizeof(int16_t) * ccap);
    if (fx->delay_buf == NULL || fx->chorus_buf == NULL) {
        ag_fx_free(fx);
        return -1;
    }
    fx->delay_cap = dcap;
    fx->chorus_cap = ccap;
    memset(fx->delay_buf, 0, sizeof(int16_t) * dcap);
    memset(fx->chorus_buf, 0, sizeof(int16_t) * ccap);

    for (i = 0; i < 4u; i++) {
        uint32_t len = scale_len(k_comb_base[i], rate);
        fx->comb_len[i] = (uint16_t)len;
        fx->comb_buf[i] = (int16_t *)ag_malloc(sizeof(int16_t) * len);
        fx->comb_buf[i + 4u] = (int16_t *)ag_malloc(sizeof(int16_t) * len);
        if (fx->comb_buf[i] == NULL || fx->comb_buf[i + 4u] == NULL) {
            ag_fx_free(fx);
            return -1;
        }
        memset(fx->comb_buf[i], 0, sizeof(int16_t) * len);
        memset(fx->comb_buf[i + 4u], 0, sizeof(int16_t) * len);
    }
    for (i = 0; i < 2u; i++) {
        uint32_t len = scale_len(k_ap_base[i], rate);
        fx->ap_len[i] = (uint16_t)len;
        fx->ap_buf[i] = (int16_t *)ag_malloc(sizeof(int16_t) * len);
        fx->ap_buf[i + 2u] = (int16_t *)ag_malloc(sizeof(int16_t) * len);
        if (fx->ap_buf[i] == NULL || fx->ap_buf[i + 2u] == NULL) {
            ag_fx_free(fx);
            return -1;
        }
        memset(fx->ap_buf[i], 0, sizeof(int16_t) * len);
        memset(fx->ap_buf[i + 2u], 0, sizeof(int16_t) * len);
    }

    fx->ready = 1u;
    return 0;
}

void ag_fx_free(ag_fx_t *fx)
{
    uint32_t i;
    if (fx == NULL) {
        return;
    }
    if (fx->delay_buf) {
        ag_free(fx->delay_buf);
    }
    if (fx->chorus_buf) {
        ag_free(fx->chorus_buf);
    }
    for (i = 0; i < 8u; i++) {
        if (fx->comb_buf[i]) {
            ag_free(fx->comb_buf[i]);
        }
    }
    for (i = 0; i < 4u; i++) {
        if (fx->ap_buf[i]) {
            ag_free(fx->ap_buf[i]);
        }
    }
    memset(fx, 0, sizeof(*fx));
}

void ag_fx_reset(ag_fx_t *fx)
{
    uint32_t i;
    if (fx == NULL || !fx->ready) {
        return;
    }
    fx->delay_w = 0;
    fx->chorus_w = 0;
    fx->chorus_phase = 0;
    if (fx->delay_buf && fx->delay_cap) {
        memset(fx->delay_buf, 0, sizeof(int16_t) * fx->delay_cap);
    }
    if (fx->chorus_buf && fx->chorus_cap) {
        memset(fx->chorus_buf, 0, sizeof(int16_t) * fx->chorus_cap);
    }
    for (i = 0; i < 4u; i++) {
        fx->comb_w[i] = 0;
        fx->comb_filter[i] = 0;
        fx->comb_filter[i + 4u] = 0;
        if (fx->comb_buf[i] && fx->comb_len[i]) {
            memset(fx->comb_buf[i], 0, sizeof(int16_t) * fx->comb_len[i]);
        }
        if (fx->comb_buf[i + 4u] && fx->comb_len[i]) {
            memset(fx->comb_buf[i + 4u], 0,
                   sizeof(int16_t) * fx->comb_len[i]);
        }
    }
    for (i = 0; i < 2u; i++) {
        fx->ap_w[i] = 0;
        if (fx->ap_buf[i] && fx->ap_len[i]) {
            memset(fx->ap_buf[i], 0, sizeof(int16_t) * fx->ap_len[i]);
        }
        if (fx->ap_buf[i + 2u] && fx->ap_len[i]) {
            memset(fx->ap_buf[i + 2u], 0, sizeof(int16_t) * fx->ap_len[i]);
        }
    }
}

void ag_fx_set_enable(ag_fx_t *fx, unsigned mask)
{
    if (fx) {
        fx->enable = mask & AG_FX_ALL;
    }
}

void ag_fx_set_params(ag_fx_t *fx, uint16_t delay_ms, uint8_t delay_fb,
                      uint8_t delay_mix, uint8_t chorus_rate,
                      uint8_t chorus_depth, uint8_t chorus_mix,
                      uint8_t rev_room, uint8_t rev_damp, uint8_t rev_wet,
                      uint8_t master_wet)
{
    if (fx == NULL) {
        return;
    }
    if (delay_ms < 1u) {
        delay_ms = 1u;
    }
    if (delay_ms > FX_DELAY_MS_MAX) {
        delay_ms = (uint16_t)FX_DELAY_MS_MAX;
    }
    fx->delay_ms = delay_ms;
    fx->delay_fb = delay_fb > 127u ? 127u : delay_fb;
    fx->delay_mix = delay_mix > 127u ? 127u : delay_mix;
    fx->chorus_rate = chorus_rate > 127u ? 127u : chorus_rate;
    fx->chorus_depth = chorus_depth > 127u ? 127u : chorus_depth;
    fx->chorus_mix = chorus_mix > 127u ? 127u : chorus_mix;
    fx->rev_room = rev_room > 127u ? 127u : rev_room;
    fx->rev_damp = rev_damp > 127u ? 127u : rev_damp;
    fx->rev_wet = rev_wet > 127u ? 127u : rev_wet;
    fx->master_wet = master_wet > 127u ? 127u : master_wet;
}

static void process_delay(ag_fx_t *fx, int16_t *L, int16_t *R)
{
    uint32_t dsamps =
        ((uint32_t)fx->delay_ms * fx->rate + 500u) / 1000u;
    uint32_t r_off;
    int32_t dryL = *L;
    int32_t dryR = *R;
    int32_t mono;
    int32_t tapL, tapR;
    int32_t fb;
    int32_t wetL, wetR;
    uint32_t mix = fx->delay_mix;
    uint32_t imix = 127u - mix;

    if (dsamps < 1u) {
        dsamps = 1u;
    }
    if (dsamps >= fx->delay_cap) {
        dsamps = fx->delay_cap - 1u;
    }
    r_off = dsamps / 17u; /* light stereo offset */
    if (r_off < 1u) {
        r_off = 1u;
    }

    tapL = fx->delay_buf[(fx->delay_w + fx->delay_cap - dsamps) % fx->delay_cap];
    tapR = fx->delay_buf[(fx->delay_w + fx->delay_cap - dsamps + r_off) %
                         fx->delay_cap];

    mono = (dryL + dryR) >> 1;
    fb = tapL + ((int32_t)fx->delay_fb * (tapL + tapR) >> 8);
    fx->delay_buf[fx->delay_w] = sat16(mono + (fb >> 1));
    fx->delay_w++;
    if (fx->delay_w >= fx->delay_cap) {
        fx->delay_w = 0;
    }

    wetL = (dryL * (int32_t)imix + tapL * (int32_t)mix) >> 7;
    wetR = (dryR * (int32_t)imix + tapR * (int32_t)mix) >> 7;
    *L = sat16(wetL);
    *R = sat16(wetR);
}

static void process_chorus(ag_fx_t *fx, int16_t *L, int16_t *R)
{
    /* Triangle LFO 0..65535; rate ~0.1..3 Hz from 0..127 */
    uint32_t step = 20u + (uint32_t)fx->chorus_rate * 6u;
    uint32_t ph = fx->chorus_phase;
    uint32_t tri;
    uint32_t base_d, depth, dL, dR;
    int32_t tapL, tapR;
    int32_t dryL = *L;
    int32_t dryR = *R;
    int32_t mono;
    uint32_t mix = fx->chorus_mix;
    uint32_t imix = 127u - mix;

    ph = (ph + step) & 65535u;
    fx->chorus_phase = ph;
    tri = (ph < 32768u) ? ph : (65535u - ph); /* 0..32767 */

    base_d = (fx->rate * 10u) / 1000u; /* ~10 ms */
    depth = ((fx->rate * 12u) / 1000u * (uint32_t)fx->chorus_depth) / 127u;
    dL = base_d + (tri * depth) / 32767u;
    dR = base_d + ((32767u - tri) * depth) / 32767u;
    if (dL >= fx->chorus_cap) {
        dL = fx->chorus_cap - 1u;
    }
    if (dR >= fx->chorus_cap) {
        dR = fx->chorus_cap - 1u;
    }

    mono = (dryL + dryR) >> 1;
    fx->chorus_buf[fx->chorus_w] = (int16_t)mono;
    tapL = fx->chorus_buf[(fx->chorus_w + fx->chorus_cap - dL) % fx->chorus_cap];
    tapR = fx->chorus_buf[(fx->chorus_w + fx->chorus_cap - dR) % fx->chorus_cap];
    fx->chorus_w++;
    if (fx->chorus_w >= fx->chorus_cap) {
        fx->chorus_w = 0;
    }

    *L = sat16((dryL * (int32_t)imix + tapL * (int32_t)mix) >> 7);
    *R = sat16((dryR * (int32_t)imix + tapR * (int32_t)mix) >> 7);
}

static int32_t comb_process(ag_fx_t *fx, int ch, int idx, int32_t in)
{
    /* ch 0=L 1=R; idx 0..3 */
    int bi = idx + ch * 4;
    uint16_t len = fx->comb_len[idx];
    uint16_t w = fx->comb_w[idx];
    int16_t *buf = fx->comb_buf[bi];
    int32_t y = buf[w];
    int32_t damp = fx->rev_damp;
    int32_t room = 90 + ((int32_t)fx->rev_room * 30) / 127; /* ~90..120 /128 */
    int32_t filtered;

    /* one-pole damp on feedback */
    filtered = fx->comb_filter[bi] +
               (((y - fx->comb_filter[bi]) * (128 - damp)) >> 7);
    fx->comb_filter[bi] = filtered;
    buf[w] = sat16(in + ((filtered * room) >> 7));
    /* advance write only once per stereo pair — caller advances after R */
    (void)len;
    return y;
}

static int32_t ap_process(ag_fx_t *fx, int ch, int idx, int32_t in)
{
    int bi = idx + ch * 2;
    uint16_t w = fx->ap_w[idx];
    int16_t *buf = fx->ap_buf[bi];
    int32_t bufout = buf[w];
    int32_t out = bufout - in;
    buf[w] = sat16(in + ((bufout * 5) >> 3)); /* ~0.625 feedback */
    return out;
}

static void process_reverb(ag_fx_t *fx, int16_t *L, int16_t *R)
{
    int32_t inL = *L;
    int32_t inR = *R;
    int32_t accL = 0;
    int32_t accR = 0;
    int32_t wetL, wetR;
    uint32_t mix = fx->rev_wet;
    uint32_t imix = 127u - mix;
    int i;

    /* input gain */
    inL = (inL * 3) >> 3;
    inR = (inR * 3) >> 3;

    for (i = 0; i < 4; i++) {
        accL += comb_process(fx, 0, i, inL);
        accR += comb_process(fx, 1, i, inR);
    }
    /* advance comb write heads once */
    for (i = 0; i < 4; i++) {
        fx->comb_w[i]++;
        if (fx->comb_w[i] >= fx->comb_len[i]) {
            fx->comb_w[i] = 0;
        }
    }

    accL >>= 2;
    accR >>= 2;
    for (i = 0; i < 2; i++) {
        accL = ap_process(fx, 0, i, accL);
        accR = ap_process(fx, 1, i, accR);
        fx->ap_w[i]++;
        if (fx->ap_w[i] >= fx->ap_len[i]) {
            fx->ap_w[i] = 0;
        }
    }

    wetL = (*L * (int32_t)imix + accL * (int32_t)mix) >> 7;
    wetR = (*R * (int32_t)imix + accR * (int32_t)mix) >> 7;
    *L = sat16(wetL);
    *R = sat16(wetR);
}

void ag_fx_process(ag_fx_t *fx, int16_t *stereo_io, int32_t frames)
{
    int32_t i;
    uint32_t mw, imw;

    if (fx == NULL || stereo_io == NULL || frames <= 0 || !fx->ready) {
        return;
    }
    if (fx->enable == 0u || fx->master_wet == 0u) {
        return;
    }

    mw = fx->master_wet;
    imw = 127u - mw;

    for (i = 0; i < frames; i++) {
        int16_t *s = stereo_io + i * 2;
        int16_t dryL = s[0];
        int16_t dryR = s[1];
        int16_t L = dryL;
        int16_t R = dryR;

        if (fx->enable & AG_FX_DELAY) {
            process_delay(fx, &L, &R);
        }
        if (fx->enable & AG_FX_CHORUS) {
            process_chorus(fx, &L, &R);
        }
        if (fx->enable & AG_FX_REVERB) {
            process_reverb(fx, &L, &R);
        }

        s[0] = sat16((dryL * (int32_t)imw + L * (int32_t)mw) >> 7);
        s[1] = sat16((dryR * (int32_t)imw + R * (int32_t)mw) >> 7);
    }
}
