/*
 * Host smoke: render the circuit solver to build/listen/ckt_*.wav.
 *
 * Not a test - what is asserted lives in host-tests/test_ckt.c.  This exists
 * because a valve stage can pass every numeric check and still sound wrong,
 * and because the one thing the numbers here cannot show is aliasing: the
 * whole argument for oversampling an overdrive is something you hear, not
 * something a peak measurement reports.  So the same four-stage preamp is
 * rendered twice, once solved at the output rate and once solved four times
 * faster and filtered down, and the difference is the point.
 *
 * Built by host-tests/CMakeLists.txt, so `argon tests` produces
 * build-host/ckt_smoke(.exe).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ag_ckt.h"
#include "ag_stage.h"
#include "ckt_circuits.h"

#define RATE   48000u
#define SECS   3u
#define FRAMES (RATE * SECS)
#define OS     4u

static void wr_u16(FILE *f, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    fwrite(b, 1, 2, f);
}

static void wr_u32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, f);
}

static int write_wav(const char *path, const int16_t *pcm, uint32_t frames)
{
    FILE          *f = fopen(path, "wb");
    const uint32_t data = frames * 2u;
    if (f == NULL) {
        return -1;
    }
    fwrite("RIFF", 1, 4, f);
    wr_u32(f, 36u + data);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    wr_u32(f, 16);
    wr_u16(f, 1); /* PCM   */
    wr_u16(f, 1); /* mono  */
    wr_u32(f, RATE);
    wr_u32(f, RATE * 2u);
    wr_u16(f, 2);
    wr_u16(f, 16);
    fwrite("data", 1, 4, f);
    wr_u32(f, data);
    fwrite(pcm, 2, frames, f);
    fclose(f);
    printf("  wrote %s (%u frames)\n", path, frames);
    return 0;
}

/*
 * A plucked note, because a sine tells you nothing about an amplifier.  Six
 * harmonics with the upper ones decaying faster, which is roughly what a
 * string does, at a peak of 0.25 V - a hot single coil.
 */
static float pluck(double t, double f0)
{
    static const double amp[6] = { 1.0, 0.55, 0.35, 0.22, 0.14, 0.09 };
    static const double dec[6] = { 1.2, 1.8, 2.4, 3.2, 4.2, 5.5 };
    double              s = 0.0;
    int                 h;
    for (h = 0; h < 6; h++) {
        s += amp[h] * exp(-dec[h] * t) * sin(2.0 * M_PI * f0 * (h + 1) * t);
    }
    return (float)(0.25 * s / 2.35);
}

/* One-pole DC blocker: the plate sits at its bias and a loudspeaker does not
 * want 170 V of it. */
typedef struct {
    float z;
} dcb_t;

static float dcb(dcb_t *d, float x, float a)
{
    d->z += (x - d->z) * a;
    return x - d->z;
}

/*
 * Windowed-sinc decimator, 22 kHz cut at the 192 kHz oversampled rate.
 *
 * The cutoff is in cycles per oversampled sample, so 22 kHz is 22/192 =
 * 0.1146 - and getting that wrong is not subtle.  The first version used 0.22,
 * reasoning about "just under half the output rate" and forgetting that half
 * of the *oversampled* rate is 0.5; the filter then cut at 42 kHz, removed
 * nothing that mattered, and the oversampled render measured 8 dB *more*
 * inharmonic content than the plain one.  Oversampling without a decimation
 * filter is not oversampling, it is four times the work for a worse answer.
 *
 * The transition has to fit between 20 kHz and 24 kHz, which is 0.021 of the
 * oversampled rate, and a Blackman window needs about 5.5/0.021 taps to do
 * that.  255 it is - this runs on a PC, and it has to be well clear of being
 * the limiting factor when the point is to measure something else.
 */
#define DEC_TAPS 255
static float s_dec[DEC_TAPS];

static void dec_init(unsigned os)
{
    const double fc = 22000.0 / (48000.0 * (double)os);
    double       sum = 0.0;
    int          i;
    for (i = 0; i < DEC_TAPS; i++) {
        const double n = (double)i - (DEC_TAPS - 1) / 2.0;
        const double sinc =
            (n == 0.0) ? 2.0 * fc : sin(2.0 * M_PI * fc * n) / (M_PI * n);
        const double w = 0.42 - 0.5 * cos(2.0 * M_PI * i / (DEC_TAPS - 1)) +
                         0.08 * cos(4.0 * M_PI * i / (DEC_TAPS - 1));
        s_dec[i] = (float)(sinc * w);
        sum += s_dec[i];
    }
    for (i = 0; i < DEC_TAPS; i++) {
        s_dec[i] = (float)(s_dec[i] / sum);
    }
}

static int16_t clip16(float v)
{
    if (v > 32767.0f) {
        return 32767;
    }
    if (v < -32768.0f) {
        return -32768;
    }
    return (int16_t)v;
}

