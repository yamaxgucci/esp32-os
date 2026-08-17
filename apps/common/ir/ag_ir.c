/*
 * Uniform partitioned convolution (overlap-add FFT) for IR FX.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_ir.h"

#include <string.h>

#include <argon/argon.h>

#include "ag_dsp.h"
#include "ag_fft.h"

#ifdef AG_IR_TRACE
#include <stdio.h>
static uint32_t s_trace;
#endif

static uint32_t ir_frames_for(uint32_t rate, uint32_t ms)
{
    uint32_t n = (rate * ms) / 1000u;
    if (n < AG_IR_BLOCK) {
        n = AG_IR_BLOCK;
    }
    return n;
}

static uint32_t ir_max_frames(uint32_t rate)
{
    return ir_frames_for(rate, AG_IR_MAX_MS);
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
    if (ir->x_sh != NULL) {
        ag_free(ir->x_sh);
        ir->x_sh = NULL;
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
    if (ir->x_sh != NULL && ir->parts > 0u) {
        memset(ir->x_sh, AG_IR_EMPTY, ir->parts);
    }
}

int ag_ir_init(ag_ir_t *ir, uint32_t rate)
{
    if (ir == NULL || rate < 8000u || rate > 48000u) {
        return -1;
    }
    memset(ir, 0, sizeof(*ir));
    ir->rate = rate;
    ir->wet = AG_IR_WET_REVERB;
    ir->gain = 64u;
    return 0;
}

/*
 * 128, not 127.
 *
 * The mix divides by 128, so a maximum of 127 left 1/128 of the dry signal in
 * the output - forty-two decibels down, which is nothing under a reverb and
 * ruinous under a cabinet.  A speaker is forty decibels down at 10 kHz, so
 * that leak was *louder* than the convolution across the whole top end: the
 * cabinet appeared to filter nothing, and the fizz it was supposed to remove
 * went straight past it.
 */
