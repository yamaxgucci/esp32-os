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
#include "nam.h"

#include <conio.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <mmsystem.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <ks.h>
#include <ksmedia.h>
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
static int      g_ref_on;
/*
 * THE REAL AMPLIFIER, RUN LIVE
 *
 * The capture itself on the same take, block by block beside this chain, and
 * 'y' swaps which of the two the card hears.  It used to be a file the walk had
 * rendered; that is gone, because two kinds of reference meant a 'y' that
 * sometimes did nothing - at a rate where the file would not load the key
 * toggled a thing that was not there and the sound did not change.  One
 * reference, always the same one, and it either plays or the tool says why not.
 *
 * It needs the capture's own rate: a model trained at 48 kHz fed 22.05 kHz
 * material is a different model, and resampling inside the block would need a
 * streaming resampler this tree does not have, plus a third source of difference
 * between the two sides.  At the wrong rate the reference is refused out loud.
 *
 * The capture has no loudspeaker of its own - gear_type "amp" - so it goes
 * through this chain's cabinet, the one out of the preset.  Both sides therefore
 * hear the same speaker, which is what makes 'y' a question about the amplifier.
 */
static nam_model_t *g_nam;
/*
 * What the capture is multiplied by so that 'y' is a comparison and not a
 * volume test.
 *
 * Measured rather than assumed, and it had to be learned: before it was
 * measured the capture played ten decibels under the chain.  An A/B that also
 * changes the loudness is answered by whichever side is louder, and ten
 * decibels down reads as thin and far away - which is what a missing
 * loudspeaker sounds like, so it got blamed on the cabinet.
 */
static float        g_nam_gain = 1.0f;
/*
 * Whether the capture follows the drive knob.
 *
 * Off - and it starts off - it plays the take as recorded and never changes,
 * which is what aiming at it needs: the target has to hold still.  On, it gets
 * the same input this chain gets, scaled by how far drive has moved from where
 * the model was fitted, and then it answers a different question - not "does it
 * sound the same" but "does it *react* the same".  At the fitted drive the two
 * agree, which is also a check that the scaling is right.
 */
static int          g_ref_follow;
static float        g_nam_in[AG_IR_BLOCK];
static float        g_fit_drive = 1.0f;

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
/* g_fir_n - 1 + FHIST_SLACK + BLK; the live window starts at g_fpos. */
#define FHIST_SLACK 8192
/* Taps to keep of the float impulse, 0 for all of it; see cab_open. */
static int      g_irtaps;
static float   *g_fhist;
static uint32_t g_fpos;
static int      g_fslack;
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
/*
 * `irf` is the impulse, already in float at `irrate`, and this owns nothing: the
 * caller frees it.  `label` is what to call it in the report - a file name, or
 * the preset it came out of.
 *
 * Split out of cab_load so that a preset's cabinet, which arrives as int16 in
 * memory, goes down exactly the same path as one read from a wav.  Two loaders
 * would be two places for the staging arithmetic below to drift apart.
 */
