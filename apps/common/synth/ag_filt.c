/*
 * Cheap Chamberlin SVF, integer.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_filt.h"

#include "ag_dsp.h"

void ag_filt_reset(ag_filt_t *f)
{
    if (f == 0) {
        return;
    }
    f->low = 0;
    f->band = 0;
}

int32_t ag_filt_tick(ag_filt_t *f, int32_t in)
{
    int32_t fcoeff;
    int32_t q;
    int32_t high;
    int32_t notch;
    int     c;
    int     r;
    if (f == 0) {
        return in;
    }
    c = ag_clampi((int)f->cutoff, 1, 127);
    r = ag_clampi((int)f->reso, 0, 120);
    /* ~20 Hz..8 kHz-ish at 22 kHz: f = 8..180 */
    fcoeff = 8 + (c * 172) / 127;
    q = 130 - r;
    if (q < 8) {
        q = 8;
    }
    f->low += (fcoeff * f->band) >> 8;
    high = in - f->low - ((f->band * q) >> 7);
    f->band += (fcoeff * high) >> 8;
    /* mild saturate state */
    if (f->low > 40000) {
        f->low = 40000;
    }
    if (f->low < -40000) {
        f->low = -40000;
    }
    if (f->band > 40000) {
        f->band = 40000;
    }
    if (f->band < -40000) {
        f->band = -40000;
    }
    notch = high + f->low;
    switch (f->mode) {
    case AG_FILT_HP:
        return high;
    case AG_FILT_BP:
        return f->band;
    case AG_FILT_NOTCH:
        return notch;
    default:
        return f->low;
    }
}