/*
 * Render `stages` valves at `os` times the output rate, decimating if os > 1.
 * `tone` is a sine in Hz for the aliasing files, or 0 for the plucked note.
 *
 * The result is normalised to -3 dBFS rather than scaled by a fixed number.
 * A fixed scale had the one-stage file clipping at int16 on the pick attack,
 * which is a square wave nobody in the circuit asked for and exactly the sort
 * of thing that would then get blamed on the valve model.
 */
static void render_chain(const char *path, int stages, unsigned os, float gain,
                         double tone)
{
    ckt_chain_t *c = (ckt_chain_t *)calloc(1, sizeof(ckt_chain_t));
    float       *buf = (float *)calloc(FRAMES, sizeof(float));
    int16_t     *pcm = (int16_t *)calloc(FRAMES, sizeof(int16_t));
    const double fs = (double)RATE * (double)os;
    float        hist[DEC_TAPS];
    dcb_t        d = { 0.0f };
    uint32_t     i;
    unsigned     j;
    double       t = 0.0;
    float        peak = 0.0f;

    memset(hist, 0, sizeof(hist));
    dec_init(os);
    if (!ckt_build_chain(c, (float)fs, stages)) {
        printf("  %s: chain of %d did not build\n", path, stages);
        free(c);
        free(buf);
        free(pcm);
        return;
    }

    /* Bias settling: half a second of silence, not written out. */
    for (i = 0; i < (uint32_t)(fs * 0.5); i++) {
        ckt_chain_tick(c, 0.0f);
    }

    for (i = 0; i < FRAMES; i++) {
        float y = 0.0f;
        for (j = 0; j < os; j++) {
            const float in =
                tone > 0.0 ? gain * (float)(0.25 * sin(2.0 * M_PI * tone * t))
                           : gain * pluck(t, 110.0);
            const float v = ckt_chain_tick(c, in);
            t += 1.0 / fs;
            if (os == 1u) {
                y = v;
            } else {
                int k;
                for (k = DEC_TAPS - 1; k > 0; k--) {
                    hist[k] = hist[k - 1];
                }
                hist[0] = v;
            }
        }
        if (os > 1u) {
            float acc = 0.0f;
            int   k;
            for (k = 0; k < DEC_TAPS; k++) {
                acc += s_dec[k] * hist[k];
            }
            y = acc;
        }
        buf[i] = dcb(&d, y, 0.002f);
    }

    for (i = RATE / 10u; i < FRAMES; i++) { /* skip the DC blocker's own step */
        const float a = buf[i] < 0.0f ? -buf[i] : buf[i];
        if (a > peak) {
            peak = a;
        }
    }
    {
        const float g = peak > 0.0f ? 23000.0f / peak : 0.0f;
        for (i = 0; i < FRAMES; i++) {
            pcm[i] = clip16(buf[i] * g);
        }
    }

    write_wav(path, pcm, FRAMES);
    printf("    %d stage(s), os %u: peak %.1f V, %u Newton passes, "
           "%u unconverged (%.1f%%)\n",
           stages, os, (double)peak, ckt_chain_iters(c),
           ckt_chain_noncvg(c),
           100.0 * ckt_chain_noncvg(c) / (double)(FRAMES * os));
    free(c);
    free(buf);
    free(pcm);
}

/*
 * The same chain, baked.  Rendered so that the claim "the table is the same
 * model, not a simpler one" can be checked by ear as well as by the -63 dB the
 * host test measures: ckt_4stage.wav and ckt_4stage_baked.wav should be
 * indistinguishable.
 */