static int cab_load_f(const float *irf_in, uint32_t irn_in, uint32_t irrate,
                      uint32_t rate, int primary, const char *label)
{
    uint32_t irn = irn_in;
    float   *irf = NULL;
    int      ok = -1;
    const char *path = label;

    if (irf_in != NULL && irn_in > 0u) {
        irf = (float *)malloc(sizeof(float) * irn_in);
        if (irf != NULL) {
            uint32_t q;
            for (q = 0; q < irn_in; q++) {
                irf[q] = irf_in[q];
            }
        }
    }
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
            /*
             * SHORTER, WHEN SOMETHING HAS TO KEEP UP WITH A PLAYER
             *
             * The float cabinet is a direct convolution, so it costs one
             * multiply-add per tap per sample: 8820 taps at 44.1 kHz is 389
             * million a second, and on this machine that is three quarters of
             * the audio a card asking every three milliseconds actually gets.
             * The int16 path fits, but its block quantisation is audible as a
             * buzz on a quiet signal - which is what a guitar is between notes.
             *
             * A cabinet does not need two hundred milliseconds of tail.  The
             * useful part is the first twenty or thirty; what follows is the
             * room the impulse was taken in, and cutting it costs a little of
             * that and nothing of the speaker.  The last sixty-four taps fade
             * out rather than stopping, because a hard cut is a rectangular
             * window and rings.
             *
             * Only when asked for: file work keeps the whole impulse, because
             * there is nothing to keep up with.
             */
            if (g_irtaps > 0 && g_fir_n > g_irtaps) {
                int q;
                for (q = 0; q < 64; q++) {
                    const float w = (float)(63 - q) / 63.0f;
                    g_fir[g_irtaps - 64 + q] *= w;
                }
                printf("  cabinet: impulse cut from %d taps to %d (%.0f ms)"
                       " so that the float path fits a period\n",
                       g_fir_n, g_irtaps,
                       1000.0 * (double)g_irtaps / (double)rate);
                g_fir_n = g_irtaps;
            }
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
            g_fslack = FHIST_SLACK;
            g_fpos = 0;
            g_fhist = (float *)calloc((size_t)(g_fir_n - 1 + FHIST_SLACK + BLK),
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
                           sizeof(float) *
                               (size_t)(g_fir_n - 1 + FHIST_SLACK + BLK));
                    g_fpos = 0;
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

/* The wav front end: read one, hand it to the loader above. */
static int cab_load(const char *path, uint32_t rate, int primary)
{
    uint32_t irn = 0, irrate = 0;
    float   *irf = path != NULL ? read_wav(path, &irn, &irrate) : NULL;
    int      rc;

    rc = cab_load_f(irf, irn, irrate, rate, primary, path);
    free(irf);
    return rc;
}

/*
 * THE CABINET OUT OF A PRESET
 *
 * A preset carries its loudspeaker, so this is where it is taken from: int16 in
 * the file, because that is what `ag_ir_load` takes on the chip, converted to
 * float here only because the loader above shares its staging arithmetic with the
 * wav path.
 *
 * Returns 0 when a cabinet was loaded, 1 when the preset is readable and says it
 * has **no** loudspeaker - which is an answer, not a failure - and -1 when there
 * is no preset to read.
 */
static int cab_from_preset(const char *path, uint32_t rate, char *label,
                           size_t label_n)
{
    FILE     *f = path != NULL ? fopen(path, "rb") : NULL;
    uint8_t  *blob = NULL;
    long      len = 0;
    uint32_t  frames = 0, irrate = 0;
    const int16_t *ir;
    int       rc = -1;

    if (f == NULL) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len > 0) {
        blob = (uint8_t *)malloc((size_t)len);
    }
    if (blob == NULL || fread(blob, 1, (size_t)len, f) != (size_t)len) {
        free(blob);
        fclose(f);
        return -1;
    }
    fclose(f);
    ir = ag_amp_preset_ir(blob, (uint32_t)len, &frames, &irrate);
    if (ir == NULL || frames == 0u) {
        /* Either the preset has no cabinet, or it is not a preset this build
         * understands.  ag_amp_preset_ir checks the magic and the version, so
         * the difference is worth reporting rather than papering over. */
        uint32_t m = 0;
        if ((uint32_t)len >= sizeof(uint32_t)) {
            m = ((const uint32_t *)(const void *)blob)[0];
        }
        if (m == AG_AMP_PRESET_MAGIC) {
            snprintf(label, label_n, "%s, which carries no cabinet", path);
            rc = 1;
        }
        free(blob);
        return rc;
    }
    {
        float *fl = (float *)malloc(sizeof(float) * frames);
        if (fl == NULL) {
            free(blob);
            return -1;
        }
        {
            uint32_t q;
            for (q = 0; q < frames; q++) {
                fl[q] = (float)ir[q] / 32768.0f;
            }
        }
        snprintf(label, label_n, "%s", path);
        rc = cab_load_f(fl, frames, irrate, rate, 1, label) == 0 ? 0 : -1;
        free(fl);
    }
    free(blob);
    return rc;
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
    const char *penv = getenv("AG_CAB_PRESET");
    char        pbuf[1024], fbuf[1024], rel[160];
    char        plabel[1024];
    const char *path = NULL, *fpath = NULL;
    /* 0 loaded from a preset, 1 the preset says there is no cabinet. */
    int         from_preset = -1;

    g_mono = (int16_t *)malloc(sizeof(int16_t) * (size_t)BLK);
    g_st = (int16_t *)malloc(sizeof(int16_t) * (size_t)BLK * 2);
    if (g_mono == NULL || g_st == NULL) {
        return -1;
    }

    if (env != NULL) {
        path = resolve(pbuf, sizeof(pbuf), env);
    }
    /*
     * THEN THE PRESET, BEFORE ANY LOOSE FILE
     *
     * A preset carries its own loudspeaker, and that is the point of it carrying
     * one: the preset says what this amplifier sounds like, so an `ir_*.wav` left
     * behind by an experiment must not outrank it.  AG_CAB_IR still wins, because
     * that is somebody asking for a particular speaker on purpose.
     */
    if (path == NULL) {
        char        prel[200];
        const char *ppath;
        snprintf(prel, sizeof(prel), "build/listen/%s.preset",
                 ag_amp_model_name(g_model));
        ppath = penv != NULL ? resolve(pbuf, sizeof(pbuf), penv)
                             : resolve(pbuf, sizeof(pbuf), prel);
        plabel[0] = 0;
        if (ppath != NULL) {
            from_preset = cab_from_preset(ppath, rate, plabel, sizeof(plabel));
        }
        if (from_preset == 0) {
            printf("  cabinet: from the preset %s\n", plabel);
        } else if (from_preset == 1) {
            printf("  cabinet: %s - so there is none, and 'c' has nothing to"
                   " switch on\n", plabel);
        }
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
    if (path == NULL && from_preset < 0) {
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
    if (path == NULL && from_preset < 0) {
        path = resolve(pbuf, sizeof(pbuf), AG_CAB_MATCHED);
    }
    if (path == NULL && from_preset < 0) {
        path = resolve(pbuf, sizeof(pbuf), AG_CAB_FALLBACK);
    }
    if (from_preset < 0 && cab_load(path, rate, 1) != 0) {
        return -1;
    }
    cab_store(0);
    snprintf(g_cab[0].path, sizeof(g_cab[0].path), "%s",
             from_preset == 0 ? plabel
                              : (from_preset == 1
                                     ? plabel
                                     : (path != NULL
                                            ? path
                                            : "ag_ir preset 4, which is not a"
                                              " cabinet")));

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
    if (from_preset == 1) {
        g_cab_mode = 0; /* the preset says there is no loudspeaker */
    }
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

/*
 * A GUITAR, RATHER THAN A RECORDING
 *
 * The take is a file on a loop, which is right for comparing two settings on the
 * same phrase and useless for the question a player actually has - how it
 * answers the picking hand.  So the same winmm that feeds the card can be asked
 * for the other direction, and an interface like a Valeton GP-5 appears as an
 * ordinary capture device.
 *
 * WHAT THIS COSTS IN LATENCY, HONESTLY
 *
 * The output queue is sized for hiccups, not for playing: seventy milliseconds
 * by default and twice that when the chain is expensive.  Add the capture side
 * and a block of render and the round trip is that plus twenty or thirty more.
 * That is fine for hearing what a knob does and too much to play tightly
 * against a drummer.  `--latency <ms>` shortens both queues for someone willing
 * to trade margin for feel, and the underrun counter at the end says whether the
 * trade held.
 *
 * The samples arrive in whatever the device offers.  Mono is asked for first
 * because a guitar input is one channel and there is no sense carrying two; a
 * device that refuses is opened as stereo and the left channel taken.
 */
#define WI_RING (32 * (uint32_t)BLK)

static HWAVEIN  g_wi;
static WAVEHDR  g_ihdr[NBUF_MAX];
static int16_t *g_ipcm[NBUF_MAX];
static int      g_inbuf;
static int      g_ich = 1;
static float    g_ring[WI_RING];
static uint32_t g_ring_w, g_ring_r;
static uint32_t g_ring_max;   /* deepest the input backlog got, in samples */
static uint32_t g_ring_trims; /* how often it had to be cut back */
static float    g_in_peak;    /* loudest the instrument got, for clipping */
static double   g_ring_frac;   /* where between two samples the reader is */
static double   g_ring_target; /* the depth the buffering settles at */
static double   g_ring_seen;   /* samples read while learning that depth */
/*
 * WHAT THE INSTRUMENT IS MULTIPLIED BY BEFORE THE CHAIN SEES IT
 *
 * The models are fitted against a take whose peak is around 0.77, and every
 * trim, every bank and the drive itself assume something of that size arriving.
 * A guitar into a line input is nowhere near it - measured here at 0.01 to 0.02,
 * which is thirty to forty decibels down - and a chain given that plays almost
 * clean: the sound Maxim described as "underprocessed", with a metallic edge
 * that is the sixteen-bit quantisation of a signal using ten of its bits being
 * amplified sixty decibels along with everything else.
 *
 * So there is a gain, and by default it is worked out rather than guessed: the
 * peak over the first second, against the take's own peak.  --ingain takes a
 * number of decibels instead, and '<' and '>' move it while playing.
 */
static float    g_in_gain = 1.0f;
static float    g_in_gain_db;   /* what was asked for, 0 if automatic */
/*
 * OFF UNLESS ASKED FOR.
 *
 * An automatic level changes how hard the model is driven, which changes the
 * overdrive - and a tool that quietly moves the thing being judged is worse than
 * one that leaves it wrong.  `--ingain auto` turns it on; `--ingain <dB>` and
 * '<' '>' set it by hand.
 */
static int      g_in_auto;
static float    g_in_seen;      /* the loudest so far while measuring */
static uint32_t g_in_learn;     /* unused now; the level comes from the peak */
static double   g_in_said;      /* the last gain reported, to keep it quiet */
static float    g_take_peak = 0.7f;
/* One period of the device, in frames - set when the stream opens.  The reader
 * needs it to know what depth to hold and the winmm path leaves it at zero. */
static uint32_t g_period;
static int      g_live;       /* playing the input rather than the take */
static int      g_live_ok;    /* the device opened */
static uint32_t g_starved;    /* blocks the input could not fill */

static void wavein_list(void)
{
    const UINT n = waveInGetNumDevs();
    UINT       i;
    printf("  capture devices:\n");
    if (n == 0u) {
        printf("    none\n");
        return;
    }
    for (i = 0; i < n; i++) {
        WAVEINCAPSA c;
        if (waveInGetDevCapsA(i, &c, sizeof(c)) == MMSYSERR_NOERROR) {
            printf("    %u: %s\n", i, c.szPname);
        }
    }
}

static int wavein_open(uint32_t rate, int dev, int nbuf)
{
    WAVEFORMATEX wf;
    int          i;
    int          ch;

    for (ch = 1; ch <= 2; ch++) {
        memset(&wf, 0, sizeof(wf));
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = (WORD)ch;
        wf.nSamplesPerSec = rate;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = (WORD)(2 * ch);
        wf.nAvgBytesPerSec = rate * (uint32_t)(2 * ch);
        if (waveInOpen(&g_wi, dev < 0 ? WAVE_MAPPER : (UINT)dev, &wf, 0, 0,
                       CALLBACK_NULL) == MMSYSERR_NOERROR) {
            g_ich = ch;
            break;
        }
        g_wi = NULL;
    }
    if (g_wi == NULL) {
        printf("  no capture device at %u Hz - the input needs the same rate as"
               " the take; try a 48 kHz one\n", rate);
        return -1;
    }
    g_inbuf = nbuf < 4 ? 4 : (nbuf > NBUF_MAX ? NBUF_MAX : nbuf);
    for (i = 0; i < g_inbuf; i++) {
        g_ipcm[i] = (int16_t *)calloc((size_t)BLK * (size_t)g_ich,
                                      sizeof(int16_t));
        if (g_ipcm[i] == NULL) {
            return -1;
        }
        memset(&g_ihdr[i], 0, sizeof(g_ihdr[i]));
        g_ihdr[i].lpData = (LPSTR)g_ipcm[i];
        g_ihdr[i].dwBufferLength =
            (DWORD)(BLK * g_ich * (int)sizeof(int16_t));
        if (waveInPrepareHeader(g_wi, &g_ihdr[i], sizeof(g_ihdr[i])) !=
                MMSYSERR_NOERROR ||
            waveInAddBuffer(g_wi, &g_ihdr[i], sizeof(g_ihdr[i])) !=
                MMSYSERR_NOERROR) {
            return -1;
        }
    }
    if (waveInStart(g_wi) != MMSYSERR_NOERROR) {
        return -1;
    }
    g_live_ok = 1;
    /*
     * And it starts playing, rather than waiting to be switched on.  Asking for
     * an input and then hearing the test loop is not a state anybody wanted - it
     * reads as the input being broken.  'L' goes back to the take.
     */
    g_live = 1;
    printf("  input: device %d, %s at %u Hz, %d blocks of %d - playing it now,"
           " 'L' switches to the take\n",
           dev, g_ich == 1 ? "mono" : "stereo, left channel", rate, g_inbuf,
           BLK);
    return 0;
}

/*
 * Drain whatever the device has finished into the ring and hand the buffers
 * back.  Called from the same loop that tops up the output, so nothing here
 * blocks and nothing needs a callback thread.
 */
/*
 * THE BACKLOG IS HELD SHORT, NOT MERELY PREVENTED FROM OVERFLOWING
 *
 * The ring used to be trimmed only when nearly full - eight thousand samples,
 * a hundred and eighty milliseconds - so the input was free to run that far
 * ahead of what was being played and stay there.  The output side measured
 * three milliseconds and said so, and the delay a player actually felt was the
 * backlog, which nothing was looking at.
 *
 * `keep` is the slack worth having: enough that a late drain does not leave the
 * render with nothing, short enough not to be heard.  Anything past it is thrown
 * away rather than played late, because a delay that grows and never shrinks is
 * the one fault an instrument cannot be played through.
 */
static void ring_trim(uint32_t keep)
{
    const uint32_t have = g_ring_w - g_ring_r;
    if (have > g_ring_max) {
        g_ring_max = have;
    }
    /*
     * Shed the surplus a sample at a time rather than jumping.
     *
     * A jump throws away everything it is over by, which is a hole in the middle
     * of a note and is heard as a click - and with a loop serving 99% of the
     * periods there is always a small surplus, so it clicked about three times a
     * second.  One sample per drain is a discontinuity too small to hear and
     * sheds a few hundred a second, which is more than the drift ever amounts
     * to.  The jump stays as the far end of the net, for a real stall.
     */
    if (keep > 0u && have > keep) {
        if (have > keep + keep) {
            /* A real stall: take the whole backlog out at once and accept the
             * discontinuity, because playing it late is worse. */
            g_ring_r = g_ring_w - keep;
        } else {
            /*
             * DROP THE SAMPLE WHERE THE SIGNAL IS QUIETEST, NOT THE NEXT ONE
             *
             * The two clock domains drift by a couple of hundred samples a
             * second, so a sample has to be shed about that often - and a sample
             * dropped in the middle of a loud waveform is a step, which at a
             * hundred a second is heard as a crackle.  Dropped where the signal
             * is near zero it is nothing at all.  Sixty-four samples is a
             * millisecond and a half to look through, which always contains
             * something quiet on anything a guitar plays.
             */
            uint32_t best = g_ring_r;
            float    q = 1.0e9f;
            uint32_t j;
            for (j = 0; j < 64u && (g_ring_r + j) != g_ring_w; j++) {
                const float v = g_ring[(g_ring_r + j) % WI_RING];
                const float a = v < 0.0f ? -v : v;
                if (a < q) {
                    q = a;
                    best = g_ring_r + j;
                }
            }
            /* Close the gap by moving what is before the dropped sample up one,
             * so the discontinuity is where the signal is smallest rather than
             * at the read pointer. */
            for (j = best; j != g_ring_r; j--) {
                g_ring[j % WI_RING] = g_ring[(j - 1u) % WI_RING];
            }
            g_ring_r++;
        }
        g_ring_trims++;
    }
}

static void wavein_poll(void)
{
    int i;
    if (!g_live_ok) {
        return;
    }
    for (i = 0; i < g_inbuf; i++) {
        if ((g_ihdr[i].dwFlags & WHDR_DONE) == 0) {
            continue;
        }
        {
            const int16_t *p = g_ipcm[i];
            int            k;
            for (k = 0; k < BLK; k++) {
                const uint32_t w = g_ring_w % WI_RING;
                g_ring[w] = (float)p[k * g_ich] / 32768.0f;
                g_ring_w++;
            }
        }
        g_ihdr[i].dwFlags &= ~WHDR_DONE;
        (void)waveInAddBuffer(g_wi, &g_ihdr[i], sizeof(g_ihdr[i]));
    }
    /*
     * If the ring has run far ahead of what is being played - which happens
     * after a stall, or when the tool spends a moment printing - throw the
     * backlog away rather than play it late.  Latency that grows and never
     * shrinks is the one fault a player notices immediately.
     */
    /*
     * THE BACKLOG IS HELD SHORT, NOT MERELY PREVENTED FROM OVERFLOWING
     *
     * This used to trim only when the ring was nearly full - eight thousand
     * samples, a hundred and eighty milliseconds - so the input was free to run
     * that far ahead of what was being played and stay there.  The output side
     * measured three milliseconds and reported it, and the latency a player
     * actually felt was the backlog, which nothing was looking at.
     *
     * Two periods of slack: enough that a late drain does not leave the render
     * with nothing, short enough that the ear cannot hear it.  Anything past
     * that is thrown away rather than played late, because a delay that grows
     * and never shrinks is the one fault an instrument cannot be played through.
     */
    /*
     * A far net, the same as the exclusive path uses.
     *
     * This trimmed to two blocks while the winmm capture holds six, so it threw
     * samples away on every call - the identical fault that was fixed for the
     * exclusive path and left standing here, which is why --out 0 still sounded
     * wrong after --out 1 was right.  The reader resamples now; the ring's depth
     * is its business and not something to be cut back to a number chosen here.
     */
    ring_trim(WI_RING - (uint32_t)(2 * BLK));
}

/*
 * ONE SAMPLE OF THE INPUT, AT WHATEVER RATE THE OUTPUT IS RUNNING
 *
 * THE TWO ENDS OF A USB INTERFACE DO NOT SHARE A CLOCK
 *
 * Measured on a Valeton GP-5: the capture side delivers 44099 samples a second
 * and the render side asks for 43989.  A quarter of a per cent, which sounds
 * like nothing and is a hundred and ten samples every second that have to go
 * somewhere.  Dropping them is what a crackle *is* - and it is only audible
 * while something is being played, because dropping a sample of silence costs
 * nothing.  It was blamed on cables, on the emulator and on the loop before the
 * counter that had been printing it all along was read properly.
 *
 * So the input is resampled rather than decimated: the read position moves by a
 * fractional step, and what comes back is interpolated between neighbours.  The
 * step is trimmed by how deep the ring is against where it should be, which is a
 * slow loop with no knowledge of either clock - it does not need one, because
 * the depth *is* the error.
 *
 * Cubic rather than linear.  Linear interpolation at a fractional delay that
 * creeps is a comb filter whose notch creeps with it, and on a guitar that is a
 * slow phasing; four points cost three more multiplies and have none of it.
 */
static float wavein_next(void)
{
    const uint32_t have = g_ring_w - g_ring_r;
    double         step;
    float          y0, y1, y2, y3, t, a, b, c;

    if (have < 4u) {
        g_starved++;
        return 0.0f;
    }
    /*
     * THE TARGET DEPTH IS LEARNED, NOT DECLARED
     *
     * It was one and a half periods, which is right for the exclusive path and
     * wrong for winmm, whose capture holds six blocks of 256.  Three times the
     * target with only half a per cent of authority means the correction sits
     * against its stop for ever - a permanent half per cent of pitch, which is
     * what "the sound is off" was.
     *
     * So the depth the buffering naturally settles at is measured over the first
     * second and taken as the target after that.  The loop then only ever has to
     * correct drift, which is what it is for; how deep the buffers are is the
     * device's business and not an error to be fought.
     */
    if (g_ring_seen < 4u * (double)(g_period ? g_period : (uint32_t)BLK) * 8.0) {
        g_ring_seen += 1.0;
        g_ring_target += ((double)have - g_ring_target) * 0.001;
        return g_ring[(g_ring_r++) % WI_RING] * g_in_gain;
    }
    /*
     * A quarter of a per cent is the drift between the two ends of a USB
     * interface; half a per cent of authority corrects it and is small enough
     * that the correction cannot itself be heard as a pitch change.
     */
    {
        /*
         * With a floor of one period.  Learned on its own the target settled at
         * a fifth of a millisecond on the exclusive path - the depth the ring
         * happens to sit at when it is being drained as fast as it fills - and a
         * reader held that close to the writer runs out on the first late drain.
         * That showed as sixty empty reads a second, which is a click each.
         */
        const double floor_d = (double)(g_period ? g_period : (uint32_t)BLK);
        double       tgt = g_ring_target > floor_d ? g_ring_target : floor_d;
        step = 1.0 + 0.005 * (((double)have - tgt) / tgt);
    }
    if (step < 0.995) {
        step = 0.995;
    }
    if (step > 1.005) {
        step = 1.005;
    }
    /*
     * The automatic level, settled once and then left alone.  A gain that kept
     * tracking would be a compressor, and a compressor in front of an amplifier
     * model is exactly the thing the model is supposed to be doing itself.
     */
    /*
     * FROM THE LOUDEST NOTE SO FAR, AND ONLY EVER UPWARDS
     *
     * Measuring the first second and stopping was wrong in the obvious way:
     * nobody is playing in the first second after starting the program, so it
     * measured silence and left the gain at one.
     *
     * Taking the loudest thing heard so far and never coming down settles after
     * the first few chords and then stays put.  An automatic gain that could
     * also fall would be a compressor, and a compressor in front of an amplifier
     * model is precisely the job the model is there to do.
     */
    if (g_in_auto) {
        const float v = g_ring[g_ring_r % WI_RING];
        const float a = v < 0.0f ? -v : v;
        if (a > g_in_seen * 1.05f && a > 1.0e-4f) {
            double db;
            g_in_seen = a;
            g_in_gain = g_take_peak / g_in_seen;
            if (g_in_gain > 200.0f) {
                g_in_gain = 200.0f;
            }
            if (g_in_gain < 1.0f) {
                g_in_gain = 1.0f;
            }
            db = 20.0 * log10((double)g_in_gain);
            /* Only when it moves by a decibel, so a rising level does not fill
             * the console while somebody is warming up. */
            if (db < g_in_said - 1.0 || db > g_in_said + 1.0) {
                g_in_said = db;
                printf("  input: loudest so far %.3f, so %+.1f dB to reach the"
                       " take's %.2f - '<' '>' to set it by hand\n",
                       (double)g_in_seen, db, (double)g_take_peak);
            }
        }
    }
    y0 = g_ring[(g_ring_r + 0u) % WI_RING];
    y1 = g_ring[(g_ring_r + 1u) % WI_RING];
    y2 = g_ring[(g_ring_r + 2u) % WI_RING];
    y3 = g_ring[(g_ring_r + 3u) % WI_RING];
    t = (float)g_ring_frac;
    /* Catmull-Rom through y1 and y2, which is where the fraction lives. */
    a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
    b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    c = -0.5f * y0 + 0.5f * y2;
    g_ring_frac += step;
    while (g_ring_frac >= 1.0) {
        g_ring_frac -= 1.0;
        g_ring_r++;
    }
    return (((a * t + b) * t + c) * t + y1) * g_in_gain;
}

static void wavein_close(void)
{
    int i;
    if (!g_live_ok) {
        return;
    }
    waveInStop(g_wi);
    waveInReset(g_wi);
    for (i = 0; i < g_inbuf; i++) {
        waveInUnprepareHeader(g_wi, &g_ihdr[i], sizeof(g_ihdr[i]));
        free(g_ipcm[i]);
    }
    waveInClose(g_wi);
    g_live_ok = 0;
}

/* Overridden by --latency; 0 keeps the built-in target. */
static int g_lat_ms;

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
        uint32_t ms = g_lat_ms > 0 ? (uint32_t)g_lat_ms : (uint32_t)NBUF_MS;
        if (g_lat_ms > 0) {
            printf("  latency: %u ms, as asked for - the underrun count at the"
                   " end says whether it held\n", ms);
        } else if (g_cost_frac > 0.25) {
            ms *= 2u;
            printf("  latency: %u ms rather than %d, because the chain costs"
                   " %.0f%% of realtime\n", ms, NBUF_MS, 100.0 * g_cost_frac);
        }
        g_nbuf = (int)((ms * rate + (uint32_t)(1000 * BLK) - 1u) /
                       (uint32_t)(1000 * BLK));
    }
    if (g_nbuf < (g_lat_ms > 0 ? 3 : 6)) {
        g_nbuf = g_lat_ms > 0 ? 3 : 6;
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

/*
 * THE CAPTURE AT ITS OWN RATE, WHATEVER THE SESSION IS RUNNING AT
 *
 * A NAM model is trained at one sample rate and is a different model at any
 * other, so the live reference used to be refused whenever the take and the
 * capture disagreed.  That made the one comparison worth having unavailable
 * exactly when it was most wanted: the Valeton GP-5 is a 44.1 kHz interface and
 * every capture in this tree is 48 kHz, so plugging a guitar in switched the
 * reference off.
 *
 * So the model keeps its rate and the signal is carried to it and back: up on
 * the way in, down on the way out, cubic both ways.  What that costs is one
 * interpolation each way on the reference side only - the chain being compared
 * against is untouched - and what it buys is being able to hear the amplifier
 * and the model on the same note while playing.
 *
 * Cubic rather than linear for the reason in wavein_next: linear at a fractional
 * delay that creeps is a comb whose notch creeps with it, and on a guitar that
 * is heard as a slow phasing.
 */
#define RS_N 4096u

static float    g_rs_in[RS_N];  /* what the chain was given, at the take's rate */
static uint32_t g_rs_in_w;
static double   g_rs_up;        /* read position into g_rs_in, take-rate units */
static float    g_rs_out[RS_N]; /* what the model made, at the model's rate */
static uint32_t g_rs_out_w;
static double   g_rs_dn;        /* read position into g_rs_out, model-rate units */
static int      g_rs_on;
static uint32_t g_rs_calls, g_rs_samples;
static uint64_t g_rs_ticks;   /* time inside the model itself */
static uint64_t g_ref_ticks;  /* time in the whole reference branch */
static uint64_t g_rs_worst;   /* the slowest single call into the model */

/*
 * A BUFFER IN FRONT OF THE MODEL, BECAUSE ITS COST IS NOT EVEN
 *
 * The capture averages 1.3 ms a period against three available, and every so
 * often takes nine - three periods' work in one, and the card goes hungry for
 * two of them.  That is what "it cannot keep up" was: not too much work, but too
 * much of it arriving at once.
 *
 * So the reference is produced a little ahead of being played.  Pressing 'y'
 * spends a few periods filling this buffer - the chain keeps playing meanwhile,
 * so the switch takes about twenty milliseconds rather than being instant - and
 * after that a slow call eats into the buffer instead of into the audio.
 *
 * It costs the reference twenty milliseconds of delay against the chain, which
 * does not matter for comparing tone and would matter for playing.  Nothing is
 * played through the reference: it is what the amplifier being copied sounds
 * like, not an amplifier to use.
 */
#define REF_FIFO 4096u
static float    g_ref_fifo[REF_FIFO];
static uint32_t g_ref_fw, g_ref_fr;
static uint32_t g_ref_prime; /* periods left to fill before it is heard */
/* The take's own position while the reference primes, so the chain being played
 * meanwhile does not fight the reference for the counter. */
static uint32_t g_pos2;

static float rs_cubic(const float *ring, double pos)
{
    const uint32_t i = (uint32_t)pos;
    const float    t = (float)(pos - (double)i);
    const float    y0 = ring[(i + 0u) % RS_N];
    const float    y1 = ring[(i + 1u) % RS_N];
    const float    y2 = ring[(i + 2u) % RS_N];
    const float    y3 = ring[(i + 3u) % RS_N];
    const float    a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
    const float    b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float    c = -0.5f * y0 + 0.5f * y2;
    return ((a * t + b) * t + c) * t + y1;
}

/*
 * n samples of the capture's output at the take's rate, given n samples of what
 * it should be fed.  Both ends of the resampling live here so that the caller
 * sees an ordinary block-in block-out reference.
 */
static void nam_at_rate(const float *in, float *out, int n)
{
    static float up[RS_N], made[RS_N];
    const double r_up = (double)nam_sample_rate(g_nam) / (double)g_rate;
    const double r_dn = 1.0 / r_up;
    int          k, m = 0;

    for (k = 0; k < n; k++) {
        g_rs_in[g_rs_in_w % RS_N] = in[k];
        g_rs_in_w++;
    }
    /* Up, while there are four points to interpolate between. */
    while (g_rs_up + 3.0 < (double)g_rs_in_w && m < (int)RS_N) {
        up[m++] = rs_cubic(g_rs_in, g_rs_up);
        g_rs_up += r_dn;
    }
    if (m > 0) {
        LARGE_INTEGER qa, qb;
        g_rs_calls++;
        g_rs_samples += (uint32_t)m;
        QueryPerformanceCounter(&qa);
        nam_process(g_nam, up, made, m);
        QueryPerformanceCounter(&qb);
        {
            const uint64_t d = (uint64_t)(qb.QuadPart - qa.QuadPart);
            g_rs_ticks += d;
            if (d > g_rs_worst) {
                g_rs_worst = d;
            }
        }
        for (k = 0; k < m; k++) {
            g_rs_out[g_rs_out_w % RS_N] = made[k];
            g_rs_out_w++;
        }
    }
    /* And down, exactly n of them. */
    for (k = 0; k < n; k++) {
        if (g_rs_dn + 3.0 < (double)g_rs_out_w) {
            out[k] = rs_cubic(g_rs_out, g_rs_dn);
            g_rs_dn += r_up;
        } else {
            /* Only at the very start, before the first block has been through
             * the model; after that the two rates keep each other fed. */
            out[k] = 0.0f;
        }
    }
    /*
     * Keep the read positions inside the ring rather than letting them run to
     * where a double loses samples: both are advanced by fractions for as long
     * as the tool runs, and at 48 kHz a float index would be losing accuracy
     * within the hour.
     */
    if (g_rs_up > (double)(4u * RS_N)) {
        g_rs_up -= (double)RS_N;
        g_rs_in_w -= RS_N;
    }
    if (g_rs_dn > (double)(4u * RS_N)) {
        g_rs_dn -= (double)RS_N;
        g_rs_out_w -= RS_N;
    }
}

/*
 * One block, and the caller says how long it is.
 *
 * It was always AG_IR_BLOCK, which is right when the card is fed from a queue
 * of whole blocks.  On the instrument path it is not: the device asks for a
 * period of 132 frames every three milliseconds, and rendering 256 of them at
 * 64% of realtime takes 3.7 ms - longer than the period it has to fit in.  The
 * card was left short by a third of its audio and the loop reported no fault,
 * because every period it did serve was served on time.
 *
 * So the granularity follows the device.  `n` may be anything up to
 * AG_IR_BLOCK, which is what the buffers here are sized for.
 */
static void render_n(int16_t *pcm, int n)
{
    float y[AG_IR_BLOCK];
    int   k;

    if (g_ref_on && g_nam != NULL) {
        /*
         * The capture, live.  It walks the take with the same position counter
         * the chain uses, so switching sides with 'y' does not jump in the take -
         * only one of the two is running at a time.
         *
         * The block here is the cabinet's, not NAM_BLOCK.  nam.h says the block
         * length is part of the arithmetic, so this does not agree with the
         * rendered file to the sample; it agrees to the ear, which is what a knob
         * is turned by.
         */
        const float rel = g_ref_follow && g_fit_drive > 0.0f
                              ? g_cfg.drive / g_fit_drive
                              : 1.0f;
        for (k = 0; k < n; k++) {
            /*
             * The guitar when there is one, exactly as the chain gets it.  This
             * read the take unconditionally, so pressing 'y' while playing swapped
             * the instrument for the recording - which is not a comparison of
             * anything.
             */
            const float f = g_in[g_pos];
            const float x = g_live ? wavein_next() : f;
            if (++g_pos >= g_frames) {
                g_pos = 0;
            }
            g_nam_in[k] = x * rel;
        }
        /*
         * Through the rate carrier, which is a straight pass when the model and
         * the take already agree - see nam_at_rate.
         */
        {
            /*
             * Into the buffer, in fewer and larger calls - see g_ref_fifo.
             *
             * The model averages a third of a period and every so often takes
             * three of them at once, and a spike inside an audio callback is a
             * hole in the sound.  Made a few periods ahead of being played, a
             * slow call empties the buffer a little instead.
             */
            static float pend[4 * AG_IR_BLOCK];
            static float made[4 * AG_IR_BLOCK];
            static int   pend_n;
            int          q;
            for (q = 0; q < n && pend_n < (int)(4 * AG_IR_BLOCK); q++) {
                pend[pend_n++] = g_nam_in[q];
            }
            if (pend_n >= 2 * n) {
                LARGE_INTEGER ra, rb;
                QueryPerformanceCounter(&ra);
                nam_at_rate(pend, made, pend_n);
                QueryPerformanceCounter(&rb);
                g_ref_ticks += (uint64_t)(rb.QuadPart - ra.QuadPart);
                for (q = 0; q < pend_n; q++) {
                    g_ref_fifo[g_ref_fw % REF_FIFO] = made[q];
                    g_ref_fw++;
                }
                pend_n = 0;
            }
        }
        if (g_ref_prime > 0u) {
            /*
             * Still filling: the chain keeps playing, so pressing 'y' is a short
             * fade rather than a gap.
             */
            g_ref_prime--;
            for (k = 0; k < n; k++) {
                const float x = g_in[g_pos2];
                if (++g_pos2 >= g_frames) {
                    g_pos2 = 0;
                }
                y[k] = g_bypass ? x : ag_amp_tick(amp(), x);
            }
        } else {
            for (k = 0; k < n; k++) {
                if (g_ref_fr == g_ref_fw) {
                    /* Deeper than the buffer: hold rather than write a zero. */
                    y[k] = k > 0 ? y[k - 1] : 0.0f;
                    g_starved++;
                } else {
                    y[k] = g_ref_fifo[(g_ref_fr++) % REF_FIFO] * g_nam_gain;
                }
            }
        }
    } else {
        for (k = 0; k < n; k++) {
            /*
             * The guitar or the take.  The take's position keeps moving either
             * way, so switching back with 'L' does not restart the phrase.
             */
            const float f = g_in[g_pos];
            const float x = g_live ? wavein_next() : f;
            if (++g_pos >= g_frames) {
                g_pos = 0;
            }
            y[k] = g_bypass ? x : ag_amp_tick(amp(), x);
        }
    }
    for (k = 0; k < n; k++) {
        const float v = y[k] < 0.0f ? -y[k] : y[k];
        if (v > g_peak_amp) {
            g_peak_amp = v;
        }
    }

    if (g_cab_mode == 1 && g_fir != NULL) {
        /*
         * The float cabinet: slide the block into the history and take one dot
         * product per sample.  Contiguous memory on both sides, so the compiler
         * vectorises it.
         */
        const int hist_n = g_fir_n - 1;
        /*
         * THE HISTORY IS APPENDED TO, AND ONLY SHIFTED WHEN IT HAS TO BE
         *
         * It used to shift the whole tail down by `n` on every call.  That is
         * `g_fir_n - 1` floats moved each time - 35 kB here - however few
         * samples were asked for, so the cost per sample doubled when the block
         * halved.  On the instrument path, where the device asks for 132 frames
         * at a time rather than 256, that alone lost a quarter of the periods
         * while the chain's own measured cost was a third of realtime and every
         * period that was served was served on time.
         *
         * Now the new samples go after the ones already there and the shift
         * happens once the slack is used up, which is every few thousand samples
         * instead of every call.  The arithmetic below is unchanged: it still
         * reads `g_fir_n` contiguous floats ending at the newest.
         */
        if (g_fpos + (uint32_t)n > (uint32_t)g_fslack) {
            memmove(g_fhist, g_fhist + g_fpos, sizeof(float) * (size_t)hist_n);
            g_fpos = 0;
        }
        memcpy(g_fhist + g_fpos + hist_n, y, sizeof(float) * (size_t)n);
        for (k = 0; k < n; k++) {
            const float *h = g_fhist + g_fpos + k;
            float        acc = 0.0f;
            int          j;
            for (j = 0; j < g_fir_n; j++) {
                acc += h[j] * g_fir[j];
            }
            y[k] = acc;
        }
        g_fpos += (uint32_t)n;
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
        for (k = 0; k < n; k++) {
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
        for (k = 0; k < n; k++) {
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
        for (k = 0; k < n; k++) {
            y[k] = (float)g_st[2 * k] / (32768.0f * h);
        }
    }

    /*
     * The cabinet's level match, which is what makes the architecture switch a
     * question about tone: the fitted impulse passes a different amount of power
     * than the one the bank was fitted through, and an A/B that also changes the
     * loudness is answered by whichever side is louder.  See g_match.
     */
    if (g_cab_mode != 0 && g_match != 1.0f) {
        for (k = 0; k < n; k++) {
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
        for (k = 0; k < n && g_rec_n < g_rec_cap; k++) {
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
        for (k = 0; k < n; k++) {
            y[k] = ag_biq_chain_tick(&g_solo_f, y[k]);
        }
    }

    for (k = 0; k < n; k++) {
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

static void render(int16_t *pcm)
{
    render_n(pcm, BLK);
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
        memset(g_fhist, 0,
               sizeof(float) * (size_t)(g_fir_n - 1 + FHIST_SLACK + BLK));
        g_fpos = 0;
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

/* Where the grid knob was when b switched it off, so that b is an A/B and not a
 * way of losing a setting. */
static float g_blk_was = 1.0f;

/* Scale both fitted banks by g_bank, from the originals rather than from what
 * they are now - otherwise the knob would only ever go down. */
/*
 * DRIVE SHAPING: MORE HARMONIC IN ONE BAND, WITHOUT MOVING THE TONE
 *
 * One knob that lifts the mid filter in front of the valves and cuts the nearest
 * band of the output bank by the same amount.  By magnitude the pair cancels;
 * the valves saw the louder signal and made more harmonic out of it, and that
 * part does not cancel.
 *
 * It exists because of what Maxim said about turning the plain mid knob up: the
 * overdrive got nearer the capture's character and the tone moved with it, and
 * the tone moving was not what he wanted.  Those are the two things a filter in
 * front of a valve does at once, and this is how they come apart.
 *
 * No number picked it.  Four measurements were tried on the setting he chose by
 * ear - the mean harmonic error, the even-to-odd balance, the error binned by
 * where the harmonic lands, and how fast the harmonics grow with playing level -
 * and every one of them ranked it below the setting he rejected, the last one
 * also ranking the model he likes best worst of the three.  So this is a knob and
 * not an objective term, and it stays that way until something measures right.
 */
static float g_shape_db;

static void shape_apply(void)
{
    int b, best = -1;
    float d = 1.0e9f;

    for (b = 0; b < AG_AMP_VOICE_N; b++) {
        const float f = g_tone0[b].hz;
        const float e = f > g_cfg.mid_hz ? f / g_cfg.mid_hz : g_cfg.mid_hz / f;
        if (f > 0.0f && e < d) {
            d = e;
            best = b;
        }
    }
    for (b = 0; b < AG_AMP_VOICE_N; b++) {
        g_cfg.tone[b].db = g_tone0[b].db * g_bank;
    }
    g_cfg.mid_db = g_mid_db0 * g_bank + g_shape_db;
    if (best >= 0) {
        g_cfg.tone[best].db -= g_shape_db;
    }
    knobs();
    printf("  shaping %+.1f dB at %.0f Hz, paid back at %.0f Hz\n",
           (double)g_shape_db, (double)g_cfg.mid_hz,
           best >= 0 ? (double)g_tone0[best].hz : 0.0);
}

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
           "   E / D   which mid - the peak's frequency, a sixth of an octave\n"
           "   R / F   how wide it is - the peak's Q, 0.3 to 4\n"
           "   T / G   drive shaping: lift the same peak in front of the valves\n"
           "           and pay it back after them, so the harmonics change and\n"
           "           the tone does not\n"
           "   t / g   trim 2 (dB)    n / m   master\n"
           "   [ / ]   volume, after the cabinet - changes nothing in the model\n"
           "   1 - 4   stages         x       which stage the blend knob turns\n"
           "   5 / 6   bass           7 / 8   mid        9 / \\   treble\n"
           "           - the passive tone stack, if this model has one; 0.5 is noon\n"
           "   b       grid on / off  - / =   grid, 0 to 1: how much of the\n"
           "           blocking - the bias walking under the playing\n"
           "   c       cabinet in / out\n"
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
           "   L       the guitar instead of the take, when --in opened one\n"
           "   y       the real amplifier in / out, for A/B against this chain\n"
           "   Y       whether it follows the drive knob; off by default, so\n"
           "           the thing being aimed at holds still\n"
           "   p       print the settings     h  this      ESC  quit\n"
           "\n");
}

static void print_settings(void)
{
    int i;
    printf("\n  drive %.2f  trim2 %+.1f dB  master %.5f  mid %+.1f dB at %.0f Hz  top"
           " %.0f Hz\n",
           (double)g_cfg.drive, (double)g_cfg.vtrim[1], (double)g_cfg.master,
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
    printf("  stages %d  os %dx  adaa %d  grid %.2f  cab %d  blend", g_n,
           g_cfg.os, g_cfg.adaa,
           g_cfg.blocking ? (double)g_cfg.block_depth : 0.0, g_cab_mode);
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
        /* The interstage level, which used to be `g12` and is the same point
         * in the chain: gain, bank and trim all sit in front of the second
         * valve, so one of them is enough and the trim is the one the matching
         * layer already fits. */
        g_cfg.vtrim[1] -= 0.5f;
        if (g_cfg.vtrim[1] < -40.0f) {
            g_cfg.vtrim[1] = -40.0f;
        }
        knobs();
        break;
    case 'g':
        g_cfg.vtrim[1] += 0.5f;
        knobs();
        break;
    case 'e':
        g_cfg.mid_db -= 0.5f;
        knobs();
        printf("  mid %+.1f dB at %.0f Hz, Q %.2f\n", (double)g_cfg.mid_db,
               (double)g_cfg.mid_hz, (double)g_cfg.mid_q);
        break;
    case 'd':
        g_cfg.mid_db += 0.5f;
        knobs();
        printf("  mid %+.1f dB at %.0f Hz, Q %.2f\n", (double)g_cfg.mid_db,
               (double)g_cfg.mid_hz, (double)g_cfg.mid_q);
        break;
    /*
     * WHICH middle, and how wide.
     *
     * `e` and `d` move one peaking filter up and down and say nothing about
     * where it sits, which is only half a control: "more mids" is a different
     * request at 400 Hz and at 1.6 kHz, and on a guitar the two are not even the
     * same instrument.  So the frequency moves too, by a sixth of an octave a
     * press - small enough to walk through the range and large enough to hear.
     *
     * The bottom stop is 80 Hz because below that this is not a middle any more,
     * and the top is under half the rate so the peak stays a peak rather than
     * folding into the corner of the band.
     */
    case 'E':
    case 'D': {
        const float step = (c == 'E') ? 1.0f / 1.122f : 1.122f;
        float       lo = 80.0f;
        float       hi = 0.45f * (float)g_rate;
        g_cfg.mid_hz *= step;
        if (g_cfg.mid_hz < lo) {
            g_cfg.mid_hz = lo;
        }
        if (g_cfg.mid_hz > hi) {
            g_cfg.mid_hz = hi;
        }
        knobs();
        printf("  mid %+.1f dB at %.0f Hz, Q %.2f\n", (double)g_cfg.mid_db,
               (double)g_cfg.mid_hz, (double)g_cfg.mid_q);
        break;
    }
    /*
     * And how wide it is.  A Q of 0.7 is most of an octave and 3 is a notch you
     * can point at; both are useful here, because a matching filter is looking
     * for the shape of a difference and that shape is sometimes broad tilt and
     * sometimes one resonance.
     */
    case '<':
    case '>': {
        /* The instrument's level into the chain; see g_in_gain.  Touching it
         * turns the automatic setting off, because a hand and a measurement
         * fighting over one number is worse than either. */
        const float d = (c == '>') ? 1.0f : -1.0f;
        g_in_auto = 0;
        g_in_learn = 0;
        g_in_gain *= (float)pow(10.0, (double)d / 20.0);
        if (g_in_gain < 0.01f) {
            g_in_gain = 0.01f;
        }
        if (g_in_gain > 500.0f) {
            g_in_gain = 500.0f;
        }
        printf("  input %+.1f dB\n", 20.0 * log10((double)g_in_gain));
        break;
    }
    case 'T':
        g_shape_db += 0.5f;
        if (g_shape_db > 18.0f) {
            g_shape_db = 18.0f;
        }
        shape_apply();
        break;
    case 'G':
        g_shape_db -= 0.5f;
        if (g_shape_db < -18.0f) {
            g_shape_db = -18.0f;
        }
        shape_apply();
        break;
    case 'R':
    case 'F':
        g_cfg.mid_q += (c == 'R') ? 0.1f : -0.1f;
        if (g_cfg.mid_q < 0.3f) {
            g_cfg.mid_q = 0.3f;
        }
        if (g_cfg.mid_q > 4.0f) {
            g_cfg.mid_q = 4.0f;
        }
        knobs();
        printf("  mid %+.1f dB at %.0f Hz, Q %.2f\n", (double)g_cfg.mid_db,
               (double)g_cfg.mid_hz, (double)g_cfg.mid_q);
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
        if (g_cfg.blocking) {
            g_blk_was = g_cfg.block_depth;
            g_cfg.blocking = 0;
        } else {
            g_cfg.blocking = 1;
            g_cfg.block_depth = g_blk_was > 0.0f ? g_blk_was : 1.0f;
        }
        knobs();
        break;
    /*
     * The grid, by twentieths.  Blocking has no knob on any amplifier and still
     * changes the feel of one more than most of the knobs that do exist, and it
     * was a switch here until it needed listening to in between.  What the
     * fraction scales, and what it deliberately leaves alone, is in the note on
     * ag_amp_cfg_t.block_depth.
     */
    case '-':
        g_cfg.block_depth -= 0.05f;
        if (g_cfg.block_depth < 0.0f) {
            g_cfg.block_depth = 0.0f;
        }
        g_cfg.blocking = g_cfg.block_depth > 0.0f;
        knobs();
        printf("  grid %.2f\n", (double)g_cfg.block_depth);
        break;
    case '=':
        g_cfg.block_depth += 0.05f;
        if (g_cfg.block_depth > 1.0f) {
            g_cfg.block_depth = 1.0f;
        }
        g_cfg.blocking = 1;
        knobs();
        printf("  grid %.2f\n", (double)g_cfg.block_depth);
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
        } else if (g_cab_want != 0) {
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
    case 'L':
        if (!g_live_ok) {
            printf("  no input open - start with --in to pick a device, or"
                   " --in list to see them\n");
        } else {
            g_live = !g_live;
            g_ring_r = g_ring_w; /* start from now, not from the backlog */
            printf("  %s\n", g_live ? "the guitar" : "the take");
        }
        break;
    case 'Y':
        g_ref_follow = !g_ref_follow;
        printf("  the capture %s\n",
               g_ref_follow ? "follows the drive knob"
                            : "holds still, whatever the knobs do");
        break;
    case 'y':
        if (g_nam == NULL) {
            printf("  no reference here - see the line about the rate at"
                   " startup\n");
        } else {
            g_ref_on = !g_ref_on;
            if (g_ref_on) {
                /* Six periods of it made before it is heard - see g_ref_fifo. */
                g_ref_prime = 6u;
                g_ref_fr = g_ref_fw;
                g_pos2 = g_pos;
            }
            printf("  %s\n", g_ref_on ? "the real amplifier" : "this chain");
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
/*
 * WASAPI, EXCLUSIVE AND EVENT DRIVEN - THE ONLY WAY TO PLAY THROUGH THIS
 *
 * winmm is a compatibility layer over WASAPI's shared mode, and shared mode
 * mixes: it holds its own buffers on top of whatever this tool asks for, and no
 * amount of shortening the queue here reaches them.  Measured on this machine
 * the card's own minimum period is three milliseconds and its default is ten,
 * and a round trip through winmm is several times that - fine for turning a knob
 * and hearing what it does, useless for picking a note and hearing it.
 *
 * Exclusive mode hands the device to one program and takes the mixer out of the
 * path.  With an event to wake on rather than a queue to poll, the latency is
 * the period plus what the render costs, and both are known numbers.
 *
 * WHAT IS TRADED FOR IT
 *
 * Nothing else can play while this holds the device - that is what exclusive
 * means - and a device that will not accept the take's rate cannot be opened at
 * all rather than being quietly resampled.  Both are the right way round for an
 * instrument: silence is better than a hidden resampler in the monitoring path.
 *
 * If exclusive is refused - some devices only offer shared - it falls back to
 * shared with the same event loop, which is still far better than winmm because
 * the wake-up is an event rather than a poll, and says so.
 */
#define WA_STAGE (8 * (uint32_t)BLK)

/*
 * The two subformat GUIDs, written out rather than taken from ksmedia.h.
 *
 * mingw declares them but ships their definitions in a library this tool does
 * not otherwise need, so linking against the header's names fails at the last
 * step.  Both values are fixed by the format specification and have not moved
 * since it was written.
 */
static const GUID k_wa_pcm = {
    0x00000001, 0x0000, 0x0010,
    { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
static const GUID k_wa_float = {
    0x00000003, 0x0000, 0x0010,
    { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};

static IMMDeviceEnumerator *g_wa_en;
static IAudioClient        *g_wa_rc, *g_wa_cc;
static IAudioRenderClient  *g_wa_rs;
static IAudioCaptureClient *g_wa_cs;
static HANDLE               g_wa_rev, g_wa_cev;
static UINT32               g_wa_rn, g_wa_cn;   /* frames a period */
static int                  g_wa_rch, g_wa_cch; /* channels */
static int                  g_wa_rfloat, g_wa_cfloat;
static int                  g_wa_on;
static int                  g_wa_excl;
static double               g_wa_ms;
static uint32_t             g_wa_periods;
static uint32_t             g_wa_capframes; /* what the input actually delivered */
static uint32_t             g_wa_nobuf;     /* GetBuffer refusals on the render side */
/* render()'s blocks waiting to be handed to the card, interleaved stereo. */
static int16_t              g_wa_stage[WA_STAGE * 2];
static uint32_t             g_wa_sw, g_wa_sr;
/* The deepest the staging buffer got: with the chain clocked by the input this
 * is where the delay between playing a note and hearing it actually lives. */
static uint32_t             g_stage_max;
static uint32_t             g_stage_drops;

static void wa_fmt(WAVEFORMATEXTENSIBLE *w, uint32_t rate, int ch, int as_float)
{
    memset(w, 0, sizeof(*w));
    w->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w->Format.nChannels = (WORD)ch;
    w->Format.nSamplesPerSec = rate;
    w->Format.wBitsPerSample = (WORD)(as_float ? 32 : 16);
    w->Format.nBlockAlign = (WORD)(ch * w->Format.wBitsPerSample / 8);
    w->Format.nAvgBytesPerSec = rate * w->Format.nBlockAlign;
    w->Format.cbSize = sizeof(*w) - sizeof(WAVEFORMATEX);
    w->Samples.wValidBitsPerSample = w->Format.wBitsPerSample;
    w->dwChannelMask = ch == 1 ? SPEAKER_FRONT_CENTER
                               : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    w->SubFormat = as_float ? k_wa_float : k_wa_pcm;
}

static IMMDevice *wa_device(EDataFlow flow, int index)
{
    IMMDeviceCollection *col = NULL;
    IMMDevice           *d = NULL;
    UINT                 n = 0;

    if (index < 0) {
        if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(
                g_wa_en, flow, eConsole, &d))) {
            return NULL;
        }
        return d;
    }
    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(g_wa_en, flow,
                                                      DEVICE_STATE_ACTIVE,
                                                      &col))) {
        return NULL;
    }
    IMMDeviceCollection_GetCount(col, &n);
    if ((UINT)index < n) {
        (void)IMMDeviceCollection_Item(col, (UINT)index, &d);
    }
    IMMDeviceCollection_Release(col);
    return d;
}

static void wa_name(IMMDevice *d, char *out, int n)
{
    IPropertyStore *ps = NULL;
    PROPVARIANT     v;
    out[0] = 0;
    if (FAILED(IMMDevice_OpenPropertyStore(d, STGM_READ, &ps))) {
        return;
    }
    PropVariantInit(&v);
    if (SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_Device_FriendlyName, &v)) &&
        v.pwszVal != NULL) {
        (void)WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, out, n, NULL, NULL);
    }
    PropVariantClear(&v);
    IPropertyStore_Release(ps);
}

void wa_list(void)
{
    int      flow;
    HRESULT  hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)hr;
    if (g_wa_en == NULL &&
        FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator, (void **)&g_wa_en))) {
        printf("  no audio enumerator\n");
        return;
    }
    for (flow = 0; flow < 2; flow++) {
        IMMDeviceCollection *col = NULL;
        UINT                 n = 0, i;
        printf("  %s:\n", flow == 0 ? "inputs (--in)" : "outputs (--out)");
        if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(
                g_wa_en, flow == 0 ? eCapture : eRender, DEVICE_STATE_ACTIVE,
                &col))) {
            continue;
        }
        IMMDeviceCollection_GetCount(col, &n);
        for (i = 0; i < n; i++) {
            IMMDevice *d = NULL;
            char       nm[256];
            if (SUCCEEDED(IMMDeviceCollection_Item(col, i, &d))) {
                wa_name(d, nm, (int)sizeof(nm));
                printf("    %u: %s\n", i, nm);
                IMMDevice_Release(d);
            }
        }
        IMMDeviceCollection_Release(col);
    }
}

/*
 * One client, opened at the shortest period the device admits to.
 *
 * Exclusive mode is asked for with the format this tool wants rather than the
 * device's mix format, because in exclusive there is no mixer to convert and a
 * device that cannot do the rate has to say so here instead of somewhere later.
 */
static int wa_client(IMMDevice *dev, int render, uint32_t rate, int want_ch,
                     double want_ms, IAudioClient **out, HANDLE *ev,
                     UINT32 *frames, int *ch, int *as_float)
{
    IAudioClient         *ac = NULL;
    WAVEFORMATEXTENSIBLE  w;
    REFERENCE_TIME        def_p = 0, min_p = 0, per;
    HRESULT               hr;
    int                   fl, c, mode;

    if (FAILED(IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL,
                                  (void **)&ac))) {
        return -1;
    }
    IAudioClient_GetDevicePeriod(ac, &def_p, &min_p);
    per = want_ms > 0.0 ? (REFERENCE_TIME)(want_ms * 10000.0) : min_p;
    if (per < min_p) {
        per = min_p;
    }
    for (mode = 0; mode < 2; mode++) {
        const AUDCLNT_SHAREMODE sm = mode == 0 ? AUDCLNT_SHAREMODE_EXCLUSIVE
                                               : AUDCLNT_SHAREMODE_SHARED;
        for (fl = 0; fl < 2; fl++) {
            for (c = want_ch; c <= 2; c++) {
                wa_fmt(&w, rate, c, fl);
                hr = IAudioClient_IsFormatSupported(ac, sm, &w.Format, NULL);
                if (hr != S_OK) {
                    continue;
                }
                hr = IAudioClient_Initialize(
                    ac, sm, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                    sm == AUDCLNT_SHAREMODE_EXCLUSIVE ? per : 0,
                    sm == AUDCLNT_SHAREMODE_EXCLUSIVE ? per : 0, &w.Format,
                    NULL);
                if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                    /*
                     * The device wants its own alignment.  Ask what it would
                     * have used, throw this client away - an initialised client
                     * cannot be initialised twice - and open a fresh one at that
                     * size.  Skipping the reopen is the classic way to get
                     * AUDCLNT_E_ALREADY_INITIALIZED and blame the driver.
                     */
                    UINT32 al = 0;
                    IAudioClient_GetBufferSize(ac, &al);
                    IAudioClient_Release(ac);
                    ac = NULL;
                    if (FAILED(IMMDevice_Activate(dev, &IID_IAudioClient,
                                                  CLSCTX_ALL, NULL,
                                                  (void **)&ac))) {
                        return -1;
                    }
                    per = (REFERENCE_TIME)(10000.0 * 1000.0 * (double)al /
                                               (double)rate +
                                           0.5);
                    hr = IAudioClient_Initialize(ac, sm,
                                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                 per, per, &w.Format, NULL);
                }
                if (SUCCEEDED(hr)) {
                    *ev = CreateEventA(NULL, FALSE, FALSE, NULL);
                    if (*ev == NULL ||
                        FAILED(IAudioClient_SetEventHandle(ac, *ev))) {
                        IAudioClient_Release(ac);
                        return -1;
                    }
                    IAudioClient_GetBufferSize(ac, frames);
                    *out = ac;
                    *ch = c;
                    *as_float = fl;
                    g_wa_excl = (sm == AUDCLNT_SHAREMODE_EXCLUSIVE);
                    g_wa_ms = 1000.0 * (double)*frames / (double)rate;
                    return 0;
                }
            }
        }
    }
    if (ac != NULL) {
        IAudioClient_Release(ac);
    }
    (void)render;
    return -1;
}

