/*
 * ckt_play - run a real recording through the circuit model and through the
 * reverb, and write the results out.
 *
 *   build-host/ckt_play <input.wav> [peak volts] [outdir]
 *
 * Renders, from one clean guitar DI:
 *
 *   *_crunch.wav      two 12AX7 stages, JCM800 2203 front end, x2 with ADAA
 *   *_crunch_cab.wav  the same through a cabinet impulse response
 *   *_hall.wav        the dry signal through the hall reverb
 *   *_spring.wav      the dry signal through the spring reverb
 *   *_crunch_hall.wav the amplifier into the hall
 *
 * A preamp with no cabinet after it is not what an amplifier sounds like - the
 * speaker is a 5 kHz brick wall and most of what makes distortion listenable
 * rather than buzzy - so the cabinet render is the one to judge the model by.
 *
 * Built by host-tests/CMakeLists.txt.  Not a test: what is asserted lives in
 * host-tests/test_ckt.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ag_ckt.h"
#include "ag_ir.h"
#include "ag_stage.h"
#include "ckt_circuits.h"

#define DEC_TAPS 255

static unsigned    OS = 2u; /* oversampling for the nonlinear part */
static const char *s_cab_ir = NULL; /* a measured cabinet, if one was given */
static double      s_tone = 0.0;    /* analyse a sine instead of a recording */
static int         s_quiet = 0;     /* the tone drops 30 dB partway through */
/*
 * How much wider than the observed driving range each table is baked.  Every
 * bit of it is resolution spent where the signal rarely goes, so it is worth
 * knowing what it buys: too little and the run clamps at the table edge,
 * which is the model guessing.  ckt_play prints the clamp count, so this can
 * be set by measurement rather than by nerves.
 */
static float       s_margin = CKT_BAKE_MARGIN;
static double      s_pickup = 0.0;  /* pickup + cable resonance, Hz; 0 = off */
static double      s_hf = 0.0;      /* grid low pass corner, Hz; 0 = as built */
static double      s_lf = 0.0;      /* interstage high pass corner, Hz        */
static int         s_extras = 0;    /* also render the things that do not
                                       distort: dry, reverbs, tanh control   */
static double      s_pot = 1.0;     /* preamp volume between the valves, 0..1 */
static int         s_resp = 0;      /* measure the small-signal response      */
static double      s_treble = 0.0;  /* tone stack treble, 1 = as built        */
static double      s_power = 0.0;   /* symmetric power-stage stand-in drive   */
static double      s_bass = 0.0;    /* tone stack bass, 1 = as built          */
static double      s_xfmr = 4500.0; /* output transformer corner, Hz          */
static double      s_xq = 2.0;      /* and its Q: the presence peak           */
static int         s_lean = 0;      /* drop the negligible capacitances       */
static int         s_nots = 0;      /* no tone stack on the clipping stage    */

/* ------------------------------------------------------------------------ */
/* WAV in and out                                                            */
/* ------------------------------------------------------------------------ */

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

/*
 * Enough of RIFF to read what a recording actually arrives as: PCM, 16, 24 or
 * 32 bits, mono or stereo.  Chunks are walked rather than assumed to be in
 * order, because plenty of files carry a LIST or fact chunk before the data
 * and a reader that assumes otherwise reads metadata as audio.
 */
static float *wav_read(const char *path, uint32_t *frames_out, uint32_t *rate_out)
{
    FILE    *f = fopen(path, "rb");
    uint8_t *buf;
    long     size;
    uint32_t pos = 12;
    uint16_t channels = 0, bits = 0, format = 0;
    uint32_t rate = 0, data_off = 0, data_len = 0;
    float   *out;
    uint32_t i, frames, bytes;

    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 44) {
        fclose(f);
        fprintf(stderr, "%s: too small to be a wav\n", path);
        return NULL;
    }
    buf = (uint8_t *)malloc((size_t)size);
    if (buf == NULL || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);

    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "%s: not a RIFF/WAVE file\n", path);
        free(buf);
        return NULL;
    }
    while (pos + 8 <= (uint32_t)size) {
        const uint32_t clen = rd_u32(buf + pos + 4);
        if (memcmp(buf + pos, "fmt ", 4) == 0 && clen >= 16) {
            format = rd_u16(buf + pos + 8);
            channels = rd_u16(buf + pos + 10);
            rate = rd_u32(buf + pos + 12);
            bits = rd_u16(buf + pos + 22);
        } else if (memcmp(buf + pos, "data", 4) == 0) {
            data_off = pos + 8;
            data_len = clen;
        }
        pos += 8 + clen + (clen & 1u); /* chunks are word aligned */
    }
    if (format != 1 || channels < 1 || data_len == 0 ||
        (bits != 16 && bits != 24 && bits != 32)) {
        fprintf(stderr, "%s: need PCM 16/24/32-bit, got format %u, %u bits\n",
                path, format, bits);
        free(buf);
        return NULL;
    }

    bytes = (uint32_t)bits / 8u;
    frames = data_len / (bytes * channels);
    out = (float *)calloc(frames, sizeof(float));
    for (i = 0; i < frames; i++) {
        double acc = 0.0;
        uint32_t c;
        for (c = 0; c < channels; c++) {
            const uint8_t *p = buf + data_off + (i * channels + c) * bytes;
            int32_t        v = 0;
            if (bits == 16) {
                v = (int16_t)rd_u16(p);
                acc += (double)v / 32768.0;
            } else if (bits == 24) {
                v = ((int32_t)p[0] << 8) | ((int32_t)p[1] << 16) |
                    ((int32_t)p[2] << 24);
                acc += (double)(v >> 8) / 8388608.0;
            } else {
                v = (int32_t)rd_u32(p);
                acc += (double)v / 2147483648.0;
            }
        }
        out[i] = (float)(acc / channels);
    }
    free(buf);
    *frames_out = frames;
    *rate_out = rate;
    return out;
}

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

static void wav_write(const char *path, const float *x, uint32_t frames,
                      uint32_t rate, uint32_t channels)
{
    FILE          *f = fopen(path, "wb");
    const uint32_t data = frames * channels * 2u;
    uint32_t       i;
    float          peak = 0.0f;
    float          g;

    if (f == NULL) {
        fprintf(stderr, "cannot write %s\n", path);
        return;
    }
    for (i = 0; i < frames * channels; i++) {
        const float a = x[i] < 0.0f ? -x[i] : x[i];
        if (a > peak) {
            peak = a;
        }
    }
    /* -3 dBFS, so that comparing two renders by ear is comparing their sound
     * and not their level. */
    g = peak > 0.0f ? 23000.0f / peak : 0.0f;

    fwrite("RIFF", 1, 4, f);
    wr_u32(f, 36u + data);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    wr_u32(f, 16);
    wr_u16(f, 1);
    wr_u16(f, (uint16_t)channels);
    wr_u32(f, rate);
    wr_u32(f, rate * channels * 2u);
    wr_u16(f, (uint16_t)(channels * 2u));
    wr_u16(f, 16);
    fwrite("data", 1, 4, f);
    wr_u32(f, data);
    for (i = 0; i < frames * channels; i++) {
        float   v = x[i] * g;
        int16_t s;
        if (v > 32767.0f) {
            v = 32767.0f;
        }
        if (v < -32768.0f) {
            v = -32768.0f;
        }
        s = (int16_t)v;
        wr_u16(f, (uint16_t)s);
    }
    fclose(f);
    printf("  wrote %s  (%u frames, %u ch, peak in was %.3f)\n", path, frames,
           channels, (double)peak);
}

/* ------------------------------------------------------------------------ */

/*
 * Kaiser window, because the Blackman one that was here stops at about -74 dB
 * and that turned out to be the floor everything else was sitting on.
 *
 * A nonlinearity makes harmonics past Nyquist; oversampling moves them up and
 * the decimator is what removes them.  Whatever the decimator leaves comes
 * back into the band as inharmonic content - the fizz that does not decay
 * with the note, because it is not related to it.  Measured on a tone, that
 * floor was -70 dB and did not improve from x2 to x8: the oversampling was
 * never the limit, the window was.  Beta 10 puts the stopband near -100 dB
 * for the same 255 taps and the same instructions.
 */
static double bessel_i0(double x)
{
    double s = 1.0, t = 1.0;
    int    k;
    for (k = 1; k < 40; k++) {
        t *= (x / (2.0 * k)) * (x / (2.0 * k));
        s += t;
        if (t < s * 1e-17) {
            break;
        }
    }
    return s;
}

static double kaiser(int i, int n, double beta)
{
    const double r = (2.0 * i) / (double)(n - 1) - 1.0;
    const double a = 1.0 - r * r;
    return bessel_i0(beta * sqrt(a > 0.0 ? a : 0.0)) / bessel_i0(beta);
}

static float s_dec[DEC_TAPS];

/*
 * The interpolation filter that takes the input up to the oversampled rate.
 *
 * It used to be a straight line between neighbouring samples, which is a two
 * tap filter and a poor one: it leaves the images of the input sitting just
 * above the audio band, and measurement put them at -50 dB of the signal at
 * exactly half the oversampled rate.  In a linear stage that is harmless -
 * the decimator takes them out again at the end.  Through a valve that is
 * clipping it is not: the nonlinearity folds them straight back down into the
 * band, and no amount of oversampling helps, because interpolating twice as
 * often with the same straight line leaves the same images.
 *
 * The same windowed sinc the decimator uses, run as a polyphase bank: for
 * phase j the taps are h[j], h[j+OS], h[j+2*OS] and so on against successive
 * input samples.
 */
static float s_up[DEC_TAPS];
#define UP_HIST ((DEC_TAPS + 8u) / 2u + 2u)
static float    s_uphist[UP_HIST];
static unsigned s_upn;

static void up_init(unsigned os, double rate)
{
    const double fc = (rate * 0.45) / (rate * (double)os);
    double       sum = 0.0;
    int          i;
    for (i = 0; i < DEC_TAPS; i++) {
        const double n = (double)i - (DEC_TAPS - 1) / 2.0;
        const double s =
            (n == 0.0) ? 2.0 * fc : sin(2.0 * M_PI * fc * n) / (M_PI * n);
        const double w = kaiser(i, DEC_TAPS, 10.0);
        s_up[i] = (float)(s * w);
        sum += s * w;
    }
    /* Unity gain per phase: the bank hands on one input sample's worth of
     * energy per output sample, not one per oversampled sample. */
    for (i = 0; i < DEC_TAPS; i++) {
        s_up[i] = (float)(s_up[i] * (double)os / sum);
    }
    for (i = 0; i < (int)UP_HIST; i++) {
        s_uphist[i] = 0.0f;
    }
    s_upn = 0;
}

/* One input sample in; call up_phase() os times to get the oversampled ones. */
static void up_push(float x)
{
    unsigned i;
    for (i = UP_HIST - 1u; i > 0u; i--) {
        s_uphist[i] = s_uphist[i - 1u];
    }
    s_uphist[0] = x;
    if (s_upn < UP_HIST) {
        s_upn++;
    }
}

