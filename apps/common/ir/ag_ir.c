/*
 * Uniform partitioned convolution (overlap-add FFT) for IR FX.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_ir.h"

#include <string.h>

#include <argon/argon.h>

#include "ag_dsp.h"
#include "ag_fft.h"

static uint32_t ir_max_frames(uint32_t rate)
{
    uint32_t n = (rate * AG_IR_MAX_MS) / 1000u;
    if (n < AG_IR_BLOCK) {
        n = AG_IR_BLOCK;
    }
    return n;
}

static void free_spectra(ag_ir_t *ir)
{
    if (ir->H != NULL) {
        ag_free(ir->H);
        ir->H = NULL;
    }
    if (ir->X != NULL) {
        ag_free(ir->X);
        ir->X = NULL;
    }
    ir->parts = 0;
    ir->ir_frames = 0;
    ir->ready = 0;
}

void ag_ir_free(ag_ir_t *ir)
{
    if (ir == NULL) {
        return;
    }
    free_spectra(ir);
    memset(ir, 0, sizeof(*ir));
}

void ag_ir_reset(ag_ir_t *ir)
{
    if (ir == NULL) {
        return;
    }
    ir->x_pos = 0;
    memset(ir->overlap, 0, sizeof(ir->overlap));
    if (ir->X != NULL && ir->parts > 0u) {
        memset(ir->X, 0,
               ir->parts * AG_IR_FFT * 2u * (uint32_t)sizeof(int16_t));
    }
}

int ag_ir_init(ag_ir_t *ir, uint32_t rate)
{
    if (ir == NULL || rate < 8000u || rate > 48000u) {
        return -1;
    }
    memset(ir, 0, sizeof(*ir));
    ir->rate = rate;
    ir->wet = 100u;
    ir->gain = 64u;
    return 0;
}

void ag_ir_set_wet(ag_ir_t *ir, uint8_t wet)
{
    if (ir != NULL) {
        ir->wet = wet > 127u ? 127u : wet;
    }
}

void ag_ir_set_gain(ag_ir_t *ir, uint8_t gain)
{
    if (ir != NULL) {
        ir->gain = gain > 127u ? 127u : gain;
    }
}

void ag_ir_set_bypass(ag_ir_t *ir, int on)
{
    if (ir != NULL) {
        ir->bypass = on ? 1u : 0u;
    }
}

static int16_t *resample_mono(const int16_t *src, uint32_t src_n, uint32_t src_rate,
                              uint32_t dst_rate, uint32_t max_frames,
                              uint32_t *out_n)
{
    uint32_t dst_n, i;
    int16_t *dst;
    if (src == NULL || src_n == 0u || src_rate == 0u || dst_rate == 0u) {
        return NULL;
    }
    if (src_rate == dst_rate) {
        dst_n = src_n > max_frames ? max_frames : src_n;
        dst = (int16_t *)ag_malloc(sizeof(int16_t) * dst_n);
        if (dst == NULL) {
            return NULL;
        }
        memcpy(dst, src, sizeof(int16_t) * dst_n);
        *out_n = dst_n;
        return dst;
    }
    dst_n = (uint32_t)((uint64_t)src_n * dst_rate / src_rate);
    if (dst_n < 8u) {
        dst_n = 8u;
    }
    if (dst_n > max_frames) {
        dst_n = max_frames;
    }
    dst = (int16_t *)ag_malloc(sizeof(int16_t) * dst_n);
    if (dst == NULL) {
        return NULL;
    }
    for (i = 0; i < dst_n; i++) {
        uint32_t si = (uint32_t)((uint64_t)i * src_rate / dst_rate);
        if (si >= src_n) {
            si = src_n - 1u;
        }
        dst[i] = src[si];
    }
    *out_n = dst_n;
    return dst;
}

static void normalize_peak(int16_t *x, uint32_t n, int32_t target_peak)
{
    int32_t peak = 1;
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t a = x[i] < 0 ? -(int32_t)x[i] : (int32_t)x[i];
        if (a > peak) {
            peak = a;
        }
    }
    if (peak <= target_peak) {
        return;
    }
    for (i = 0; i < n; i++) {
        x[i] = ag_sat16(((int32_t)x[i] * target_peak) / peak);
    }
}

static uint32_t isqrt64(uint64_t v)
{
    uint64_t rem = v;
    uint64_t root = 0;
    uint64_t bit = 1ull << 62;
    while (bit > rem) {
        bit >>= 2;
    }
    while (bit != 0ull) {
        if (rem >= root + bit) {
            rem -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)root;
}

/*
 * Scale the IR by its energy, not its peak.
 *
 * The gain a convolution applies is sqrt(sum h^2), not max|h|: a 500 ms noise
 * tail carries hundreds of times the energy of a 20 ms cabinet with the same
 * peak sample.  Peak-normalising both left the long ones running the output
 * into the rail on every sample - the wet signal stopped depending on the
 * input level at all, which is the one thing a linear filter must never do.
 */
