/*
 * tube_live - the amplifier with the knobs still attached.
 *
 * A recording is looped through ag_amp and the cabinet and pushed straight at
 * the sound card, while the keyboard moves the knobs.  Everything else in this
 * tree renders a file and asks somebody to listen to it afterwards, which is
 * fine for a measurement and useless for a question like "how much of the diode
 * curve is too much" - that one is answered by turning something slowly and
 * hearing where it stops sounding right.
 *
 *   build-host/tube_live [take.wav]
 *
 * WHY THE KNOBS DO NOT CLICK, AND WHERE THEY STILL DO
 *
 * ag_amp_set_voicing clears the filters, which is right for a tool that measures
 * one setting after another and wrong here - it would drop the note on every
 * keypress.  So the knobs go through ag_amp_set_knobs, which redesigns the
 * coefficients and leaves the state alone.  A coefficient that moves under a
 * running filter is still a step, but a step in proportion to how far the knob
 * moved, and the increments here are small.
 *
 * Two things are honestly discontinuous and are marked as such when they happen:
 * changing the oversampling, because the halfbands hold history at the old rate,
 * and changing the number of stages, because that is a different chain.
 *
 * THE BLEND KNOB IS THE POINT
 *
 * ag_amp_blend_stage re-mixes a stage's tables from two baked curves, which is
 * about 50 000 instructions for 2048 points - a fifth of a millisecond, against
 * the 11.6 ms of a block.  So the knob really does run at block rate, and this
 * program is also the measurement of whether that is fast enough to sound
 * continuous: 86 steps a second, each one a fresh table under a running signal.
 * If it zippers, it zippers here first.
 *
 * The bake that the first blend on a stage needs is done at startup for every
 * stage of every chain, so that no keypress ever waits for a curve.
 *
 * LATENCY
 *
 * Seventy milliseconds out of the card, plus the cabinet's own group delay.
 * That is fine for turning a knob and listening; it is not a number to play a
 * guitar through, and there is no guitar input here anyway.
 *
 * Seventy is held at any rate, by counting buffers rather than fixing them - see
 * NBUF_MS.  It used to be six blocks of 256, which was 70 ms only at 22.05 kHz;
 * when the default take became a 44.1 kHz file the same six blocks became 35 ms
 * and the tool crackled.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <conio.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include <mmsystem.h>
/* For flush-to-zero.  See the note in main: a decaying convolution tail is what
 * makes an audio program slower when there is less to hear. */
#include <xmmintrin.h>

#include "ag_amp.h"
#include "ag_ckt.h"
#include "ag_ir.h"
#include "ag_tube.h"

#define BLK  ((int)AG_IR_BLOCK) /* the cabinet's block, so nothing has to buffer */
/*
 * The queue is sized in **milliseconds, not blocks**, and that is a bug fix.
 *
 * The card is opened at whatever rate the input file has, so a fixed count of
 * fixed-size blocks buys a length of audio that shrinks as the rate rises: six
 * blocks of 256 is 70 ms at 22.05 kHz and 35 ms at 44.1.  When the default take
 * became a 44.1 kHz file the queue halved without anybody choosing that, and
 * 35 ms across six buffers is not enough for a polling loop that also writes to
 * a Windows console - a console write can block for tens of milliseconds, and
 * what that sounds like is crackle.
 *
 * 70 ms is the number the design was written around, so it is the number kept
 * here, at any rate.
 */
#define NBUF_MS  70
#define NBUF_MAX 32

/*
 * The default take, and the two names are not interchangeable.
 *
 * `tube_di_22k.wav` is 44.1 kHz despite its name (the module README says so at
 * length), and at 44.1 this program costs four times what it costs at 22.05 -
 * twice the samples and twice the cabinet taps - for a comparison that is *less*
 * faithful to the product, which runs at 22.05.  The fitted impulses are 22.05
 * too, so at 44.1 the A/B on `z` also has one side band-limited and the other
 * not.  So the 22.05 kHz take is the default when it is there, and the misnamed
 * one is the fallback.
 */
#ifndef AG_DI_DEFAULT
#define AG_DI_DEFAULT "build/listen/tube_di_22050.wav"
#endif
#ifndef AG_DI_FALLBACK
#define AG_DI_FALLBACK "build/listen/tube_di_22k.wav"
#endif
/* The impulse the voicing in ag_amp_defaults was fitted through; see the note in
 * tube_render.c about why the ag_ir presets are not cabinets. */
#ifndef AG_CAB_MATCHED
#define AG_CAB_MATCHED "build/nam/imp_mars.wav"
#endif
#ifndef AG_CAB_FALLBACK
#define AG_CAB_FALLBACK "assets/audio/guitar-di/1 Vox AC30 1.wav"
#endif

/* ------------------------------------------------------------------------ */
/* where the tree is                                                         */
/* ------------------------------------------------------------------------ */

/*
 * Every default path in this file is written from the root of the tree, and the
 * natural place to start the program from is build-host, where it is built - so
 * from there they are all one level off.  Rather than telling anybody to cd,
 * find the root: argon.cmd sits in it and nowhere else.
 *
 * Above the working directory first and above the executable second, so it works
 * both from a shell that is already somewhere in the tree and from a double
 * click on the exe.
 */
static char g_root[512];

static int file_at(const char *root, const char *rel)
{
    char  b[1024];
    FILE *f;

    snprintf(b, sizeof(b), "%s%s", root, rel);
    f = fopen(b, "rb");
    if (f == NULL) {
        return 0;
    }
    fclose(f);
    return 1;
}

static void find_root(void)
{
    static const char *up[] = { "", "../", "../../", "../../../",
                                "../../../../" };
    /* One shorter than what it is copied into, so that the separator appended
     * below cannot be the character that gets truncated. */
    char               exe[500];
    size_t             i;

    for (i = 0; i < sizeof(up) / sizeof(up[0]); i++) {
        if (file_at(up[i], "argon.cmd")) {
            snprintf(g_root, sizeof(g_root), "%s", up[i]);
            return;
        }
    }
    if (GetModuleFileNameA(NULL, exe, sizeof(exe)) > 0) {
        for (i = 0; i < sizeof(up) / sizeof(up[0]); i++) {
            char *sep = strrchr(exe, '\\');
            char  cand[512];
            if (sep == NULL) {
                break;
            }
            *sep = 0;
            snprintf(cand, sizeof(cand), "%s\\", exe);
            if (file_at(cand, "argon.cmd")) {
                snprintf(g_root, sizeof(g_root), "%s", cand);
                return;
            }
        }
    }
}

/* `rel` as it stands if it opens from here, from the root otherwise.  NULL if it
 * is in neither place. */