static int wa_open(uint32_t rate, int in_dev, int out_dev, double want_ms)
{
    IMMDevice *di = NULL, *dor = NULL;
    char       nm[256];

    (void)CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (g_wa_en == NULL &&
        FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator, (void **)&g_wa_en))) {
        printf("  no audio enumerator\n");
        return -1;
    }
    dor = wa_device(eRender, out_dev);
    di = wa_device(eCapture, in_dev);
    if (dor == NULL) {
        printf("  no output device %d\n", out_dev);
        return -1;
    }
    if (wa_client(dor, 1, rate, 2, want_ms, &g_wa_rc, &g_wa_rev, &g_wa_rn,
                  &g_wa_rch, &g_wa_rfloat) != 0) {
        /*
         * And say what it means, not only what happened.  This printed the
         * reason and fell through to the old path, which had no input open - so
         * the tool played the take, and a refused output looked like the live
         * input being ignored.
         */
        printf("  the output will not open at %u Hz in exclusive mode, and"
               " exclusive mode does not resample.\n"
               "  Both ends have to be the same interface at its own rate: put"
               " --out on the device\n"
               "  that --in is on, with a take at that rate.\n",
               rate);
        return -1;
    }
    wa_name(dor, nm, (int)sizeof(nm));
    printf("  out: %s, %s, %u frames a period (%.1f ms), %s\n", nm,
           g_wa_excl ? "exclusive" : "shared", (unsigned)g_wa_rn, g_wa_ms,
           g_wa_rfloat ? "float" : "16-bit");
    if (di != NULL &&
        wa_client(di, 0, rate, 1, want_ms, &g_wa_cc, &g_wa_cev, &g_wa_cn,
                  &g_wa_cch, &g_wa_cfloat) == 0) {
        wa_name(di, nm, (int)sizeof(nm));
        printf("  in:  %s, %s, %u frames a period, %s\n", nm,
               g_wa_excl ? "exclusive" : "shared", (unsigned)g_wa_cn,
               g_wa_cfloat ? "float" : "16-bit");
        if (SUCCEEDED(IAudioClient_GetService(g_wa_cc, &IID_IAudioCaptureClient,
                                              (void **)&g_wa_cs))) {
            g_live_ok = 1;
            g_live = 1;
        }
    } else if (in_dev != -2) {
        printf("  no input at %u Hz - playing the take\n", rate);
    }
    if (FAILED(IAudioClient_GetService(g_wa_rc, &IID_IAudioRenderClient,
                                       (void **)&g_wa_rs))) {
        return -1;
    }
    if (g_wa_cc != NULL) {
        IAudioClient_Start(g_wa_cc);
    }
    IAudioClient_Start(g_wa_rc);
    /*
     * THE THREAD HAS TO BE TOLD IT IS AN AUDIO THREAD
     *
     * Without this the loop ran at three quarters of the device's rate on every
     * model, every period and every cabinet - and a bare probe program doing
     * nothing but filling a sine on the same two streams ran at the full rate.
     * The difference was the work: about 1.3 ms of it inside a 3 ms period, at
     * ordinary priority, is enough for the scheduler to take the thread away
     * often enough to miss one period in four.  Nothing reports it as a fault,
     * because every period that is served is served on time.
     *
     * MMCSS is what Windows offers for this, and "Pro Audio" is the task name
     * meant for it.  timeBeginPeriod as well, for the same reason the winmm path
     * has it.
     */
    {
        DWORD idx = 0;
        HANDLE h = AvSetMmThreadCharacteristicsA("Pro Audio", &idx);
        (void)timeBeginPeriod(1);
        if (h == NULL) {
            printf("  note: could not raise the thread to Pro Audio priority;"
                   " expect the odd dropout\n");
        }
    }
    g_period = g_wa_rn;
    /* A second of the instrument to measure its level over, if nobody said. */
    if (g_in_auto) {
        g_in_learn = rate;
    }
    g_wa_on = 1;
    printf("  round trip: about %.0f ms, out and back - one period each way,"
           " and the render is done a period at a time\n",
           2.0 * g_wa_ms);
    return 0;
}