#define AG_IR_ENERGY 16384 /* sqrt(sum h^2): wet lands ~6..12 dB under dry */

static void normalize_energy(int16_t *x, uint32_t n, int32_t target)
{
    uint64_t e2 = 0;
    uint32_t i, rms;
    for (i = 0; i < n; i++) {
        const int32_t v = x[i];
        e2 += (uint64_t)(v * v);
    }
    rms = isqrt64(e2);
    if (rms < 1u) {
        return;
    }
    for (i = 0; i < n; i++) {
        x[i] = ag_sat16((int32_t)(((int64_t)x[i] * target) / (int64_t)rms));
    }
    /* No single tap at the rail either, whatever the energy scaling did. */
    normalize_peak(x, n, 32000);
}

/* Forward FFT of partition p of mono[] into re/im. */
static int part_fft(const int16_t *mono, uint32_t frames, uint32_t p,
                    int32_t *re, int32_t *im)
{
    const uint32_t off = p * AG_IR_BLOCK;
    uint32_t       i;

    for (i = 0; i < AG_IR_FFT; i++) {
        re[i] = 0;
        im[i] = 0;
    }
    for (i = 0; i < AG_IR_BLOCK; i++) {
        const uint32_t idx = off + i;
        re[i] = (idx < frames) ? (int32_t)mono[idx] : 0;
    }
    return ag_fft_cplx_i32(re, im, (int)AG_IR_FFT, 1);
}

static int build_partitions(ag_ir_t *ir, int16_t *mono, uint32_t frames)
{
    uint32_t parts, p, i;
    uint32_t bytes;
    int32_t  biggest = 1;
    uint8_t  shift = 0;
    static int32_t re[AG_IR_FFT];
    static int32_t im[AG_IR_FFT];

    free_spectra(ir);
    if (frames < 4u) {
        return -1;
    }
    parts = (frames + AG_IR_BLOCK - 1u) / AG_IR_BLOCK;
    if (parts < 1u) {
        parts = 1u;
    }

    /* Pass one: how big do the bins actually get for this IR? */
    for (p = 0; p < parts; p++) {
        if (part_fft(mono, frames, p, re, im) != 0) {
            return -1;
        }
        for (i = 0; i < AG_IR_FFT; i++) {
            const int32_t a = re[i] < 0 ? -re[i] : re[i];
            const int32_t b = im[i] < 0 ? -im[i] : im[i];
            if (a > biggest) {
                biggest = a;
            }
            if (b > biggest) {
                biggest = b;
            }
        }
    }
    while (biggest > 30000 && shift < 15u) {
        biggest >>= 1;
        shift++;
    }

    bytes = parts * AG_IR_FFT * 2u * (uint32_t)sizeof(int16_t);
    ir->H = (int16_t *)ag_malloc(bytes);
    ir->X = (int16_t *)ag_malloc(bytes);
    if (ir->H == NULL || ir->X == NULL) {
        free_spectra(ir);
        return -1;
    }
    memset(ir->H, 0, bytes);
    memset(ir->X, 0, bytes);

    /* Pass two: store, using the whole of int16. */
    for (p = 0; p < parts; p++) {
        int16_t *Hp = ir->H + p * AG_IR_FFT * 2u;
        if (part_fft(mono, frames, p, re, im) != 0) {
            free_spectra(ir);
            return -1;
        }
        for (i = 0; i < AG_IR_FFT; i++) {
            Hp[i * 2u] = ag_sat16(re[i] >> shift);
            Hp[i * 2u + 1u] = ag_sat16(im[i] >> shift);
        }
    }

    ir->h_shift = shift;
    ir->parts = parts;
    ir->ir_frames = frames;
    ir->ready = 1;
    ag_ir_reset(ir);
    return 0;
}

