/*
 * General N-op FM. SPDX-License-Identifier: Apache-2.0
 */
#include "ag_fmx.h"

void ag_fmx_reset(ag_fmx_t *f)
{
    int i, j;
    if (f == 0) {
        return;
    }
    ag_dsp_zero(f, sizeof(*f));
    f->n_ops = 2;
    f->carrier = 0x02; /* op1 carrier */
    f->route[0][1] = 80;
    f->fb = 32;
    for (i = 0; i < AG_FMX_MAX_OPS; i++) {
        f->op[i].ratio_x2 = (i == 0) ? 2 : 2;
        f->op[i].level = (i == 0) ? 90 : 100;
        (void)j;
    }
}

void ag_fmx_set_n(ag_fmx_t *f, int n)
{
    if (f == 0) {
        return;
    }
    if (n < 2) {
        n = 2;
    }
    if (n > AG_FMX_MAX_OPS) {
        n = AG_FMX_MAX_OPS;
    }
    f->n_ops = (uint8_t)n;
}

void ag_fmx_algo_stack(ag_fmx_t *f)
{
    int i, j;
    if (f == 0) {
        return;
    }
    for (i = 0; i < AG_FMX_MAX_OPS; i++) {
        for (j = 0; j < AG_FMX_MAX_OPS; j++) {
            f->route[i][j] = 0;
        }
    }
    for (i = 0; i + 1 < (int)f->n_ops; i++) {
        f->route[i][i + 1] = 90;
    }
    f->carrier = (uint8_t)(1u << (f->n_ops - 1u));
}

void ag_fmx_set_hz(ag_fmx_t *f, int32_t hz_x100, uint32_t rate)
{
    int i;
    if (f == 0) {
        return;
    }
    for (i = 0; i < (int)f->n_ops; i++) {
        int32_t hz = (hz_x100 * (int32_t)f->op[i].ratio_x2) / 2;
        f->op[i].step = ag_dsp_hz_to_step(hz, rate);
        /* Operator EGs are ticked per sample, so that is their tick rate. */
        ag_dsp_adsr_set_rate(&f->op[i].eg, rate);
    }
}

void ag_fmx_note_on(ag_fmx_t *f)
{
    int i;
    if (f == 0) {
        return;
    }
    for (i = 0; i < (int)f->n_ops; i++) {
        ag_dsp_adsr_on(&f->op[i].eg);
        f->op[i].phase = 0;
        f->op[i].out = 0;
    }
    f->fb_mem = 0;
}

void ag_fmx_note_off(ag_fmx_t *f)
{
    int i;
    if (f == 0) {
        return;
    }
    for (i = 0; i < (int)f->n_ops; i++) {
        ag_dsp_adsr_off(&f->op[i].eg);
    }
}

int32_t ag_fmx_tick(ag_fmx_t *f, uint8_t a, uint8_t d, uint8_t s, uint8_t r,
                    int gate)
{
    int     i, j;
    int32_t mix = 0;
    int     n;
    if (f == 0) {
        return 0;
    }
    n = (int)f->n_ops;
    for (i = 0; i < n; i++) {
        int32_t mod = 0;
        int32_t samp;
        int32_t amp;
        ag_dsp_adsr_tick(&f->op[i].eg, a, d, s, r, gate);
        /*
         * mod is a phase offset in 1/65536 of a cycle, not in phase units:
         * op->phase counts a whole cycle as 2^32, so adding a raw sample to
         * it moves the phase by about a millionth of a cycle and no sideband
         * is produced at all.  At depth 64 a full-scale modulator is ~1.2 rad.
         */
        for (j = 0; j < n; j++) {
            unsigned dep = f->route[j][i];
            if (dep > 0u) {
                mod += (f->op[j].out * (int32_t)dep) >> 7;
            }
        }
        if (i == 0 && f->fb > 0u) {
            mod += (f->fb_mem * (int32_t)f->fb) >> 7;
        }
        samp = ag_dsp_sin(f->op[i].phase + ((uint32_t)mod << 16));
        amp = (f->op[i].eg.level * (int32_t)f->op[i].level) >> 7;
        f->op[i].out = (samp * amp) >> 8;
        f->op[i].phase += f->op[i].step;
        if (((f->carrier >> i) & 1u) != 0u) {
            mix += f->op[i].out;
        }
    }
    f->fb_mem = f->op[0].out;
    return mix;
}