/*
 * THE CHAIN IS CLOCKED BY THE INPUT, NOT BY THE OUTPUT
 *
 * Rendering on demand from the output looked natural and quietly required a
 * sample to be thrown away about a hundred times a second: the loop served
 * 99.8% of the render periods, so the input ran ahead by the missing 0.2% and
 * the surplus had to go somewhere.  Shedding it is audible - that is what the
 * crackle was - and no amount of care about *where* to shed makes it right.
 *
 * Driven from the input there is nothing to shed.  Whatever arrives is rendered
 * immediately and waits in the staging buffer; the output takes a period from it
 * when the card asks.  Production equals consumption by construction, and the
 * jitter between the two lives in the buffer instead of in the signal.
 */
static void wa_stage_render_all(void)
{
    static int16_t blk[2 * (uint32_t)BLK];
    while (g_ring_w != g_ring_r) {
        const uint32_t avail = g_ring_w - g_ring_r;
        const uint32_t room = WA_STAGE - (g_wa_sw - g_wa_sr);
        uint32_t       nn = avail > (uint32_t)BLK ? (uint32_t)BLK : avail;
        uint32_t       k;
        if (room < nn) {
            /* The staging buffer is full: the output is not taking what is
             * being made, which is a stall rather than drift.  Leave the rest in
             * the ring; the far end of ring_trim will deal with it. */
            break;
        }
        render_n(blk, (int)nn);
        for (k = 0; k < nn; k++) {
            const uint32_t w = (g_wa_sw % WA_STAGE) * 2u;
            g_wa_stage[w] = blk[2 * k];
            g_wa_stage[w + 1] = blk[2 * k + 1];
            g_wa_sw++;
        }
        /*
         * AND THE STAGING BUFFER IS CAPPED TOO
         *
         * Clocking the chain from the input took the backlog out of the ring and
         * put it here instead: rendered audio waiting to be played, which is the
         * same delay wearing a different hat.  It built to thirty-three
         * milliseconds, all of it from the start, where the capture runs for a
         * while before the render client asks for anything.
         *
         * Two periods is the most that is useful - one being played, one ready -
         * and anything older than that is dropped rather than played late.  In
         * steady running this never fires; it is the startup transient it is
         * here for.
         */
        {
            const uint32_t cap = 3u * (g_wa_rn ? g_wa_rn : (uint32_t)BLK);
            const uint32_t depth = g_wa_sw - g_wa_sr;
            if (depth > cap + cap) {
                /* A stall: take it all out at once rather than play it late. */
                g_wa_sr = g_wa_sw - cap;
                g_stage_drops++;
            } else if (depth > cap) {
                /*
                 * Over by a little, which is jitter rather than drift now that
                 * the chain is clocked by the input.  Shedding a whole period of
                 * it leaves a three millisecond hole; shedding one sample leaves
                 * nothing anyone can hear, and the excess is gone within a few
                 * periods anyway.
                 */
                g_wa_sr++;
                g_stage_drops++;
            }
            if (g_wa_sw - g_wa_sr > g_stage_max) {
                g_stage_max = g_wa_sw - g_wa_sr;
            }
        }
    }
}