int ag_ir_load(ag_ir_t *ir, const int16_t *mono, uint32_t frames, uint32_t src_rate)
{
    int16_t *tmp;
    uint32_t n = 0;
    int rc;
    if (ir == NULL || mono == NULL || frames < 4u) {
        return -1;
    }
    if (src_rate < 8000u) {
        src_rate = ir->rate;
    }
    tmp = resample_mono(mono, frames, src_rate, ir->rate, ir_max_frames(ir->rate),
                        &n);
    if (tmp == NULL) {
        return -1;
    }
    normalize_energy(tmp, n, AG_IR_ENERGY);
    rc = build_partitions(ir, tmp, n);
    ag_free(tmp);
    return rc;
}

static uint32_t s_rng = 0xA5F1523Du;

static int16_t rnd16(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return (int16_t)((int32_t)(s_rng >> 16) - 32768);
}

/*
 * Reseed per load.  The generator is file-scope, so without this the same
 * preset built a different room every time it was selected - the reverb
 * changed under the player, and no rendering of it could be reproduced.
 */
static void rnd_seed(int preset)
{
    s_rng = 0xA5F1523Du + (uint32_t)preset * 0x9E3779B9u;
}

int ag_ir_load_preset(ag_ir_t *ir, int preset)
{
    uint32_t n, i;
    int16_t *buf;
    int32_t env, decay_q;
    int rc;

    if (ir == NULL) {
        return -1;
    }
    if (preset < 0) {
        preset = 0;
    }
    if (preset > 4) {
        preset = 4;
    }
    rnd_seed(preset);

    n = ir_max_frames(ir->rate);
    buf = (int16_t *)ag_malloc(sizeof(int16_t) * n);
    if (buf == NULL) {
        return -1;
    }
    memset(buf, 0, sizeof(int16_t) * n);

    if (preset >= 3) {
        /* Short cabinet-ish IR: 20 ms, LF bump + HF roll-off. */
        uint32_t cab_n = (ir->rate * 20u) / 1000u;
        int32_t  lp = 0;
        if (cab_n < 32u) {
            cab_n = 32u;
        }
        if (cab_n > n) {
            cab_n = n;
        }
        buf[0] = 24000;
        for (i = 1; i < cab_n; i++) {
            int32_t noise = (int32_t)rnd16() >> 4;
            int32_t env = (int32_t)((cab_n - i) * 20000u / cab_n);
            int32_t x = ((noise * env) >> 15);
            /* one-pole LP: dark=stronger */
            lp += ((x - lp) * (preset == 3 ? 24 : 48)) >> 7;
            buf[i] = ag_sat16(lp);
        }
        n = cab_n;
        normalize_energy(buf, n, AG_IR_ENERGY);
        rc = build_partitions(ir, buf, n);
        ag_free(buf);
        return rc;
    }
    if (preset == 0) {
        decay_q = 32768 - (32768 * 8) / (int32_t)(ir->rate / 3u + 1u);
    } else if (preset == 1) {
        decay_q = 32768 - (32768 * 5) / (int32_t)(ir->rate / 2u + 1u);
    } else {
        decay_q = 32768 - (32768 * 6) / (int32_t)(ir->rate / 2u + 1u);
    }
    if (decay_q < 30000) {
        decay_q = 30000;
    }
    if (decay_q > 32766) {
        decay_q = 32766;
    }

    env = 28000;
    buf[0] = 28000;
    for (i = 1; i < n; i++) {
        int32_t noise = (int32_t)rnd16() >> 3;
        int32_t early = 0;
        if (i == ir->rate / 40u || i == ir->rate / 28u || i == ir->rate / 19u) {
            early = env / 2;
        }
        if (preset == 2 && (i % (ir->rate / 90u + 1u)) == 0u) {
            early += env / 5;
        }
        env = (env * decay_q) >> 15;
        buf[i] = ag_sat16(early + ((noise * env) >> 15));
        if (env < 40) {
            break;
        }
    }

    normalize_energy(buf, n, AG_IR_ENERGY);
    rc = build_partitions(ir, buf, n);
    ag_free(buf);
    return rc;
}