static float up_phase(unsigned j, unsigned os)
{
    float    acc = 0.0f;
    unsigned k, t;
    for (k = 0, t = j; t < (unsigned)DEC_TAPS && k < UP_HIST; k++, t += os) {
        acc += s_up[t] * s_uphist[k];
    }
    return acc;
}

static void dec_init(unsigned os, double out_rate)
{
    const double fc = (out_rate * 0.45) / (out_rate * (double)os);
    double       sum = 0.0;
    int          i;
    for (i = 0; i < DEC_TAPS; i++) {
        const double n = (double)i - (DEC_TAPS - 1) / 2.0;
        const double s =
            (n == 0.0) ? 2.0 * fc : sin(2.0 * M_PI * fc * n) / (M_PI * n);
        const double w = kaiser(i, DEC_TAPS, 10.0);
        s_dec[i] = (float)(s * w);
        sum += s_dec[i];
    }
    for (i = 0; i < DEC_TAPS; i++) {
        s_dec[i] = (float)(s_dec[i] / sum);
    }
}

/* One-pole DC blocker: the plate sits at its bias and a speaker does not want
 * two hundred volts of it. */
static float dcb(float *z, float x, float a)
{
    *z += (x - *z) * a;
    return x - *z;
}

/* ------------------------------------------------------------------------ */
/* the amplifier                                                             */
/* ------------------------------------------------------------------------ */

/*
 * Two.  A third valve after the tone stack is what the real 2203 has and it is
 * not what "more crunch" should mean here: the ask was for the front of the
 * amplifier to work harder, not for more amplifier.  More gain in V1a changes
 * the texture, because then both valves clip instead of only the second one;
 * another stage just adds level.
 */
#ifndef AMP_STAGES
#define AMP_STAGES 2
#endif

#define TAB_N1_MAX 2048
#define TAB_N2_MAX 65

static int TAB_N1 = CKT_BAKE_N1;
static int TAB_N2 = CKT_BAKE_N2;

typedef struct {
    ag_stage_t stage[AMP_STAGES];
    float      tab[AMP_STAGES][TAB_N1_MAX * TAB_N2_MAX * 2];
    float      tabH[AMP_STAGES][TAB_N1_MAX * TAB_N2_MAX * 2];
} amp_t;

static amp_t     s_amp;
static ag_ckt_t  s_scratch;

/*
 * Override the grid capacitance from a corner frequency, so it can be chosen
 * by ear instead of in picofarads.  The capacitor is what actually goes into
 * the matrix - it is a component of the circuit, not a filter bolted on after
 * it, so the valve sees the filtered signal and clips what it is given.
 */
static void stage_hf(ckt_stage_spec_t *sp, int index)
{
    if (s_hf > 0.0) {
        sp->cmiller =
            (float)(1.0 / (2.0 * 3.14159265358979 * s_hf * sp->rsrc));
    }
    /*
     * Bass out of the way before the valve that clips.
     *
     * This is the lever every high gain amplifier pulls and this model did
     * not have: the coupling capacitor into the clipping stage.  At 22 nF
     * against half a megohm it corners at 14 Hz, so the whole of the low end
     * arrives at the grid and intermodulates with everything above it -
     * which is the mush that a tone control after the distortion can never
     * take back out.  A 5150 or a Soldano puts 1 nF or less here on purpose.
     *
     * Only between stages: the first valve's input is the guitar, and
     * thinning that is a different decision with a different sound.
     */
    /*
     * A pot set part way is not just an attenuator: its own track appears in
     * series with whatever drives the next grid, worst at half travel.  A 1 M
     * pot at 0.3 adds 210k to the 38k of the valve's plate, which changes
     * what the grid capacitance does as well as the level.
     */
    if (s_treble > 0.0) {
        sp->ts_treble = (float)s_treble;
    }
    if (s_bass > 0.0) {
        sp->ts_bass = (float)s_bass;
    }
    /*
     * Drop the capacitances that are too small to do anything, to find out
     * whether they are worth what they cost.  Every capacitor in a stage adds
     * a row and a column to the baked model's inner loop, which is quadratic
     * in their number; a 0.46 pF plate-cathode stamps 9e-8 siemens at 96 kHz,
     * which is nine orders below the plate resistor it sits across.
     */
    if (s_lean) {
        sp->cgk = 0.0f;
        sp->cpk = 0.0f;
    }
    /*
     * Take the tone stack out of the clipping stage's plate.
     *
     * It is the one thing that stage has and the other does not, and the
     * listening said the buzz is made there: the first valve through a real
     * amplifier is clean, the first valve through ours is not.  Inside one
     * matrix the stack puts 250 pF next to 0.68 uF - six orders of magnitude
     * of companion conductance - and it is the stage the solver has most
     * trouble with.  Worth knowing whether the sound follows.
     */
    if (s_nots) {
        sp->tonestack = 0;
    }
    if (index > 0 && s_pot < 1.0) {
        sp->rsrc += (float)(1.0e6 * s_pot * (1.0 - s_pot));
    }
    if (s_lf > 0.0 && index > 0) {
        sp->ccouple = (float)(1.0 / (2.0 * 3.14159265358979 * s_lf *
                                     ((double)sp->rsrc + sp->rgrid)));
    }
}

/*
 * What each stage handed on, kept from the last tick.  Listening to a chain
 * in parts is what found the last two defects in this program, and a stage
 * that misbehaves on its own is invisible once the stage after it has
 * clipped whatever came out.
 */
static float s_stage_v[AMP_STAGES];

static float amp_tick(float vin)
{
    int   i;
    float v = vin;
    for (i = 0; i < AMP_STAGES; i++) {
        v = ag_stage_tick(&s_amp.stage[i], v);
        s_stage_v[i] = v;
        /*
         * The preamp volume, which sits between the first valve and the
         * second in a real 2203 and was missing here.  It is the amplifier's
         * main control, and without it the only way to get more distortion
         * was to drive the second valve harder - so the first one stayed
         * almost linear and the second did all the clipping in one go.  One
         * stage clipping to a given depth is harsher than two sharing it,
         * and harsher mostly at the top, because the corners it puts in the
         * waveform are sharper.
         */
        if (i == 0) {
            v *= (float)s_pot;
        }
    }
    return v;
}

static void amp_reset(void)
{
    int i;
    for (i = 0; i < AMP_STAGES; i++) {
        ag_stage_reset(&s_amp.stage[i]);
    }
}

static unsigned amp_clamped(void)
{
    unsigned t = 0;
    int      i;
    for (i = 0; i < AMP_STAGES; i++) {
        t += s_amp.stage[i].clamped;
    }
    return t;
}

static void amp_find_dc(int settle)
{
    int i;
    amp_reset();
    for (i = 0; i < settle; i++) {
        (void)amp_tick(0.0f);
    }
    for (i = 0; i < AMP_STAGES; i++) {
        ag_stage_mark_dc(&s_amp.stage[i]);
    }
    amp_reset();
}

/*
 * Bake both stages against the signal that is about to be played, iterating
 * the ranges the way ckt_bake_chain does - a table fitted to a sine and then
 * fed a plucked note runs off its own edge, and a table fitted while the rails
 * are still charging is fitted to a road the model never travels again.
 */