void ag_ir_set_wet(ag_ir_t *ir, uint8_t wet)
{
    if (ir != NULL) {
        ir->wet = wet > AG_IR_WET_MAX ? AG_IR_WET_MAX : wet;
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

/*
 * How far a block whose samples sum to `asum` in absolute value can be lifted
 * before ag_fft_cplx_i32 could run it out of int32.
 *
 * Every value anywhere in the transform is a sum of the inputs with unit
 * coefficients, so no part of it can exceed the sum of their magnitudes -
 * that bound is exact, and it is what makes the lift safe rather than
 * hopeful.  The first version of this used max|x| * 512, the worst case a
 * single bin can reach, and threw away three bits on real audio and ten on an
 * impulse response for the privilege.
 *
 * The lift matters because the transform truncates a Q15 product in every
 * butterfly and the stages that follow amplify it: it injects about 6 LSB of
 * hash at the output whatever went in.  Against int16 taps whose rms is a few
 * hundred that is 40 dB of noise on everything the convolution ever produces,
 * and it does not go down when the music does.
 */
static int fft_headroom(uint32_t asum)
{
    int pre = 0;
    if (asum < 1u) {
        asum = 1u;
    }
    while ((asum << (pre + 1)) <= 0x40000000u && pre < 24) {
        pre++;
    }
    return pre;
}

/* Forward FFT of partition p of mono[], lifted by `pre`, into re/im. */
static int part_fft(const int16_t *mono, uint32_t frames, uint32_t p, int pre,
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
        re[i] = (idx < frames) ? ((int32_t)mono[idx] << pre) : 0;
    }
    /* Real input, so half a transform does it; same bins, same scale. */
    return ag_fft_real_fwd(re, im, (int)AG_IR_FFT);
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

    /*
     * One lift for the whole impulse response, from its heaviest partition:
     * h_shift is picked from all of them together, so they have to arrive on
     * one scale.
     */
    {
        uint32_t worst = 1;
        for (p = 0; p < parts; p++) {
            uint32_t sum = 0;
            for (i = 0; i < AG_IR_BLOCK; i++) {
                const uint32_t idx = p * AG_IR_BLOCK + i;
                if (idx < frames) {
                    sum += (uint32_t)(mono[idx] < 0 ? -(int32_t)mono[idx]
                                                    : (int32_t)mono[idx]);
                }
            }
            if (sum > worst) {
                worst = sum;
            }
        }
        ir->h_pre = (uint8_t)fft_headroom(worst);
    }

    /* Pass one: how big do the bins actually get for this IR? */
    for (p = 0; p < parts; p++) {
        if (part_fft(mono, frames, p, (int)ir->h_pre, re, im) != 0) {
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
    while (biggest > 30000 && shift < 31u) {
        biggest >>= 1;
        shift++;
    }

    bytes = parts * AG_IR_FFT * 2u * (uint32_t)sizeof(int16_t);
    ir->H = (int16_t *)ag_malloc(bytes);
    ir->X = (int16_t *)ag_malloc(bytes);
    ir->x_sh = (int8_t *)ag_malloc(parts);
    if (ir->H == NULL || ir->X == NULL || ir->x_sh == NULL) {
        free_spectra(ir);
        return -1;
    }
    memset(ir->H, 0, bytes);
    memset(ir->X, 0, bytes);
    memset(ir->x_sh, AG_IR_EMPTY, parts);

    /* Pass two: store, using the whole of int16. */
    for (p = 0; p < parts; p++) {
        int16_t *Hp = ir->H + p * AG_IR_FFT * 2u;
        if (part_fft(mono, frames, p, (int)ir->h_pre, re, im) != 0) {
            free_spectra(ir);
            return -1;
        }
        for (i = 0; i < AG_IR_FFT; i++) {
            Hp[i * 2u] = ag_sat16(re[i] >> shift);
            Hp[i * 2u + 1u] = ag_sat16(im[i] >> shift);
        }
    }

    /*
     * How far one partition's product has to come down so that all of them
     * still fit the accumulator: one bit per doubling of the partition count,
     * plus one, since |xr*hr| + |xii*hi| already reaches 2^30.
     */
    ir->p_shift = 1u;
    while (((uint32_t)1 << (ir->p_shift - 1u)) < parts) {
        ir->p_shift++;
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
    if (rc == 0) {
        /*
         * Fully wet.  An impulse response someone went and loaded is a
         * cabinet far more often than a room, and the init default of
         * AG_IR_WET_REVERB puts 22% of the dry signal - about -13 dB - around
         * the convolution.  Under a reverb that is inaudible; under a cabinet
         * it is the loudest thing above 6 kHz, because the dry guitar still
         * has its whole top end while a loudspeaker is 17 dB down at 8 kHz.
         * The measured result was a "cabinet" within 1-2 dB of dry in every
         * band and an amplifier that sounded like it hissed.
         */
        ir->wet = AG_IR_WET_MAX;
    }
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

    /* Fixed length, not the cap: see AG_IR_PRESET_MS. */
    n = ir_frames_for(ir->rate, AG_IR_PRESET_MS);
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
        if (rc == 0) {
            /* A cabinet is fully wet: see ag_ir_load. */
            ir->wet = AG_IR_WET_MAX;
        }
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
    if (rc == 0) {
        /*
         * Set, not left alone: the cabinet presets above take wet to the top,
         * and a preset switch from one of those back to a room has to bring
         * the room's dry signal back with it.
         */
        ir->wet = AG_IR_WET_REVERB;
    }
    return rc;
}

#if defined(__GNUC__)
#define AG_IR_NOINLINE __attribute__((noinline))
#else
#define AG_IR_NOINLINE
#endif

/*
 * One partition into the accumulator: Y += X * H, bin by bin.
 *
 * This is where a long impulse response spends its time - 512 bins per
 * partition, 87 partitions for a second at 22 kHz - so it gets its own
 * function, and `noinline` is load-bearing.  Written inside the block loop, at
 * -Os the compiler ran out of registers and spilled the loop counter, the
 * rounding constant and both shift setups to the stack on every bin: 34
 * instructions where the arithmetic is four multiplies and two adds.  Xtensa
 * gives a called function a fresh register window, so out here it fits.
 *
 * The round rather than truncate is not cosmetic: an arithmetic shift floors,
 * and the half-a-bit bias would add up over every partition into a DC step.
 */
AG_IR_NOINLINE static void mac_part(int32_t *yre, int32_t *yim,
                                    const int16_t *Xp, const int16_t *Hp,
                                    uint32_t sh)
{
    const int32_t rnd = (int32_t)1 << (sh - 1u);
    uint32_t      i;

    for (i = 0; i < AG_IR_FFT; i++) {
        const int32_t xr = Xp[0], xii = Xp[1];
        const int32_t hr = Hp[0], hi = Hp[1];
        *yre += (xr * hr - xii * hi + rnd) >> sh;
        *yim += (xr * hi + xii * hr + rnd) >> sh;
        yre++;
        yim++;
        Xp += 2;
        Hp += 2;
    }
}

void ag_ir_process_block(ag_ir_t *ir, const int16_t *mono_in, int16_t *stereo_out)
{
    uint32_t i, p, parts;
    int      xref, ygain, pre;
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

    /*
     * FFT input block (zero-padded to FFT), lifted to the top of int32 for
     * the same reason part_fft lifts the impulse response: the transform's
     * rounding is a fixed number of units, so the further the signal sits
     * from the bottom of the word the less of it there is.  A quiet block
     * gets lifted further, which is what stops the noise from staying put
     * while the music goes down past it.
     */
    /* re only: the transform below is the real-input one and writes the whole
     * of im itself.  What re needs is the zero pad above the block. */
    memset(re, 0, sizeof(re));
    {
        uint32_t asum = 0;
        for (i = 0; i < AG_IR_BLOCK; i++) {
            asum += (uint32_t)(mono_in[i] < 0 ? -(int32_t)mono_in[i]
                                              : (int32_t)mono_in[i]);
        }
        pre = fft_headroom(asum);
        for (i = 0; i < AG_IR_BLOCK; i++) {
            re[i] = (int32_t)mono_in[i] << pre;
        }
    }
    (void)ag_fft_real_fwd(re, im, (int)AG_IR_FFT);

    /*
     * Store this block's spectrum with a shift of its own, picked from what
     * the block actually contains rather than from what a full-scale block
     * would.  The old fixed >> 9 threw away nine bits whatever came in, so a
     * note fading to -40 dB kept seven bits of its spectrum and the error sat
     * where it was: a hiss that does not go down with the signal, which is
     * the one kind of noise the ear will not forgive.
     *
     * What is recorded per slot is the shift net of the lift above, so the
     * rest of the arithmetic sees the same scale it always did.
     *
     * This int16 is what the engine finally runs into: cutting it by three
     * bits costs 12 dB of accuracy, which is the measurement that says the
     * rest of the path is no longer the limit.  The whole of it is used -
     * |xr*hr| + |xii*hi| reaches 2 * 32767 * 30000 = 1.97e9 and int32 holds
     * 2.15e9, because H is bounded to 30000 rather than to its own rail.
     */
    {
        int32_t xmax = 1;
        int     xs = 0;
        int32_t half;
        for (i = 0; i < AG_IR_FFT; i++) {
            const int32_t a = re[i] < 0 ? -re[i] : re[i];
            const int32_t b = im[i] < 0 ? -im[i] : im[i];
            if (a > xmax) {
                xmax = a;
            }
            if (b > xmax) {
                xmax = b;
            }
        }
        while (xmax > 32767 && xs < 31) {
            xmax >>= 1;
            xs++;
        }
        half = xs > 0 ? ((int32_t)1 << (xs - 1)) : 0;
        slot = ir->X + ir->x_pos * AG_IR_FFT * 2u;
        for (i = 0; i < AG_IR_FFT; i++) {
            slot[i * 2u] = ag_sat16((re[i] + half) >> xs);
            slot[i * 2u + 1u] = ag_sat16((im[i] + half) >> xs);
        }
        ir->x_sh[ir->x_pos] = (int8_t)(xs - pre);
    }

    /*
     * The partitions no longer share a scale, so bring each one down to the
     * coarsest in the delay line before adding it in.  Slots that have never
     * been written hold zeros and are marked AG_IR_EMPTY: they contribute
     * nothing, and letting their nominal shift into the reference would drag
     * the scale of a genuinely quiet start-up block down with it.
     */
    xref = -128;
    for (p = 0; p < parts; p++) {
        if (ir->x_sh[p] != AG_IR_EMPTY && (int)ir->x_sh[p] > xref) {
            xref = (int)ir->x_sh[p];
        }
    }

    /*
     * Y = Σ X[pos−p] * H[p], kept as wide as the accumulator allows.
     *
     * The obvious >> 15 here is what a Q15 multiply wants, and it is wrong:
     * it leaves the sum around 2^15 when int32 could hold 2^30, and the
     * output shift at the end then has to put the missing bits back by
     * multiplying - so the result comes out quantised in steps of 128, which
     * is 42 dB of noise on a signal that is otherwise exact.  p_shift is the
     * least the partition count allows instead, and the surplus is taken off
     * once, after the sum, where it costs one bit rather than seven.
     */
    memset(yre, 0, sizeof(yre));
    memset(yim, 0, sizeof(yim));
    for (p = 0; p < parts; p++) {
        uint32_t xi = (ir->x_pos + parts - p) % parts;
        const int16_t *Hp = ir->H + p * AG_IR_FFT * 2u;
        const int16_t *Xp = ir->X + xi * AG_IR_FFT * 2u;
        uint32_t sh;
        if (ir->x_sh[xi] == AG_IR_EMPTY) {
            continue;
        }
        sh = ir->p_shift + (uint32_t)(xref - (int)ir->x_sh[xi]);
        if (sh > 31u) {
            continue; /* older than the scale can express: it is inaudible */
        }
        mac_part(yre, yim, Xp, Hp, sh);
    }

    /*
     * The inverse transform multiplies by up to 512 on the way through, so
     * hand it a spectrum scaled to leave that much room and no more.  Whatever
     * is borrowed here is given back in the output shift below.
     */
    {
        /*
         * Same exact bound as the forward direction: no value in the inverse
         * can exceed the sum of the magnitudes going in.  Summed in a uint64
         * because 512 bins of a full accumulator do not fit a uint32, and a
         * bound that wraps is not a bound.
         *
         * 2^29 rather than 2^30 because the inverse below is the real-signal
         * one, and its first act is to combine each bin with its mirror and
         * rotate the difference - which can reach 2.41 times a single bin, so
         * the bound needs a bit of room above it.  That bit is given straight
         * back in the output shift, which is written in terms of ygain.
         */
        uint64_t ysum = 0;
        for (i = 0; i < AG_IR_FFT; i++) {
            const int32_t a = yre[i] < 0 ? -yre[i] : yre[i];
            const int32_t b = yim[i] < 0 ? -yim[i] : yim[i];
            ysum += (uint64_t)a + (uint64_t)b;
        }
        if (ysum < 1u) {
            ysum = 1u;
        }
        ygain = 0;
        while ((ysum << 1) <= 0x20000000ull && ygain < 24) {
            ysum <<= 1;
            ygain++;
        }
        while (ysum > 0x20000000ull && ygain > -24) {
            ysum >>= 1;
            ygain--;
        }
        /* Bins 0..n/2 only: the rest are the mirror, and the real inverse
         * works them out for itself rather than reading them. */
        if (ygain > 0) {
            const uint32_t g = (uint32_t)ygain;
            for (i = 0; i <= AG_IR_FFT / 2u; i++) {
                re[i] = yre[i] << g;
                im[i] = yim[i] << g;
            }
        } else if (ygain < 0) {
            const uint32_t g = (uint32_t)(-ygain);
            const int32_t  r = (int32_t)1 << (g - 1u);
            for (i = 0; i <= AG_IR_FFT / 2u; i++) {
                re[i] = (yre[i] + r) >> g;
                im[i] = (yim[i] + r) >> g;
            }
        } else {
            for (i = 0; i <= AG_IR_FFT / 2u; i++) {
                re[i] = yre[i];
                im[i] = yim[i];
            }
        }
    }
    (void)ag_fft_real_inv(re, im, (int)AG_IR_FFT);

    /*
     * X is scaled by 2^xref, H by 2^h_shift and the product by 2^p_shift; the
     * spectrum was then scaled by 2^ygain and the unnormalized inverse
     * multiplies by 512.  What comes out is
     * (x*h) * 2^(9 + ygain - xref - h_shift - p_shift), so undoing exactly
     * that and leaving one factor of 32768 gives (x*h)/32768 - the IR read as
     * Q15 gains, which is what normalize_energy set it up to be.
     *
     * The shift is usually to the right now, which is the whole point: the
     * result arrives with bits to spare and rounds down to the output word
     * instead of arriving short and being multiplied up into steps.
     */
    {
        int osh = (int)ir->h_shift - (int)ir->h_pre + xref +
                  (int)ir->p_shift - ygain - 24;
        uint32_t lsh, rsh;
        int32_t  rnd;
        /* Only reachable on a block that is already silence either way, but a
         * shift wider than the word is undefined and would not stay quiet. */
        if (osh > 24) {
            osh = 24;
        }
        if (osh < -30) {
            osh = -30;
        }
        lsh = osh > 0 ? (uint32_t)osh : 0u;
        rsh = osh < 0 ? (uint32_t)(-osh) : 0u;
        rnd = rsh > 0u ? ((int32_t)1 << (rsh - 1u)) : 0;
        for (i = 0; i < AG_IR_BLOCK; i++) {
            int32_t wet = (((re[i] << lsh) + rnd) >> rsh) + ir->overlap[i];
            int32_t dry = (int32_t)mono_in[i];
            int32_t m;
            ir->overlap[i] = ((re[i + AG_IR_BLOCK] << lsh) + rnd) >> rsh;
            /* Rounded, not floored: these two shifts ran on every sample, so
             * their half-a-bit each showed up as a DC step in the output. */
            wet = (wet * (int32_t)ir->gain + 32) >> 6;
            wet = ag_sat16(wet);
            m = dry + (((wet - dry) * (int32_t)ir->wet + 64) >> 7);
            m = ag_sat16(m);
            stereo_out[i * 2u] = (int16_t)m;
            stereo_out[i * 2u + 1u] = (int16_t)m;
        }
    }

#ifdef AG_IR_TRACE
    if ((s_trace++ % 900u) == 5u) {
        int32_t  ymax = 0, omax = 0;
        uint32_t j;
        for (j = 0; j < AG_IR_FFT; j++) {
            const int32_t a = yre[j] < 0 ? -yre[j] : yre[j];
            const int32_t b = re[j] < 0 ? -re[j] : re[j];
            if (a > ymax) {
                ymax = a;
            }
            if (b > omax) {
                omax = b;
            }
        }
        printf("      trace: pre %2d net %3d xref %3d | h_pre %2u h_shift %2u "
               "p_shift %u | ymax %10d ygain %3d | ifft max %10d osh %3d\n",
               pre, (int)ir->x_sh[ir->x_pos], xref, ir->h_pre, ir->h_shift,
               ir->p_shift, ymax, ygain, omax,
               (int)ir->h_shift - (int)ir->h_pre + xref + (int)ir->p_shift -
                   ygain - 24);
    }
#endif

    ir->x_pos = (ir->x_pos + 1u) % parts;
}