static void render_baked(const char *path, int stages, unsigned os, double tone,
                         int adaa)
{
    static ckt_baked_chain_t chain;
    static ag_ckt_t          scratch;
    /*
     * The probe is the signal that is about to be played, not a convenient
     * sine.  A steady tone never shows the model the attack of a note, and the
     * attack is where a valve stage goes furthest - bias shifting under grid
     * current, capacitors not yet caught up.  Baked against a sine, the same
     * four stages then ran off the edge of their tables fourteen thousand
     * times in three seconds of playing.
     */
    const int   probe_n = (int)RATE;
    float      *probe = (float *)calloc(probe_n, sizeof(float));
    float      *buf = (float *)calloc(FRAMES, sizeof(float));
    int16_t    *pcm = (int16_t *)calloc(FRAMES, sizeof(int16_t));
    const double fs = (double)RATE * (double)os;
    float        hist[DEC_TAPS];
    dcb_t        d = { 0.0f };
    uint32_t     i;
    unsigned     j;
    double       t = 0.0;
    float        peak = 0.0f;

    memset(hist, 0, sizeof(hist));
    dec_init(os);
    for (i = 0; i < (uint32_t)probe_n; i++) {
        const double t2 = (double)i / fs;
        probe[i] = tone > 0.0 ? (float)(0.25 * sin(2.0 * M_PI * tone * t2))
                              : pluck(t2, 110.0);
    }
    if (ckt_bake_chain(&chain, &scratch, (float)fs, stages, probe, probe_n,
                       6000, CKT_VARIANT_PLAIN, adaa) != 0) {
        printf("  %s: bake failed\n", path);
        free(probe);
        free(buf);
        free(pcm);
        return;
    }

    for (i = 0; i < FRAMES; i++) {
        float y = 0.0f;
        for (j = 0; j < os; j++) {
            const float in =
                tone > 0.0 ? (float)(0.25 * sin(2.0 * M_PI * tone * t))
                           : pluck(t, 110.0);
            const float v = ckt_baked_chain_tick(&chain, in);
            t += 1.0 / fs;
            if (os == 1u) {
                y = v;
            } else {
                int k;
                for (k = DEC_TAPS - 1; k > 0; k--) {
                    hist[k] = hist[k - 1];
                }
                hist[0] = v;
            }
        }
        if (os > 1u) {
            float acc = 0.0f;
            int   k;
            for (k = 0; k < DEC_TAPS; k++) {
                acc += s_dec[k] * hist[k];
            }
            y = acc;
        }
        buf[i] = dcb(&d, y, 0.002f);
    }

    for (i = RATE / 10u; i < FRAMES; i++) {
        const float a = buf[i] < 0.0f ? -buf[i] : buf[i];
        if (a > peak) {
            peak = a;
        }
    }
    {
        const float g = peak > 0.0f ? 23000.0f / peak : 0.0f;
        for (i = 0; i < FRAMES; i++) {
            pcm[i] = clip16(buf[i] * g);
        }
    }
    write_wav(path, pcm, FRAMES);
    printf("    %d baked stage(s), os %u: peak %.1f V, %u lookups off the "
           "edge of a table\n",
           stages, os, (double)peak, ckt_baked_chain_clamped(&chain));
    free(probe);
    free(buf);
    free(pcm);
}

static void render_diode(const char *path)
{
    ag_ckt_t *k = (ag_ckt_t *)calloc(1, sizeof(ag_ckt_t));
    int16_t  *pcm = (int16_t *)calloc(FRAMES, sizeof(int16_t));
    uint32_t  i;
    double    t = 0.0;
    const int out_node = ckt_build_diode_clipper(k, (float)RATE);

    for (i = 0; i < FRAMES; i++) {
        const float in = 20.0f * pluck(t, 110.0);
        pcm[i] = clip16(ag_ckt_tick(k, in, out_node) * 30000.0f);
        t += 1.0 / (double)RATE;
    }
    write_wav(path, pcm, FRAMES);
    free(k);
    free(pcm);
}

int main(void)
{
    printf("ckt_smoke: rendering to build/listen\n");
    render_chain("build/listen/ckt_1stage.wav", 1, 1u, 1.0f, 0.0);
    render_chain("build/listen/ckt_2stage.wav", 2, 1u, 1.0f, 0.0);
    render_chain("build/listen/ckt_4stage.wav", 4, 1u, 1.0f, 0.0);
    render_chain("build/listen/ckt_4stage_os4.wav", 4, OS, 1.0f, 0.0);
    render_baked("build/listen/ckt_4stage_baked.wav", 4, 1u, 0.0, 0);
    render_baked("build/listen/ckt_4stage_baked_os4.wav", 4, OS, 0.0, 0);
    /* The one that would actually ship: two times oversampled with
     * antialiasing, which measures better than four times without. */
    render_baked("build/listen/ckt_4stage_adaa_os2.wav", 4, 2u, 0.0, 1);
    render_diode("build/listen/ckt_diode.wav");

    /*
     * The aliasing pair.  3.7 kHz is chosen so that its harmonics do not fold
     * back onto multiples of itself: at a 48 kHz rate the ninth harmonic lands
     * at 33.3 kHz and reflects to 14.7 kHz, which is not a multiple of 3.7 and
     * so cannot hide inside the distortion.  Solved at the output rate, that
     * inharmonic rubbish is the output; solved four times faster and filtered
     * down, it is not there to fold.
     */
    render_chain("build/listen/ckt_alias_os1.wav", 4, 1u, 1.0f, 3700.0);
    render_chain("build/listen/ckt_alias_os4.wav", 4, OS, 1.0f, 3700.0);

    /*
     * The same pair through the baked model, because a table has an aliasing
     * story of its own: bilinear interpolation makes the transfer curve
     * piecewise linear, and the corners between the pieces are discontinuities
     * in slope, which generate their own high frequencies.  Whether that
     * matters next to the harmonics the valve is making on purpose is a
     * question with a number, so here is the number.
     */
    render_baked("build/listen/ckt_alias_baked_os1.wav", 4, 1u, 3700.0, 0);
    render_baked("build/listen/ckt_alias_baked_os4.wav", 4, OS, 3700.0, 0);
    render_baked("build/listen/ckt_alias_adaa_os2.wav", 4, 2u, 3700.0, 1);
    return 0;
}