static const char *resolve(char *buf, size_t n, const char *rel)
{
    if (file_at("", rel)) {
        snprintf(buf, n, "%s", rel);
        return buf;
    }
    if (g_root[0] != 0 && file_at(g_root, rel)) {
        snprintf(buf, n, "%s%s", g_root, rel);
        return buf;
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* wav in                                                                    */
/* ------------------------------------------------------------------------ */

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t rd_u16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

/* Mono float, first channel only, 16 or 24 bit - the same reader tube_render
 * uses, because the cabinet impulses in packs are 24-bit. */
static float *read_wav(const char *path, uint32_t *frames, uint32_t *rate)
{
    FILE    *f = fopen(path, "rb");
    uint8_t *buf;
    long     len;
    uint32_t pos = 12, ch = 1, bits = 16, off = 0, dlen = 0, i;
    float   *out;

    if (f == NULL) {
        printf("  cannot read %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 44) {
        fclose(f);
        return NULL;
    }
    buf = (uint8_t *)malloc((size_t)len);
    if (buf == NULL || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    while (pos + 8 <= (uint32_t)len) {
        const uint32_t sz = rd_u32(buf + pos + 4);
        if (memcmp(buf + pos, "fmt ", 4) == 0) {
            ch = rd_u16(buf + pos + 10);
            *rate = rd_u32(buf + pos + 12);
            bits = rd_u16(buf + pos + 22);
        } else if (memcmp(buf + pos, "data", 4) == 0) {
            off = pos + 8;
            dlen = sz;
        }
        pos += 8 + sz + (sz & 1u);
    }
    if (off == 0 || ch == 0 || (bits != 16 && bits != 24)) {
        printf("  %s: need 16- or 24-bit PCM (got %u)\n", path, bits);
        free(buf);
        return NULL;
    }
    {
        const uint32_t bps = bits / 8u;
        *frames = dlen / (bps * ch);
        out = (float *)malloc(sizeof(float) * (*frames ? *frames : 1));
        if (out == NULL) {
            free(buf);
            return NULL;
        }
        for (i = 0; i < *frames; i++) {
            const uint8_t *p = buf + off + bps * ch * i;
            if (bits == 16) {
                out[i] = (float)(int16_t)rd_u16(p) / 32768.0f;
            } else {
                const int32_t s =
                    (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 |
                              (uint32_t)p[2] << 24);
                out[i] = (float)(s >> 8) / 8388608.0f;
            }
        }
    }
    free(buf);
    return out;
}

static double peak_of(const float *x, uint32_t n)
{
    double   p = 0.0;
    uint32_t i;
    for (i = 0; i < n; i++) {
        const double v = x[i] < 0.0f ? -(double)x[i] : (double)x[i];
        if (v > p) {
            p = v;
        }
    }
    return p;
}

/* ------------------------------------------------------------------------ */
/* the chain                                                                 */
/* ------------------------------------------------------------------------ */

static float   *g_in;
static uint32_t g_frames, g_rate;
static uint32_t g_pos;

/*
 * The reference: the NAM Marshall's render of the same performance, played
 * straight to the card - no chain, no cabinet, because it already went through
 * the real thing.  One key away from the model so the band solo can be held on
 * a frequency while the two trade places; that is the only honest way to ask
 * whether a fizz in the model is a defect or what an amplifier sounds like,
 * because the answer up close is "both sound like that" more often than anyone
 * expects.  Kept on its own loop position so switching does not restart the
 * phrase.  Fitted at drive 0.5, so that is where the comparison is fair.
 */
#ifndef AG_REF_DEFAULT
#define AG_REF_DEFAULT "build/listen/ref_mars_22k.wav"
#endif
static float   *g_ref;
static uint32_t g_ref_frames;
static uint32_t g_ref_pos;
static int      g_ref_on;
/*
 * What the reference is multiplied by so that `y` is a comparison and not a
 * volume test.
 *
 * A capture's output level is whatever the person who made it played at, and our
 * master knob is set to fill sixteen bits: measured on the three models, the
 * reference came in +2.2 dB, +0.8 dB and **-13.7 dB** against the chain it is
 * meant to be compared with.  Fourteen decibels of level difference is not a tone
 * comparison, it is a loudness comparison, and loudness wins every one of those.
 * Matched on rms over the first second of the take - the same material on both
 * sides - and printed.
 */
static float    g_ref_gain = 1.0f;

static ag_ckt_t *g_ckt;

/*
 * One built chain per stage count, so that 1..4 is a keypress and not a bake.
 *
 * Four chains is 1.5 MB of tables on a host that has gigabytes, and the
 * alternative is a two-second stall in the middle of listening - which is the
 * kind of thing that stops somebody from trying the comparison at all.
 */
static ag_amp_t *g_amp[AG_AMP_STAGES];
static float    *g_tab[AG_AMP_STAGES];
static float    *g_work[AG_AMP_STAGES][AG_AMP_STAGES]; /* [chain][stage] */
static float     g_mix[AG_AMP_STAGES][AG_AMP_STAGES];  /* what each is set to */

static int          g_n = 2;      /* live stage count, 1..AG_AMP_STAGES */
static int          g_bstage;     /* which stage the blend knob turns */
static ag_amp_cfg_t g_cfg;        /* the knobs, shared across the chains */
/* Which amplifier, from --model; fixed for the run - see the note where it is
 * parsed for why it is not on a key. */
static int          g_model = AG_AMP_MODEL_JCM800;
/* Whether the post-valve matching bank is in the chain; `z` toggles it. */
static int          g_post = 1;

/*
 * How much of the two fitted voicing banks to apply, 0 to 1.
 *
 * Not a nicety - it is the knob that found the fizz.  Measured against
 * ckt_exact on a signal with nothing above 1 kHz, so that every decibel up
 * there is something the chain made: the bare valves put -48.9 dB into
 * 3.5-6 kHz relative to their own 200-1500 band and the exact solver puts
 * -47.7, which says the tables, the antialiasing and the oversampling are
 * faithful to about a decibel.  Then the pre bank adds 8 dB there and the tone
 * stack adds another 17.
 *
 * Twenty-five decibels of gain on an intermodulation product is what an
 * unpleasant fizz is.  The fit that produced those numbers matched the
 * third-octave *magnitude* of a Marshall capture, and a magnitude fit cannot
 * tell a dense harmonic series from sparse hash - it only knows how much energy
 * belongs in the band.  The reference has that energy because a three-valve amp
 * and a power section make it; ours got it by amplifying what two valves made.
 *
 * So this knob exists to be turned down, and what it says at 0.3 or 0.5 is
 * evidence about how the voicing should be re-fitted.
 */
static float        g_bank = 1.0f;
/*
 * The originals of every matching bank, so that the `bank` knob scales from the
 * fitted answer rather than from wherever it left the numbers last.
 *
 * One per stage now, not one for the chain: `ladder` fits a block in front of each
 * valve, and a knob that scaled only the first block would be turning down a
 * seventh of the fit while claiming to turn down all of it.  What it does *not*
 * scale is `cfg.vtrim`, and that is deliberate - the trims are where the drive
 * sits, not what colour it is, and taking them out at bank 0 would move the level
 * by twenty decibels and make the comparison useless.  They are printed in the
 * status line instead.
 */
static ag_amp_band_t g_voice0[AG_AMP_STAGES][AG_AMP_VOICE_N];
static ag_amp_band_t g_tone0[AG_AMP_VOICE_N];
static float         g_mid_db0;
static int          g_bypass;
static float        g_vol = 1.0f; /* after the cabinet, so it changes nothing */

static ag_amp_t *amp(void) { return g_amp[g_n - 1]; }

/* ------------------------------------------------------------------------ */
/* the cabinet                                                               */
/* ------------------------------------------------------------------------ */

/*
 * TWO CABINETS AT ONCE, BECAUSE THE A/B IS BETWEEN TWO ARCHITECTURES
 *
 * The question this tool has to answer is "a bank of biquads after the valves,
 * or an impulse fitted to do that job instead" - and that is not a comparison
 * unless the two are one keypress apart.  Asking it needs two impulses in the
 * program at the same time: the one the shipping voicing was fitted *through* (a
 * cabinet, with the bank in front of it) and the one fitted to carry the
 * correction *itself* (no bank).  So both are loaded and built at startup -
 * int16 and float, staging measured, levels matched - and 'z' swaps the bank and
 * the impulse together.
 *
 * It was two runs of the program before this, and two runs cannot be compared:
 * the note is in a different place, the ear has moved on, and what comes back is
 * an opinion about a memory.  The price is a second ag_ir and a second set of
 * float taps, which on a PC is nothing.
 *
 * The globals below are the *live* cabinet - the hot loop reads them directly -
 * and a slot is a saved copy of them.  Swapping is pointer shuffling, so each
 * cabinet keeps its own history and its own staging, and nothing is rebuilt when
 * the key is pressed.
 */
static ag_ir_t *g_ir;
static int16_t *g_mono, *g_st;
static float    g_head = 1.0f;      /* the fixed floor, sized as run_cab does */
static float    g_head_lim = 1.0f;  /* 1 / the impulse's own gain above unity */
static float    g_head_auto = 1.0f; /* what the staging is actually using now */
static uint32_t g_clip;
/*
 * What the live cabinet's output is multiplied by, so that the switch is not a
 * volume test.
 *
 * Both impulses are peak-normalised, and a peak says nothing about how loud a
 * cabinet is: a fitted impulse carries a presence lift and a top cut, so at the
 * same peak it passes a different amount of power.  Matched on the impulse's own
 * energy - the broadband power gain the convolution has for any input - and the
 * decibels are printed at startup, because if they are large that is itself a
 * fact about the fit.
 */
static float    g_match = 1.0f;
static double   g_energy; /* sqrt of the sum of squares of the live impulse */

typedef struct {
    ag_ir_t *ir;                        /* the chip's int16 convolution */
    float   *fir;                       /* the same impulse in float, reversed */
    int      fir_n;
    float   *fhist;
    float    head, head_lim, head_auto; /* the int16 staging, per impulse */
    float    match;
    double   energy;
    int      loaded;
    char     path[600];
} cab_t;

/* [0] the cabinet the shipping chain plays through, [1] the fitted impulse that
 * stands in for the whole post-valve bank. */
static cab_t g_cab[2];
static int   g_cabi; /* which slot the live globals are holding */
/* What slot 1 was looked for under, so that the key which needs it can say what
 * is missing rather than leaving somebody to guess. */
static char  g_fit_rel[200] = "no fitted impulse was looked for yet";

/*
 * The same cabinet in float, which on a PC is what a listening tool should use.
 *
 * ag_ir is int16 end to end because the chip is, and int16 has a floor:
 * measured against the identical convolution in double precision on the same
 * take, the error is -24 dB to the signal in the 0.5-2 kHz band, -12 dB at
 * 4-6 kHz, and *louder than the signal* above 6 kHz - because the error is
 * spectrally flat while a loudspeaker falls off a cliff up there.  It is heard
 * as a signal-modulated rattle from 2 kHz up, nearly independent of the note.
 * Adaptive staging brought the level to the convolution's own floor; the floor
 * itself is the arithmetic.
 *
 * So the float path is the default here, and the int16 path stays one keypress
 * away - not as a fallback but as the measurement: the difference between the
 * two IS the chip's convolution noise, and ears on that difference are worth
 * more than any number in this comment.
 *
 * Direct form, 4410 taps at 22.05 kHz - about a hundred million multiplies a
 * second, which a PC does without noticing and the chip is why ag_ir exists.
 */
static float *g_fir;      /* the impulse, resampled, reversed, level-matched */
static int    g_fir_n;
static float *g_fhist;    /* g_fir_n - 1 + BLK, newest sample last */
static int    g_cab_mode; /* 0 off, 1 float, 2 int16 through ag_ir */
static int    g_cab_want = 1; /* which arithmetic 'c' brings back */

/*
 * Windowed-sinc resampling, because the float path cannot borrow ag_ir's - that
 * one is int16 too, and the point is to have a reference free of the arithmetic
 * being judged.  64 taps of Blackman-windowed sinc, cut at 0.45 of the lower
 * rate: -74 dB stopband, flat to a tenth of a decibel below 9 kHz.
 */
static float *resample_ir(const float *x, uint32_t n, uint32_t sr_in,
                          uint32_t sr_out, uint32_t *out_n)
{
    const double ratio = (double)sr_out / (double)sr_in;
    const double fc = 0.45 * (ratio < 1.0 ? ratio : 1.0);
    const int    half = 32;
    const double pi = 3.14159265358979;
    uint32_t     m = (uint32_t)((double)n * ratio);
    float       *y;
    uint32_t     i;

    y = (float *)malloc(sizeof(float) * (m > 0 ? m : 1));
    if (y == NULL) {
        return NULL;
    }
    for (i = 0; i < m; i++) {
        const double t = (double)i / ratio;
        const int    c = (int)t;
        double       acc = 0.0;
        int          k;
        for (k = c - half + 1; k <= c + half; k++) {
            double d, s, w;
            if (k < 0 || (uint32_t)k >= n) {
                continue;
            }
            d = t - (double)k;
            s = d == 0.0 ? 2.0 * fc : sin(2.0 * pi * fc * d) / (pi * d);
            w = 0.42 + 0.5 * cos(pi * d / (double)half) +
                0.08 * cos(2.0 * pi * d / (double)half);
            acc += (double)x[k] * s * w;
        }
        y[i] = (float)acc;
    }
    *out_n = m;
    return y;
}

/* The band solo: two bandpasses a third of an octave wide, for walking the
 * spectrum by ear when something in it should not be there. */
static int            g_solo;
static float          g_solo_hz = 1000.0f;
static ag_biq_chain_t g_solo_f;

static void solo_design(void)
{
    ag_biq_t *s;
    ag_biq_chain_init(&g_solo_f);
    if ((s = ag_biq_chain_push(&g_solo_f)) != 0) {
        (void)ag_biq_bandpass(s, (float)g_rate, g_solo_hz, 0.333f);
    }
    if ((s = ag_biq_chain_push(&g_solo_f)) != 0) {
        (void)ag_biq_bandpass(s, (float)g_rate, g_solo_hz, 0.333f);
    }
}

/*
 * One impulse into the live globals: int16 for the chip's path, float for the
 * PC's, and the int16 staging measured through it.
 *
 * `primary` marks the cabinet the program plays first.  It gets the cost
 * measurement printed - the one number here that says whether the convolution
 * keeps up with the sound card, and measuring it twice tells nobody anything -
 * and it is the only one allowed to fall back on an ag_ir preset, because a
 * preset is a reverb and not a cabinet: if the *fitted* impulse is missing, the
 * answer is to say so, not to put a hall into the comparison.
 */
static int cab_load(const char *path, uint32_t rate, int primary)
{
    uint32_t irn = 0, irrate = 0;
    float   *irf = NULL;
    int      ok = -1;

    g_ir = (ag_ir_t *)calloc(1, sizeof(ag_ir_t));
    g_fir = NULL;
    g_fhist = NULL;
    g_fir_n = 0;
    g_match = 1.0f;
    g_energy = 0.0;
    if (g_ir == NULL || ag_ir_init(g_ir, rate) != 0) {
        return -1;
    }
    /* Fully wet.  ag_ir_init leaves 22% of the dry signal through, which behind
     * a reverb is inaudible and behind a cabinet is a hiss louder than what the
     * speaker passes - the note in tube_render.c has the whole story. */
    ag_ir_set_wet(g_ir, AG_IR_WET_MAX);

    irf = path != NULL ? read_wav(path, &irn, &irrate) : NULL;
    if (irf != NULL) {
        /* 200 ms, for the reason measured in tube_render.c: past that an impulse
         * taken from a nonlinear model is the model's own noise floor, and
         * convolving with it spreads that across everything. */
        const uint32_t want = (uint32_t)(0.200 * (double)irrate);
        int16_t       *irs;
        double         pk = peak_of(irf, irn);
        uint32_t       k;
        if (pk < 1e-9) {
            pk = 1.0;
        }
        if (want > 32u && want < irn) {
            irn = want;
        }
        irs = (int16_t *)malloc(sizeof(int16_t) * irn);
        if (irs != NULL) {
            for (k = 0; k < irn; k++) {
                irs[k] = (int16_t)((double)irf[k] / pk * 32000.0);
            }
            if (ag_ir_load(g_ir, irs, irn, irrate) == 0) {
                ok = 0;
                printf("  %s: %s, %u taps at %u Hz -> %u at %u Hz,"
                       " %u partitions\n",
                       primary ? "cabinet" : "fitted ", path, irn, irrate,
                       g_ir->ir_frames, rate, g_ir->parts);
                /*
                 * And the one way this comparison can lie without anybody
                 * noticing: a fitted impulse carries the whole post-valve
                 * correction, so above its own Nyquist it carries *nothing*,
                 * while the biquad bank on the other side of the key keeps
                 * working all the way up.  ir_bogner_fitted.wav is 22.05 kHz
                 * because that is the rate the chip runs at; played against the
                 * bank at 44.1 it measures 28 dB down at 10 kHz, and what that
                 * sounds like is a duller impulse rather than a different
                 * architecture.  Measured: at 44.1 the two sides differ by
                 * 5.9 dB and the top two bands are -64 against -36; at 22.05 -
                 * the chip's rate, where the impulse is native - they differ by
                 * 9.6 dB and no band is an artefact of bandwidth.
                 */
                if (!primary && irrate < rate) {
                    printf("  fitted : NOTE this impulse is %u Hz and the take"
                           " is %u, so it has nothing above %u Hz while the bank"
                           " does - play a %u Hz take for a fair A/B\n",
                           irrate, rate, irrate / 2u, irrate);
                }
            }
            free(irs);
        }
        /* And the same impulse in float, for the path that has no arithmetic
         * to hide.  Level-matched to the int16 path further down, once that
         * path's gain has been measured. */
        {
            uint32_t fn = 0;
            g_fir = resample_ir(irf, irn, irrate, rate, &fn);
            g_fir_n = (int)fn;
        }
        free(irf);
    }
    if (ok != 0 && primary && ag_ir_load_preset(g_ir, 4) == 0) {
        ok = 0;
        printf("  cabinet: no impulse found, falling back on ag_ir preset 4 -"
               " which is not a cabinet, see tube_render.c\n");
    }
    if (ok != 0) {
        return -1;
    }
    /*
     * How far to back off before int16.
     *
     * Measured rather than assumed, exactly as in run_cab: a delta through the
     * loaded impulse says what the convolution does to a level, and this one
     * comes back at about 0.16 - a cabinet that is quieter than unity is limited
     * by its input and not its output, so dividing by the gain alone would drive
     * the input past full scale.
     *
     * Unlike run_cab this cannot look at the peak of the whole take, because the
     * take has not happened yet and the drive knob is about to move.  So it is a
     * constant, and filling the sixteen bits is the master knob's job - which is
     * what a master knob is for, and the meter says where it is.
     */
    {
        double g = 0.0;
        int    j;
        for (j = 0; j < BLK; j++) {
            g_mono[j] = (j == 0) ? 16384 : 0;
        }
        ag_ir_process_block(g_ir, g_mono, g_st);
        for (j = 0; j < BLK; j++) {
            const double v =
                g_st[2 * j] < 0 ? -(double)g_st[2 * j] : (double)g_st[2 * j];
            if (v > g) {
                g = v;
            }
        }
        g /= 16384.0;
        ag_ir_reset(g_ir);
        if (g < 1e-6) {
            g = 1.0;
        }
        g_head_lim = (float)(1.0 / (g > 1.0 ? g : 1.0));
        g_head = 0.9f * g_head_lim;
        g_head_auto = g_head;
        printf("  cabinet: impulse gain %.2f, staging floor %+.1f dB,"
               " adaptive to %+.1f\n",
               g, 20.0 * log10((double)g_head),
               20.0 * log10((double)g_head * 32.0));

        /*
         * Finish the float path: match its level to the int16 path's - a delta
         * through either has to come back at the same peak, or the A/B between
         * them is a volume test and not an arithmetic test - and store the
         * impulse reversed, so the hot loop is one forward dot product.
         */
        if (g_fir != NULL && g_fir_n > 0) {
            float  fpk = 0.0f, m;
            float *rev;
            int    j;
            for (j = 0; j < g_fir_n; j++) {
                const float v = g_fir[j] < 0.0f ? -g_fir[j] : g_fir[j];
                if (v > fpk) {
                    fpk = v;
                }
            }
            m = fpk > 1e-12f ? (float)g / fpk : 1.0f;
            rev = (float *)malloc(sizeof(float) * (size_t)g_fir_n);
            g_fhist = (float *)calloc((size_t)(g_fir_n - 1 + BLK),
                                      sizeof(float));
            if (rev != NULL && g_fhist != NULL) {
                for (j = 0; j < g_fir_n; j++) {
                    rev[j] = g_fir[g_fir_n - 1 - j] * m;
                }
                {
                    /* Its power gain, for the level match - see g_match. */
                    double e = 0.0;
                    for (j = 0; j < g_fir_n; j++) {
                        e += (double)rev[j] * (double)rev[j];
                    }
                    g_energy = sqrt(e);
                }
                free(g_fir);
                g_fir = rev;
                g_cab_mode = 1;
                /* What the direct convolution costs, measured here rather than
                 * assumed, because it is the one thing in this program that
                 * could fail to keep up with the sound card. */
                if (!primary) {
                    printf("  fitted:  %s, %d taps in float\n", path, g_fir_n);
                } else {
                    /*
                     * One second of audio at the rate actually in use.  It was
                     * hard-coded to 86 blocks, which is a second only at
                     * 22.05 kHz - at 44.1 the figure printed below was half the
                     * real cost, in the one place in this program that exists to
                     * say whether the convolution keeps up with the card.
                     */
                    const int     blocks =
                        (int)((rate + (uint32_t)BLK - 1u) / (uint32_t)BLK);
                    clock_t       t0;
                    double        ms, sink = 0.0;
                    int           b, kk, jj;
                    /*
                     * The history is filled with a *signal* before timing, and
                     * the result goes into a sum rather than back into the
                     * history.  Both of those are bug fixes.
                     *
                     * It used to start from a zeroed buffer and write each result
                     * back scaled by 1e-9, which after one block means every
                     * multiply is on a denormal - and denormals on x86 are an
                     * order of magnitude slower.  What it printed was the cost of
                     * gradual underflow: 508 ms per second of audio at 44.1 kHz,
                     * where the whole chain including this convolution actually
                     * measures 398.  A tool whose job is to say whether the
                     * program keeps up with the sound card cannot be pessimistic
                     * by a quarter.
                     */
                    for (jj = 0; jj < g_fir_n - 1 + BLK; jj++) {
                        g_fhist[jj] = (jj % 7 == 0) ? 0.25f : -0.125f;
                    }
                    t0 = clock();
                    for (b = 0; b < blocks; b++) {
                        for (kk = 0; kk < BLK; kk++) {
                            const float *hh = g_fhist + kk;
                            float        acc = 0.0f;
                            for (jj = 0; jj < g_fir_n; jj++) {
                                acc += hh[jj] * g_fir[jj];
                            }
                            sink += (double)acc;
                        }
                    }
                    ms = 1000.0 * (double)(clock() - t0) /
                         (double)CLOCKS_PER_SEC;
                    if (sink == 12345.6789) {
                        printf(" "); /* the compiler may not drop the loop */
                    }
                    memset(g_fhist, 0,
                           sizeof(float) * (size_t)(g_fir_n - 1 + BLK));
                    printf("  cabinet: float path %d taps, %.0f ms per second"
                           " of audio, the default; 'c' cycles float -> int16"
                           " -> off\n",
                           g_fir_n, ms);
                }
            } else {
                free(rev);
                free(g_fhist);
                free(g_fir);
                g_fir = NULL;
                g_fhist = NULL;
            }
        }
        if (g_cab_mode != 1) {
            g_cab_mode = 2;
            printf("  cabinet: no float path, using ag_ir int16\n");
        }
    }
    return 0;
}

/* The live cabinet into a slot, and back out of one.  Pointers, so each slot
 * keeps the history buffers it was playing through. */
static void cab_store(int i)
{
    cab_t *cb = &g_cab[i];
    cb->ir = g_ir;
    cb->fir = g_fir;
    cb->fir_n = g_fir_n;
    cb->fhist = g_fhist;
    cb->head = g_head;
    cb->head_lim = g_head_lim;
    cb->head_auto = g_head_auto;
    cb->match = g_match;
    cb->energy = g_energy;
    cb->loaded = 1;
}

static void cab_restore(int i)
{
    const cab_t *cb = &g_cab[i];
    g_ir = cb->ir;
    g_fir = cb->fir;
    g_fir_n = cb->fir_n;
    g_fhist = cb->fhist;
    g_head = cb->head;
    g_head_lim = cb->head_lim;
    g_head_auto = cb->head_auto;
    g_match = cb->match;
    g_energy = cb->energy;
    g_cabi = i;
}

/*
 * Both cabinets, at startup.
 *
 * Slot 0 is what the chain ships through: AG_CAB_IR if it is set, the impulse the
 * voicing was fitted through otherwise, the measured Vox after that, and an
 * ag_ir preset as a last resort - which is not a cabinet, and says so.
 *
 * Slot 1 is the other architecture: the impulse fitted to carry the post-valve
 * correction on its own.  `tube_render irfit` writes one per model.  Optional -
 * without it 'z' still takes the bank out, and then nothing stands in its place,
 * which is a third thing worth hearing but not the comparison that was asked
 * for, so the program says which it is doing.
 */
static int cab_open(uint32_t rate)
{
    const char *env = getenv("AG_CAB_IR");
    const char *fenv = getenv("AG_CAB_FIT");
    char        pbuf[1024], fbuf[1024], rel[160];
    const char *path = NULL, *fpath = NULL;

    g_mono = (int16_t *)malloc(sizeof(int16_t) * (size_t)BLK);
    g_st = (int16_t *)malloc(sizeof(int16_t) * (size_t)BLK * 2);
    if (g_mono == NULL || g_st == NULL) {
        return -1;
    }

    if (env != NULL) {
        path = resolve(pbuf, sizeof(pbuf), env);
    }
    /*
     * Then mode 1's impulse: the one fitted with the post bank still in.
     *
     * This is what makes `z` a comparison of the two things actually in question.
     * Both sides of that key are *matched* chains - the bank plus an impulse
     * fitted to the remainder, against an impulse carrying the whole correction -
     * and the difference between them is small, which is the point.  It used to be
     * the bank into a raw, unfitted cabinet against a fitted impulse, which
     * compares a matched chain with an unmatched one and reads as a large
     * difference for the wrong reason.
     *
     * Then the cabinet `match` used, for the case where mode 1's impulse has not
     * been written yet: the fitted impulse in slot 1 was built around *that*
     * speaker, and an A/B whose two sides carry different loudspeakers is a
     * comparison of loudspeakers.
     */
    if (path == NULL) {
        char rel0[200];
        snprintf(rel0, sizeof(rel0), "build/listen/ir_%s_bank.wav",
                 ag_amp_model_name(g_model));
        path = resolve(pbuf, sizeof(pbuf), rel0);
        if (path == NULL) {
            snprintf(rel0, sizeof(rel0), "build/listen/match_cab_%s.wav",
                     ag_amp_model_name(g_model));
            path = resolve(pbuf, sizeof(pbuf), rel0);
        }
    }
    if (path == NULL) {
        path = resolve(pbuf, sizeof(pbuf), AG_CAB_MATCHED);
    }
    if (path == NULL) {
        path = resolve(pbuf, sizeof(pbuf), AG_CAB_FALLBACK);
    }
    if (cab_load(path, rate, 1) != 0) {
        return -1;
    }
    cab_store(0);
    snprintf(g_cab[0].path, sizeof(g_cab[0].path), "%s",
             path != NULL ? path : "ag_ir preset 4, which is not a cabinet");

    snprintf(rel, sizeof(rel), "build/listen/ir_%s_fitted.wav",
             ag_amp_model_name(g_model));
    snprintf(g_fit_rel, sizeof(g_fit_rel),
             "Looked for %s and $AG_CAB_FIT; `argon match` writes one.", rel);
    if (fenv != NULL) {
        fpath = resolve(fbuf, sizeof(fbuf), fenv);
    }
    if (fpath == NULL) {
        fpath = resolve(fbuf, sizeof(fbuf), rel);
    }
    if (fpath != NULL && cab_load(fpath, rate, 0) == 0) {
        cab_store(1);
        snprintf(g_cab[1].path, sizeof(g_cab[1].path), "%s", fpath);
        /* Matched on energy, so that 'z' asks about spectrum and dynamics and
         * not about loudness. */
        if (g_cab[0].energy > 1e-12 && g_cab[1].energy > 1e-12) {
            g_cab[1].match = (float)(g_cab[0].energy / g_cab[1].energy);
        }
        printf("  fitted:  level matched %+.1f dB against the one above;"
               " 'z' swaps mode 1 <-> mode 2\n",
               20.0 * log10((double)g_cab[1].match));
    } else {
        printf("  fitted:  none - looked for %s and AG_CAB_FIT.  'z' will only"
               " take the post bank out;\n           `argon match <capture.nam>"
               " -Model %s` writes one\n",
               rel, ag_amp_model_name(g_model));
    }
    cab_restore(0);
    /* Slot 0 is what plays, so the arithmetic follows slot 0 - if the fitted
     * impulse happened to have no float path, that is its business and not the
     * shipping cabinet's. */
    g_cab_mode = g_fir != NULL ? 1 : 2;
    g_cab_want = g_cab_mode;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* audio out                                                                 */
/* ------------------------------------------------------------------------ */

static HWAVEOUT g_wo;
static WAVEHDR  g_hdr[NBUF_MAX];
static int16_t *g_pcm[NBUF_MAX];
static int      g_nbuf; /* NBUF_MS worth of BLK at the open rate */

/*
 * WHAT THE LIVE PATH IS DOING, AS OPPOSED TO WHAT THE RENDER COSTS
 *
 * The cost printed at startup is `render` against the wall clock, and a listener
 * reporting torn audio is not talking about that: they are talking about the
 * queue running dry, which depends on the sound card, on Windows' scheduler, and
 * on whatever else is running.  Measuring the first and answering a question
 * about the second is how "it keeps up here" gets said about a program that does
 * not, so the loop counts what actually happened.
 *
 * `g_under` is the number of times the poll found **every** buffer free, which
 * means the card had nothing left to play - an underrun, heard as a gap or a
 * click.  `g_minq` is how close it came the rest of the time.  `g_worst_ms` is
 * the longest a single block ever took to render, against the 11.6 ms a block
 * lasts.
 */
static uint32_t g_under;
static int      g_minq = NBUF_MAX + 1;
static double   g_worst_ms;
static double   g_cost_frac; /* what the startup measurement said */

static int wave_open(uint32_t rate)
{
    WAVEFORMATEX wf;
    int          i;

    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = rate * 4u;
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) !=
        MMSYSERR_NOERROR) {
        printf("  cannot open the sound card at %u Hz stereo\n", rate);
        return -1;
    }
    /*
     * Round up, so the queue is never shorter than the target, and keep the old
     * six as a floor for a rate low enough that the arithmetic asks for fewer.
     *
     * And the target follows the measured cost.  70 ms is a fine queue for a chain
     * that costs a tenth of realtime and a thin one for a chain that costs a
     * third: what has to fit in it is not the render but every hiccup of the
     * machine, and the render eating a third of the time makes each hiccup that
     * much harder to catch up from.  Past a quarter it doubles, which costs
     * latency nobody turning a knob will notice and buys the margin that was
     * missing when this was reported as torn audio.
     */
    {
        uint32_t ms = (uint32_t)NBUF_MS;
        if (g_cost_frac > 0.25) {
            ms *= 2u;
            printf("  latency: %u ms rather than %d, because the chain costs"
                   " %.0f%% of realtime\n", ms, NBUF_MS, 100.0 * g_cost_frac);
        }
        g_nbuf = (int)((ms * rate + (uint32_t)(1000 * BLK) - 1u) /
                       (uint32_t)(1000 * BLK));
    }
    if (g_nbuf < 6) {
        g_nbuf = 6;
    }
    if (g_nbuf > NBUF_MAX) {
        g_nbuf = NBUF_MAX;
    }
    for (i = 0; i < g_nbuf; i++) {
        g_pcm[i] = (int16_t *)calloc((size_t)BLK * 2, sizeof(int16_t));
        if (g_pcm[i] == NULL) {
            return -1;
        }
        memset(&g_hdr[i], 0, sizeof(g_hdr[i]));
        g_hdr[i].lpData = (LPSTR)g_pcm[i];
        g_hdr[i].dwBufferLength = (DWORD)(BLK * 2 * (int)sizeof(int16_t));
        if (waveOutPrepareHeader(g_wo, &g_hdr[i], sizeof(g_hdr[i])) !=
            MMSYSERR_NOERROR) {
            return -1;
        }
    }
    return 0;
}

static void wave_close(void)
{
    int i;
    (void)timeEndPeriod(1);
    waveOutReset(g_wo);
    for (i = 0; i < g_nbuf; i++) {
        waveOutUnprepareHeader(g_wo, &g_hdr[i], sizeof(g_hdr[i]));
        free(g_pcm[i]);
    }
    waveOutClose(g_wo);
}

/* ------------------------------------------------------------------------ */
/* one block                                                                 */
/* ------------------------------------------------------------------------ */

/*
 * Everything this program plays, written to a file instead - same code path,
 * same knob presses, no sound card.
 *
 *   tube_live --keys "qqqq..." --dump out.wav 8
 *
 * This exists because of a listening report that the offline renders could not
 * reproduce: a buzz heard live has to be either in these samples or in the
 * audio layer, and a dump splits that in two.  The keys are fed through the
 * same key() handler a finger would use, so the knob path - including every
 * set_knobs call and its state carry-over - is exercised exactly.
 */
static void wr32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void wr16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

static int dump_wav(const char *path, const int16_t *x, uint32_t n,
                    uint32_t rate)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return -1;
    }
    fwrite("RIFF", 4, 1, f);
    wr32(f, 36 + n * 2);
    fwrite("WAVEfmt ", 8, 1, f);
    wr32(f, 16);
    wr16(f, 1);
    wr16(f, 1);
    wr32(f, rate);
    wr32(f, rate * 2);
    wr16(f, 2);
    wr16(f, 16);
    fwrite("data", 4, 1, f);
    wr32(f, n * 2);
    fwrite(x, 2, n, f);
    fclose(f);
    return 0;
}

static float g_peak_amp, g_peak_out;

/*
 * Recording what is playing, straight from the samples.
 *
 * Asked for as "a tool that records the Windows audio", and this is better than
 * that: it is the samples themselves, before the card, so there is no loopback
 * driver, no resampling, no unknown mixer gain, and no question about whether
 * something in Windows added or removed anything.  What lands in the file is
 * exactly what the chain produced with the knobs where they were.
 *
 * Taken after the band solo and before the output volume, so that soloing a band
 * captures that band and the listening level cannot change the file.
 */
static float   *g_rec;
static uint32_t g_rec_cap, g_rec_n;
static int      g_rec_on, g_rec_seq;
/* The peak meter, off by default: it is the only thing on the status line that
 * moves without anybody touching a key, and a line that redraws by itself is
 * what pushed the key map off the screen. */
static int   g_meter;

static void render(int16_t *pcm)
{
    float y[AG_IR_BLOCK];
    int   k;

    if (g_ref_on && g_ref != NULL) {
        for (k = 0; k < BLK; k++) {
            y[k] = g_ref[g_ref_pos] * g_ref_gain;
            if (++g_ref_pos >= g_ref_frames) {
                g_ref_pos = 0;
            }
        }
    } else {
        for (k = 0; k < BLK; k++) {
            const float x = g_in[g_pos];
            if (++g_pos >= g_frames) {
                g_pos = 0;
            }
            y[k] = g_bypass ? x : ag_amp_tick(amp(), x);
        }
    }
    for (k = 0; k < BLK; k++) {
        const float v = y[k] < 0.0f ? -y[k] : y[k];
        if (v > g_peak_amp) {
            g_peak_amp = v;
        }
    }

    if (g_ref_on) {
        /* the reference already went through a real cabinet */
    } else if (g_cab_mode == 1 && g_fir != NULL) {
        /*
         * The float cabinet: slide the block into the history and take one dot
         * product per sample.  Contiguous memory on both sides, so the compiler
         * vectorises it.
         */
        const int hist_n = g_fir_n - 1;
        memmove(g_fhist, g_fhist + BLK, sizeof(float) * (size_t)hist_n);
        memcpy(g_fhist + hist_n, y, sizeof(float) * (size_t)BLK);
        for (k = 0; k < BLK; k++) {
            const float *h = g_fhist + k;
            float        acc = 0.0f;
            int          j;
            for (j = 0; j < g_fir_n; j++) {
                acc += h[j] * g_fir[j];
            }
            y[k] = acc;
        }
    } else if (g_cab_mode == 2 && g_ir != NULL) {
        /*
         * The staging into the convolution follows the signal, and this is a
         * bug fix and not a refinement.  ag_ir is int16, and with a fixed
         * scaling sized for a full-scale signal, a chain at drive 0.1 walks in
         * 25 dB down - eleven bits - and the convolution's block quantisation
         * comes back up with the volume knob as a buzz at roughly the block
         * rate, nearly independent of the note.  Measured with ir_check on the
         * same take: error to signal -66.5 dB going in at full scale, -49 dB
         * going in at -25, and -13 dB in the note decays, which is where it was
         * heard.
         *
         * So each block is scaled up to fill the sixteen bits and scaled back
         * down on the way out - exact but for the tail of previous blocks,
         * which entered at the previous scale, so the scale is only allowed to
         * *rise* slowly (0.35 dB a block, thirty a second).  Falling is
         * immediate, because the alternative to falling is clipping.
         */
        float blk_pk = 1e-6f;
        float h;
        for (k = 0; k < BLK; k++) {
            const float v = y[k] < 0.0f ? -y[k] : y[k];
            if (v > blk_pk) {
                blk_pk = v;
            }
        }
        h = g_head_auto * 1.04f;
        /* At most 30 dB above the floor.  The bound is on the *ratio* between
         * the scale a tail entered at and the scale it leaves at: a sudden
         * attack drops the scale instantly, and what that costs is the previous
         * quiet tail amplified by the whole allowed range for one impulse
         * length - 30 dB on a signal at least 30 dB down, under a full-scale
         * attack, which stays masked. */
        if (h > g_head * 32.0f) {
            h = g_head * 32.0f;
        }
        if (h * blk_pk > 0.9f * g_head_lim) {
            h = 0.9f * g_head_lim / blk_pk;
        }
        if (h < g_head) {
            h = g_head; /* never quieter than the fixed staging run_cab uses */
        }
        g_head_auto = h;
        for (k = 0; k < BLK; k++) {
            float v = y[k] * h * 32767.0f;
            if (v > 32767.0f) {
                v = 32767.0f;
                g_clip++;
            } else if (v < -32768.0f) {
                v = -32768.0f;
                g_clip++;
            }
            g_mono[k] = (int16_t)v;
        }
        ag_ir_process_block(g_ir, g_mono, g_st);
        for (k = 0; k < BLK; k++) {
            y[k] = (float)g_st[2 * k] / (32768.0f * h);
        }
    }

    /*
     * The cabinet's level match, which is what makes the architecture switch a
     * question about tone: the fitted impulse passes a different amount of power
     * than the one the bank was fitted through, and an A/B that also changes the
     * loudness is answered by whichever side is louder.  See g_match.
     */
    if (!g_ref_on && g_cab_mode != 0 && g_match != 1.0f) {
        for (k = 0; k < BLK; k++) {
            y[k] *= g_match;
        }
    }

    /*
     * The recording is taken here, **before** the band solo, and that ordering
     * was learned the hard way.
     *
     * It used to be after, and the first recording made with it was of a solo at
     * about 3.9 kHz.  Analysed, it showed a burst of a tone at a fixed 3.9 kHz on
     * every note - which is exactly what a third-octave band pass produces, and
     * which I read as a filter resonance and spent a whole round chasing.  A
     * diagnostic that quietly writes its own centre frequency into the evidence
     * is worse than no diagnostic.  So the file is always the full output, and
     * whichever band is interesting can be filtered afterwards, by whoever is
     * looking, on purpose.
     */
    if (g_rec_on && g_rec != NULL) {
        for (k = 0; k < BLK && g_rec_n < g_rec_cap; k++) {
            g_rec[g_rec_n++] = y[k];
        }
        if (g_rec_n >= g_rec_cap) {
            g_rec_on = 2; /* full; the main loop writes it out */
        }
    }

    /*
     * The band solo, for hunting a noise by ear: two bandpasses in cascade at
     * g_solo_hz, after everything, so that whatever the chain makes can be
     * listened to one third of an octave at a time.
     */
    if (g_solo) {
        for (k = 0; k < BLK; k++) {
            y[k] = ag_biq_chain_tick(&g_solo_f, y[k]);
        }
    }

    for (k = 0; k < BLK; k++) {
        float v = y[k] * g_vol;
        const float a = v < 0.0f ? -v : v;
        if (a > g_peak_out) {
            g_peak_out = a;
        }
        v *= 32767.0f;
        if (v > 32767.0f) {
            v = 32767.0f;
        } else if (v < -32768.0f) {
            v = -32768.0f;
        }
        pcm[2 * k] = (int16_t)v;
        pcm[2 * k + 1] = pcm[2 * k];
    }
}

/* ------------------------------------------------------------------------ */
/* knobs                                                                     */
/* ------------------------------------------------------------------------ */

/*
 * Switch to one of the cabinet paths, cleanly.
 *
 * Each one keeps its own history, and a history left over from the other path is
 * a burst of whatever was in it - so entering always clears, which costs the tail
 * of the note being compared and is the honest price of an instant A/B.  Falls
 * back to the path that exists if the requested one does not.
 */
static void cab_enter(int mode)
{
    if (mode == 1 && g_fir == NULL) {
        mode = 2;
    }
    if (mode == 2 && g_ir == NULL) {
        mode = g_fir != NULL ? 1 : 0;
    }
    g_cab_mode = mode;
    if (mode != 0) {
        g_cab_want = mode;
    }
    if (mode == 1 && g_fhist != NULL) {
        memset(g_fhist, 0, sizeof(float) * (size_t)(g_fir_n - 1 + BLK));
    } else if (mode == 2 && g_ir != NULL) {
        ag_ir_reset(g_ir);
        g_head_auto = g_head;
    }
}

/*
 * Which cabinet is in the chain.
 *
 * The one being left keeps its state - so switching back and forth does not cost
 * a rebuild - and the one being entered is cleared, for the same reason the
 * arithmetic switch clears: a history from a different impulse is a burst of
 * whatever was in it.  Returns 0 if that slot was never loaded.
 */
static int cab_select(int i)
{
    if (i < 0 || i > 1 || !g_cab[i].loaded || i == g_cabi) {
        return i == g_cabi;
    }
    cab_store(g_cabi);
    cab_restore(i);
    if (g_cab_mode != 0) {
        cab_enter(g_cab_mode);
    }
    return 1;
}

static void reblend(void)
{
    if (g_bstage < amp()->n && g_work[g_n - 1][g_bstage] != NULL) {
        (void)ag_amp_blend_stage(amp(), NULL, g_bstage,
                                 g_mix[g_n - 1][g_bstage],
                                 g_work[g_n - 1][g_bstage]);
    }
}

/* Every knob that only moves coefficients.  Never resets, so the note keeps
 * ringing through it. */
static void knobs(void)
{
    (void)ag_amp_set_knobs(amp(), &g_cfg);
}

/* Scale both fitted banks by g_bank, from the originals rather than from what
 * they are now - otherwise the knob would only ever go down. */
static void bank_apply(void)
{
    int b;
    for (b = 0; b < AG_AMP_VOICE_N; b++) {
        int st;
        for (st = 0; st < AG_AMP_STAGES; st++) {
            g_cfg.voice[st][b].db = g_voice0[st][b].db * g_bank;
        }
        g_cfg.tone[b].db = g_tone0[b].db * g_bank;
    }
    g_cfg.mid_db = g_mid_db0 * g_bank;
    knobs();
}

/* And the two that cannot avoid a discontinuity. */
static void rebuild_state(void)
{
    (void)ag_amp_set_voicing(amp(), &g_cfg);
}

static void help(void)
{
    printf("\n"
           "   q / a   drive          w / s   blend of the chosen stage\n"
           "   e / d   mid, dB        r / f   top cut, Hz\n"
           "   t / g   g12            n / m   master\n"
           "   [ / ]   volume, after the cabinet - changes nothing in the model\n"
           "   1 - 4   stages         x       which stage the blend knob turns\n"
           "   5 / 6   bass           7 / 8   mid        9 / \\   treble\n"
           "           - the passive tone stack, if this model has one; 0.5 is noon\n"
           "   b       blocking       c       cabinet in / out\n"
           "   i       cabinet arithmetic: float (PC) or int16 (the chip)\n"
           "   o       oversampling (clears the filters)\n"
           "   v       antialiasing   space   bypass, for A/B\n"
           "   k       band solo on/off, a third of an octave; j / l move it\n"
           "   , / .   the fitted voicing banks, 0 to 1.5\n"
           "   ;       tone stack top: resonant peaks or shelves\n"
           "   z       post-valve matching, two fitted modes: the bank plus an\n"
           "           impulse for the remainder, or one impulse carrying all\n"
           "   0       record what is playing; again to write build/listen/live_recN.wav\n"
           "   u       peak meter, off by default so nothing redraws by itself\n"
           "   y       the NAM reference of the same take, for A/B against the model\n"
           "   p       print the settings     h  this      ESC  quit\n"
           "\n");
}

static void print_settings(void)
{
    int i;
    printf("\n  drive %.2f  g12 %.2f  master %.5f  mid %+.1f dB at %.0f Hz  top"
           " %.0f Hz\n",
           (double)g_cfg.drive, (double)g_cfg.g12, (double)g_cfg.master,
           (double)g_cfg.mid_db, (double)g_cfg.mid_hz, (double)g_cfg.top_hz);
    if (g_cfg.tone_stack) {
        printf("  tone stack: bass %.2f  mid %.2f  treble %.2f  (0.5 is noon)\n",
               (double)g_cfg.tone_bass, (double)g_cfg.tone_mid,
               (double)g_cfg.tone_treble);
    } else {
        printf("  tone stack: none - this model has no schematic for one\n");
    }
    printf("  post-valve matching: %s\n  impulse: %s%s\n",
           g_post ? "mode 1, the bank plus an impulse for the remainder"
                  : "mode 2, one impulse carrying all of it",
           g_cab[g_cabi].path,
           g_match != 1.0f ? " (level matched)" : "");
    /* The drive distribution, which the `bank` knob does not scale and which is
     * the whole reason a stage-by-stage fit exists - see g_voice0. */
    printf("  drive per stage:");
    for (i = 0; i < g_n; i++) {
        printf(" %+.1f", (double)g_cfg.vtrim[i]);
    }
    printf(" dB of matching trim\n");
    printf("  stages %d  os %dx  adaa %d  blocking %d  cab %d  blend", g_n,
           g_cfg.os, g_cfg.adaa, g_cfg.blocking, g_cab_mode);
    for (i = 0; i < g_n; i++) {
        printf(" %.2f", (double)g_mix[g_n - 1][i]);
    }
    printf("\n\n");
}

/*
 * Stop recording and write it out, with the settings printed beside it - because
 * a captured artefact is only useful with the knob positions that produced it.
 */
static void rec_stop(void)
{
    /* Room for the root prefix, which find_root may have filled with an
     * absolute path from GetModuleFileName. */
    char     path[640];
    int16_t *pcm;
    uint32_t i, clipped = 0;

    g_rec_on = 0;
    if (g_rec == NULL || g_rec_n == 0) {
        return;
    }
    pcm = (int16_t *)malloc(sizeof(int16_t) * g_rec_n);
    if (pcm == NULL) {
        return;
    }
    for (i = 0; i < g_rec_n; i++) {
        float v = g_rec[i] * 32767.0f;
        if (v > 32767.0f) {
            v = 32767.0f;
            clipped++;
        } else if (v < -32768.0f) {
            v = -32768.0f;
            clipped++;
        }
        pcm[i] = (int16_t)v;
    }
    snprintf(path, sizeof(path), "%sbuild/listen/live_rec%d.wav", g_root,
             ++g_rec_seq);
    if (dump_wav(path, pcm, g_rec_n, g_rate) == 0) {
        printf("\n  recorded %.2f s to %s%s\n", (double)g_rec_n / (double)g_rate,
               path, clipped ? " (clipped - turn master down)" : "");
        print_settings();
    } else {
        printf("\n  cannot write %s\n", path);
    }
    free(pcm);
    g_rec_n = 0;
}

/* Returns 0 to keep going. */
static int key(int c)
{
    switch (c) {
    case 27:
        return 1;
    case 'q':
        g_cfg.drive -= 0.02f;
        if (g_cfg.drive < 0.0f) {
            g_cfg.drive = 0.0f;
        }
        knobs();
        break;
    case 'a':
        g_cfg.drive += 0.02f;
        knobs();
        break;
    case 't':
        g_cfg.g12 -= 0.02f;
        if (g_cfg.g12 < 0.0f) {
            g_cfg.g12 = 0.0f;
        }
        knobs();
        break;
    case 'g':
        g_cfg.g12 += 0.02f;
        knobs();
        break;
    case 'e':
        g_cfg.mid_db -= 0.5f;
        knobs();
        break;
    case 'd':
        g_cfg.mid_db += 0.5f;
        knobs();
        break;
    /*
     * The tone stack's three pots, which are the first controls in this tool
     * that are a *circuit* rather than a setting: they change resistances in a
     * passive network, that network is solved again, and its capacitor voltages
     * carry across the change the way they do when a hand moves a wiper.
     *
     * Only a model with a schematic for one has them - `ag_amp_tone_spec` says
     * which - and turning them on a model without one does nothing, which is the
     * honest behaviour rather than a silent equaliser standing in.
     */
    case '5':
        g_cfg.tone_bass -= 0.05f;
        knobs();
        break;
    case '6':
        g_cfg.tone_bass += 0.05f;
        knobs();
        break;
    case '7':
        g_cfg.tone_mid -= 0.05f;
        knobs();
        break;
    case '8':
        g_cfg.tone_mid += 0.05f;
        knobs();
        break;
    case '9':
        g_cfg.tone_treble -= 0.05f;
        knobs();
        break;
    case '\\':
        g_cfg.tone_treble += 0.05f;
        knobs();
        break;
    case 'r':
        g_cfg.top_hz /= 1.05f;
        if (g_cfg.top_hz < 1000.0f) {
            g_cfg.top_hz = 1000.0f;
        }
        knobs();
        break;
    case 'f':
        g_cfg.top_hz *= 1.05f;
        if (g_cfg.top_hz > 0.45f * (float)g_rate) {
            g_cfg.top_hz = 0.45f * (float)g_rate;
        }
        knobs();
        break;
    case 'n':
        g_cfg.master /= 1.1f;
        knobs();
        break;
    case 'm':
        g_cfg.master *= 1.1f;
        knobs();
        break;
    case '[':
        g_vol /= 1.1f;
        break;
    case ']':
        g_vol *= 1.1f;
        break;
    case 'w':
        g_mix[g_n - 1][g_bstage] -= 0.02f;
        if (g_mix[g_n - 1][g_bstage] < 0.0f) {
            g_mix[g_n - 1][g_bstage] = 0.0f;
        }
        reblend();
        break;
    case 's':
        g_mix[g_n - 1][g_bstage] += 0.02f;
        if (g_mix[g_n - 1][g_bstage] > 1.0f) {
            g_mix[g_n - 1][g_bstage] = 1.0f;
        }
        reblend();
        break;
    /*
     * The whole post-valve architecture, on one key.
     *
     * One press moves two things together, because they are two halves of one
     * design decision: the bank of biquads after the valves comes out, and the
     * cabinet becomes the impulse that was fitted to do that job by itself.  Both
     * impulses are already loaded and level matched, so the note keeps playing
     * and the only thing that changed is where the correction lives.
     *
     * What stays put in both positions, deliberately: the pre-valve bank and the
     * mid lift, which decide *what gets distorted*, and the passive tone stack,
     * which is a circuit out of the schematic rather than a correction.  Only the
     * post bank is a candidate for being replaced by an impulse - a filter in
     * front of a clipper cannot be, because moving it behind changes what the
     * valves see.
     */
    case 'z': {
        int b;
        g_post = !g_post;
        for (b = 0; b < AG_AMP_VOICE_N; b++) {
            g_cfg.tone[b].db = g_post ? g_tone0[b].db * g_bank : 0.0f;
        }
        knobs();
        (void)cab_select(g_post ? 0 : 1);
        /*
         * Said as it is, in both cases.  This used to announce "the fitted
         * impulse, carrying it alone" and then add that no fitted impulse had
         * been found - two lines that contradict each other, and the status line
         * agreed with the wrong one.  With no impulse loaded this key is a third
         * thing: the post correction simply gone, which is worth hearing and is
         * not the comparison the key exists for.
         */
        if (g_cab[1].loaded) {
            printf("\n  post-valve matching: %s\n    %s\n",
                   g_post ? "mode 1 - the biquad bank, and an impulse fitted to"
                            " the remainder"
                          : "mode 2 - one fitted impulse, carrying all of it",
                   g_cab[g_cabi].path);
        } else {
            printf("\n  post-valve matching: %s\n    no fitted impulse is"
                   " loaded, so this key only takes the bank out - the cabinet"
                   " is\n    unchanged.  %s\n",
                   g_post ? "the biquad bank, as the code ships"
                          : "NOTHING - the bank is out and nothing replaces it",
                   g_fit_rel);
        }
        break;
    }
    case 'x':
        g_bstage = (g_bstage + 1) % g_n;
        break;
    case '1':
    case '2':
    case '3':
    case '4':
        g_n = c - '0';
        if (g_bstage >= g_n) {
            g_bstage = g_n - 1;
        }
        /* A different chain, so its state is stale whatever we do: reset it and
         * push the current knobs and the current blend onto it. */
        rebuild_state();
        reblend();
        break;
    case 'b':
        g_cfg.blocking = !g_cfg.blocking;
        knobs();
        break;
    case 'v':
        g_cfg.adaa = !g_cfg.adaa;
        knobs();
        break;
    case 'c':
        /* The cabinet in or out, remembering which arithmetic was in use. */
        if (g_cab_mode != 0) {
            g_cab_want = g_cab_mode;
            g_cab_mode = 0;
        } else {
            cab_enter(g_cab_want);
        }
        break;
    case 'i':
        /*
         * The A/B, on its own key rather than as a stop on a three-way cycle:
         * comparing two things means going back and forth between them, and a
         * cycle that passes through silence makes that three presses and a
         * memory test.
         */
        cab_enter(g_cab_mode == 1 ? 2 : 1);
        break;
    case 'o':
        g_cfg.os = g_cfg.os >= 8 ? 1 : g_cfg.os * 2;
        rebuild_state();
        reblend();
        break;
    case 'u':
        g_meter = !g_meter;
        break;
    case ';':
        /*
         * The top of the tone stack as shelves instead of peaking sections.
         *
         * This is the A/B for the rattle.  The fitted +9.95 dB at 3150 and
         * +14.94 at 5000, both Q 1, put their poles at radius 0.80 and ring for
         * 1.4 ms at their centres; behind a clipper every clipping edge pings
         * them twice a cycle, which is a fixed pitch under a moving note.  The
         * shelves have one real pole each, radius 0.25, which decays in five
         * samples and has no pitch at all.  Same broad boost, no resonance.
         */
        g_cfg.tone_shelf = !g_cfg.tone_shelf;
        knobs();
        break;
    case '0':
        if (g_rec_on) {
            rec_stop();
        } else {
            if (g_rec == NULL) {
                g_rec_cap = g_rate * 120u; /* two minutes is 10 MB */
                g_rec = (float *)malloc(sizeof(float) * g_rec_cap);
                if (g_rec == NULL) {
                    g_rec_cap = 0;
                    break;
                }
            }
            g_rec_n = 0;
            g_rec_on = 1;
            if (g_solo) {
                printf("\n  note: the file is the full output, not the solo"
                       " band - the solo is a listening aid only");
            }
            printf("\n  recording - \'0\' again to stop, up to 120 s\n");
        }
        break;
    case ',':
        g_bank -= 0.05f;
        if (g_bank < 0.0f) {
            g_bank = 0.0f;
        }
        bank_apply();
        break;
    case '.':
        g_bank += 0.05f;
        if (g_bank > 1.5f) {
            g_bank = 1.5f;
        }
        bank_apply();
        break;
    case 'y':
        if (g_ref != NULL) {
            g_ref_on = !g_ref_on;
        }
        break;
    case 'k':
        g_solo = !g_solo;
        if (g_solo) {
            solo_design();
        }
        break;
    case 'j':
        g_solo_hz /= 1.1225f; /* a sixth of an octave a press */
        if (g_solo_hz < 60.0f) {
            g_solo_hz = 60.0f;
        }
        solo_design();
        break;
    case 'l':
        g_solo_hz *= 1.1225f;
        if (g_solo_hz > 0.45f * (float)g_rate) {
            g_solo_hz = 0.45f * (float)g_rate;
        }
        solo_design();
        break;
    case ' ':
        g_bypass = !g_bypass;
        break;
    case 'p':
        print_settings();
        break;
    case 'h':
        help();
        break;
    default:
        break;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */

/*
 * One line, rewritten in place, and only when something changed.
 *
 * Both halves of that matter and the first one was got wrong: a line longer than
 * the console is wide wraps, and then the carriage return only reaches the start
 * of its *last* row - so every update leaves the previous row behind and the
 * screen scrolls forever, taking the key map with it.  Hence the width budget
 * below, kept well inside eighty columns.
 *
 * And printing on a timer rather than on a change is noise: nothing on this line
 * moves on its own except the meter, which is why the meter is off unless it is
 * asked for.
 */
static void status(void)
{
    char line[128];
    int  k;

    k = snprintf(line, sizeof(line),
                 " dr %.2f g12 %.2f bl%d %.2f mid %+.1f top %5.0f %dx%s%s n%d %s",
                 (double)g_cfg.drive, (double)g_cfg.g12, g_bstage + 1,
                 (double)g_mix[g_n - 1][g_bstage], (double)g_cfg.mid_db,
                 (double)g_cfg.top_hz, g_cfg.os, g_cfg.adaa ? "+aa" : "",
                 g_cfg.blocking ? " blk" : "", g_n,
                 g_cab_mode == 1   ? "cabF"
                 : g_cab_mode == 2 ? "cab16"
                                   : "dry");
    /*
     * Which architecture, because it is the one switch here that changes two
     * things at once and it must never be ambiguous which side is playing.
     *
     * `IRfit` only when there really is a fitted impulse in the chain.  With none
     * loaded the same keypress leaves the cabinet alone and just removes the post
     * correction, which is a different thing and now says so: `noPost`.
     */
    if (k > 0 && k < (int)sizeof(line) && !g_post) {
        k += snprintf(line + k, sizeof(line) - (size_t)k,
                      g_cab[1].loaded ? " IRonly" : " noPost");
    }
    if (k > 0 && k < (int)sizeof(line) && g_cfg.tone_shelf) {
        k += snprintf(line + k, sizeof(line) - (size_t)k, " shelf");
    }
    if (k > 0 && k < (int)sizeof(line) && g_rec_on) {
        k += snprintf(line + k, sizeof(line) - (size_t)k, " REC%.0fs",
                      (double)g_rec_n / (double)g_rate);
    }
    if (k > 0 && k < (int)sizeof(line) && g_bank != 1.0f) {
        k += snprintf(line + k, sizeof(line) - (size_t)k, " bank%.2f", (double)g_bank);
    }
    if (k > 0 && k < (int)sizeof(line) && g_solo) {
        k += snprintf(line + k, sizeof(line) - (size_t)k, " s%.0f",
                      (double)g_solo_hz);
    }
    /* Only when it has happened: a counter that reads zero forever is noise, and
     * one that does not is the answer to "is it keeping up". */
    if (k > 0 && k < (int)sizeof(line) && g_under != 0) {
        k += snprintf(line + k, sizeof(line) - (size_t)k, " UNDER%u", g_under);
    }
    if (k > 0 && k < (int)sizeof(line) && g_bypass) {
        k += snprintf(line + k, sizeof(line) - (size_t)k, " BYP");
    }
    if (k > 0 && k < (int)sizeof(line) && g_ref_on) {
        k += snprintf(line + k, sizeof(line) - (size_t)k, " REF");
    }
    if (k > 0 && k < (int)sizeof(line) && g_meter) {
        k += snprintf(line + k, sizeof(line) - (size_t)k, " %+.0f/%+.0f",
                      20.0 * log10((double)g_peak_amp + 1e-9),
                      20.0 * log10((double)g_peak_out + 1e-9));
    }
    /* Padded to a fixed width so that a shorter line cannot leave the tail of a
     * longer one behind it, and no wider than a default console. */
    printf("\r%-78.78s", line);
    fflush(stdout);
    g_peak_amp = 0.0f;
    g_peak_out = 0.0f;
}

/* Mean square of what the chain produces over `secs` seconds of the take, taken
 * from the same `render` the audio loop calls, so nothing can differ between what
 * is measured and what is heard. */
static double render_ms2(double secs)
{
    const int blocks = (int)(secs * (double)g_rate / (double)BLK);
    int16_t  *scratch = (int16_t *)malloc(sizeof(int16_t) * (size_t)BLK * 2u);
    double    sq = 0.0;
    int       b, j;

    if (scratch == NULL || blocks < 1) {
        free(scratch);
        return 0.0;
    }
    for (b = 0; b < blocks; b++) {
        render(scratch);
        for (j = 0; j < BLK; j++) {
            const double v = (double)scratch[2 * j] / 32768.0;
            sq += v * v;
        }
    }
    free(scratch);
    return sq / (double)(blocks * BLK);
}

/* The `z` state, without the printing: the post bank in or out, and the cabinet
 * that goes with it. */
static void post_state(int on)
{
    int b;
    g_post = on;
    for (b = 0; b < AG_AMP_VOICE_N; b++) {
        g_cfg.tone[b].db = on ? g_tone0[b].db * g_bank : 0.0f;
    }
    knobs();
    (void)cab_select(on ? 0 : 1);
}

/*
 * BOTH SIDES OF `z` TO THE SAME LOUDNESS, MEASURED ON THE AUDIO
 *
 * This was matched on the impulses' own energy, which is the same thing only if
 * the signal is white - and a guitar is the opposite of white, with most of its
 * energy in two octaves around 200 Hz.  What that error sounded like, reported by
 * a listener before it was measured: *with the baked-in biquads it is quieter and
 * duller*.  It was quieter: 2.7 dB on jcm800, 5.7 dB on bogner, and 2.3 dB
 * *louder* on slo, so it was not even a consistent offset that a volume knob would
 * have taken out.
 *
 * So the two architectures are rendered - one second of the take each, through the
 * same `render` the audio loop uses - and slot 1 is scaled by whatever it takes to
 * make the two mean squares equal.  A level difference is the one thing an A/B
 * must not have, because loudness wins every comparison it is allowed into.
 */
static void match_states(void)
{
    const uint32_t pos = g_pos;
    double         a, b;
    float          g;

    if (!g_cab[1].loaded) {
        return;
    }
    post_state(1);
    rebuild_state();
    reblend();
    g_pos = pos;
    a = render_ms2(1.0);

    post_state(0);
    rebuild_state();
    reblend();
    g_pos = pos;
    b = render_ms2(1.0);

    /* Back to the shipping state first: leaving slot 1 stores whatever `g_match`
     * currently holds into it, which would undo the correction below. */
    post_state(1);
    rebuild_state();
    reblend();
    g_pos = pos;

    if (a > 1e-20 && b > 1e-20) {
        g = (float)sqrt(a / b);
        g_cab[1].match *= g;
        printf("  fitted:  %+.1f dB further level match, measured on the audio"
               " rather than on the impulse\n", 20.0 * log10((double)g));
    }
}

/*
 * What the run actually did, printed on the way out - because "it sounded torn"
 * and "it was keeping up" are both testable and neither was being tested.
 */
static void live_report(void)
{
    const double blk_ms = 1000.0 * (double)BLK / (double)g_rate;

    printf("\n\n  live path: %u underruns, queue never below %d of %d blocks"
           " (%.0f ms),\n             worst single block %.1f ms against the"
           " %.1f ms it lasts, startup said %.0f%%\n",
           g_under, g_minq > NBUF_MAX ? 0 : g_minq, g_nbuf,
           (double)((g_minq > NBUF_MAX ? 0 : g_minq) * BLK) * 1000.0 /
               (double)g_rate,
           g_worst_ms, blk_ms, 100.0 * g_cost_frac);
    if (g_under != 0) {
        printf("             ^ THAT is the tearing: the card ran dry.  Cheaper,"
               " in order: a 22.05 kHz\n               take, 'o' for less"
               " oversampling, 'i' for the int16 cabinet, 'c' for none.\n");
    }
}

int main(int argc, char **argv)
{
    const char *want = AG_DI_DEFAULT;
    const char *dump_path = NULL, *keys = NULL;
    double      dump_sec = 8.0;
    /* `--live N` plays for N seconds through the real audio path and then prints
     * the report and leaves.  It exists because the thing a listener complains
     * about is the live path, and until this there was no way to exercise it
     * without a finger on ESC. */
    double      run_secs = 0.0;
    clock_t     t_start = 0;
    int         primed = 0; /* the queue has been full at least once */
    char        tbuf[1024];
    const char *path;
    int         i, s, ticks = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            dump_path = argv[++i];
            if (i + 1 < argc && atof(argv[i + 1]) > 0.0) {
                dump_sec = atof(argv[++i]);
            }
        } else if (strcmp(argv[i], "--keys") == 0 && i + 1 < argc) {
            keys = argv[++i];
        } else if (strcmp(argv[i], "--live") == 0 && i + 1 < argc) {
            run_secs = atof(argv[++i]);
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            /*
             * An argument rather than a key, and that is a cost decision rather
             * than a design one.  This tool bakes one chain per stage count at
             * startup so that 1 to 4 is a keypress and never a stall; a model is
             * another axis of that, so putting it on a key would mean either
             * three times the bakes up front - a bake is most of a second - or a
             * silence in the middle of playing.  Restart the tool.
             */
            g_model = ag_amp_model_by_name(argv[++i]);
            if (g_model < 0) {
                printf("  %s is not a model.  Known: jcm800 bogner slo\n",
                       argv[i]);
                return 1;
            }
        } else {
            want = argv[i];
        }
    }

    printf("\n  tube_live - the chain with its knobs attached\n\n");

    /*
     * THREE THINGS AN AUDIO PROGRAM ON WINDOWS HAS TO ASK FOR, AND THIS ONE DID
     * NOT
     *
     * **Flush denormals to zero.**  Every filter here is direct form, so when a
     * note decays the histories fill with numbers below 1e-38, and on x86 an
     * arithmetic operation on a denormal costs an order of magnitude more than one
     * on a normal number.  The effect is a program that gets *slower when there is
     * less to hear*: measured on one second of signal followed by seven of
     * silence, the silence cost 24% more per second at 22.05 kHz and 11% more at
     * 44.1.  Backwards, free to fix, and the kind of thing that turns a note's
     * tail into a click on a loaded machine.
     *
     * **One millisecond of timer resolution.**  The loop sleeps when the queue is
     * full, and by default a Windows Sleep(1) can last 15.6 ms - two thirds of the
     * whole 70 ms queue in one nap on a bad day.  winmm is already linked for the
     * sound card, and timeBeginPeriod is in it.
     *
     * **Priority above the desktop.**  A polling audio loop that shares a core
     * with a compiler loses, and what that sounds like is exactly what was
     * reported.  HIGHEST rather than TIME_CRITICAL, because starving the input
     * thread of the console this program reads its keys from would be a poor
     * trade.
     */
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
    (void)timeBeginPeriod(1);
    (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    find_root();
    path = resolve(tbuf, sizeof(tbuf), want);
    if (path == NULL && want == AG_DI_DEFAULT) {
        path = resolve(tbuf, sizeof(tbuf), AG_DI_FALLBACK);
        if (path != NULL) {
            printf("  no %s; falling back on %s, which is 44.1 kHz - see the"
                   " note at AG_DI_DEFAULT\n", AG_DI_DEFAULT, AG_DI_FALLBACK);
        }
    }
    if (path == NULL) {
        printf("  cannot find %s, from here or from the root of the tree\n",
               want);
        return 1;
    }
    g_in = read_wav(path, &g_frames, &g_rate);
    if (g_in == NULL || g_frames < (uint32_t)BLK) {
        printf("  no signal to play\n");
        return 1;
    }
    printf("  take: %s, %u frames at %u Hz, %.1f s, peak %.3f\n", path, g_frames,
           g_rate, (double)g_frames / (double)g_rate, peak_of(g_in, g_frames));

    g_ckt = (ag_ckt_t *)malloc(sizeof(ag_ckt_t));
    if (g_ckt == NULL) {
        return 1;
    }
    ag_amp_model(&g_cfg, g_model, (float)g_rate);
    printf("  model: %s, %s\n", ag_amp_model_name(g_model),
           ag_amp_model_fitted(g_model)
               ? "voicing fitted against a capture of the real amplifier"
               : "voicing is a starting point, not fitted");
    /*
     * The model's own stage count is where the stage-count key starts, so that
     * `--model slo` plays four valves rather than the two this tool has always
     * defaulted to and the third one is a keypress away rather than the default.
     */
    g_n = g_cfg.n_stages;
    {
        int b;
        for (b = 0; b < AG_AMP_VOICE_N; b++) {
            {
                int st;
                for (st = 0; st < AG_AMP_STAGES; st++) {
                    g_voice0[st][b] = g_cfg.voice[st][b];
                }
            }
            g_tone0[b] = g_cfg.tone[b];
        }
        g_mid_db0 = g_cfg.mid_db;
    }

    /*
     * Four chains, and the axes fitted to this take rather than to the synthetic
     * probe - the same reason tube_render's render mode does it: fitted to the
     * probe, the second stage's axis came out a third of the range the take
     * actually drives it to.
     */
    for (i = 0; i < AG_AMP_STAGES; i++) {
        ag_amp_cfg_t c = g_cfg;
        const uint32_t pn =
            g_frames < g_rate * 10u ? g_frames : g_rate * 10u;
        c.n_stages = i + 1;
        g_amp[i] = (ag_amp_t *)malloc(sizeof(ag_amp_t));
        g_tab[i] = (float *)malloc(sizeof(float) * AG_AMP_TAB_FLOATS);
        if (g_amp[i] == NULL || g_tab[i] == NULL) {
            printf("  out of memory\n");
            return 1;
        }
        printf("  baking %d stage%s ...", i + 1, i ? "s" : " ");
        fflush(stdout);
        if (ag_amp_build(g_amp[i], g_ckt, &c, g_tab[i], 0, g_in, (int)pn) != 0) {
            printf(" failed\n");
            return 1;
        }
        /*
         * And the blend's second curve for every stage, now rather than on the
         * first keypress.  It is the one part of the knob that needs the solver,
         * so doing it here is the difference between a knob and a stall.
         */
        for (s = 0; s < g_amp[i]->n; s++) {
            g_work[i][s] =
                (float *)malloc(sizeof(float) * AG_AMP_BLEND_FLOATS(g_amp[i]->tab_n));
            g_mix[i][s] = 0.0f;
            if (g_work[i][s] == NULL ||
                ag_amp_blend_stage(g_amp[i], g_ckt, s, 0.0f, g_work[i][s]) != 0) {
                printf(" no blend on stage %d", s + 1);
                free(g_work[i][s]);
                g_work[i][s] = NULL;
            }
        }
        printf(" ok\n");
    }
    g_bstage = g_n - 1;

    /*
     * The reference, if it has been rendered.  Optional: everything works without
     * it, there is just nothing to compare against.
     *
     * This model's own reference first, and the Marshall one only as a fallback.
     * It used to be the Marshall render unconditionally, which meant that on the
     * crunch and lead models the `y` key put a *different amplifier* in the chain
     * and called it the reference - the one comparison in this program that has to
     * be against the right thing.  `match` writes one per model, with the speaker
     * on it when the capture had none.
     */
    {
        char        rbuf[1024], rel[200];
        const char *rp = NULL;
        uint32_t    rrate = 0;

        snprintf(rel, sizeof(rel), "build/listen/match_ref_%s_cab.wav",
                 ag_amp_model_name(g_model));
        rp = resolve(rbuf, sizeof(rbuf), rel);
        if (rp == NULL) {
            snprintf(rel, sizeof(rel), "build/listen/match_ref_%s.wav",
                     ag_amp_model_name(g_model));
            rp = resolve(rbuf, sizeof(rbuf), rel);
        }
        if (rp == NULL && g_model == AG_AMP_MODEL_JCM800) {
            rp = resolve(rbuf, sizeof(rbuf), AG_REF_DEFAULT);
        }
        if (rp != NULL) {
            g_ref = read_wav(rp, &g_ref_frames, &rrate);
            if (g_ref != NULL && rrate != g_rate) {
                free(g_ref);
                g_ref = NULL;
            }
            if (g_ref != NULL) {
                printf("  reference: %s, %.1f s - 'y' swaps the real amplifier"
                       " in\n", rp, (double)g_ref_frames / (double)g_rate);
            } else {
                printf("  reference: %s is %u Hz and this take is %u, so 'y' has"
                       " nothing to play\n", rp, rrate, g_rate);
            }
        }
    }

    if (cab_open(g_rate) != 0) {
        printf("  no cabinet; playing dry\n");
        g_cab_mode = 0;
    }

    /*
     * WHETHER THIS KEEPS UP, MEASURED ON THE THING THAT HAS TO KEEP UP
     *
     * The cabinet prints its own cost above, and that is not the question: the
     * question is the whole block - oversampled valves, the tables, the filters,
     * the tone stack and the convolution - against the wall clock.  So one second
     * of the take goes through `render` exactly as the audio loop will call it,
     * before the card is opened, and what comes out is a percentage.
     *
     * It is printed every run because the answer changes with the sample rate by
     * a factor of four (four times the samples, twice the taps) and nothing on the
     * screen used to say so.  A report of torn audio has exactly one first
     * question - "was it keeping up?" - and this is the line that answers it.
     */
    {
        const int      blocks = (int)((g_rate + (uint32_t)BLK - 1u) /
                                      (uint32_t)BLK);
        int16_t       *scratch = (int16_t *)malloc(sizeof(int16_t) * BLK * 2);
        const uint32_t save_pos = g_pos;
        clock_t        t0;
        double         frac;
        int            b;

        if (scratch != NULL) {
            double chain_sq = 0.0;
            t0 = clock();
            for (b = 0; b < blocks; b++) {
                int j;
                render(scratch);
                for (j = 0; j < BLK; j++) {
                    const double v = (double)scratch[2 * j] / 32768.0;
                    chain_sq += v * v;
                }
            }
            frac = ((double)(clock() - t0) / (double)CLOCKS_PER_SEC);
            free(scratch);
            /*
             * The reference, brought to the same rms as what was just rendered.
             * Done here because this is the one place that has both: a second of
             * this chain's output and a reference over the same second of the take.
             */
            if (g_ref != NULL && g_ref_frames > 0) {
                const uint32_t n = g_ref_frames < g_rate ? g_ref_frames : g_rate;
                double         ref_sq = 0.0;
                uint32_t       j;
                for (j = 0; j < n; j++) {
                    ref_sq += (double)g_ref[j] * (double)g_ref[j];
                }
                if (ref_sq > 1e-20 && chain_sq > 1e-20) {
                    g_ref_gain = (float)sqrt((chain_sq / (double)(blocks * BLK)) /
                                             (ref_sq / (double)n));
                    printf("  reference: level matched %+.1f dB to the chain,"
                           " so 'y' compares tone\n",
                           20.0 * log10((double)g_ref_gain));
                }
            }
            g_pos = save_pos;
            rebuild_state();
            reblend();
            g_cost_frac = frac;
            printf("\n  cost: %.0f%% of realtime for the whole chain at %u Hz"
                   " (%d stages, %dx%s, %s)\n",
                   100.0 * frac, g_rate, g_n, g_cfg.os,
                   g_cfg.adaa ? " with ADAA" : "",
                   g_cab_mode == 1   ? "float cabinet"
                   : g_cab_mode == 2 ? "int16 cabinet"
                                     : "no cabinet");
            if (frac > 0.55) {
                printf("  ^ THIS WILL TEAR on anything else happening on the"
                       " machine.  Cheaper, in order:\n    a 22.05 kHz take"
                       " (four times cheaper and the rate the chip runs at),"
                       " 'o' for\n    less oversampling, 'i' for the int16"
                       " cabinet, 'c' for none.\n");
            }
        }
    }

    /* Both sides of the architecture switch to the same loudness, now that there
     * is something to render them with. */
    match_states();

    /* Simulated keypresses, through the same handler a finger uses. */
    if (keys != NULL) {
        const char *c;
        for (c = keys; *c != 0; c++) {
            (void)key((unsigned char)*c);
        }
        printf("  keys \"%s\": drive %.2f g12 %.2f top %.0f mid %+.1f cab %d\n",
               keys, (double)g_cfg.drive, (double)g_cfg.g12,
               (double)g_cfg.top_hz, (double)g_cfg.mid_db, g_cab_mode);
    }

    if (dump_path != NULL) {
        const uint32_t blocks =
            (uint32_t)(dump_sec * (double)g_rate) / (uint32_t)BLK;
        int16_t *pcm = (int16_t *)malloc(sizeof(int16_t) * (size_t)BLK * 2);
        int16_t *out =
            (int16_t *)malloc(sizeof(int16_t) * (size_t)blocks * BLK);
        uint32_t b, k2;
        if (pcm == NULL || out == NULL) {
            return 1;
        }
        for (b = 0; b < blocks; b++) {
            render(pcm);
            for (k2 = 0; k2 < (uint32_t)BLK; k2++) {
                out[b * BLK + k2] = pcm[2 * k2];
            }
        }
        if (dump_wav(dump_path, out, blocks * (uint32_t)BLK, g_rate) != 0) {
            printf("  cannot write %s\n", dump_path);
            return 1;
        }
        printf("  dumped %.1f s to %s\n",
               (double)(blocks * (uint32_t)BLK) / (double)g_rate, dump_path);
        return 0;
    }

    if (wave_open(g_rate) != 0) {
        return 1;
    }

    printf("\n  latency: %d blocks of %d, %.0f ms out of the card\n", g_nbuf,
           BLK, 1000.0 * (double)(g_nbuf * BLK) / (double)g_rate);
    if (run_secs > 0.0) {
        printf("  --live %.1f s, then the report\n", run_secs);
    }
    help();
    t_start = clock();

    for (;;) {
        int did = 0;
        int queued = 0;

        /* How much audio the card still has.  Counted before anything is
         * rendered, so it is the depth at the moment the loop woke up. */
        for (i = 0; i < g_nbuf; i++) {
            if ((g_hdr[i].dwFlags & WHDR_INQUEUE) != 0) {
                queued++;
            }
        }
        /*
         * Only once the queue has been full once.  The first pass through this
         * loop necessarily finds it empty - nothing has been written yet - and
         * counting that reported one underrun on every run, including the runs
         * that were audibly perfect.  A counter that always reads 1 is a counter
         * nobody will believe when it reads 2.
         */
        if (primed) {
            if (queued == 0) {
                g_under++;
            }
            if (queued < g_minq) {
                g_minq = queued;
            }
        } else if (queued >= g_nbuf) {
            primed = 1;
        }
        if (run_secs > 0.0 &&
            (double)(clock() - t_start) / (double)CLOCKS_PER_SEC > run_secs) {
            break;
        }

        /*
         * The queue is topped up **before** the keyboard and the console,
         * not after.  A console write on Windows can block for tens of
         * milliseconds - a whole buffer or two - and doing that while the
         * card is running dry is how a keypress becomes audible as a
         * crackle rather than as the knob it was.
         */
        for (i = 0; i < g_nbuf; i++) {
            if ((g_hdr[i].dwFlags & WHDR_INQUEUE) == 0) {
                const clock_t tb = clock();
                double        bms;
                render(g_pcm[i]);
                bms = 1000.0 * (double)(clock() - tb) / (double)CLOCKS_PER_SEC;
                if (bms > g_worst_ms) {
                    g_worst_ms = bms;
                }
                if (waveOutWrite(g_wo, &g_hdr[i], sizeof(g_hdr[i])) !=
                    MMSYSERR_NOERROR) {
                    printf("\n  the sound card stopped taking blocks\n");
                    wave_close();
                    return 1;
                }
                did = 1;
                /* Only the meter has anything to say between keypresses, and it
                 * is off unless it was asked for.  Four times a second is enough
                 * to catch a clip and slow enough to read. */
                if (g_meter && ++ticks >= 21) {
                    ticks = 0;
                    status();
                }
            }
        }

        while (_kbhit()) {
            const int c = _getch();
            if (c == 0 || c == 224) {
                (void)_getch(); /* an arrow or a function key; not a knob */
                continue;
            }
            if (key(c)) {
                live_report();
                wave_close();
                return 0;
            }
            status();
        }
        if (g_rec_on == 2) {
            printf("\n  120 s reached");
            rec_stop();
        }
        if (!did) {
            Sleep(1);
        }
    }
    live_report();
    wave_close();
    return 0;
}