/* Everything the capture side has, into the same ring the winmm path used. */
static void wa_drain(void)
{
    int guard = 64;
    if (g_wa_cs == NULL) {
        return;
    }
    /*
     * GetBuffer directly, rather than asking GetNextPacketSize first.
     *
     * The packet-size call is a shared-mode idea: in exclusive mode it answers
     * zero however much the device has, so a drain built on it reads nothing and
     * the chain plays silence with the input meter showing starvation.  GetBuffer
     * works in both modes and says AUDCLNT_S_BUFFER_EMPTY when there is nothing,
     * which is the condition to stop on.
     */
    while (guard-- > 0) {
        BYTE   *p = NULL;
        UINT32  fr = 0;
        DWORD   fg = 0;
        HRESULT hr = IAudioCaptureClient_GetBuffer(g_wa_cs, &p, &fr, &fg, NULL,
                                                   NULL);
        if (hr != S_OK || fr == 0u) {
            if (hr == S_OK) {
                IAudioCaptureClient_ReleaseBuffer(g_wa_cs, 0);
            }
            break;
        }
        {
            UINT32 i;
            for (i = 0; i < fr; i++) {
                float v = 0.0f;
                if ((fg & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && p != NULL) {
                    v = g_wa_cfloat
                            ? ((const float *)p)[i * (UINT32)g_wa_cch]
                            : (float)((const int16_t *)p)[i * (UINT32)g_wa_cch] /
                                  32768.0f;
                }
                const float a = v < 0.0f ? -v : v;
                if (a > g_in_peak) {
                    g_in_peak = a;
                }
                g_ring[g_ring_w % WI_RING] = v;
                g_ring_w++;
            }
        }
        g_wa_capframes += fr;
        IAudioCaptureClient_ReleaseBuffer(g_wa_cs, fr);
    }
    /*
     * Four periods, not two.  Two was tight enough that ordinary jitter pushed
     * the backlog over it three times in six seconds, and every trim throws
     * away the samples it is over by - which is a hole in the middle of a note.
     * Four is twelve milliseconds here: still short enough to play through, and
     * loose enough that the trim is a safety net rather than part of the sound.
     */
    /*
     * The ring is left to the reader, which takes from it at a rate trimmed to
     * hold it near its target - see wavein_next.  Nothing is rendered here any
     * more: driving the chain from the input made production equal consumption
     * on paper and left the two clocks to fight in the buffer, which came out as
     * a hundred dropped samples a second.
     *
     * The trim stays as the far end of the net, for a stall the reader's half a
     * per cent of authority cannot pull back.
     */
    ring_trim(WI_RING - (uint32_t)(2 * BLK));
}

/* Top the staging FIFO up to `need` frames, a render() block at a time. */
static void wa_stage_fill(uint32_t need)
{
    static int16_t blk[2 * (uint32_t)BLK];
    while (g_wa_sw - g_wa_sr < need) {
        /*
         * Exactly what is short, not a whole AG_IR_BLOCK.  See render_n: a full
         * block does not fit in a three-millisecond period and the card goes
         * hungry without anything here noticing.
         */
        const int m = (int)(need - (g_wa_sw - g_wa_sr));
        const int nn = m > BLK ? BLK : m;
        int       k;
        render_n(blk, nn);
        for (k = 0; k < nn; k++) {
            const uint32_t w = (g_wa_sw % WA_STAGE) * 2u;
            g_wa_stage[w] = blk[2 * k];
            g_wa_stage[w + 1] = blk[2 * k + 1];
            g_wa_sw++;
        }
    }
}

static void wa_close(void)
{
    if (!g_wa_on) {
        return;
    }
    if (g_wa_cc != NULL) {
        IAudioClient_Stop(g_wa_cc);
    }
    IAudioClient_Stop(g_wa_rc);
    g_wa_on = 0;
}

static void status(void)
{
    char line[128];
    int  k;

    k = snprintf(line, sizeof(line),
                 " dr %.2f tr2 %+.1f bl%d %.2f mid %+.1f top %5.0f %dx%s%s n%d %s",
                 (double)g_cfg.drive, (double)g_cfg.vtrim[1], g_bstage + 1,
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
    int         in_dev = -2;  /* -2 no input, -1 the default device */
    int         out_dev = -1;
    int         lat_ms = 0;   /* 0 means the built-in target */
    int         use_wasapi = 0;
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
        } else if (strcmp(argv[i], "--in") == 0 && i + 1 < argc) {
            /*
             * A guitar instead of the take.  "list" prints what Windows has and
             * stops, because picking a capture device by guessing its number is
             * how somebody ends up listening to a laptop microphone and blaming
             * the amplifier model.
             */
            if (strcmp(argv[i + 1], "list") == 0) {
                wa_list();
                return 0;
            }
            in_dev = atoi(argv[++i]);
            use_wasapi = 1;
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_dev = atoi(argv[++i]);
            use_wasapi = 1;
        } else if (strcmp(argv[i], "--api") == 0 && i + 1 < argc) {
            use_wasapi = strcmp(argv[++i], "winmm") != 0;
        } else if (strcmp(argv[i], "--ingain") == 0 && i + 1 < argc) {
            if (strcmp(argv[i + 1], "auto") == 0) {
                i++;
                g_in_auto = 1;
            } else {
                g_in_gain_db = (float)atof(argv[++i]);
                g_in_gain = (float)pow(10.0, (double)g_in_gain_db / 20.0);
                g_in_auto = 0;
            }
        } else if (strcmp(argv[i], "--irtaps") == 0 && i + 1 < argc) {
            g_irtaps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--latency") == 0 && i + 1 < argc) {
            lat_ms = atoi(argv[++i]);
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
    /* Kept: it is what a live instrument's level is aimed at - see g_in_gain. */
    g_take_peak = peak_of(g_in, g_frames);
    printf("  take: %s, %u frames at %u Hz, %.1f s, peak %.3f\n", path, g_frames,
           g_rate, (double)g_frames / (double)g_rate, (double)g_take_peak);

    g_ckt = (ag_ckt_t *)malloc(sizeof(ag_ckt_t));
    if (g_ckt == NULL) {
        return 1;
    }
    ag_amp_model(&g_cfg, g_model, (float)g_rate);
    /*
     * A model that ships with the grid off carries a full `block_depth` anyway -
     * the two fields are independent on purpose, so that switching blocking on
     * in code gets the circuit and not a zero.  For a knob that is the wrong
     * starting point: it would jump the whole way on its first twentieth.  So
     * the knob's position is squared with the switch once, here, and after that
     * both `-` `=` and `b` are simple.
     */
    if (!g_cfg.blocking) {
        g_cfg.block_depth = 0.0f;
    }
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
     * THE CAPTURE ITSELF, FOR THE LIVE REFERENCE
     *
     * Cheap enough to be an option rather than a mode: 7.9x realtime on this
     * machine against the chain's 2%, so both sides fit in a block with room to
     * spare.  What it costs instead is the rate - see g_nam.
     */
    {
        const char *cp = ag_amp_model_capture(g_model);
        char        cbuf[1024];
        const char *cres = cp != NULL ? resolve(cbuf, sizeof(cbuf), cp) : NULL;

        g_fit_drive = g_cfg.drive;
        if (cres == NULL) {
            printf("  no capture for this model, so 'Y' has only the render\n");
        } else {
            g_nam = nam_load(cres, 1, 0);
            if (g_nam == NULL) {
                printf("  %s: %s - 'Y' has only the render\n", cres, nam_err());
            } else {
                /*
                 * A model trained at one rate stays at it and the signal is
                 * carried to it and back - see nam_at_rate.  This used to refuse
                 * the reference outright when the rates disagreed, which
                 * switched it off exactly when an instrument was plugged in:
                 * every capture in this tree is 48 kHz and the interface is
                 * 44.1, so `y` did nothing on the one path where it mattered
                 * most.
                 */
                if ((uint32_t)nam_sample_rate(g_nam) != g_rate) {
                    g_rs_on = 1;
                    printf("  capture: %s at %d Hz against a %u Hz take -"
                           " carried both ways, cubic\n",
                           cres, nam_sample_rate(g_nam), g_rate);
                }
                printf("  capture: %s, live - 'y' puts it in place of this"
                       " chain, 'Y' makes it follow the drive\n", cres);
            }
        }
    }

    g_lat_ms = lat_ms;
    /*
     * The instrument path gets a shorter impulse unless one was asked for.
     *
     * 1024 taps is 23 ms here, which is the speaker and the first of its room.
     * Measured against the card's rate: the whole 8820 taps fed it 74%, 4096 fed
     * 93%, 2048 fed 99% and 1024 feeds all of it - and the last percent matters,
     * because a loop that is one percent short lets the input run ahead and the
     * backlog has to be shed, which is audible.  The
     * alternative was the int16 convolution, which fits easily and buzzes on a
     * quiet signal - and a guitar is a quiet signal between notes.
     */
    if (use_wasapi && g_irtaps == 0) {
        g_irtaps = 1024;
    }
    /* The old capture path is winmm's; WASAPI opens its own further down. */
    if (in_dev != -2 && !use_wasapi) {
        (void)wavein_open(g_rate, in_dev, lat_ms > 0 ? 4 : 6);
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
             * The capture, brought to the same rms as what was just rendered.
             * Done here because this is the one place that has both: a second of
             * this chain's output and a second of the reference over the same
             * material.
             *
             * A capture's level is whatever the person who made it played at,
             * and our master is set to fill sixteen bits - measured on the three
             * models the two sides were +2.2, +0.8 and -13.7 dB apart.  Fourteen
             * decibels is not a tone comparison, it is a loudness comparison,
             * and loudness wins every one of those.
             */
            if (g_nam != NULL) {
                const int save_on = g_ref_on;
                double    nam_sq = 0.0;
                int16_t  *sc2 = (int16_t *)malloc(sizeof(int16_t) * (size_t)BLK * 2);
                if (sc2 != NULL) {
                    g_ref_on = 1;
                    g_pos = save_pos;
                    for (b = 0; b < blocks; b++) {
                        int j;
                        render(sc2);
                        for (j = 0; j < BLK; j++) {
                            const double v = (double)sc2[2 * j] / 32768.0;
                            nam_sq += v * v;
                        }
                    }
                    free(sc2);
                    g_ref_on = save_on;
                    if (nam_sq > 1e-20 && chain_sq > 1e-20) {
                        /*
                         * nam_sq was measured with g_nam_gain already applied, so
                         * the correction multiplies what is there rather than
                         * replacing it.
                         */
                        g_nam_gain *= (float)sqrt(chain_sq / nam_sq);
                        printf("  capture: level matched %+.1f dB to the chain,"
                               " so 'y' compares tone\n",
                               20.0 * log10((double)g_nam_gain));
                    }
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
        printf("  keys \"%s\": drive %.2f trim2 %+.1f top %.0f mid %+.1f cab %d\n",
               keys, (double)g_cfg.drive, (double)g_cfg.vtrim[1],
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

    /*
     * THE LOW LATENCY PATH, WHEN THERE IS AN INSTRUMENT TO PLAY
     *
     * Taken whenever an input was asked for, because that is the only case where
     * the latency is the point; a tool being used to turn knobs against a
     * recording is better off on winmm, which shares the card with everything
     * else on the machine.  --api winmm forces the old path back for comparison.
     */
    if (use_wasapi && wa_open(g_rate, in_dev, out_dev,
                              lat_ms > 0 ? (double)lat_ms : 0.0) == 0) {
        const clock_t   t0 = clock();
        const ULONGLONG tw0 = GetTickCount64();
        const uint32_t  st0 = g_starved;
        int             kb = 0;
        for (;;) {
            /*
             * Both events, not just the render one.
             *
             * An exclusive-mode capture client hands its buffer over when its
             * own event fires and answers "empty" at any other moment, so a loop
             * that only waits on the render side reads nothing at all - the
             * chain plays silence and the starve counter says every sample.
             */
            const DWORD w = WaitForSingleObject(g_wa_rev, 200);
            /*
             * ONE EVENT TO WAIT ON, AND THE INPUT READ ON THE SAME BEAT
             *
             * Waiting on both events looked right and was not.
             * WaitForMultipleObjects returns the *lowest* signalled index, so
             * with both devices on the same period the two woke the loop in
             * turn - and every wake-up that belonged to the capture side was a
             * render period not served.  The card ended up fed 28 thousand
             * frames a second instead of 44, which is a third of the audio
             * simply missing.
             *
             * The two run at the same period, so a drain on each render wake
             * collects exactly what one period delivered.  One clock, both
             * directions.
             */
            /*
             * Only when the capture side says it has something.
             *
             * An exclusive-mode capture GetBuffer does not return "empty" when
             * the device has nothing ready - it waits for the next period.  
             * Calling it unconditionally therefore cost a whole period on every
             * pass, and the loop settled at exactly three quarters of the rate
             * it needed on every model, every period size and every cabinet:
             * too round a number for a shortage of processor, which is what it
             * was mistaken for twice.
             */
            if (g_wa_cev != NULL &&
                WaitForSingleObject(g_wa_cev, 0) == WAIT_OBJECT_0) {
                wa_drain();
            }
            if (w == WAIT_OBJECT_0) {
                UINT32        want = g_wa_rn, pad = 0;
                BYTE         *p = NULL;
                const clock_t tb = clock();
                double        bms;
                if (!g_wa_excl) {
                    IAudioClient_GetCurrentPadding(g_wa_rc, &pad);
                    want = g_wa_rn > pad ? g_wa_rn - pad : 0u;
                }
                if (want > 0u &&
                    FAILED(IAudioRenderClient_GetBuffer(g_wa_rs, want, &p))) {
                    g_wa_nobuf++;
                    p = NULL;
                }
                if (want > 0u && p != NULL) {
                    UINT32 i;
                    int    c;
                    /*
                     * Whatever the input has made by now, and no more: the chain
                     * is clocked by the output and the input is resampled to
                     * match - see wavein_next.  If the staging buffer is short
                     * the last sample is held rather than a zero written,
                     * because a held sample is a moment of flatness and a zero
                     * is a step.
                     */
                    wa_stage_fill(want);
                    for (i = 0; i < want; i++) {
                        const uint32_t r = (g_wa_sr % WA_STAGE) * 2u;
                        if (g_wa_sr == g_wa_sw) {
                            g_starved++;
                            for (c = 0; c < g_wa_rch; c++) {
                                const int16_t v =
                                    g_wa_stage[((g_wa_sr - 1u) % WA_STAGE) * 2u +
                                               (c ? 1 : 0)];
                                if (g_wa_rfloat) {
                                    ((float *)p)[i * (UINT32)g_wa_rch +
                                                 (UINT32)c] =
                                        (float)v / 32768.0f;
                                } else {
                                    ((int16_t *)p)[i * (UINT32)g_wa_rch +
                                                   (UINT32)c] = v;
                                }
                            }
                            continue;
                        }
                        for (c = 0; c < g_wa_rch; c++) {
                            const int16_t v = g_wa_stage[r + (c ? 1 : 0)];
                            if (g_wa_rfloat) {
                                ((float *)p)[i * (UINT32)g_wa_rch + (UINT32)c] =
                                    (float)v / 32768.0f;
                            } else {
                                ((int16_t *)p)[i * (UINT32)g_wa_rch +
                                               (UINT32)c] = v;
                            }
                        }
                        g_wa_sr++;
                    }
                    IAudioRenderClient_ReleaseBuffer(g_wa_rs, want, 0);
                }
                bms = 1000.0 * (double)(clock() - tb) /
                      (double)CLOCKS_PER_SEC;
                if (bms > g_worst_ms) {
                    g_worst_ms = bms;
                }
                g_wa_periods++;
            } else if (w == WAIT_TIMEOUT) {
                /* The card stopped asking for audio.  A wake-up from the capture
                 * event is not that, and counting it as one reported thousands
                 * of dropouts on a path that had none. */
                g_under++;
            }
            if (run_secs > 0.0 &&
                (double)(clock() - t0) / (double)CLOCKS_PER_SEC > run_secs) {
                break;
            }
            /*
             * The keyboard, but not on every period.
             *
             * _kbhit goes through the console, and a console call costs enough
             * that at three hundred periods a second it was the loop's largest
             * expense: the card was served two hundred times a second instead of
             * three hundred and thirty and went hungry by a third, with every
             * period it did serve served on time and nothing reporting a fault.
             * Twenty times a second is still faster than a hand.
             */
            if (++kb >= 16) {
                kb = 0;
                while (_kbhit()) {
                    const int c = _getch();
                    if (c == 0 || c == 224) {
                        (void)_getch();
                        continue;
                    }
                    if (key(c)) {
                        wa_close();
                        return 0;
                    }
                    status();
                }
            }
        }
        {
            /*
             * The one number that matters: what fraction of the audio the card
             * asked for actually reached it.  Anything under a hundred is a
             * dropout however tidy the rest of the report looks - which is how
             * a quarter of the audio went missing for an afternoon while every
             * other counter read zero.
             */
            const double secs = (double)(GetTickCount64() - tw0) / 1000.0;
            const double fed =
                secs > 0.0 ? 100.0 * (double)g_wa_periods * (double)g_wa_rn /
                                 ((double)g_rate * secs)
                           : 0.0;
            printf("\n  the instrument path: %.0f%% of the audio the card asked"
                   " for, %u periods of %.1f ms in %.1f s\n",
                   fed, g_wa_periods, g_wa_ms, secs);
            printf("  input %u frames, peak %.2f%s, %u starved, backlog at"
                   " most %.1f ms (%.1f at the end), %u shed, staging at most"
                   " %.1f ms (%u drops),"
                   " worst render %.1f ms, %u waits timed out\n",
                   g_wa_capframes, (double)g_in_peak,
                   g_in_peak > 0.99f ? " - CLIPPING BEFORE US" : "",
                   g_starved - st0,
                   1000.0 * (double)g_ring_max / (double)g_rate,
                   1000.0 * (double)(g_ring_w - g_ring_r) / (double)g_rate,
                   g_ring_trims,
                   1000.0 * (double)g_stage_max / (double)g_rate,
                   g_stage_drops, g_worst_ms, g_under);
            if (g_rs_calls > 0u) {
                LARGE_INTEGER f;
                QueryPerformanceFrequency(&f);
                /*
                 * The reference costs what it costs, and it is worth printing:
                 * the model is a third of a period on average and takes three
                 * of them now and then, which is why it is made ahead rather
                 * than in the callback - see g_ref_fifo.
                 */
                printf("  reference: %u calls, %u samples, %.0f ms in the"
                       " model, worst call %.2f ms against a %.1f ms period\n",
                       g_rs_calls, g_rs_samples,
                       1000.0 * (double)g_rs_ticks / (double)f.QuadPart,
                       1000.0 * (double)g_rs_worst / (double)f.QuadPart,
                       g_wa_ms);
            }
        }
        wa_close();
        return 0;
    }

    /*
     * The instrument path was asked for and could not open.  Rather than play
     * the take and look as though the input had been ignored, open the input on
     * the old path and say what that costs.
     */
    if (use_wasapi && in_dev != -2) {
        printf("  falling back on winmm for the instrument: the latency will be"
               " tens of milliseconds rather than ten\n");
        (void)wavein_open(g_rate, in_dev, lat_ms > 0 ? 4 : 6);
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
        /* Whatever the guitar has played since the last pass, before anything
         * is rendered from it. */
        wavein_poll();
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
