/*
 * VA oscillators — PolyBLEP saw/square, no libm.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_osc.h"

#include "ag_dsp.h"

void ag_osc_set_hz_x100(ag_osc_t *o, int32_t hz_x100, uint32_t rate)
{
    if (o == 0) {
        return;
    }
    o->step = ag_dsp_hz_to_step(hz_x100, rate);
}

/* PolyBLEP residual in Q15. t and dt are Q16 (0..65536 = 0..1). */
static int32_t polyblep_q15(uint32_t t_q16, uint32_t dt_q16)
{
    int32_t x;
    if (dt_q16 < 1u) {
        return 0;
    }
    if (t_q16 < dt_q16) {
        x = (int32_t)((t_q16 << 15) / dt_q16); /* 0..32767 */
        return ((x + x - ((x * x) >> 15) - 32767));
    }
    if (t_q16 + dt_q16 > 65536u) {
        x = (int32_t)(((int32_t)t_q16 - 65536) * 32767) / (int32_t)dt_q16;
        return (x + x + ((x * x) >> 15) + 32767);
    }
    return 0;
}

int32_t ag_osc_tick(ag_osc_t *o)
{
    uint32_t t, dt;
    int32_t  s;
    uint8_t  wave;
    if (o == 0) {
        return 0;
    }
    wave = o->wave;
    t = o->phase >> 16;
    dt = o->step >> 16;
    if (dt < 1u) {
        dt = 1u;
    }
    switch (wave) {
    case AG_OSC_SIN:
        s = ag_dsp_sin(o->phase);
        break;
    case AG_OSC_TRI: {
        uint32_t x = t;
        if (x < 32768u) {
            s = ((int32_t)x * 2) - 32767;
        } else {
            s = 98301 - ((int32_t)x * 2);
        }
        break;
    }
    case AG_OSC_NOISE:
        if (o->rng == 0u) {
            o->rng = 0xA3C59AC3u;
        }
        s = (int32_t)(int16_t)(ag_dsp_rng(&o->rng) >> 16);
        break;
    case AG_OSC_SQR: {
        uint32_t pw = 32768u;
        if (o->pwm < 64u) {
            pw = 16384u + (uint32_t)o->pwm * 256u;
        } else {
            pw = 32768u + (uint32_t)(o->pwm - 64u) * 256u;
        }
        if (pw < 4096u) {
            pw = 4096u;
        }
        if (pw > 61440u) {
            pw = 61440u;
        }
        s = (t < pw) ? 32767 : -32767;
        s += polyblep_q15(t, dt);
        {
            uint32_t t2 = t + (65536u - pw);
            s -= polyblep_q15(t2 & 0xffffu, dt);
        }
        break;
    }
    default: /* saw */
        s = ((int32_t)t * 2) - 32767;
        s -= polyblep_q15(t, dt);
        break;
    }
    o->phase += o->step;
    return s;
}