void ag_ir_process_block(ag_ir_t *ir, const int16_t *mono_in, int16_t *stereo_out)
{
    uint32_t i, p, parts;
    /* Static: ~8 KB — keep off the small app stack. */
    static int32_t re[AG_IR_FFT];
    static int32_t im[AG_IR_FFT];
    static int32_t yre[AG_IR_FFT];
    static int32_t yim[AG_IR_FFT];
    int16_t *slot;

    if (ir == NULL || mono_in == NULL || stereo_out == NULL) {
        return;
    }

    if (!ir->ready || ir->bypass || ir->parts == 0u) {
        for (i = 0; i < AG_IR_BLOCK; i++) {
            stereo_out[i * 2u] = mono_in[i];
            stereo_out[i * 2u + 1u] = mono_in[i];
        }
        return;
    }

    parts = ir->parts;

    /* FFT input block (zero-padded to FFT) */
    memset(re, 0, sizeof(re));
    memset(im, 0, sizeof(im));
    for (i = 0; i < AG_IR_BLOCK; i++) {
        re[i] = (int32_t)mono_in[i];
    }
    (void)ag_fft_cplx_i32(re, im, (int)AG_IR_FFT, 1);

    slot = ir->X + ir->x_pos * AG_IR_FFT * 2u;
    for (i = 0; i < AG_IR_FFT; i++) {
        slot[i * 2u] = ag_sat16(re[i] >> 9);
        slot[i * 2u + 1u] = ag_sat16(im[i] >> 9);
    }

    /* Y = Σ X[pos−p] * H[p] */
    memset(yre, 0, sizeof(yre));
    memset(yim, 0, sizeof(yim));
    for (p = 0; p < parts; p++) {
        uint32_t xi = (ir->x_pos + parts - p) % parts;
        const int16_t *Hp = ir->H + p * AG_IR_FFT * 2u;
        const int16_t *Xp = ir->X + xi * AG_IR_FFT * 2u;
        for (i = 0; i < AG_IR_FFT; i++) {
            int32_t xr = Xp[i * 2u];
            int32_t xii = Xp[i * 2u + 1u];
            int32_t hr = Hp[i * 2u];
            int32_t hi = Hp[i * 2u + 1u];
            /* Round, not truncate: an arithmetic shift floors, and the bias
             * would add up over every partition into a DC step. */
            yre[i] += (xr * hr - xii * hi + 16384) >> 15;
            yim[i] += (xr * hi + xii * hr + 16384) >> 15;
        }
    }

    for (i = 0; i < AG_IR_FFT; i++) {
        re[i] = yre[i];
        im[i] = yim[i];
    }
    (void)ag_fft_cplx_i32(re, im, (int)AG_IR_FFT, 0);

    /*
     * X is scaled /512 and H by 2^h_shift; the unnormalized inverse puts the
     * 512 back, so what comes out is (x*h) / (2^h_shift * 32768).  Shifting
     * left by h_shift leaves (x*h)/32768 - the IR read as Q15 gains, which is
     * what normalize_energy set it up to be.
     */
    for (i = 0; i < AG_IR_BLOCK; i++) {
        int32_t wet = (re[i] << ir->h_shift) + (int32_t)ir->overlap[i];
        int32_t dry = (int32_t)mono_in[i];
        int32_t m;
        wet = (wet * (int32_t)ir->gain) >> 6;
        wet = ag_sat16(wet);
        ir->overlap[i] = ag_sat16(re[i + AG_IR_BLOCK] << ir->h_shift);
        m = dry + (((wet - dry) * (int32_t)ir->wet) >> 7);
        m = ag_sat16(m);
        stereo_out[i * 2u] = (int16_t)m;
        stereo_out[i * 2u + 1u] = (int16_t)m;
    }

    ir->x_pos = (ir->x_pos + 1u) % parts;
}