static int amp_bake(float fs, const float *probe, int probe_n, int settle)
{
    float p1lo[AMP_STAGES], p1hi[AMP_STAGES];
    float p2lo[AMP_STAGES], p2hi[AMP_STAGES];
    int   pass, s, i;

    for (s = 0; s < AMP_STAGES; s++) {
        ckt_stage_spec_t sp;
        int              out_node;
        ag_stage_init(&s_amp.stage[s]);
        ckt_jcm800_stage(&sp, s);
        stage_hf(&sp, s);
        out_node = ckt_build_spec_stage(&s_scratch, fs, &sp);
        if (!out_node ||
            ag_stage_bake(&s_amp.stage[s], &s_scratch, out_node, s_amp.tab[s],
                           0, TAB_N1, TAB_N2, -200.0f, 100.0f, -100.0f, 500.0f) != 0) {
            fprintf(stderr,
                    "stage %d: wide bake failed (%d nodes, %d capacitors, "
                    "limit %d)\n",
                    s, s_scratch.n, s_scratch.nc, AG_STAGE_MAX_C);
            return -1;
        }
        p1lo[s] = 1e30f;
        p1hi[s] = -1e30f;
        p2lo[s] = 1e30f;
        p2hi[s] = -1e30f;
    }

    for (pass = 0; pass < 3; pass++) {
        amp_find_dc(settle * 8);
        for (i = 0; i < settle + probe_n; i++) {
            (void)amp_tick(probe[i % probe_n]);
            for (s = 0; s < AMP_STAGES; s++) {
                const float a = s_amp.stage[s].last_p1;
                const float b = s_amp.stage[s].last_p2;
                if (a < p1lo[s]) p1lo[s] = a;
                if (a > p1hi[s]) p1hi[s] = a;
                if (b < p2lo[s]) p2lo[s] = b;
                if (b > p2hi[s]) p2hi[s] = b;
            }
        }
        for (s = 0; s < AMP_STAGES; s++) {
            ckt_stage_spec_t sp;
            const float m1 = s_margin * (p1hi[s] - p1lo[s]) + 1.0f;
            const float m2 = s_margin * (p2hi[s] - p2lo[s]) + 1.0f;
            const int   last = (pass == 2);
            int         out_node;
            ckt_jcm800_stage(&sp, s);
        stage_hf(&sp, s);
            out_node = ckt_build_spec_stage(&s_scratch, fs, &sp);
            if (!out_node ||
                ag_stage_bake(&s_amp.stage[s], &s_scratch, out_node,
                              s_amp.tab[s], last ? s_amp.tabH[s] : 0, TAB_N1, TAB_N2,
                              p1lo[s] - m1, p1hi[s] + m1, p2lo[s] - m2,
                              p2hi[s] + m2) != 0) {
                fprintf(stderr, "stage %d: bake failed\n", s);
                return -1;
            }
            if (last) {
                ag_stage_set_adaa(&s_amp.stage[s], 1);
            }
        }
    }
    amp_find_dc(settle * 8);

    /*
     * How much of each table is a guess.
     *
     * The table is filled by the same Newton solve that runs the reference,
     * and that solve does not always converge on a hard driven valve.  A grid
     * point that hit the iteration limit holds whatever the iteration had
     * reached, and every lookup that lands near it inherits it.  This number
     * was in the structure all along and never printed - which is why it took
     * until now to ask whether the tables are right at all.
     */
    for (s = 0; s < AMP_STAGES; s++) {
        const uint32_t pts = (uint32_t)TAB_N1 * (uint32_t)TAB_N2;
        printf("  stage %d: %u of %u table points did not converge (%.1f%%), "
               "worst %u iterations\n",
               s, s_amp.stage[s].bake_nonconverged, pts,
               100.0 * s_amp.stage[s].bake_nonconverged / (double)pts,
               s_amp.stage[s].bake_worst_iters);
    }
    for (s = 0; s < AMP_STAGES; s++) {
        printf("  stage %d: grid drive %.2f..%.2f V, plate drive %.1f..%.1f V,"
               " gain path K11 %.0f K22 %.0f\n",
               s, p1lo[s], p1hi[s], p2lo[s], p2hi[s], (double)s_amp.stage[s].k11,
               (double)s_amp.stage[s].k22);
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* reverb                                                                    */
/* ------------------------------------------------------------------------ */

/*
 * ag_ir is a fixed-point block engine: int16 in, interleaved stereo int16 out,
 * 256 frames at a time.  Everything else here is float, so the conversion
 * happens at the edge rather than being smeared through the code.
 */
/*
 * What is in the recording before the amplifier gets to it.
 *
 * Sixty decibels of gain does not care whether it is amplifying a guitar or
 * the hum and string noise underneath it, and a direct recording of a basic
 * instrument through a consumer interface has both.  Judging a distortion
 * model without looking at what was fed to it is how you spend a day
 * measuring the model.
 */
static void input_report(const float *x, uint32_t frames, uint32_t rate)
{
    const uint32_t n = 8192;
    static double  re[8192], im[8192];
    uint32_t       i, k, quiet = 0;
    double         best = 1e30, peak = 0.0;
    double         top[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    int            at[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    int            len, half, a, b, q;

    for (i = 0; i < frames; i++) {
        const double v = x[i] < 0.0f ? -(double)x[i] : (double)x[i];
        if (v > peak) {
            peak = v;
        }
    }
    /* The quietest window that is not the padding after the take. */
    for (k = 0; k + n <= frames - rate * 2u; k += n) {
        double e = 0.0;
        for (i = 0; i < n; i++) {
            e += (double)x[k + i] * (double)x[k + i];
        }
        if (e < best) {
            best = e;
            quiet = k;
        }
    }
    printf("  input: peak %.4f V, quietest 170 ms %.1f dB below it\n",
           peak, 10.0 * log10((best / n) / (peak * peak + 1e-30) + 1e-30));

    for (i = 0; i < n; i++) {
        const double t = (double)i / (double)n;
        const double w = 0.35875 - 0.48829 * cos(2.0 * 3.14159265358979 * t) +
                         0.14128 * cos(4.0 * 3.14159265358979 * t) -
                         0.01168 * cos(6.0 * 3.14159265358979 * t);
        re[i] = (double)x[quiet + i] * w;
        im[i] = 0.0;
    }
    for (a = 1, b = 0; a < (int)n; a++) {
        int m = (int)n >> 1;
        for (; m >= 1 && b >= m; m >>= 1) {
            b -= m;
        }
        b += m;
        if (a < b) {
            double tr = re[a], ti = im[a];
            re[a] = re[b];
            im[a] = im[b];
            re[b] = tr;
            im[b] = ti;
        }
    }
    for (len = 2; len <= (int)n; len <<= 1) {
        half = len >> 1;
        for (a = 0; a < (int)n; a += len) {
            for (b = 0; b < half; b++) {
                const double ang = -2.0 * 3.14159265358979 * b / len;
                const double wr = cos(ang), wi = sin(ang);
                const int    i0 = a + b, i1 = i0 + half;
                const double tr = re[i1] * wr - im[i1] * wi;
                const double ti = re[i1] * wi + im[i1] * wr;
                re[i1] = re[i0] - tr;
                im[i1] = im[i0] - ti;
                re[i0] += tr;
                im[i0] += ti;
            }
        }
    }
    for (i = 1; i < n / 2; i++) {
        const double p = re[i] * re[i] + im[i] * im[i];
        for (q = 0; q < 8; q++) {
            if (p > top[q]) {
                int m;
                for (m = 7; m > q; m--) {
                    top[m] = top[m - 1];
                    at[m] = at[m - 1];
                }
                top[q] = p;
                at[q] = (int)i;
                break;
            }
        }
    }
    printf("    what is in that window:");
    for (q = 0; q < 8; q++) {
        printf(" %.0fHz", (double)at[q] * rate / (double)n);
    }
    printf("\n");

    /*
     * And the level second by second.  An artefact that starts and stops at a
     * particular moment in a take is telling you which part of the playing
     * causes it, and that is worth more than any spectrum: it turns "there is
     * a noise" into "there is a noise while the signal is above X".
     */
    printf("    peak by second, dBFS relative to the loudest:");
    for (k = 0; k * rate + rate <= frames; k++) {
        double p = 0.0;
        for (i = 0; i < rate; i++) {
            const double v = x[k * rate + i] < 0.0f
                                 ? -(double)x[k * rate + i]
                                 : (double)x[k * rate + i];
            if (v > p) {
                p = v;
            }
        }
        if ((k % 10u) == 0u) {
            printf("\n      %2us:", k);
        }
        printf(" %5.1f", 20.0 * log10((p + 1e-30) / (peak + 1e-30)));
    }
    printf("\n");
}

/*
 * What the amplifier does to a signal too small to distort it.
 *
 * Its own frequency response, in other words - and nothing in this program
 * had ever measured it.  Every conclusion about "fizz" assumes the model is
 * flat and the fizz is made by the clipping; if the model is voiced bright,
 * it is amplifying the fizz instead, and no amount of looking at the
 * distortion will say so.
 *
 * Noise in, ratio of the averaged spectra out.  Blocks are averaged because
 * one block of noise has a ragged spectrum and the ratio of two ragged
 * spectra is unreadable.
 */
static void response_report(const float *in, const float *out, uint32_t frames,
                            uint32_t rate)
{
#define RESP_N 8192
    static double xr[RESP_N], xi[RESP_N], yr[RESP_N], yi[RESP_N];
    static double px[RESP_N / 2], py[RESP_N / 2];
    uint32_t      i, k, blocks = 0;
    int           b;
    static const double edge[] = { 60,   100,  160,  250,  400,   630,
                                   1000, 1600, 2500, 4000, 6300,  10000,
                                   16000, 22000 };
    const int nb = (int)(sizeof(edge) / sizeof(edge[0]));

    for (i = 0; i < RESP_N / 2; i++) {
        px[i] = 0.0;
        py[i] = 0.0;
    }
    for (k = RESP_N; k + RESP_N <= frames; k += RESP_N) {
        int len, half, a, j;
        for (i = 0; i < RESP_N; i++) {
            const double w =
                0.5 - 0.5 * cos(2.0 * 3.14159265358979 * i / RESP_N);
            xr[i] = (double)in[k + i] * w;
            xi[i] = 0.0;
            yr[i] = (double)out[k + i] * w;
            yi[i] = 0.0;
        }
        for (a = 1, j = 0; a < RESP_N; a++) {
            int m = RESP_N >> 1;
            for (; m >= 1 && j >= m; m >>= 1) {
                j -= m;
            }
            j += m;
            if (a < j) {
                double t;
                t = xr[a]; xr[a] = xr[j]; xr[j] = t;
                t = xi[a]; xi[a] = xi[j]; xi[j] = t;
                t = yr[a]; yr[a] = yr[j]; yr[j] = t;
                t = yi[a]; yi[a] = yi[j]; yi[j] = t;
            }
        }
        for (len = 2; len <= RESP_N; len <<= 1) {
            half = len >> 1;
            for (a = 0; a < RESP_N; a += len) {
                for (j = 0; j < half; j++) {
                    const double ang = -2.0 * 3.14159265358979 * j / len;
                    const double wr = cos(ang), wi = sin(ang);
                    const int    i0 = a + j, i1 = i0 + half;
                    double       tr, ti;
                    tr = xr[i1] * wr - xi[i1] * wi;
                    ti = xr[i1] * wi + xi[i1] * wr;
                    xr[i1] = xr[i0] - tr;
                    xi[i1] = xi[i0] - ti;
                    xr[i0] += tr;
                    xi[i0] += ti;
                    tr = yr[i1] * wr - yi[i1] * wi;
                    ti = yr[i1] * wi + yi[i1] * wr;
                    yr[i1] = yr[i0] - tr;
                    yi[i1] = yi[i0] - ti;
                    yr[i0] += tr;
                    yi[i0] += ti;
                }
            }
        }
        for (i = 0; i < RESP_N / 2; i++) {
            px[i] += xr[i] * xr[i] + xi[i] * xi[i];
            py[i] += yr[i] * yr[i] + yi[i] * yi[i];
        }
        blocks++;
    }
    if (blocks == 0u) {
        return;
    }
    printf("  small signal response, dB relative to 1 kHz:\n   ");
    {
        double ref = 1.0;
        double band[32];
        for (b = 0; b < nb - 1; b++) {
            double sx = 0.0, sy = 0.0;
            for (i = 1; i < RESP_N / 2; i++) {
                const double hz = (double)i * rate / RESP_N;
                if (hz >= edge[b] && hz < edge[b + 1]) {
                    sx += px[i];
                    sy += py[i];
                }
            }
            band[b] = (sx > 0.0) ? sy / sx : 0.0;
            if (edge[b] <= 1000.0 && edge[b + 1] > 1000.0) {
                ref = band[b];
            }
        }
        for (b = 0; b < nb - 1; b++) {
            printf(" %.0f:%+.1f", edge[b],
                   10.0 * log10((band[b] + 1e-30) / (ref + 1e-30)));
        }
    }
    printf("\n");
#undef RESP_N
}

/*
 * How much of a signal sits above 6 kHz - where a guitar speaker is dead and
 * anything still present is either the model's or a leak past the cabinet.
 * `stride` picks one channel out of an interleaved buffer.
 */
static void hf_report(const float *x, uint32_t frames, uint32_t stride,
                      uint32_t rate, const char *tag)
{
    const uint32_t n = 8192;
    static double  re[8192], im[8192];
    double         lo = 0.0, hi = 0.0;
    uint32_t       i, k, blocks = 0;

    for (k = 0; k + n <= frames; k += n * 4u) {
        int len, half, a, b;
        for (i = 0; i < n; i++) {
            const double t = (double)i / (double)n;
            const double w = 0.5 - 0.5 * cos(2.0 * 3.14159265358979 * t);
            re[i] = (double)x[(size_t)(k + i) * stride] * w;
            im[i] = 0.0;
        }
        for (a = 1, b = 0; a < (int)n; a++) {
            int m = (int)n >> 1;
            for (; m >= 1 && b >= m; m >>= 1) {
                b -= m;
            }
            b += m;
            if (a < b) {
                double tr = re[a], ti = im[a];
                re[a] = re[b];
                im[a] = im[b];
                re[b] = tr;
                im[b] = ti;
            }
        }
        for (len = 2; len <= (int)n; len <<= 1) {
            half = len >> 1;
            for (a = 0; a < (int)n; a += len) {
                for (b = 0; b < half; b++) {
                    const double ang = -2.0 * 3.14159265358979 * b / len;
                    const double wr = cos(ang), wi = sin(ang);
                    const int    i0 = a + b, i1 = i0 + half;
                    const double tr = re[i1] * wr - im[i1] * wi;
                    const double ti = re[i1] * wi + im[i1] * wr;
                    re[i1] = re[i0] - tr;
                    im[i1] = im[i0] - ti;
                    re[i0] += tr;
                    im[i0] += ti;
                }
            }
        }
        for (i = 1; i < n / 2; i++) {
            const double p = re[i] * re[i] + im[i] * im[i];
            if ((double)i * rate / n < 6000.0) {
                lo += p;
            } else {
                hi += p;
            }
        }
        blocks++;
    }
    if (blocks == 0u) {
        return;
    }
    printf("    %-24s above 6 kHz: %6.1f dB relative to below\n", tag,
           10.0 * log10((hi + 1e-30) / (lo + 1e-30)));
}

/*
 * Where the noise actually is.
 *
 * A sine in, and everything that comes out which is not a harmonic of it is
 * something the model invented.  Aliasing, table steps and arithmetic noise
 * all land here, and each of them moves when a different knob does - which is
 * the only way to tell them apart without guessing.
 *
 * Blackman-Harris rather than Hann: the sidelobes have to be further down
 * than the thing being measured, or the window's own leakage is what gets
 * reported.
 */
static void spectrum_junk(const float *x, uint32_t frames, uint32_t rate,
                          double f0, const char *tag)
{
#define SPEC_N 65536
    static double re[SPEC_N], im[SPEC_N];
    /* Two thirds in for the step case - past the drop, clear of it. */
    const uint32_t off = frames > (uint32_t)SPEC_N * 4u
                             ? (s_quiet ? frames / 2u : frames / 3u)
                             : 0u;
    double         harm = 0.0, junk = 0.0;
    double         band[4] = { 0.0, 0.0, 0.0, 0.0 };
    int            i, j, len, half;
    int            worst_bin = 0;
    double         worst = 0.0;

    if (frames < off + SPEC_N) {
        return;
    }
    for (i = 0; i < SPEC_N; i++) {
        const double t = (double)i / (double)SPEC_N;
        const double w = 0.35875 - 0.48829 * cos(2.0 * 3.14159265358979 * t) +
                         0.14128 * cos(4.0 * 3.14159265358979 * t) -
                         0.01168 * cos(6.0 * 3.14159265358979 * t);
        re[i] = (double)x[off + (uint32_t)i] * w;
        im[i] = 0.0;

    }
    /* Plain radix-2, double: this is analysis, not the hot path. */
    for (i = 1, j = 0; i < SPEC_N; i++) {
        int m = SPEC_N >> 1;
        for (; m >= 1 && j >= m; m >>= 1) {
            j -= m;
        }
        j += m;
        if (i < j) {
            double tr = re[i], ti = im[i];
            re[i] = re[j];
            im[i] = im[j];
            re[j] = tr;
            im[j] = ti;
        }
    }
    for (len = 2; len <= SPEC_N; len <<= 1) {
        half = len >> 1;
        for (i = 0; i < SPEC_N; i += len) {
            for (j = 0; j < half; j++) {
                const double a = -2.0 * 3.14159265358979 * j / len;
                const double wr = cos(a), wi = sin(a);
                const int    i0 = i + j, i1 = i0 + half;
                const double tr = re[i1] * wr - im[i1] * wi;
                const double ti = re[i1] * wi + im[i1] * wr;
                re[i1] = re[i0] - tr;
                im[i1] = im[i0] - ti;
                re[i0] += tr;
                im[i0] += ti;
            }
        }
    }

    /*
     * Harmonics get four bins either side - the window spreads a tone that
     * far and no further.  Everything from bin 8 up that is not inside one of
     * those groups is counted as invented.  Below bin 8 is the DC blocker's
     * own corner, which is not the model's doing.
     */
    for (i = 20; i < SPEC_N / 2; i++) {
        const double p = re[i] * re[i] + im[i] * im[i];
        const double hz = (double)i * rate / (double)SPEC_N;
        const double n = hz / f0;
        const double nearest = (double)(int)(n + 0.5);
        const double dist_bins =
            (nearest * f0 - hz) * (double)SPEC_N / (double)rate;
        const int is_harm = nearest >= 1.0 &&
                            dist_bins < 5.5 && dist_bins > -5.5;
        if (is_harm) {
            harm += p;
        } else {
            const int b = hz < 1000.0 ? 0 : hz < 4000.0 ? 1 : hz < 10000.0 ? 2
                                                                          : 3;
            junk += p;
            band[b] += p;
            if (p > worst) {
                worst = p;
                worst_bin = i;
            }
        }
    }
    printf("  %-22s %6.1f dB below the tone   "
           "(<1k %6.1f, 1-4k %6.1f, 4-10k %6.1f, >10k %6.1f)   "
           "worst stray %5.0f Hz\n",
           tag, 10.0 * log10((junk + 1e-30) / (harm + 1e-30)),
           10.0 * log10((band[0] + 1e-30) / (harm + 1e-30)),
           10.0 * log10((band[1] + 1e-30) / (harm + 1e-30)),
           10.0 * log10((band[2] + 1e-30) / (harm + 1e-30)),
           10.0 * log10((band[3] + 1e-30) / (harm + 1e-30)),
           (double)worst_bin * rate / (double)SPEC_N);

    /*
     * The harmonic series itself, which is the sound rather than the noise.
     * A valve stage rolls its harmonics off; a series that stays flat out to
     * the twentieth is a buzz, and no amount of looking at the noise floor
     * will show that, because every one of those partials is deliberate.
     */
    {
        int h;
        printf("      harmonics vs the first:");
        for (h = 2; h <= 120; h += (h < 20 ? 1 : 5)) {
            const double hz = (double)h * f0;
            const int    b = (int)(hz * (double)SPEC_N / rate + 0.5);
            double       p = 0.0, p1 = 0.0;
            int          q;
            if (b + 5 >= SPEC_N / 2) {
                break;
            }
            for (q = b - 5; q <= b + 5; q++) {
                p += re[q] * re[q] + im[q] * im[q];
            }
            {
                const int b1 = (int)(f0 * (double)SPEC_N / rate + 0.5);
                for (q = b1 - 5; q <= b1 + 5; q++) {
                    p1 += re[q] * re[q] + im[q] * im[q];
                }
            }
            printf(" %d:%.0f", h, 10.0 * log10((p + 1e-30) / (p1 + 1e-30)));
        }
        printf("\n");
    }

    /*
     * And the six loudest strays by name.  Broadband hash and a handful of
     * intermodulation products read the same in a single number and want
     * opposite fixes, so the shape has to be visible.
     */
    {
        double top[6] = { 0, 0, 0, 0, 0, 0 };
        int    at[6] = { 0, 0, 0, 0, 0, 0 };
        int    k;
        for (i = 20; i < SPEC_N / 2; i++) {
            const double p = re[i] * re[i] + im[i] * im[i];
            const double hz = (double)i * rate / (double)SPEC_N;
            const double nearest = (double)(int)(hz / f0 + 0.5);
            const double d = (nearest * f0 - hz) * (double)SPEC_N / rate;
            if (nearest >= 1.0 && d < 5.5 && d > -5.5) {
                continue;
            }
            for (k = 0; k < 6; k++) {
                if (p > top[k]) {
                    int m;
                    for (m = 5; m > k; m--) {
                        top[m] = top[m - 1];
                        at[m] = at[m - 1];
                    }
                    top[k] = p;
                    at[k] = (int)i;
                    break;
                }
            }
        }
        printf("      loudest strays:");
        for (k = 0; k < 6; k++) {
            printf("  %.0f Hz %.0f dB", (double)at[k] * rate / (double)SPEC_N,
                   10.0 * log10((top[k] + 1e-30) / (harm + 1e-30)));
        }
        printf("\n");
    }
#undef SPEC_N
}

static float *reverb(const float *x, uint32_t frames, uint32_t rate,
                     int preset, uint8_t wet, uint32_t *out_frames)
{
    static ag_ir_t ir;
    float         *out;
    int16_t        inblk[AG_IR_BLOCK];
    int16_t        outblk[AG_IR_BLOCK * 2];
    uint32_t       i, b, clipped = 0;
    float          peak = 0.0f;
    /* The input is already padded to four seconds, so the tail has room. */
    const uint32_t total =
        ((frames + AG_IR_BLOCK - 1) / AG_IR_BLOCK) * AG_IR_BLOCK;

    for (i = 0; i < frames; i++) {
        const float a = x[i] < 0.0f ? -x[i] : x[i];
        if (a > peak) {
            peak = a;
        }
    }
    if (peak <= 0.0f) {
        peak = 1.0f;
    }

    memset(&ir, 0, sizeof(ir));
    if (ag_ir_init(&ir, rate) != 0) {
        fprintf(stderr, "reverb: init failed\n");
        return NULL;
    }
    /*
     * A measured cabinet in place of the synthetic one, when the caller gives
     * a file.  The built-in "cab" preset is a noise burst under a one-pole
     * roll-off - close enough to hear the difference a speaker makes, nothing
     * like close enough to judge a distortion by.
     */
    if (preset == 4 && s_cab_ir != NULL) {
        uint32_t hn = 0, hrate = 0;
        float   *hf = wav_read(s_cab_ir, &hn, &hrate);
        int16_t *h;
        int      rc;
        if (hf == NULL) {
            return NULL;
        }
        h = (int16_t *)malloc(sizeof(int16_t) * hn);
        for (i = 0; i < hn; i++) {
            float v = hf[i] * 32767.0f;
            h[i] = (int16_t)(v > 32767.0f    ? 32767.0f
                             : v < -32768.0f ? -32768.0f
                                             : v);
        }
        rc = ag_ir_load(&ir, h, hn, hrate);
        printf("    cabinet from %s: %u taps at %u Hz (%.0f ms)%s\n", s_cab_ir,
               hn, hrate, 1000.0 * hn / (double)hrate,
               rc == 0 ? "" : " -- FAILED");
        /*
         * What the impulse response itself does, straight from the taps.  A
         * guitar speaker is dead above about 6 kHz; if this says otherwise
         * then the file is not the cabinet it is being asked to be, and no
         * amount of looking at the convolution will show that.
         */
        {
            static const double f[8] = { 100.0,  400.0,   1000.0,  2000.0,
                                         4000.0, 6000.0, 10000.0, 15000.0 };
            double mag[8];
            int    q;
            for (q = 0; q < 8; q++) {
                double   sr = 0.0, si = 0.0;
                uint32_t t;
                for (t = 0; t < hn; t++) {
                    const double a = -2.0 * 3.14159265358979 * f[q] * t / hrate;
                    sr += hf[t] * cos(a);
                    si += hf[t] * sin(a);
                }
                mag[q] = sqrt(sr * sr + si * si);
            }
            printf("      its response, relative to 1 kHz:");
            for (q = 0; q < 8; q++) {
                printf(" %.0fHz %+.0f", f[q],
                       20.0 * log10((mag[q] + 1e-30) / (mag[2] + 1e-30)));
            }
            printf("\n");
        }
        free(hf);
        free(h);
        if (rc != 0) {
            return NULL;
        }
    } else if (ag_ir_load_preset(&ir, preset) != 0) {
        fprintf(stderr, "reverb: preset %d failed\n", preset);
        return NULL;
    }
    ag_ir_set_wet(&ir, wet);

    /*
     * How far to back the input off before the convolution, found rather than
     * assumed.
     *
     * An impulse response has whatever gain it has: a cabinet is +11 dB at
     * 100 Hz, and one taken from a whole amplifier is +18.  Any fixed
     * headroom is therefore wrong for some response, and being wrong means
     * clipping inside int16 - which sounds like distortion and is not.  So
     * try, count the clipped samples, halve, and try again.
     */
    out = (float *)calloc(total * 2u, sizeof(float));
    {
        float scale = 9000.0f;
        int   attempt;
        for (attempt = 0; attempt < 6; attempt++) {
            clipped = 0;
            ag_ir_reset(&ir);
            for (b = 0; b < total / AG_IR_BLOCK; b++) {
                for (i = 0; i < AG_IR_BLOCK; i++) {
                    const uint32_t n = b * AG_IR_BLOCK + i;
                    const float    v = (n < frames) ? x[n] / peak * scale : 0.0f;
                    inblk[i] = (int16_t)(v > 32767.0f    ? 32767.0f
                                         : v < -32768.0f ? -32768.0f
                                                         : v);
                }
                ag_ir_process_block(&ir, inblk, outblk);
                for (i = 0; i < AG_IR_BLOCK * 2u; i++) {
                    const int16_t s = outblk[i];
                    if (s >= 32767 || s <= -32768) {
                        clipped++;
                    }
                    out[(b * AG_IR_BLOCK * 2u) + i] = (float)s;
                }
            }
            if (clipped == 0u) {
                break;
            }
            scale *= 0.5f;
        }
        if (scale < 9000.0f) {
            printf("    backed the convolution off to %.0f (%d dB) to stop it "
                   "clipping\n",
                   (double)scale, (int)(20.0 * log10(scale / 32768.0)));
        }
    }
    ag_ir_free(&ir);

    /*
     * What the convolution costs in dynamic range, because it is not free and
     * it is where the crackle turned out to live.
     *
     * ag_ir is fixed point end to end: int16 in, int16 spectra, int16 out.
     * Sixteen bits is 96 dB at best, and the input has to be backed off to
     * leave the convolution headroom, so what actually reaches the tail of a
     * decaying note is a handful of least significant bits.  That granularity
     * is the crackle - it comes from the reverb, not from the valves, which is
     * why the dry amplifier renders are clean and every processed one is not.
     */
    {
        double in_pk = 0.0, out_pk = 0.0, in_lo = 1e30, out_lo = 1e30;
        uint32_t w = rate / 20u, k;
        for (k = 0; k + w <= frames; k += w) {
            double a = 0.0, b = 0.0;
            uint32_t t;
            for (t = 0; t < w; t++) {
                const double xa = x[k + t] / peak;
                const double xb = out[(k + t) * 2u] / 32768.0;
                a += xa * xa;
                b += xb * xb;
            }
            a = sqrt(a / w);
            b = sqrt(b / w);
            if (a > in_pk) in_pk = a;
            if (b > out_pk) out_pk = b;
            if (a > 0.0 && a < in_lo) in_lo = a;
            if (b > 0.0 && b < out_lo) out_lo = b;
        }
        printf("    preset %d: dynamic range in %.0f dB, out %.0f dB%s\n",
               preset, 20.0 * log10(in_pk / (in_lo + 1e-30)),
               20.0 * log10(out_pk / (out_lo + 1e-30)),
               clipped ? "  -- CLIPPED INSIDE THE CONVOLUTION" : "");
    }

    *out_frames = total;
    return out;
}

/* ------------------------------------------------------------------------ */

/*
 * The valve model against the same formula in double precision with libm.
 *
 * Everything downstream assumes the device is right.  It is computed in float
 * with transcendentals written for this project, over a range that spans nine
 * orders of magnitude, and the stage that misbehaves is the one that visits
 * the ends of that range - deep cutoff and grid conduction - while the stage
 * that behaves stays in a narrow band near its bias point.  That is a reason
 * to check the device rather than to assume it.
 *
 * Reports the worst relative error in the current, and separately the worst
 * error in the derivatives, since Newton is steered by those.
 */
static void triode_check(void)
{
    ag_triode_model_t m;
    double            worst_ip = 0.0, worst_g = 0.0;
    double            at_vg = 0, at_vp = 0, at_vg_g = 0, at_vp_g = 0;
    int               i, j;

    ag_triode_model_12ax7(&m);
    printf("  valve model against double precision, over what stage 1 visits\n");

    for (i = 0; i <= 560; i++) {
        const double vgk = -20.0 + i * 0.05;
        for (j = 0; j <= 350; j++) {
            const double vpk = j * 1.0;
            ag_triode_op_t op;
            double         s, arg, l, e1, ip, dvg, dvp, sig, dip_de1;
            double         rel;

            ag_triode_eval(&m, (float)vgk, (float)vpk, &op);

            /* The same equations, in double, with the library's functions. */
            s = sqrt((double)m.kvb + vpk * vpk);
            arg = (double)m.kp * (1.0 / (double)m.mu + vgk / s);
            l = arg > 30.0 ? arg : log1p(exp(arg));
            e1 = (vpk / (double)m.kp) * l;
            if (e1 > 1.0e-9) {
                dip_de1 = 2.0 * (double)m.ex * pow(e1, (double)m.ex - 1.0) /
                          (double)m.kg1;
                ip = 2.0 * pow(e1, (double)m.ex) / (double)m.kg1;
                sig = 1.0 / (1.0 + exp(-arg));
                dvg = dip_de1 * (vpk * sig / s);
                dvp = dip_de1 * (l / (double)m.kp -
                                 vpk * vpk * vgk * sig / (s * s * s));
            } else {
                ip = 0.0;
                dvg = 0.0;
                dvp = 0.0;
            }

            rel = fabs((double)op.ip - ip) / (fabs(ip) + 1.0e-9);
            if (ip > 1.0e-7 && rel > worst_ip) {
                worst_ip = rel;
                at_vg = vgk;
                at_vp = vpk;
            }
            rel = fabs((double)op.dip_dvgk - dvg) / (fabs(dvg) + 1.0e-9);
            if (dvg > 1.0e-7 && rel > worst_g) {
                worst_g = rel;
                at_vg_g = vgk;
                at_vp_g = vpk;
            }
        }
    }
    printf("    worst current error   %.2e  at Vgk %.2f V, Vpk %.0f V\n",
           worst_ip, at_vg, at_vp);
    printf("    worst gradient error  %.2e  at Vgk %.2f V, Vpk %.0f V\n",
           worst_g, at_vg_g, at_vp_g);
}

/*
 * Where each valve actually sits with nothing at the input.
 *
 * The most basic question that can be asked of an amplifier, and it had never
 * been printed: a 12AX7 with an 820 ohm cathode should idle at about -0.7 V
 * on the grid, drawing a milliamp or so, with its plate well below the
 * supply.  A valve biased somewhere else is not the amplifier that was drawn,
 * however carefully everything downstream is computed.
 *
 * Read off the reference solver, settled on silence, because that is the
 * circuit rather than any reduction of it.
 */
static void bias_report(float fs)
{
    static ag_ckt_t k;
    unsigned        s;

    printf("  operating point, from the solver on silence:\n");
    for (s = 0; s < AMP_STAGES; s++) {
        ckt_stage_spec_t sp;
        int              out_node, i;
        float            vg, vp, vc;
        ag_triode_op_t   op;
        ag_triode_model_t m;

        ckt_jcm800_stage(&sp, (int)s);
        stage_hf(&sp, (int)s);
        out_node = ckt_build_spec_stage(&k, fs, &sp);
        if (!out_node) {
            continue;
        }
        for (i = 0; i < (int)(fs / 2.0f); i++) {
            (void)ag_ckt_tick(&k, 0.0f, out_node);
        }
        vg = k.x[5 - 1];
        vp = k.x[6 - 1];
        vc = k.x[7 - 1];
        ag_triode_model_12ax7(&m);
        ag_triode_eval(&m, vg - vc, vp - vc, &op);
        printf("    stage %u: grid %+.2f V, cathode %+.2f V, plate %.1f V"
               "  ->  Vgk %+.2f V, Vpk %.1f V, Ip %.3f mA, Ig %.4f mA\n",
               s, (double)vg, (double)vc, (double)vp, (double)(vg - vc),
               (double)(vp - vc), (double)op.ip * 1000.0,
               (double)op.ig * 1000.0);
    }
}

/*
 * The static transfer curve of each stage, and what the grid is doing along
 * it.
 *
 * A ramp slow enough that every capacitor in the stage has settled at every
 * point of it, so what comes out is the curve itself rather than the curve
 * plus its own history.  If a stage has a step, a kink or a fold in it, this
 * shows it as a number; nothing about listening to a guitar through it will.
 *
 * Both stages, from the solver, so the table is not part of the answer.
 */
static void curve_report(float fs, float lo, float hi)
{
    static ag_ckt_t   k;
    ag_triode_model_t m;
    unsigned          s;

    ag_triode_model_12ax7(&m);
    for (s = 0; s < AMP_STAGES; s++) {
        ckt_stage_spec_t sp;
        int              out_node, i, step;
        float            prev_out = 0.0f, prev_slope = 0.0f;

        ckt_jcm800_stage(&sp, (int)s);
        stage_hf(&sp, (int)s);
        out_node = ckt_build_spec_stage(&k, fs, &sp);
        if (!out_node) {
            continue;
        }
        for (i = 0; i < (int)fs; i++) {
            (void)ag_ckt_tick(&k, lo, out_node);
        }
        /*
         * A sine rather than a ramp, because a stage with a coupling
         * capacitor has no static curve at all - the first attempt held each
         * level for ten milliseconds and measured a dead flat line, which is
         * exactly right for a 143 Hz high pass and says nothing about the
         * valve.
         *
         * 300 Hz, well inside the passband, and one cycle printed after the
         * stage has settled.  What that shows is the working curve including
         * whatever the capacitors remember: a single-valued curve is a
         * well-behaved stage, and a wide or ragged loop is not.
         */
        {
            const double w = 2.0 * 3.14159265358979 * 300.0 / fs;
            const int    per = (int)(fs / 300.0f);
            float        out = 0.0f;
            for (i = 0; i < (int)fs; i++) {
                out = ag_ckt_tick(&k, hi * (float)sin(w * i), out_node);
            }
            printf("  stage %u, one cycle at 300 Hz, peak %.2f V "
                   "(in -> out, Vgk, Ig uA):\n",
                   s, (double)hi);
            for (step = 0; step < per; step++) {
                const float in = hi * (float)sin(w * step);
                float       vg, vp, vc;
                ag_triode_op_t op;
                out = ag_ckt_tick(&k, in, out_node);
                if ((step % (per / 24)) != 0) {
                    continue;
                }
                vg = k.x[5 - 1];
                vp = k.x[6 - 1];
                vc = k.x[7 - 1];
                ag_triode_eval(&m, vg - vc, vp - vc, &op);
                printf("    %+7.3f -> %+9.3f   Vgk %+7.3f  Ig %9.3f\n",
                       (double)in, (double)out, (double)(vg - vc),
                       (double)op.ig * 1.0e6);
            }
            (void)lo;
            (void)prev_out;
            (void)prev_slope;
        }
    }
}

int main(int argc, char **argv)
{
    const char  *path = argc > 1 ? argv[1] : NULL;
    const float  drive = argc > 2 ? (float)atof(argv[2]) : 0.25f;
    const char  *outdir = argc > 3 ? argv[3] : "build/listen";
    const unsigned os_arg = argc > 4 ? (unsigned)atoi(argv[4]) : 2u;
    uint32_t     frames = 0, rate = 0;
    float       *dry;
    float       *amp_out;
    char         name[512];
    /*
     * The source recording's own name, carried into every file this run
     * writes.  Without it two takes render on top of each other, and a
     * directory of renders that cannot be told apart is worse than no
     * renders: it is renders you believe.
     */
    char         tag[64];
    uint32_t     i;
    unsigned     j;
    float        peak = 0.0f, hist[DEC_TAPS], z = 0.0f;
    static float hists[AMP_STAGES][DEC_TAPS];
    float        zs[AMP_STAGES];
    float       *stage_out[AMP_STAGES];

    if (path == NULL) {
        fprintf(stderr,
                "usage: ckt_play <input.wav> [peak volts] [outdir] [os]\n");
        return 2;
    }
    OS = (os_arg >= 1u && os_arg <= 8u) ? os_arg : 2u;
    if (argc > 5) {
        const int v = atoi(argv[5]);
        TAB_N1 = (v >= 2 && v <= TAB_N1_MAX) ? v : TAB_N1;
    }
    if (argc > 6) {
        const int v = atoi(argv[6]);
        TAB_N2 = (v >= 2 && v <= TAB_N2_MAX) ? v : TAB_N2;
    }
    if (argc > 7 && argv[7][0] != '-') {
        s_cab_ir = argv[7];
    }
    if (argc > 8 && argv[8][0] != '-') {
        s_margin = (float)atof(argv[8]);
    }
    if (argc > 9 && argv[9][0] != '-') {
        s_pickup = atof(argv[9]);
    }
    if (argc > 10 && argv[10][0] != '-') {
        s_hf = atof(argv[10]);
    }
    if (argc > 11 && argv[11][0] != '-') {
        s_lf = atof(argv[11]);
    }
    if (argc > 12 && argv[12][0] != '-') {
        s_extras = atoi(argv[12]) != 0;
    }
    if (argc > 13 && argv[13][0] != '-') {
        s_pot = atof(argv[13]);
        if (s_pot <= 0.0 || s_pot > 1.0) {
            s_pot = 1.0;
        }
    }
    if (argc > 14 && argv[14][0] != '-') {
        s_treble = atof(argv[14]);
    }
    if (argc > 15 && argv[15][0] != '-') {
        s_power = atof(argv[15]);
    }
    if (argc > 16 && argv[16][0] != '-') {
        s_bass = atof(argv[16]);
    }
    if (argc > 17 && argv[17][0] != '-') {
        s_xfmr = atof(argv[17]);
    }
    if (argc > 18 && argv[18][0] != '-') {
        s_xq = atof(argv[18]);
    }
    if (argc > 19 && argv[19][0] != '-') {
        s_lean = atoi(argv[19]) != 0;
    }
    if (argc > 20) {
        s_nots = atoi(argv[20]) != 0;
    }
    {
        const char *b = strrchr(path, '\\');
        const char *b2 = strrchr(path, '/');
        size_t      k;
        if (b2 != NULL && (b == NULL || b2 > b)) {
            b = b2;
        }
        b = (b != NULL) ? b + 1 : path;
        snprintf(tag, sizeof(tag), "%s", b);
        for (k = 0; k < sizeof(tag) && tag[k] != '\0'; k++) {
            if (tag[k] == '.' || tag[k] == ':' || tag[k] == ' ') {
                tag[k] = '\0';
                break;
            }
        }
        if (tag[0] == '\0') {
            snprintf(tag, sizeof(tag), "take");
        }
    }
    printf("  table %d x %d, oversampling x%u, range margin %.0f%%", TAB_N1,
           TAB_N2, OS, (double)s_margin * 100.0);
    if (s_hf > 0.0) {
        printf(", grid low pass %.0f Hz", s_hf);
    }
    if (s_lf > 0.0) {
        printf(", interstage high pass %.0f Hz", s_lf);
    }
    if (s_pot < 1.0) {
        printf(", preamp volume %.2f", s_pot);
    }
    printf("\n");
    if (strcmp(path, "triode") == 0) {
        triode_check();
        return 0;
    }
    if (strcmp(path, "bias") == 0) {
        bias_report(48000.0f * 2.0f);
        return 0;
    }
    if (strncmp(path, "curve", 5) == 0) {
        const float span = (argc > 2) ? (float)atof(argv[2]) : 1.0f;
        curve_report(48000.0f * 2.0f, -span, span);
        return 0;
    }
    if (strncmp(path, "resp", 4) == 0) {
        /*
         * White noise, small enough that the amplifier stays linear.  Its own
         * frequency response is the one thing that had never been measured
         * here, and every judgement about the distortion assumed it flat.
         */
        uint32_t seed = 0x13579BDFu;
        rate = 48000u;
        frames = rate * 4u;
        dry = (float *)calloc(frames, sizeof(float));
        for (i = 0; i < frames; i++) {
            seed = seed * 1664525u + 1013904223u;
            dry[i] = ((float)(int32_t)(seed >> 8) / 8388608.0f - 1.0f) * 0.5f;
        }
        s_resp = 1;
    } else if (strncmp(path, "tone:", 5) == 0 ||
               strncmp(path, "step:", 5) == 0) {
        /*
         * Three seconds of one sine, so that what comes out can be sorted
         * into what the valve made and what the model invented.
         *
         * `step:` drops the level by 30 dB after the first second and looks
         * at the quiet part.  That is the case a steady tone cannot show: the
         * tables are fitted to the loudest thing in the take, so a note that
         * has decayed lives in a handful of their cells, and whatever that
         * costs is exactly what a player hears as a note dies away.
         */
        const int step = path[0] == 's';
        s_tone = atof(path + 5);
        rate = 48000u;
        frames = rate * 5u; /* long enough for a 65536-point analysis */
        dry = (float *)calloc(frames, sizeof(float));
        for (i = 0; i < frames; i++) {
            const float a = (step && i > rate) ? 0.9f / 32.0f : 0.9f;
            dry[i] = a * (float)sin(2.0 * 3.14159265358979 * s_tone * i / rate);
        }
        s_quiet = step;
    } else {
        dry = wav_read(path, &frames, &rate);
    }
    if (dry == NULL) {
        return 1;
    }
    printf("ckt_play: %s, %u frames at %u Hz (%.2f s)\n", path, frames, rate,
           (double)frames / rate);

    /*
     * A second and a half of silence after the take, so that everything
     * downstream - the amplifier's own decay, and the reverb tail especially -
     * has somewhere to ring out instead of being cut off at the last sample of
     * the recording.  Added to whatever came in rather than padding up to a
     * fixed length: a thirty second take needs the tail just as much as a two
     * second one, and needs it in the same place.
     */
    {
        const uint32_t want = frames + rate + rate / 2u;
        float         *bigger = (float *)calloc(want, sizeof(float));
        memcpy(bigger, dry, frames * sizeof(float));
        free(dry);
        dry = bigger;
        frames = want;
        printf("  %.2f s with the tail\n", (double)frames / rate);
    }

    /*
     * The guitar and its cable, which a direct recording leaves out.
     *
     * A DI box presents a megohm and does not load the pickup.  The cable to
     * an amplifier does: a few hundred picofarads across two to four henries
     * of pickup inductance is a resonant low pass around 3 kHz falling at
     * 12 dB an octave, and above it a real valve stage is fed nothing at all.
     * This recording carries pick noise out to 15 kHz, and clipping that
     * produces intermodulation across a band no amplifier has ever seen.
     *
     * Second order, Q of 1.6: the peak at resonance is as much a part of an
     * electric guitar's sound as the roll-off above it.
     *
     * Before the peak is measured, deliberately.  The filter takes the peak
     * down, so normalising first would feed the amplifier less signal and the
     * comparison would be between two different amounts of clipping rather
     * than between two bandwidths.
     */
    if (s_pickup > 0.0) {
        const double w = 2.0 * 3.14159265358979 * s_pickup / rate;
        const double q = 1.6;
        const double al = sin(w) / (2.0 * q);
        const double c = cos(w);
        const double b0 = (1.0 - c) * 0.5, b1 = 1.0 - c, b2 = b0;
        const double a0 = 1.0 + al, a1 = -2.0 * c, a2 = 1.0 - al;
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
        for (i = 0; i < frames; i++) {
            const double x0 = dry[i];
            const double y0 =
                (b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
            x2 = x1;
            x1 = x0;
            y2 = y1;
            y1 = y0;
            dry[i] = (float)y0;
        }
        printf("  pickup and cable: 2nd order, %.0f Hz, Q 1.6\n", s_pickup);
    }

    for (i = 0; i < frames; i++) {
        const float a = dry[i] < 0.0f ? -dry[i] : dry[i];
        if (a > peak) {
            peak = a;
        }
    }
    /*
     * Drive of zero means "cabinet only": put this file through the speaker
     * and nothing else.  It is how a signal that came from somewhere else -
     * another clipper, another program - gets compared on equal terms, which
     * is the only way to tell "our model does this" from "clipping this
     * recording does this".
     */
    if (drive <= 0.0f) {
        uint32_t n = 0;
        float   *c = reverb(dry, frames, rate, 4, AG_IR_WET_MAX, &n);
        printf("  cabinet only, no amplifier\n");
        if (c != NULL) {
            snprintf(name, sizeof(name), "%s/cabonly.wav", outdir);
            wav_write(name, c, n, rate, 2);
            free(c);
        }
        hf_report(dry, frames, 1u, rate, "input, no cabinet");
        return 0;
    }

    printf("  peak %.3f of full scale; driving the amplifier at %.2f V\n",
           (double)peak, (double)drive);
    if (peak > 0.0f) {
        for (i = 0; i < frames; i++) {
            dry[i] = dry[i] / peak * drive;
        }
    }

    input_report(dry, frames, rate);

    /* The amplifier, oversampled, tables fitted to this very recording. */
    dec_init(OS, (double)rate);
    memset(hist, 0, sizeof(hist));
    printf("  baking two JCM800 stages against the recording...\n");
    if (amp_bake((float)(rate * OS), dry, (int)frames, 6000) != 0) {
        return 1;
    }

    /*
     * Prime the DC blocker at the plate's resting voltage before the first
     * sample.  Started from zero it charges through a hundred and seventy
     * volts over its own time constant, and that ramp is bigger than anything
     * in the music - it survives into the file and the normaliser then scales
     * the actual guitar down to nothing.
     */
    {
        float v = 0.0f;
        /*
         * What each stage hands on at rest.  For a stage whose load is a tone
         * stack this is the wiper, not the plate, and the wiper sits at zero
         * because the stack is capacitively coupled - so a zero here is the
         * circuit being right, not the model being broken.
         */
        printf("  resting output of each stage:");
        for (j = 0; j < AMP_STAGES; j++) {
            v = ag_stage_tick(&s_amp.stage[j], v);
            printf("  %u: %.1f V", j, (double)v);
        }
        printf("\n");
        z = v;
        /*
         * The decimator's history has to start there too.  Left at zero it
         * fills over its own length with a step of a hundred and ninety volts
         * - a click bigger than the music, which then owns the peak and the
         * normalisation with it.
         */
        for (j = 0; j < AMP_STAGES; j++) {
            zs[j] = s_stage_v[j];
            for (i = 0; i < DEC_TAPS; i++) {
                hists[j][i] = s_stage_v[j];
            }
        }
        for (i = 0; i < DEC_TAPS; i++) {
            hist[i] = v;
        }
        amp_reset();
    }

    for (j = 0; j < AMP_STAGES; j++) {
        stage_out[j] = (float *)calloc(frames, sizeof(float));
    }
    amp_out = (float *)calloc(frames, sizeof(float));
    up_init(OS, (double)rate);
    for (i = 0; i < frames; i++) {
        float y = 0.0f;
        up_push(dry[i]);
        for (j = 0; j < OS; j++) {
            /*
             * Up to the oversampled rate through the polyphase bank above,
             * not by drawing a straight line between samples.  A straight
             * line is a two tap filter and leaves the input's images sitting
             * just above the audio band, where a clipping valve folds them
             * back down.  Whether that is audible is a question for ears; that
             * it is wrong is not.
             */
            const float in = up_phase(j, OS);
            const float v = amp_tick(in);
            int         k;
            unsigned    st;
            for (k = DEC_TAPS - 1; k > 0; k--) {
                hist[k] = hist[k - 1];
            }
            hist[0] = v;
            for (st = 0; st < AMP_STAGES; st++) {
                for (k = DEC_TAPS - 1; k > 0; k--) {
                    hists[st][k] = hists[st][k - 1];
                }
                hists[st][0] = s_stage_v[st];
            }
        }
        {
            float acc = 0.0f;
            int   k;
            unsigned st;
            for (k = 0; k < DEC_TAPS; k++) {
                acc += s_dec[k] * hist[k];
            }
            y = acc;
            for (st = 0; st < AMP_STAGES; st++) {
                float a = 0.0f;
                for (k = 0; k < DEC_TAPS; k++) {
                    a += s_dec[k] * hists[st][k];
                }
                stage_out[st][i] = dcb(&zs[st], a, 0.002f);
            }
        }
        amp_out[i] = dcb(&z, y, 0.002f);
    }
    printf("  %u lookups off the edge of a table\n", amp_clamped());

    /*
     * A stand-in for the power stage, and nothing more than that.
     *
     * The measurement that prompted it: on two tones, a real amplifier's
     * intermodulation is 29% even-order and ours is 42%, and on one tone its
     * harmonic series is strongly odd - which is what a push-pull output
     * stage does, because the two halves cancel the even orders.  Our whole
     * amplifier is single-ended preamp, which is asymmetric by construction.
     *
     * This is a symmetric waveshaper, not a circuit, and it is here to answer
     * one question: does symmetry account for the difference?  If it does,
     * the thing to build is a modelled push-pull pair, not this.  It is off
     * unless asked for.
     */
    if (s_power > 0.0) {
        float peak_amp = 0.0f;
        for (i = 0; i < frames; i++) {
            const float a = amp_out[i] < 0.0f ? -amp_out[i] : amp_out[i];
            if (a > peak_amp) {
                peak_amp = a;
            }
        }
        if (peak_amp > 0.0f) {
            const double g = s_power;
            for (i = 0; i < frames; i++) {
                const double u = (double)amp_out[i] / peak_amp;
                amp_out[i] = (float)(peak_amp * tanh(g * u) / tanh(g));
            }
        }
        /*
         * And the output transformer's bandwidth, which is the half of a
         * power stage that turned out to matter more.
         *
         * Measured against a capture of a real amplifier on the same take,
         * our model runs 17 dB hotter at 10 kHz and 14 dB hotter at 16 kHz,
         * because nothing in it rolls off up there at all: the preamp is flat
         * to 10 kHz and the cabinet impulse only reaches -24 dB by then.  A
         * real amplifier does not drive its speaker directly; it drives a
         * transformer whose leakage inductance gives out around 6 kHz and
         * falls steeply after.  Four poles is what matches the reference.
         *
         * The low end is the same story from the other side: the primary
         * inductance runs out below about 70 Hz.
         */
        {
            const double fl = 70.0;
            const double w = 2.0 * 3.14159265358979 * s_xfmr / rate;
            const double al = sin(w) / (2.0 * s_xq);
            const double c = cos(w);
            const double b0 = (1.0 - c) * 0.5, b1 = 1.0 - c, b2 = b0;
            const double a0 = 1.0 + al, a1 = -2.0 * c, a2 = 1.0 - al;
            double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
            double s3 = 0.0, s4 = 0.0, h1 = 0.0, h2 = 0.0;
            const double cl = 1.0 - exp(-2.0 * 3.14159265358979 * fl / rate);
            const double ch =
                1.0 - exp(-2.0 * 3.14159265358979 * s_xfmr * 2.0 / rate);
            for (i = 0; i < frames; i++) {
                double v = amp_out[i], y0;
                h1 += cl * (v - h1);
                v -= h1;
                h2 += cl * (v - h2);
                v -= h2;
                /* Resonant pole pair: the peak is the presence, the skirt is
                 * the transformer giving out. */
                y0 = (b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
                x2 = x1;
                x1 = v;
                y2 = y1;
                y1 = y0;
                /* Two more poles an octave up, for the cliff that a single
                 * resonant pair is too gentle to give. */
                s3 += ch * (y0 - s3);
                s4 += ch * (s3 - s4);
                amp_out[i] = (float)s4;
            }
        }
        printf("  power stage stand-in: symmetric drive %.1f, "
               "transformer 70 Hz, %.0f Hz Q %.1f\n",
               s_power, s_xfmr, s_xq);
    }

    if (s_tone > 0.0) {
        spectrum_junk(dry, frames, rate, s_tone, "input");
        spectrum_junk(amp_out, frames, rate, s_tone, "amplifier output");
    }
    if (s_resp) {
        unsigned st;
        response_report(dry, amp_out, frames, rate);
        for (st = 0; st < AMP_STAGES; st++) {
            printf("  stage %u alone:\n", st);
            response_report(st == 0 ? dry : stage_out[st - 1], stage_out[st],
                            frames, rate);
        }
        return 0;
    }

    /*
     * Against the reference solver, on this recording.
     *
     * The silence measurement above says what the model does when nothing is
     * happening, and after the antialiasing was fixed it says -200 dB, which
     * is to say nothing at all.  That does not clear the model: an artefact
     * that only appears while the signal moves - a table cell boundary being
     * crossed, say - is invisible to it.  The only honest reference is the
     * circuit itself, solved the slow way, on the same input.
     */
    {
        static ag_ckt_t ref[AMP_STAGES];
        int             out_node[AMP_STAGES];
        double          err = 0.0, sig = 0.0, ma = 0.0, mb = 0.0;
        int             ok = 1;
        uint32_t        n;

        for (j = 0; j < AMP_STAGES; j++) {
            ckt_stage_spec_t sp;
            ckt_jcm800_stage(&sp, (int)j);
            /* The same circuit the baked model was made from, knobs and all -
             * otherwise the comparison is between two different amplifiers. */
            stage_hf(&sp, (int)j);
            out_node[j] = ckt_build_spec_stage(&ref[j], (float)(rate * OS), &sp);
            if (!out_node[j]) {
                ok = 0;
            }
        }
        if (ok) {
            float *refout = (float *)calloc(frames, sizeof(float));
            float  rz;
            float  rhist[DEC_TAPS];
            /* Settle the solver to its own operating point. */
            for (i = 0; i < (uint32_t)(rate * OS / 2u); i++) {
                float v = 0.0f;
                for (j = 0; j < AMP_STAGES; j++) {
                    v = ag_ckt_tick(&ref[j], v, out_node[j]);
                }
            }
            {
                float v = 0.0f;
                for (j = 0; j < AMP_STAGES; j++) {
                    v = ag_ckt_tick(&ref[j], v, out_node[j]);
                }
                rz = v;
                for (i = 0; i < DEC_TAPS; i++) {
                    rhist[i] = v;
                }
            }
            for (i = 0; i < frames; i++) {
                float acc = 0.0f;
                int   k;
                for (j = 0; j < OS; j++) {
                    const float prev = (i > 0) ? dry[i - 1] : dry[0];
                    const float frac =
                        (OS > 1u) ? (float)(j + 1) / (float)OS : 1.0f;
                    const float in = prev + (dry[i] - prev) * frac;
                    float       v = in;
                    unsigned    q;
                    for (q = 0; q < AMP_STAGES; q++) {
                        v = ag_ckt_tick(&ref[q], v, out_node[q]);
                        /* The preamp volume lives between the valves here as
                         * well, or the two chains are not the same chain. */
                        if (q == 0) {
                            v *= (float)s_pot;
                        }
                    }
                    for (k = DEC_TAPS - 1; k > 0; k--) {
                        rhist[k] = rhist[k - 1];
                    }
                    rhist[0] = v;
                }
                for (k = 0; k < DEC_TAPS; k++) {
                    acc += s_dec[k] * rhist[k];
                }
                refout[i] = dcb(&rz, acc, 0.002f);
            }
            n = frames;
            for (i = 0; i < n; i++) {
                ma += refout[i];
                mb += amp_out[i];
            }
            ma /= n;
            mb /= n;
            for (i = 0; i < n; i++) {
                const double d = (refout[i] - ma) - (amp_out[i] - mb);
                err += d * d;
                sig += (refout[i] - ma) * (refout[i] - ma);
            }
            printf("  baked against the solver on this take: %.1f dB\n",
                   10.0 * log10(err / (sig + 1e-30)));
            /*
             * How much of that gap belongs to the reference rather than to
             * the model.  The stage carrying the tone stack is hard for the
             * running solver - 250 pF sitting next to 0.68 uF is eight orders
             * of magnitude of companion conductance in one matrix - so if it
             * is the one failing to converge, the disagreement is the
             * reference wobbling, not the table.
             */
            printf("  (solver unconverged, per stage:");
            for (j = 0; j < AMP_STAGES; j++) {
                printf(" %u: %u/%u", j, ref[j].nonconverged, ref[j].samples);
            }
            printf(")\n");
            /*
             * And write it out.  Until now this render existed only to be
             * subtracted; but "does the table do this, or does the circuit?"
             * is a question for the ears as much as for a number, and the
             * only way to answer it is to hear the circuit solved the slow
             * way, on the same take, through the same cabinet.
             */
            {
                uint32_t nn = 0;
                float   *c;
                snprintf(name, sizeof(name), "%s/%s_%dmV_os%u%s_solver.wav",
                         outdir, tag, (int)(drive * 1000.0f + 0.5f), OS, "");
                wav_write(name, refout, frames, rate, 1);
                c = reverb(refout, frames, rate, 4, AG_IR_WET_MAX, &nn);
                if (c != NULL) {
                    snprintf(name, sizeof(name),
                             "%s/%s_%dmV_os%u_solver_cab.wav", outdir, tag,
                             (int)(drive * 1000.0f + 0.5f), OS);
                    wav_write(name, c, nn, rate, 2);
                    free(c);
                }
            }
            free(refout);
        }
    }

    /*
     * The model's own noise floor, measured against digital silence.
     *
     * Measuring it from the quietest stretch of the music does not work once
     * the amplifier is distorting: three stages of compression hold a decaying
     * note up, so the quiet part is not quiet and the figure reports the
     * sustain rather than the noise.  Half a second of nothing at the input
     * separates the two - whatever comes out is the model talking to itself.
     */
    {
        double sum = 0.0;
        float  zz = z;
        uint32_t n = rate / 2u;
        amp_reset();
        for (i = 0; i < DEC_TAPS; i++) {
            hist[i] = z;
        }
        for (i = 0; i < n; i++) {
            float y;
            for (j = 0; j < OS; j++) {
                const float v = amp_tick(0.0f);
                int         k;
                for (k = DEC_TAPS - 1; k > 0; k--) {
                    hist[k] = hist[k - 1];
                }
                hist[0] = v;
            }
            {
                float acc = 0.0f;
                int   k;
                for (k = 0; k < DEC_TAPS; k++) {
                    acc += s_dec[k] * hist[k];
                }
                y = dcb(&zz, acc, 0.002f);
            }
            if (i > rate / 10u) { /* past the blocker's own settling */
                sum += (double)y * y;
            }
        }
        {
            const double rms = sqrt(sum / (n - rate / 10u));
            float        out_peak = 0.0f;
            for (i = 0; i < frames; i++) {
                const float a = amp_out[i] < 0.0f ? -amp_out[i] : amp_out[i];
                if (a > out_peak) {
                    out_peak = a;
                }
            }
            printf("  noise floor on silence: %.1f dB below the loudest the "
                   "amplifier got (%.2e V rms against %.1f V)\n",
                   20.0 * log10((rms + 1e-30) /
                                (out_peak > 0.0f ? out_peak : 1.0f)),
                   rms, (double)out_peak);
        }
    }

    /*
     * A control clipper: the same recording through plain tanh, sixteen times
     * oversampled, in double.  Off unless asked for - it is a control, and
     * once it has answered its question it is sixteen times the work of the
     * thing actually being listened to.
     *
     * It answers a question no amount of looking at the model can: whether
     * heavy clipping of *this material* is gritty by itself.  If this comes
     * out smooth and the valve stage does not, the fault is the model's; if
     * both rattle, it is the recording being amplified sixty decibels and no
     * amplifier would have been kind to it.
     *
     * Deliberately not a valve: symmetric, memoryless, no operating point and
     * no table.  There is nothing in it to go wrong, which is the point.
     */
    if (s_extras) {
        const unsigned tos = 16u;
        float         *ref = (float *)calloc(frames, sizeof(float));
        double         zt = 0.0;
        float          th[DEC_TAPS];
        const float    g = 20.0f / (drive > 0.0f ? drive : 1.0f);

        dec_init(tos, (double)rate);
        memcpy(th, s_dec, sizeof(th));
        memset(hist, 0, sizeof(hist));
        for (i = 0; i < frames; i++) {
            double acc = 0.0;
            int    k;
            for (j = 0; j < tos; j++) {
                const float  prev = (i > 0) ? dry[i - 1] : dry[0];
                const float  frac = (float)(j + 1) / (float)tos;
                const double in = prev + (dry[i] - prev) * frac;
                for (k = DEC_TAPS - 1; k > 0; k--) {
                    hist[k] = hist[k - 1];
                }
                hist[0] = (float)tanh((double)g * in);
            }
            for (k = 0; k < DEC_TAPS; k++) {
                acc += (double)th[k] * hist[k];
            }
            ref[i] = (float)acc;
            (void)zt;
        }
        snprintf(name, sizeof(name), "%s/%s_tanh_ref.wav", outdir, tag);
        wav_write(name, ref, frames, rate, 1);
        {
            uint32_t n = 0;
            float   *c = reverb(ref, frames, rate, 4, AG_IR_WET_MAX, &n);
            if (c != NULL) {
                snprintf(name, sizeof(name), "%s/%s_tanh_ref_cab.wav", outdir, tag);
                wav_write(name, c, n, rate, 2);
                free(c);
            }
        }
        hf_report(ref, frames, 1u, rate, "tanh control, no cabinet");
        free(ref);
        dec_init(OS, (double)rate); /* put back what the amplifier used */
    }

    {
        const int mv = (int)(drive * 1000.0f + 0.5f);
        /*
         * Anything that changes the sound goes in the file name, so that a
         * sweep lands in one directory instead of a directory per setting.
         * Renders that overwrite each other, or hide in folders, cannot be
         * compared - and comparing them is the whole point.
         */
        char sfx[48];
        sfx[0] = '\0';
        if (s_hf > 0.0) {
            snprintf(sfx, sizeof(sfx), "_hf%.0f", s_hf);
        }
        if (s_lf > 0.0) {
            const size_t k = strlen(sfx);
            snprintf(sfx + k, sizeof(sfx) - k, "_lf%.0f", s_lf);
        }
        if (s_pot < 1.0) {
            const size_t k = strlen(sfx);
            snprintf(sfx + k, sizeof(sfx) - k, "_pot%.0f", s_pot * 100.0);
        }
        if (s_treble > 0.0) {
            const size_t k = strlen(sfx);
            snprintf(sfx + k, sizeof(sfx) - k, "_tre%.0f", s_treble * 100.0);
        }
        if (s_power > 0.0) {
            const size_t k = strlen(sfx);
            snprintf(sfx + k, sizeof(sfx) - k, "_pwr%.0f", s_power * 10.0);
        }
        if (s_bass > 0.0) {
            const size_t k = strlen(sfx);
            snprintf(sfx + k, sizeof(sfx) - k, "_bas%.0f", s_bass * 100.0);
        }
        if (s_nots) {
            const size_t k = strlen(sfx);
            snprintf(sfx + k, sizeof(sfx) - k, "_nots");
        }
        if (AMP_STAGES != 2) {
            const size_t k = strlen(sfx);
            snprintf(sfx + k, sizeof(sfx) - k, "_st%d", AMP_STAGES);
        }
        if (s_lean) {
            const size_t k = strlen(sfx);
            snprintf(sfx + k, sizeof(sfx) - k, "_lean");
        }
        if (s_pickup > 0.0) {
            const size_t k = strlen(sfx);
            snprintf(sfx + k, sizeof(sfx) - k, "_pu%.0f", s_pickup);
        }
        snprintf(name, sizeof(name), "%s/%s_%dmV_os%u%s_crunch.wav", outdir, tag,
                 mv, OS, sfx);
        wav_write(name, amp_out, frames, rate, 1);

        /*
         * And each stage on its own.  A stage that misbehaves is inaudible
         * once the stage after it has clipped what came out, so the only way
         * to hear which one is at fault is to hear them apart.
         */
        for (j = 0; j < AMP_STAGES; j++) {
            uint32_t n = 0;
            float   *c;
            snprintf(name, sizeof(name), "%s/%s_%dmV_os%u_stage%u.wav", outdir, tag,
                     mv, OS, j);
            wav_write(name, stage_out[j], frames, rate, 1);
            if (s_tone > 0.0) {
                char t[32];
                snprintf(t, sizeof(t), "stage %u alone", j);
                spectrum_junk(stage_out[j], frames, rate, s_tone, t);
            }
            c = reverb(stage_out[j], frames, rate, 4, AG_IR_WET_MAX, &n);
            if (c != NULL) {
                snprintf(name, sizeof(name), "%s/%s_%dmV_os%u_stage%u_cab.wav",
                         outdir, tag, mv, OS, j);
                wav_write(name, c, n, rate, 2);
                free(c);
            }
        }

        /*
         * The control: the recording through the same cabinet with no
         * amplifier at all.  If a rattle is audible here it belongs to the
         * convolution or to the file; if it is not, it belongs to the model,
         * and there is no third place for it to come from.
         */
        if (s_extras) {
            uint32_t n = 0;
            float   *c = reverb(dry, frames, rate, 4, AG_IR_WET_MAX, &n);
            if (c != NULL) {
                snprintf(name, sizeof(name), "%s/%s_dry_cab.wav", outdir, tag);
                wav_write(name, c, n, rate, 2);
                free(c);
            }
        }

        /* Through a cabinet, which is what makes a preamp sound like an amp. */
        {
            uint32_t n = 0;
            float   *cab =
                /* A cabinet is not a mix: fully wet, or the top end it exists
                 * to remove walks straight past it through the dry path. */
                reverb(amp_out, frames, rate, 4 /* cab */, AG_IR_WET_MAX, &n);
            if (cab != NULL) {
                hf_report(amp_out, frames, 1u, rate, "amplifier, no cabinet");
                hf_report(cab, n, 2u, rate, "after the cabinet");
                snprintf(name, sizeof(name),
                         "%s/%s_%dmV_os%u%s_crunch_cab.wav", outdir, tag, mv, OS,
                         sfx);
                wav_write(name, cab, n, rate, 2);
                free(cab);
            }
        }
        if (s_extras) {
            uint32_t n = 0;
            float   *rv = reverb(amp_out, frames, rate, 1 /* hall */, 64, &n);
            if (rv != NULL) {
                snprintf(name, sizeof(name), "%s/%s_%dmV_os%u_crunch_hall.wav",
                         outdir, tag, mv, OS);
                wav_write(name, rv, n, rate, 2);
                free(rv);
            }
        }
    }

    /* And the dry guitar through the reverbs on their own. */
    if (s_extras) {
        static const struct {
            int         preset;
            uint8_t     wet;
            const char *tag;
        } rv[] = { { 0, 80, "room" }, { 1, 90, "hall" }, { 2, 90, "spring" } };
        unsigned r;
        for (r = 0; r < sizeof(rv) / sizeof(rv[0]); r++) {
            uint32_t n = 0;
            float   *y = reverb(dry, frames, rate, rv[r].preset, rv[r].wet, &n);
            if (y != NULL) {
                snprintf(name, sizeof(name), "%s/%s_%s.wav", outdir, tag,
                         rv[r].tag);
                wav_write(name, y, n, rate, 2);
                free(y);
            }
        }
    }

    free(amp_out);
    free(dry);
    return 0;
}

/*
 * The noise floor, measured, because it is the one thing about this model that
 * is worse than it looks.
 *
 * Peak against the quietest fiftieth of a second, on the same clean guitar
 * note, after normalising every render to the same peak:
 *
 *   drive     x1      x2      x4
 *   10 mV   50.0    56.7    66.4 dB
 *   250 mV    -     76.2      -
 *
 * Two things to read out of that.  The floor is roughly constant in absolute
 * terms, so it matters least where the amplifier is loudest - at crunch levels
 * 76 dB is past what a real valve amplifier manages.  And it improves by seven
 * to ten decibels for every doubling of the oversampling, which says what it
 * is: the table is interpolated linearly, so it has a corner at every cell
 * boundary, and a corner is broadband.  On a quiet signal those corners are
 * the largest nonlinearity in the circuit, which is an artefact pretending to
 * be a valve.
 *
 * The fix is a smoother interpolation - Hermite across four points rather than
 * linear across two - which removes the corners rather than filtering them.
 * That costs more per lookup and has not been measured yet.
 */
