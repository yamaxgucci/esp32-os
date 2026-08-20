/*
 * tube_render - look at, measure and listen to the one-dimensional valve model.
 *
 * A tool rather than a test: the things here need a recording, or produce a
 * curve to look at, or print a number that has to be judged rather than
 * asserted.  What is asserted lives in host-tests/test_tube.c.
 *
 *   tube_render curve            the baked curves, the operating points, and
 *                                what is lost beyond the end of each table
 *   tube_render resp             the three filters, as decibels against
 *                                frequency, so "cuts above 12 kHz" is a curve
 *                                before it is a claim
 *   tube_render alias            non-harmonic products against oversampling and
 *                                antialiasing, plus what each costs
 *   tube_render res              how many points the curve needs and how much of
 *                                the grid-current tail to keep - the two errors
 *                                that compete on a uniform axis
 *   tube_render noise in.wav [drive]   whose hiss it is: the recording's, the
 *                                     amplifier's compression, or arithmetic
 *   tube_render floor in.wav [start_s] peak and noise floor of any file, in a
 *                                     named window, with the floor by band
 *   tube_render cab                    what the cabinet impulse does
 *   tube_render attack [drive]         what blocking does to a hard pick
 *   tube_render render in out [drive [os [adaa [cab [g12 [blocking
 *                                [ccouple2_nF [stages]]]]]]]]
 *                                writes `out` dry and `out_cab` through the
 *                                cabinet convolution, which is where the chain
 *                                actually ends.  cab: 3 dark, 4 bright, 0 off.
 *
 * Built by host-tests/CMakeLists.txt, so `argon tests` produces
 * build-host/tube_render(.exe).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ag_amp.h"
#include "ag_biq.h"
#include "ag_ckt.h"
#include "ag_ir.h"
#include "ag_os.h"
#include "ag_tube.h"
/* The capture player and the resampler, so that the reference a fit is
 * measured against can be made here rather than by three passes through
 * files with a Go binary in the middle. */
#include "nam.h"
#include "wavrate_core.h"

#define RATE 22050.0f

/*
 * The measured cabinet, which is a real Vox AC30 impulse: 151 ms at 48 kHz,
 * 24-bit - which is why read_wav understands 24-bit at all - and -17 dB at 8 kHz,
 * which is what a loudspeaker looks like.  AG_CAB_IR overrides it.
 *
 * The alternative is an ag_ir preset, and those are not cabinets: 3 and 4 are one
 * large first tap plus twenty milliseconds of low-passed noise, and the tap
 * dominates so completely that the response measures flat within two decibels
 * from 80 Hz to 10 kHz.  run_cab prints whichever it used and checks the shape,
 * because a convolution with the wrong impulse sounds like a modelling problem
 * and is not one.
 */
#ifndef AG_CAB_DEFAULT
#define AG_CAB_DEFAULT "assets/audio/guitar-di/1 Vox AC30 1.wav"
#endif
/*
 * And the one the voicing in ag_amp_defaults was fitted through: the measured
 * linear response of the NAM Marshall this chain is being matched to.  Preferred
 * when it is there, because a voicing and the cabinet it was fitted with belong
 * together - through the Vox impulse instead, the same fit only reaches 2.53 dB
 * rms against 1.29.
 */
#ifndef AG_CAB_MATCHED
#define AG_CAB_MATCHED "build/nam/imp_mars.wav"
#endif

/*
 * Which cabinet to put after the amplifier.
 *
 * `g_cab_path` is a real impulse response from a file and is what should be used;
 * `g_cab` falls back on an ag_ir preset, and it is worth writing down what those
 * presets are, because listening to one and calling it a cabinet wastes an
 * afternoon: presets 3 and 4 are synthetic - one large first tap and twenty
 * milliseconds of low-passed white noise with a linear decay.  The big first tap
 * makes the whole thing close to a delta, so it barely changes the signal (dry
 * and "cabinet" renders measured 1-2 dB apart in every band), and the tail is
 * literally noise, so what the music gets convolved with is noise.
 *
 * 0 disables the cabinet entirely.
 */
static int         g_cab = 4;
static const char *g_cab_path;

/*
 * Which amplifier every mode in this tool builds - AG_MODEL=bogner, slo or
 * jcm800, resolved once in main.
 *
 * An environment variable rather than another positional argument, for the same
 * reason as AG_TOP_HZ and the rest: `render` already takes eleven of them, and
 * the model has to reach `curve`, `fit`, `humps` and `render` alike, which do not
 * share an argument list.  `preset` takes it as an argument as well, because
 * writing a file called bogner.preset out of whatever the environment happened to
 * hold is exactly the kind of mistake that is only found much later.
 */
static int g_model = AG_AMP_MODEL_JCM800;

/* Everything the reader has to know before believing a number from this run. */
static void print_model(void)
{
    printf("  model: %s, %s\n", ag_amp_model_name(g_model),
           ag_amp_model_fitted(g_model)
               ? "voicing fitted against a capture of the real amplifier"
               : "voicing is a STARTING POINT, not fitted - see README.md,"
                 " \"Authoring a preset for another amplifier\", step 4");
}

/* One config for the model this run is about.  Every mode goes through here so
 * that no mode can quietly stay on the default. */
static void model_cfg(ag_amp_cfg_t *cfg, float fs)
{
    ag_amp_model(cfg, g_model, fs);
    /*
     * AG_COUPLE_MUL="1,4,8,8" - the per-stage coupling corners, as multiples of
     * the schematic's.  An environment variable for the same reason AG_TOP_HZ is
     * one: it has to reach every mode, and what it is for is trying a component
     * value across all of them before deciding to believe it.
     *
     * 8 on a 22 nF coupling is 2.7 nF, which is an ordinary value for a hot stage;
     * that is the range this is meant to explore, and anything past about 16 is
     * not a bass cut, it is a different amplifier.
     */
    {
        /* AG_GAIN_MUL="1,1,0.5,0.3" - the gain into each stage, for trying a
         * different distribution of the work across the valves before believing
         * it.  See `knee`: on the four-stage model the last valve was doing all of
         * it and was already saturated at the quietest probe. */
        const char *g = getenv("AG_GAIN_MUL");
        if (g != NULL) {
            int i;
            for (i = 0; i < AG_AMP_STAGES && *g != 0; i++) {
                const float v = (float)atof(g);
                if (i == 0) {
                    cfg->drive *= v;
                } else if (i == 1) {
                    cfg->g12 *= v;
                } else {
                    cfg->gain[i] *= v;
                }
                while (*g != 0 && *g != ',') {
                    g++;
                }
                if (*g == ',') {
                    g++;
                }
            }
            /* Once: model_cfg is called inside row loops, and this line was
             * landing in the middle of a table. */
            static int said_g = 0;
            if (said_g++ == 0) {
                printf("  AG_GAIN_MUL: drive %.3f g12 %.3f gain2 %.3f"
                       " gain3 %.3f\n",
                       (double)cfg->drive, (double)cfg->g12,
                       (double)cfg->gain[2], (double)cfg->gain[3]);
            }
        }
    }
    {
        const char *e = getenv("AG_COUPLE_MUL");
        if (e != NULL) {
            int i;
            for (i = 0; i < AG_AMP_STAGES && *e != 0; i++) {
                cfg->couple_mul[i] = (float)atof(e);
                while (*e != 0 && *e != ',') {
                    e++;
                }
                if (*e == ',') {
                    e++;
                }
            }
            printf("  AG_COUPLE_MUL: coupling corners x%.2f %.2f %.2f %.2f\n",
                   (double)cfg->couple_mul[0], (double)cfg->couple_mul[1],
                   (double)cfg->couple_mul[2], (double)cfg->couple_mul[3]);
        }
    }
}
#define PI   3.14159265358979

/* ------------------------------------------------------------------------ */
/* wav, mono 16 bit, which is all this needs                                 */
/* ------------------------------------------------------------------------ */

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

static int write_wav(const char *path, const float *x, uint32_t frames,
                     uint32_t rate)
{
    FILE          *f = fopen(path, "wb");
    const uint32_t data = frames * 2u;
    uint32_t       i, clipped = 0;
    if (f == NULL) {
        printf("  cannot write %s\n", path);
        return -1;
    }
    fwrite("RIFF", 1, 4, f);
    wr_u32(f, 36u + data);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    wr_u32(f, 16);
    wr_u16(f, 1);
    wr_u16(f, 1);
    wr_u32(f, rate);
    wr_u32(f, rate * 2u);
    wr_u16(f, 2);
    wr_u16(f, 16);
    fwrite("data", 1, 4, f);
    wr_u32(f, data);
    for (i = 0; i < frames; i++) {
        float v = x[i] * 32767.0f;
        if (v > 32767.0f) {
            v = 32767.0f;
            clipped++;
        } else if (v < -32768.0f) {
            v = -32768.0f;
            clipped++;
        }
        wr_u16(f, (uint16_t)(int16_t)(v < 0.0f ? v - 0.5f : v + 0.5f));
    }
    fclose(f);
    printf("  wrote %s (%u frames", path, frames);
    if (clipped) {
        printf(", %u CLIPPED", clipped);
    }
    printf(")\n");
    return 0;
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t rd_u16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

/* Mono float, first channel only, whatever rate the file says. */
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
    /* 24-bit as well as 16, because the cabinet impulses that come in packs are
     * 24-bit and refusing them is refusing the only real cabinet in the tree. */
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

/* ------------------------------------------------------------------------ */

static ag_ckt_t *g_ckt;
static float    *g_tab;

/*
 * The signal every axis in this tool is fitted to, two seconds of it.  It comes
 * from ag_amp so that the fit here and the fit on the board are the same fit -
 * an axis fitted to one signal and used on another is the mistake docs/08
 * records twice.
 */
#define G_PROBE_N 44100
static float *g_probe;

static int alloc_all(void)
{
    g_ckt = (ag_ckt_t *)malloc(sizeof(ag_ckt_t));
    g_tab = (float *)malloc(sizeof(float) * AG_AMP_TAB_FLOATS);
    g_probe = (float *)malloc(sizeof(float) * G_PROBE_N);
    if (g_ckt == NULL || g_tab == NULL || g_probe == NULL) {
        return -1;
    }
    ag_amp_probe_pluck(g_probe, G_PROBE_N, RATE);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* curve                                                                     */
/* ------------------------------------------------------------------------ */

/* Two triodes to a bottle, so the fourth stage is V2b.  Only a label. */
static const char *valve_name(int s)
{
    static const char *const n[AG_AMP_STAGES] = { "V1a", "V1b", "V2a", "V2b" };
    return (s >= 0 && s < AG_AMP_STAGES) ? n[s] : "V?";
}

static void mode_curve(void)
{
    ag_amp_cfg_t   cfg;
    ag_tube_spec_t sp;
    ag_tube_t      tb;
    int            s, i, ns;
    FILE          *csv = fopen("build/listen/tube_curve.csv", "w");

    printf("Baked curves.  Volts at the stage's source in, volts of plate swing\n"
           "out, operating point removed.  The axis is fitted to the curve: in\n"
           "one dimension the range is a property of the curve and not of the\n"
           "signal, so there is no fixed point to chase.\n\n");
    print_model();
    printf("\n");
    /* However many stages this model has, which is the whole point of printing
     * them: a cold clipper is a valve whose numbers look wrong until they are
     * read beside the two hot stages in front of it. */
    model_cfg(&cfg, RATE);
    ns = cfg.n_stages;
    if (csv != NULL) {
        fprintf(csv, "v");
        for (s = 0; s < ns; s++) {
            fprintf(csv, ",%s", valve_name(s));
        }
        fprintf(csv, "\n");
    }

    for (s = 0; s < ns; s++) {
        float fz = 0.0f, db = 0.0f, gain, tail;

        ag_amp_spec(g_model, s, &sp);
        ag_tube_init(&tb);
        if (ag_tube_bake(&tb, g_ckt, &sp, g_tab, g_tab + AG_AMP_TAB_N,
                         g_tab + 2 * AG_AMP_TAB_N, AG_AMP_TAB_N, 1.0f, 0.0f,
                         0.01f) != 0) {
            printf("  bake failed\n");
            return;
        }
        ag_tube_shelf(&tb, &sp, &fz, &db);
        gain = (ag_tube_curve(&tb, 0.005f) - ag_tube_curve(&tb, -0.005f)) / 0.01f;

        printf("%s  rplate %.0fk  rcath %.0f%s  rload %.0fk\n", valve_name(s),
               (double)(sp.rplate * 1e-3f), (double)sp.rcath,
               sp.ccath > 0.0f ? "" : " UNBYPASSED", (double)(sp.rload * 1e-3f));
        printf("     operating point: plate %.2f V  cathode %.3f V  grid %.4f V\n",
               (double)tb.vq_plate, (double)tb.vq_cath, (double)tb.vq_grid);
        printf("     small-signal gain %.1f (bypassed) / %.1f (open cathode)\n",
               (double)tb.gain_bypassed, (double)tb.gain_unbypassed);
        printf("     cathode shelf: %.0f Hz, %.2f dB below\n", (double)fz,
               (double)db);
        printf("     axis %.3f .. %.3f V, %d points, %.3f mV a cell\n",
               (double)tb.lo, (double)tb.hi, tb.n, (double)(tb.step * 1e3f));
        printf("     curve at zero %.3e V (must be exact), gain there %.1f\n",
               (double)ag_tube_curve(&tb, 0.0f), (double)gain);
        printf("     slope at the ends: %.2f (bottom) %.2f (top), against %.1f at"
               " the bias point\n",
               (double)((ag_tube_curve(&tb, tb.lo + 0.05f) -
                         ag_tube_curve(&tb, tb.lo)) /
                        0.05f),
               (double)((ag_tube_curve(&tb, tb.hi) -
                         ag_tube_curve(&tb, tb.hi - 0.05f)) /
                        0.05f),
               (double)gain);
        printf("     asymmetry at +-2 V: %.2f up, %.2f down\n",
               (double)ag_tube_curve(&tb, -2.0f), (double)ag_tube_curve(&tb, 2.0f));
        /*
         * The grid current, which is what drives blocking.  It has to be exactly
         * zero at the operating point or the coupling capacitor charges on
         * silence and the amplifier is not silent when nothing is playing.
         */
        {
            static const float pv[] = { -1.0f, 0.0f, 0.3f, 0.6f, 1.0f, 2.0f,
                                        5.0f, 20.0f };
            float              rc = sp.ccouple * sp.rgrid;
            printf("     grid current, uA:");
            for (i = 0; i < (int)(sizeof(pv) / sizeof(pv[0])); i++) {
                const float p = tb.zero_p + pv[i] * tb.step_inv;
                const int   j = p <= 0.0f ? 0
                                : p >= (float)(tb.n - 1) ? tb.n - 1
                                                         : (int)p;
                printf("  %.1fV %.4f", (double)pv[i],
                       (double)(tb.g != NULL ? tb.g[j] * 1e6f : 0.0f));
            }
            printf("\n     blocking recovers in %.1f ms (%.0f nF into %.0f k)\n",
                   (double)(rc * 1000.0f), (double)(sp.ccouple * 1e9f),
                   (double)(sp.rgrid * 1e-3f));
        }
        printf("     ends: %.2f V at the bottom, %.2f V at the top\n",
               (double)ag_tube_curve(&tb, tb.lo), (double)ag_tube_curve(&tb, tb.hi));

        /*
         * What clamping actually costs.
         *
         * The table is extended flat beyond its ends, and the claim is that this
         * is the physical answer because the curve is flat there.  That claim is
         * worth a number, and here it is: how much further the real stage would
         * have gone if it had been asked, at ten and at a hundred times the
         * range.  The second valve of a 2203 is driven by tens of volts, so this
         * is not a corner case - it is where it lives.
         */
        {
            static const float probe[] = { -6.0f,  -3.0f, -1.5f, -1.0f, -0.5f,
                                           0.5f,   1.0f,  2.0f,  4.0f,  8.0f,
                                           16.0f,  32.0f, 64.0f, 128.0f };
            ag_ckt_t          *k2 = (ag_ckt_t *)malloc(sizeof(ag_ckt_t));
            float             *t2 = (float *)malloc(sizeof(float) * 8192);
            ag_tube_t          wide;
            if (k2 != NULL && t2 != NULL) {
                ag_tube_init(&wide);
                if (ag_tube_bake(&wide, k2, &sp, t2, NULL, NULL, 8192, -40.0f, 4000.0f,
                                 0.0f) ==
                    0) {
                    /*
                     * The two extremes as found, not as read off the ends of the
                     * axis.  On an unbypassed stage the top of this axis is four
                     * thousand volts on a grid whose cathode is free to follow
                     * it, Newton gives up there, and what comes back looks like
                     * cut-off - which reported a swing of -0.1 V and a column of
                     * nan.  ag_tube.c's fit_range had the same bug for the same
                     * reason; see the note there.
                     */
                    float sat = t2[0], cut = t2[0], swing;
                    int   w;
                    for (w = 1; w < 8192; w++) {
                        if (t2[w] < sat) {
                            sat = t2[w];
                        }
                        if (t2[w] > cut) {
                            cut = t2[w];
                        }
                    }
                    swing = cut - sat;
                    printf("     saturates at %.2f V (in) and %.2f V (out); the"
                           " whole swing is %.1f V\n",
                           (double)sat, (double)cut, (double)swing);
                    printf("        v in      out    left to go   as dB of swing\n");
                    for (i = 0; i < (int)(sizeof(probe) / sizeof(probe[0])); i++) {
                        const float y = ag_tube_curve(&wide, probe[i]);
                        const float left = y - sat;
                        printf("     %8.2f %8.2f %10.3f      %8.1f\n",
                               (double)probe[i], (double)y, (double)left,
                               (double)(20.0f *
                                        log10f(fabsf(left) / swing + 1e-12f)));
                    }
                    tail = 0.0f;
                    (void)tail;
                }
            }
            free(k2);
            free(t2);
        }
        printf("\n");

        if (csv != NULL) {
            /* Every stage on the same axis, one column each, so they can be
             * plotted together: one block of rows a stage, the value in that
             * stage's column and the rest of the row empty. */
            for (i = 0; i < 1200; i++) {
                const float v = -6.0f + 30.0f * (float)i / 1199.0f;
                int         c;
                if (s == 0) {
                    fprintf(csv, "%.5f", (double)v);
                }
                for (c = 0; c < ns; c++) {
                    if (c == s) {
                        fprintf(csv, ",%.5f", (double)ag_tube_curve(&tb, v));
                    } else {
                        fprintf(csv, ",");
                    }
                }
                fprintf(csv, "\n");
            }
        }
    }
    if (csv != NULL) {
        fclose(csv);
        printf("  wrote build/listen/tube_curve.csv\n");
    }
}

/* ------------------------------------------------------------------------ */
/* resp                                                                      */
/* ------------------------------------------------------------------------ */

static void mode_resp(void)
{
    static const float f[] = { 20.0f,   40.0f,   82.0f,   110.0f,  150.0f,
                               220.0f,  330.0f,  500.0f,  700.0f,  1000.0f,
                               2000.0f, 3000.0f, 5000.0f, 8000.0f, 10000.0f };
    ag_amp_cfg_t       cfg;
    ag_amp_t          *a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    int                i;

    if (a == NULL) {
        return;
    }
    model_cfg(&cfg, RATE);
    if (ag_amp_build(a, g_ckt, &cfg, g_tab, 0, g_probe, G_PROBE_N) != 0) {
        printf("  build failed\n");
        free(a);
        return;
    }
    printf("The three filters, at %.0f Hz base rate and %dx oversampling.\n\n",
           (double)cfg.fs, cfg.os);
    printf("  F1 = input coupling %.2f Hz + V1a cathode shelf %.0f Hz %.2f dB"
           " + top cut %.0f Hz (asked %.0f)\n",
           (double)a->couple_hz[0], (double)a->shelf_hz[0], (double)a->shelf_db[0],
           (double)a->top_hz_used, (double)cfg.top_hz);
    printf("  F2 = V1b coupling %.1f Hz + V1b cathode shelf %.0f Hz %.2f dB"
           " + mid %+.1f dB at %.0f Hz Q %.2f\n",
           (double)a->couple_hz[1], (double)a->shelf_hz[1], (double)a->shelf_db[1],
           (double)cfg.mid_db, (double)a->mid_hz_used, (double)cfg.mid_q);
    /*
     * Every stage's coupling corner, not the first two.  The rule these are chosen
     * by is that none of them may sit above the lowest note on the instrument -
     * 82 Hz - because above that a bass cut stops being a bass cut and starts
     * removing fundamentals while passing their harmonics.  A four-valve chain has
     * four of these and the two-stage line above shows two.
     */
    {
        int st;
        printf("  coupling corners:");
        for (st = 0; st < a->n; st++) {
            printf(" %.1f", (double)a->couple_hz[st]);
        }
        printf(" Hz\n");
        for (st = 0; st < a->n; st++) {
            if (a->couple_hz[st] > 82.0f) {
                printf("    stage %d is at %.0f Hz, above the 82 Hz open E - it"
                       " will read as asymmetry\n", st + 1,
                       (double)a->couple_hz[st]);
            }
        }
    }
    printf("  F3 = output coupling %.2f Hz\n", (double)a->load_hz);
    printf("  latency %.2f samples (%.2f ms); the cabinet convolution after this"
           " adds 256 (11.6 ms)\n\n",
           (double)a->latency_samples,
           (double)(1000.0f * a->latency_samples / cfg.fs));

    printf("     Hz");
    for (i = 0; i < a->n; i++) {
        printf("    pre%d", i);
    }
    printf("     out     sum\n");
    for (i = 0; i < (int)(sizeof(f) / sizeof(f[0])); i++) {
        const float fos = cfg.fs * (float)cfg.os;
        float       sum = 0.0f;
        int         k;
        printf("  %6.0f", (double)f[i]);
        for (k = 0; k < a->n; k++) {
            /* The first block is outside the oversampled region; the rest are
             * inside it and were designed at that rate. */
            const float d =
                ag_biq_chain_mag_db(&a->pre[k], f[i], k == 0 ? cfg.fs : fos);
            sum += d;
            printf("  %+6.2f", (double)d);
        }
        {
            const float d = ag_biq_chain_mag_db(&a->f_out, f[i], cfg.fs);
            sum += d;
            printf("  %+6.2f  %+6.2f\n", (double)d, (double)sum);
        }
    }

    /*
     * And the oversampling filters, which are the ones nobody looks at until an
     * amplifier sounds dull and the reason turns out to be a decimator.
     */
    {
        ag_os2_t     os;
        static float buf[8192];
        int          j;
        printf("\n  Halfband up and down together, %d taps:\n", 2 * AG_OS_M + 1);
        ag_os2_init(&os, AG_OS_M);
        for (i = 0; i < (int)(sizeof(f) / sizeof(f[0])); i++) {
            double re = 0.0, im = 0.0, amp;
            ag_os2_reset(&os);
            for (j = 0; j < 8192; j++) {
                float u[2];
                ag_os2_up(&os, (float)sin(2.0 * PI * (double)f[i] * (double)j /
                                          (double)RATE),
                          u);
                buf[j] = ag_os2_down(&os, u[0], u[1]);
            }
            for (j = 2048; j < 8192; j++) {
                const double w = 2.0 * PI * (double)f[i] * (double)j / (double)RATE;
                re += (double)buf[j] * cos(w);
                im -= (double)buf[j] * sin(w);
            }
            amp = 2.0 * sqrt(re * re + im * im) / 6144.0;
            printf("  %6.0f  %+6.2f dB\n", (double)f[i],
                   20.0 * log10(amp > 1e-12 ? amp : 1e-12));
        }
    }
    free(a);
}

/* ------------------------------------------------------------------------ */
/* alias                                                                     */
/* ------------------------------------------------------------------------ */

#define AL_N   4096
#define AL_BIN 197 /* every folded harmonic lands 41 bins off the grid */

/*
 * Non-harmonic energy, optionally only below `bmax`.
 *
 * The whole-band figure has a floor that is not the nonlinearity: the halfband
 * decimator's transition runs from about 9.2 to 12.8 kHz, so a harmonic landing
 * in there is only partly removed and folds back just under Nyquist.  For a
 * clipped 1060 Hz tone that is the eleventh, at about -21 dB with -16 dB of
 * filtering, which is -37 dB - and -37.8 dB is what the whole-band column
 * measures however much oversampling is thrown at it.
 *
 * Above 5 kHz a guitar cabinet is thirty decibels down, so that floor is not
 * what anyone hears.  Below it is.  Two columns, therefore, and the second is
 * the one to read.
 */
static double nonharmonic_db(const float *x, int n, int bin, int bmax)
{
    double tot = 0.0, junk = 0.0;
    int    b, i;
    if (bmax <= 0 || bmax > n / 2) {
        bmax = n / 2;
    }
    for (b = 1; b <= bmax; b++) {
        double re = 0.0, im = 0.0, p;
        int    d;
        for (i = 0; i < n; i++) {
            const double w = 0.5 - 0.5 * cos(2.0 * PI * (double)i / (double)n);
            const double a = 2.0 * PI * (double)b * (double)i / (double)n;
            re += (double)x[i] * w * cos(a);
            im -= (double)x[i] * w * sin(a);
        }
        p = re * re + im * im;
        tot += p;
        d = b % bin;
        if (d > bin / 2) {
            d = bin - d;
        }
        if (d > 4) {
            junk += p;
        }
    }
    if (tot < 1e-300 || junk < 1e-300) {
        return -300.0;
    }
    return 10.0 * log10(junk / tot);
}

static void mode_alias(void)
{
    static const int   oss[] = { 1, 1, 2, 2, 4, 4, 8, 8 };
    static const int   ads[] = { 0, 1, 0, 1, 0, 1, 0, 1 };
    ag_amp_cfg_t       cfg;
    ag_amp_t          *a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    float             *buf = (float *)malloc(sizeof(float) * AL_N);
    int                c, i;

    if (a == NULL || buf == NULL) {
        free(a);
        free(buf);
        return;
    }
    model_cfg(&cfg, RATE);
    if (ag_amp_build(a, g_ckt, &cfg, g_tab, 0, g_probe, G_PROBE_N) != 0) {
        printf("  build failed\n");
        free(a);
        free(buf);
        return;
    }
    printf("Non-harmonic products, %.1f Hz tone at %.0f Hz, drive %.2f.\n"
           "Bin %d of %d, chosen so folded harmonics land 41 bins off the grid -\n"
           "an aliasing measurement taken at the wrong frequency reports that the\n"
           "aliasing is not there.\n\n",
           (double)(AL_BIN * RATE / AL_N), (double)RATE, (double)cfg.drive, AL_BIN,
           AL_N);
    /*
     * The floor of the metric, before any of it is read.  A pure tone through
     * nothing at all: whatever this reports is window leakage and the four-bin
     * exclusion, not aliasing, and no row below can mean less than it.
     */
    {
        for (i = 0; i < AL_N; i++) {
            buf[i] = (float)(0.5 * sin(2.0 * PI * (double)AL_BIN * (double)i /
                                       (double)AL_N));
        }
        printf("  metric floor, the bare tone: %+.1f dB all bands, %+.1f dB"
               " under 5 kHz\n\n",
               nonharmonic_db(buf, AL_N, AL_BIN, 0),
               nonharmonic_db(buf, AL_N, AL_BIN,
                              (int)(5000.0 * (double)AL_N / (double)RATE)));
    }
    printf("  mode        all bands   under 5 kHz   clamped of         ns/sample\n");

    for (c = 0; c < (int)(sizeof(oss) / sizeof(oss[0])); c++) {
        clock_t  t0, t1;
        double   ns, junk, low;
        int      n;
        uint32_t cl, tot;

        cfg.os = oss[c];
        cfg.adaa = ads[c];
        if (ag_amp_set_voicing(a, &cfg) != 0) {
            continue;
        }
        /*
         * Long enough for the 1.6 Hz input coupling to stop moving.  Eight
         * thousand samples is three and a half of its time constants, which
         * leaves a couple of percent of drift - and a slow drift smears the whole
         * spectrum, which put a floor of -38 dB under this table and made 4x look
         * no better than 2x.  A hundred and ten thousand is fifty time
         * constants.  A measurement of a spectrum has to be taken on a signal
         * that is actually periodic in the window.
         */
        for (i = 0; i < 110000; i++) {
            (void)ag_amp_tick(
                a, (float)(0.5 * sin(2.0 * PI * (double)AL_BIN * (double)i /
                                     (double)AL_N)));
        }
        for (i = 0; i < AG_AMP_STAGES; i++) {
            a->tube[i].clamped = 0;
            a->tube[i].samples = 0;
        }
        for (i = 0; i < AL_N; i++) {
            buf[i] = ag_amp_tick(
                a, (float)(0.5 * sin(2.0 * PI * (double)AL_BIN * (double)i /
                                     (double)AL_N)));
        }
        junk = nonharmonic_db(buf, AL_N, AL_BIN, 0);
        low = nonharmonic_db(buf, AL_N, AL_BIN,
                             (int)(5000.0 * (double)AL_N / (double)RATE));
        cl = ag_amp_clamped(a);
        tot = a->tube[0].samples + a->tube[1].samples;

        /* Cost.  Host nanoseconds say nothing about the chip, but the ratio
         * between two rows of this table does, because it is the same
         * arithmetic. */
        n = 200000;
        t0 = clock();
        for (i = 0; i < n; i++) {
            (void)ag_amp_tick(
                a, (float)(0.5 * sin(2.0 * PI * (double)AL_BIN * (double)i /
                                     (double)AL_N)));
        }
        t1 = clock();
        ns = 1e9 * (double)(t1 - t0) / (double)CLOCKS_PER_SEC / (double)n;

        printf("  %dx%-9s  %+7.1f dB   %+7.1f dB   %6lu of %6lu   %8.1f\n", cfg.os,
               cfg.adaa ? " + adaa" : "", junk, low, (unsigned long)cl,
               (unsigned long)tot, ns);
    }
    /*
     * And what blocking costs in the same currency.
     *
     * The antialiasing average assumes a fixed piecewise-linear curve, and
     * blocking slides that curve sideways every sample - so the products it
     * makes are not covered by ADAA, only by the oversampling around it.  Whether
     * that is enough is exactly the sort of thing that gets called a modelling
     * choice when it is really a sample rate.
     */
    printf("\n  With the grid charge on the coupling capacitors, at 4x + adaa:\n");
    cfg.os = 4;
    cfg.adaa = 1;
    for (c = 0; c < 2; c++) {
        double junk, low;
        cfg.blocking = c;
        if (ag_amp_set_voicing(a, &cfg) != 0) {
            continue;
        }
        for (i = 0; i < 110000; i++) {
            (void)ag_amp_tick(
                a, (float)(0.5 * sin(2.0 * PI * (double)AL_BIN * (double)i /
                                     (double)AL_N)));
        }
        for (i = 0; i < AL_N; i++) {
            buf[i] = ag_amp_tick(
                a, (float)(0.5 * sin(2.0 * PI * (double)AL_BIN * (double)i /
                                     (double)AL_N)));
        }
        junk = nonharmonic_db(buf, AL_N, AL_BIN, 0);
        low = nonharmonic_db(buf, AL_N, AL_BIN,
                             (int)(5000.0 * (double)AL_N / (double)RATE));
        printf("  blocking %-3s  %+7.1f dB   %+7.1f dB\n", c ? "on" : "off", junk,
               low);
    }

    printf("\n  Instruction counts belong to the guest and -icount, not to this\n"
           "  machine; what this column is for is the ratio between rows.\n");
    free(a);
    free(buf);
}

/* ------------------------------------------------------------------------ */
/* level helpers                                                             */
/* ------------------------------------------------------------------------ */

/* RMS over [a, b). */
static double rms_of(const float *x, int a, int b)
{
    double s = 0.0;
    int    i;
    if (b <= a) {
        return 0.0;
    }
    for (i = a; i < b; i++) {
        s += (double)x[i] * (double)x[i];
    }
    return sqrt(s / (double)(b - a));
}

static double peak_of(const float *x, int n)
{
    double p = 0.0;
    int    i;
    for (i = 0; i < n; i++) {
        const double a = x[i] < 0.0f ? -(double)x[i] : (double)x[i];
        if (a > p) {
            p = a;
        }
    }
    return p;
}

static double db(double v) { return 20.0 * log10(v > 1e-15 ? v : 1e-15); }

/* ------------------------------------------------------------------------ */
/* the cabinet                                                               */
/* ------------------------------------------------------------------------ */

/*
 * Convolve `x` in place with the cabinet.  Returns 0 if it ran.
 *
 * The headroom is not a nicety.  ag_ir is int16 throughout and a real cabinet
 * impulse puts several decibels into the low end, so feeding it a signal that
 * peaks near full scale clips inside the convolution - rake 15 in docs/08, found
 * by accident on a file rendered for another question.  Twelve decibels down
 * going in and the same back out; the count of samples that still would not fit
 * is printed rather than assumed to be zero.
 */
static int run_cab(float *x, uint32_t frames, uint32_t rate)
{
    ag_ir_t *ir;
    int16_t *mono, *st;
    int      ok = -1;

    if (g_cab == 0 && g_cab_path == NULL) {
        return -1;
    }
    ir = (ag_ir_t *)calloc(1, sizeof(ag_ir_t));
    mono = (int16_t *)malloc(sizeof(int16_t) * AG_IR_BLOCK);
    st = (int16_t *)malloc(sizeof(int16_t) * AG_IR_BLOCK * 2);
    if (ir == NULL || mono == NULL || st == NULL || ag_ir_init(ir, rate) != 0) {
        free(ir);
        free(mono);
        free(st);
        return -1;
    }
    /*
     * Fully wet, and this is the whole difference between a cabinet and a hiss.
     *
     * ag_ir_init leaves wet at AG_IR_WET_REVERB, which passes 22% of the dry
     * signal - about -13 dB of it - straight through.  Behind a reverb that is
     * inaudible; behind a cabinet it is fatal, because the dry signal still has
     * all of its top end and the cabinet is twenty decibels down up there, so
     * the leak is *louder* than what the speaker passes.  ag_ir.h says exactly
     * this and I did not read it: what it cost was a render where dry and
     * cabinet were indistinguishable and a constant hiss that looked like a
     * modelling fault.
     *
     * ag_ir_load and the cabinet presets now do this themselves; the call stays
     * because nothing here should depend on that to be safe.
     */
    ag_ir_set_wet(ir, AG_IR_WET_MAX);

    if (g_cab_path != NULL) {
        uint32_t irn = 0, irrate = 0;
        float   *irf = read_wav(g_cab_path, &irn, &irrate);
        int16_t *irs = NULL;
        if (irf != NULL) {
            uint32_t k;
            /* ag_ir resamples to its own rate; it wants int16, so normalise the
             * impulse to fill it rather than losing the quiet tail. */
            double pk = peak_of(irf, (int)irn);
            if (pk < 1e-9) {
                pk = 1.0;
            }
            /*
             * Optionally shortened.  A long impulse is not free: ag_ir carries a
             * partition per 256 taps, each with its own block-float scale, and
             * the tail of a capture is the part with the least signal in it and
             * the most of everything else.  Measured on a one-stage render, which
             * is the quiet case that shows it: the 151 ms Vox impulse costs 2 dB
             * of peak-to-floor, the 500 ms one costs 18.
             */
            {
                /*
                 * 200 ms by default, AG_CAB_MS to override.  A real cabinet is
                 * 20 to 200 ms and anything past that is either room or, in the
                 * case of an impulse taken from a *nonlinear* model, the
                 * model.s own noise floor - by then the delta has decayed into
                 * it.  Convolving with that spreads it across everything.
                 *
                 * Measured on a one-stage render at 3.10 s, peak to floor:
                 *
                 *     no cabinet at all           79.5 dB
                 *     Vox, 151 ms, 13 parts       77.3 dB
                 *     mars cut to 100 ms           73.0 dB
                 *     mars cut to 200 ms           73.1 dB
                 *     mars whole, 500 ms          61.4 dB
                 *
                 * Twelve decibels in the last three hundred milliseconds, and
                 * 100 against 200 is a wash - so it is the tail and not the
                 * partition count.
                 */
                const char    *ms = getenv("AG_CAB_MS");
                const double   want_ms = ms != NULL ? atof(ms) : 200.0;
                const uint32_t want =
                    (uint32_t)(want_ms * (double)irrate / 1000.0);
                if (want > 32u && want < irn) {
                    irn = want;
                }
            }
            irs = (int16_t *)malloc(sizeof(int16_t) * irn);
            if (irs != NULL) {
                for (k = 0; k < irn; k++) {
                    irs[k] = (int16_t)((double)irf[k] / pk * 32000.0);
                }
                if (ag_ir_load(ir, irs, irn, irrate) == 0) {
                    ok = 0;
                    printf("  cabinet: %s, %u taps at %u Hz -> %u at %u Hz,"
                           " %u partitions\n",
                           g_cab_path, irn, irrate, ir->ir_frames, rate,
                           ir->parts);
                }
                /*
                 * What the file itself does, from its own taps, before anything
                 * else is believed about it.  A guitar speaker is dead above
                 * about 6 kHz; if this says otherwise then the file is not a
                 * cabinet, and no amount of listening to the convolution will
                 * make that visible - it will just sound like the amplifier
                 * hisses.
                 */
                {
                    static const double f[5] = { 1000.0, 2000.0, 4000.0, 8000.0,
                                                 12000.0 };
                    double              mag[5];
                    int                 q;
                    for (q = 0; q < 5; q++) {
                        double   sr = 0.0, si = 0.0;
                        uint32_t t;
                        if (f[q] >= 0.5 * (double)irrate) {
                            mag[q] = -1.0;
                            continue;
                        }
                        for (t = 0; t < irn; t++) {
                            const double al =
                                -2.0 * PI * f[q] * (double)t / (double)irrate;
                            sr += (double)irf[t] * cos(al);
                            si += (double)irf[t] * sin(al);
                        }
                        mag[q] = sqrt(sr * sr + si * si);
                    }
                    printf("    its own response, dB relative to 1 kHz:");
                    for (q = 1; q < 5; q++) {
                        if (mag[q] < 0.0) {
                            printf(" %.0fk n/a", f[q] / 1000.0);
                        } else {
                            printf(" %.0fk %+.0f", f[q] / 1000.0,
                                   20.0 * log10((mag[q] + 1e-30) /
                                                (mag[0] + 1e-30)));
                        }
                    }
                    printf("\n");
                    /* -10 dB at 8 kHz, not -20: a bright cabinet really does sit
                     * around -17 there, and a threshold set from an expectation
                     * rather than from a measurement called the one real impulse
                     * in the tree a fake. */
                    if (mag[3] > 0.0 && mag[3] > mag[0] * 0.32) {
                        printf("    ^ this does not look like a cabinet: a 12-inch"
                               " speaker is well down by 8 kHz.\n");
                    }
                }
            }
            free(irs);
            free(irf);
        }
        if (ok != 0) {
            printf("    falling back on the preset\n");
        }
    }
    if (ok != 0 && g_cab != 0 && ag_ir_load_preset(ir, g_cab) == 0) {
        ok = 0;
        printf("  cabinet: ag_ir preset %d - synthetic, see the note in this"
               " file\n",
               g_cab);
    }

    if (ok == 0) {
        /*
         * How far to back the signal off before it becomes int16, measured
         * rather than assumed.
         *
         * It used to be a flat -12 dB, and that is a bug the moment the input is
         * quiet: a one-stage chain peaks 27 dB below full scale, so a quarter of
         * that left about nine of the sixteen bits in use and the convolution's
         * own quantisation came back up with the output.  Measured on that
         * render, the dry file had 79.5 dB of peak to floor and the cabinet
         * version 61.2 - eighteen decibels thrown away by a constant.
         *
         * So: put a delta through the loaded impulse, see what it does, and scale
         * the input so peak times that gain lands just under full scale.  The
         * gain is not guessable - ag_ir normalises the impulse's energy and a
         * cabinet puts several decibels into its low end - which is exactly why
         * the constant existed and exactly why it was wrong.
         */
        float    head;
        uint32_t b, k, over = 0;
        {
            const double pk = peak_of(x, (int)frames);
            double       g;
            int          j;
            for (j = 0; j < (int)AG_IR_BLOCK; j++) {
                mono[j] = (j == 0) ? 16384 : 0;
            }
            ag_ir_process_block(ir, mono, st);
            g = 0.0;
            for (j = 0; j < (int)AG_IR_BLOCK; j++) {
                const double v = st[2 * j] < 0 ? -(double)st[2 * j]
                                               : (double)st[2 * j];
                if (v > g) {
                    g = v;
                }
            }
            g /= 16384.0;
            ag_ir_reset(ir);
            if (g < 1e-6) {
                g = 1.0;
            }
            /*
             * Whichever end binds.  A cabinet that is quieter than unity - this
             * one measures 0.16 - is limited by its input, not its output, and
             * dividing by the gain alone drove the input to five times full
             * scale and cost twenty-seven decibels instead of saving eighteen.
             */
            head = (float)(pk > 1e-9 ? 0.9 / (pk * (g > 1.0 ? g : 1.0)) : 1.0);
            printf("  cabinet: impulse gain %.2f, input scaled %+.1f dB to fill"
                   " int16\n",
                   g, 20.0 * log10((double)head));
        }
        /*
         * One scale for the whole take, and the tempting fix is worse.
         *
         * A note decaying sixty decibels below the peak enters the convolution
         * sixty decibels down, and int16 quantisation noise comes back with it -
         * spectrally flat, so it lands hardest where the music is weakest, and a
         * third-octave comparison normalised to 1 kHz reads that as a tilt.
         * Measured against the identical convolution in float: 1.18 dB rms on a
         * guitar render, up to 2 dB of apparent bass deficit.
         *
         * That is noise and not bias, and the two tests that say so are worth
         * keeping: a **delta** through this engine comes back at 0.00 dB in every
         * band, and so does **pink noise**, which has no decays for the staging to
         * get wrong.  The filter is exact; the level going in is what is not.
         *
         * `tube_live` fixes the audible version of this with a scale that follows
         * the signal, and putting the same thing here made the measurement three
         * times worse - 1.37 dB rms on pink noise, +6.1 dB at 6.3 kHz, where the
         * fixed scale had been exact.  The reason is the convolution's own memory:
         * raising the scale rescales only the *new* input while the impulse is
         * still summing the tail of blocks that entered at the old one, and a
         * cabinet keeps its low end in that tail.  Live it is worth it because
         * quantisation buzz is more audible than a decibel of tail; in an analysis
         * path it is not, so this one stays fixed and the exact analysis is done in
         * float - see convolve_f, which is what `fit` and `match` actually use.
         */
        for (b = 0; b + AG_IR_BLOCK <= frames; b += AG_IR_BLOCK) {
            for (k = 0; k < AG_IR_BLOCK; k++) {
                float v = x[b + k] * head * 32767.0f;
                if (v > 32767.0f) {
                    v = 32767.0f;
                    over++;
                } else if (v < -32768.0f) {
                    v = -32768.0f;
                    over++;
                }
                mono[k] = (int16_t)v;
            }
            ag_ir_process_block(ir, mono, st);
            for (k = 0; k < AG_IR_BLOCK; k++) {
                x[b + k] = (float)st[2 * k] / (32768.0f * head);
            }
        }
        for (; b < frames; b++) {
            x[b] = 0.0f;
        }
        if (over) {
            printf("  cabinet: %u samples did not fit int16 going in\n", over);
        }
    }
    ag_ir_free(ir);
    free(ir);
    free(mono);
    free(st);
    return ok;
}

/*
 * What the cabinet actually does to the spectrum, measured by putting an impulse
 * through the loaded convolution rather than by reading the file.
 *
 * That distinction earned itself: whatever the impulse looks like on disk, what
 * matters is what it is after ag_ir has resampled it to the working rate,
 * normalised its energy and picked its shifts.  A cabinet that fails to remove
 * the top octave leaves a hiss the amplifier put there.
 */
static void cab_response(uint32_t rate)
{
    const int n = 8192;
    float    *x = (float *)calloc((size_t)n, sizeof(float));
    if (x == NULL) {
        return;
    }
    x[0] = 0.5f;
    if (run_cab(x, (uint32_t)n, rate) == 0) {
        static const float f[] = { 80.0f,   160.0f,  320.0f,  640.0f, 1250.0f,
                                   2500.0f, 4000.0f, 5000.0f, 6300.0f, 8000.0f,
                                   10000.0f };
        int                i;
        printf("  cabinet response, dB relative to 1250 Hz:\n   ");
        {
            double ref = 0.0;
            double v[11];
            for (i = 0; i < 11; i++) {
                double re = 0.0, im = 0.0;
                int    k;
                for (k = 0; k < n; k++) {
                    const double t =
                        2.0 * PI * (double)f[i] * (double)k / (double)rate;
                    re += (double)x[k] * cos(t);
                    im -= (double)x[k] * sin(t);
                }
                v[i] = sqrt(re * re + im * im);
            }
            ref = v[4] > 1e-12 ? v[4] : 1e-12;
            for (i = 0; i < 11; i++) {
                printf(" %.0f:%+.1f", (double)f[i], 20.0 * log10(v[i] / ref));
            }
            printf("\n");
        }
    }
    free(x);
}

/* ------------------------------------------------------------------------ */
/* spec / match - the spectrum against a reference                           */
/* ------------------------------------------------------------------------ */

/*
 * Third-octave band energies, which is the shape of a sound in the terms an ear
 * uses.
 *
 * Measured with a filter bank rather than a transform, for a reason worth
 * stating: what is being compared is two renders of a *distorting* amplifier, so
 * the interesting quantity is how much energy ends up in each band over the whole
 * performance, not the fine structure of any one moment.  A bank of constant-Q
 * band passes gives exactly that, and the same biquads the amplifier is built
 * from are the analyser, so there is one bilinear transform in the tree.
 */
#define SPEC_N 22
static const float k_spec_f[SPEC_N] = {
    50.0f,  63.0f,  80.0f,   100.0f,  125.0f,  160.0f,  200.0f,  250.0f,
    315.0f, 400.0f, 500.0f,  630.0f,  800.0f,  1000.0f, 1250.0f, 1600.0f,
    2000.0f, 2500.0f, 3150.0f, 4000.0f, 5000.0f, 6300.0f
};

/* Band energies in dB, normalised so the 1 kHz band reads 0. */
static void spectrum_of(const float *x, int n, float rate, double *out)
{
    int i;
    for (i = 0; i < SPEC_N; i++) {
        ag_biq_t a, b;
        double   s = 0.0;
        int      k;
        if (k_spec_f[i] >= rate * 0.45f) {
            out[i] = -300.0;
            continue;
        }
        /* Two in cascade, so the skirts fall fast enough that a loud neighbour
         * does not read as energy in this band. */
        (void)ag_biq_bandpass(&a, rate, k_spec_f[i], 0.333f);
        (void)ag_biq_bandpass(&b, rate, k_spec_f[i], 0.333f);
        for (k = 0; k < n; k++) {
            const float v = ag_biq_tick(&b, ag_biq_tick(&a, x[k]));
            s += (double)v * (double)v;
        }
        out[i] = 10.0 * log10(s / (double)n + 1e-30);
    }
    {
        const double ref = out[13]; /* 1 kHz */
        for (i = 0; i < SPEC_N; i++) {
            if (out[i] > -299.0) {
                out[i] -= ref;
            }
        }
    }
}

static void print_spec(const char *label, const double *s)
{
    int i;
    printf("  %-22s", label);
    for (i = 0; i < SPEC_N; i++) {
        printf(" %+5.1f", s[i] > -299.0 ? s[i] : 0.0);
    }
    printf("\n");
}

static void print_spec_header(void)
{
    int i;
    printf("  %-22s", "Hz");
    for (i = 0; i < SPEC_N; i++) {
        if (k_spec_f[i] >= 1000.0f) {
            printf(" %4.1fk", (double)k_spec_f[i] / 1000.0);
        } else {
            printf(" %5.0f", (double)k_spec_f[i]);
        }
    }
    printf("\n");
}

static void mode_spec(int argc, char **argv)
{
    int i;
    if (argc < 3) {
        printf("  usage: tube_render spec a.wav [b.wav ...]\n");
        return;
    }
    print_spec_header();
    for (i = 2; i < argc; i++) {
        uint32_t frames = 0, rate = 0;
        float   *x = read_wav(argv[i], &frames, &rate);
        double   s[SPEC_N];
        if (x == NULL) {
            continue;
        }
        spectrum_of(x, (int)frames, (float)rate, s);
        print_spec(argv[i], s);
        free(x);
    }
}

/* ------------------------------------------------------------------------ */
/* fit - bring the spectrum to a reference by voicing what reaches the valves */
/* ------------------------------------------------------------------------ */

/*
 * Fit the voicing bank so that the render's third-octave spectrum matches a
 * reference render of the same take.
 *
 * Why it has to be iterative: the bank sits in front of the valves, so a decibel
 * added at 3 kHz does not arrive as a decibel at 3 kHz - it arrives as more
 * clipping, whose harmonics land everywhere.  The mapping is monotone though, so
 * moving each band by a fraction of its own error converges, and doing it any
 * other way would be equalising the output, which cannot help: what is missing
 * from this chain is upper-midrange that was never generated, and no filter after
 * the fact can make harmonics that are not there.
 *
 * Bands below 200 Hz are left out of the error on purpose.  Down there the
 * cabinet decides everything - a Marshall's is +9 dB at 125 Hz and the Vox
 * impulse in the tree has no bump at all - and asking the pre-distortion voicing
 * to correct a loudspeaker would put twelve decibels of bass into the grid of a
 * valve that is already clipping, which is the one thing every high gain
 * amplifier is designed not to do.
 */
/*
 * Declared here, defined with the matcher further down: the fit and its probes sit
 * above the code that plays captures, and moving either would put a thousand lines
 * between a measurement and the thing it measures.
 */
static double chain_compression(const ag_amp_cfg_t *cfg, const float *in,
                                uint32_t n, uint32_t rate);
static double goertzel_db(const float *x, uint32_t n, double f, double rate);
static float *capture_render(const char *path, const float *in48, uint32_t n48,
                             float gain, int verbose);
static float *convolve_f(const float *x, uint32_t n, const float *h,
                         uint32_t hn);
static double two_tone_imd(const ag_amp_cfg_t *cfg, uint32_t rate, float level,
                           const float *cab, uint32_t cn, double *tone_out);
static void   chain_character(const ag_amp_cfg_t *cfg, const float *in,
                              uint32_t n, uint32_t rate, double *comp,
                              double *hash);

/*
 * How much of a signal sits above 2 kHz, in dB relative to all of it.
 *
 * The other half of what "the character of the overdrive" means, and the half a
 * third-octave fit cannot see: a guitar DI has almost nothing up there, so
 * whatever is above 2 kHz was *made* by the distortion.  Two chains can have the
 * same third-octave spectrum with one of them generating that energy and the other
 * having it lifted into place by a filter, and they do not sound alike - the first
 * is grain that follows the note, the second is a treble control.
 *
 * One pole, because the point is a ratio and not a filter design: the DI's own
 * harmonics stop around 1.3 kHz.
 */
static double above_2k_db(const float *x, uint32_t n, uint32_t rate)
{
    const double k = exp(-2.0 * PI * 2000.0 / (double)(rate ? rate : 1u));
    double       lp = 0.0, all = 0.0, high = 0.0;
    uint32_t     i;

    for (i = 0; i < n; i++) {
        const double v = (double)x[i];
        const double h = v - (lp = (1.0 - k) * v + k * lp);
        all += v * v;
        high += h * h;
    }
    return 10.0 * log10(high / (all + 1e-300) + 1e-300);
}

/*
 * The same two tones through the capture, so pass one has a target for the third
 * character number rather than only a reading.
 *
 * Measured at the capture's own rate and by frequency, so nothing is resampled
 * between the amplifier and the number.
 */
static double capture_two_tone_imd(const char *cap, uint32_t crate, float level)
{
    const double   f1 = 146.83, f2 = 196.0;
    const uint32_t edge = (uint32_t)(0.02 * (double)crate);
    const uint32_t body = (uint32_t)(0.30 * (double)crate);
    const uint32_t n = 2u * edge + body;
    float         *x = (float *)malloc(sizeof(float) * n);
    float         *y;
    double         out = -300.0;
    uint32_t       i;

    if (x == NULL) {
        return out;
    }
    for (i = 0; i < n; i++) {
        const double t = (double)i / (double)crate;
        const double env = i < edge ? 0.5 - 0.5 * cos(PI * (double)i /
                                                      (double)edge)
                           : (i > edge + body
                                  ? 0.5 - 0.5 * cos(PI * (double)(n - i) /
                                                    (double)edge)
                                  : 1.0);
        x[i] = (float)(0.5 * (double)level * env *
                       (sin(2.0 * PI * f1 * t) + sin(2.0 * PI * f2 * t)));
    }
    y = capture_render(cap, x, n, 1.0f, 0);
    if (y != NULL) {
        out = goertzel_db(y + edge, body, f2 - f1, (double)crate) -
              goertzel_db(y + edge, body, f2, (double)crate);
    }
    free(x);
    free(y);
    return out;
}

/*
 * PASS ONE: THE FRONT, FITTED AGAINST THE CHARACTER AND NOT THE SPECTRUM
 *
 * What the filters in front of the valves control is how hard the valves are
 * driven and therefore what the distortion *is*; what the bank behind them
 * controls is tone.  Fitting the front against a third-octave spectrum ignores
 * that, and worse, it does not work: scored on the bands it can move it diverged
 * monotonically - 3.66 dB to 6.19 over eight iterations - because raising the
 * midrange in front of a clipper raises the clipper's own output in the same bands
 * and the shape barely moves while the compression grows.
 *
 * So the front is fitted against **compression**: the same take twenty decibels
 * down, and how much of that came back.  One parameter - a lift across the bands
 * the guitar actually carries, 100 Hz to 800 - because that is the drive into the
 * valves, and the sensitivity of it is measured rather than assumed:
 * `tube_render sens` says +6 dB across those four bands is worth about 1.1 dB of
 * compression on the crunch model, so 0.18 dB per dB, which is what the step below
 * uses under relaxation.
 *
 * `hash` - how much of the output the valves *made* above 2 kHz - is reported and
 * not fitted, and the reason is honest rather than principled: the capture's own
 * figure includes the real amplifier's tone stack and loudspeaker, and there is no
 * way to take those out, so the two numbers are not on the same footing.
 * Compression survives that comparison because it is a ratio of two levels through
 * the same filter; a spectral fraction does not.
 */
static void fit_pre_character(ag_amp_cfg_t *cfg, const float *in, uint32_t dn,
                              uint32_t drate, double cap_comp, double cap_hash,
                              double cap_imd, const float *cab, uint32_t cn)
{
    /*
     * The two levers and what they are worth, measured by `tube_render sens` on the
     * crunch model rather than assumed:
     *
     *   a lift across 100-800 Hz   +0.18 dB of compression per dB, and +3.2 dB of
     *                              subsonic intermodulation per dB
     *   a cut at 100 and 200 Hz    -2.3 dB of intermodulation per dB, and -0.08 dB
     *                              of compression per dB
     *
     * Two knobs, two targets, and the matrix is far from diagonal - both levers move
     * both numbers, and the drive lever moves the fart more than it moves the
     * compression it is there for.  So it is solved as a 2x2 and then damped hard:
     * the slopes are one model's, the chain is nonlinear, and a step that overshoots
     * costs an iteration rather than a wrong answer.
     */
    const double j11 = 0.18, j12 = -0.083; /* d comp / d lift, d comp / d cut */
    const double j21 = 3.20, j22 = -2.30;  /* d imd  / d lift, d imd  / d cut */
    const double det = j11 * j22 - j12 * j21;
    double       lift = 0.0, cut = 0.0;
    int          it, b;

    printf("\n  PASS 1 - the front, against what the amplifier's overdrive *is*\n"
           "    the capture: compression %.1f dB, %.1f dB above 2 kHz,"
           " 49 Hz product %.1f dB down\n", cap_comp, cap_hash, cap_imd);

    /*
     * PASS ZERO: THE BASS CUT IN FRONT OF EACH CLIPPING STAGE, AS A COMPONENT
     *
     * Every high gain design cuts bass before each hot stage - small interstage
     * capacitors - and the reason is exactly the number this is fitted against:
     * the subsonic difference product of two low notes.  So before any filter is
     * moved, the coupling capacitors get one bounded, ordinary-looking choice.
     *
     * The list is short and its top end is not absurd: x8 on a 22 nF coupling is
     * 2.7 nF, which is a perfectly normal value in front of a hot stage.  Anything
     * past that is not a bass cut, it is a different amplifier, and if a reasonable
     * cut does not close the gap then it does not - what is left is the output
     * impulse's problem.
     *
     * The first stage is left alone because it measures as doing nothing: its
     * corner is 1.6 Hz, so even x8 is 13 Hz.  And the choice is the *smallest*
     * multiplier that gets within a decibel of the best on offer, because a bigger
     * capacitor change than the measurement asks for is a guess.
     */
    {
        static const float tries[4] = { 1.0f, 2.0f, 4.0f, 8.0f };
        double got[4], tone;
        int    k, pick = 0, st;

        printf("    coupling caps in front of the later stages, against the"
               " 49 Hz product:\n");
        for (k = 0; k < 4; k++) {
            for (st = 1; st < AG_AMP_STAGES; st++) {
                cfg->couple_mul[st] = tries[k];
            }
            got[k] = two_tone_imd(cfg, drate, 0.35f, cab, cn, &tone);
            printf("      x%-4.1f (%.1f nF from 22)   49 Hz %5.1f dB\n",
                   (double)tries[k], 22.0 / (double)tries[k], got[k]);
        }
        /*
         * Cut only if this chain makes *more* of the product than the amplifier
         * does, and then only as far as it takes to reach it.
         *
         * The first version of this rule simply minimised the product, and on the
         * Marshall model it was a disaster: that chain already puts the 49 Hz
         * product 22 dB further down than its own capture does, so there was
         * nothing to fix, and x8 was chosen anyway - which took the bass out of a
         * two-valve front end and dropped its compression from 12.2 dB to 7.0.
         * A cut that nothing asked for is not a reasonable cut.
         */
        if (got[0] <= cap_imd + 1.0) {
            pick = 0;
            printf("      -> x1.0: this chain already makes %.1f dB less of it"
                   " than the amplifier does,\n         so there is nothing here"
                   " to cut\n", got[0] < cap_imd ? cap_imd - got[0] : 0.0);
        } else {
            pick = 3;
            for (k = 0; k < 4; k++) {
                if (got[k] <= cap_imd + 1.0) {
                    pick = k; /* the smallest that reaches the amplifier */
                    break;
                }
            }
            printf("      -> x%.1f, %s\n", (double)tries[pick],
                   got[pick] <= cap_imd + 1.0
                       ? "which is where the amplifier's own figure is"
                       : "the most a reasonable capacitor buys - the rest is not"
                         " a bass cut's to fix");
        }
        for (st = 1; st < AG_AMP_STAGES; st++) {
            cfg->couple_mul[st] = tries[pick];
        }
    }

    printf("    lift 100-800   cut 100-200   compression    above 2 kHz"
           "   49 Hz product\n");
    for (it = 0; it < 6; it++) {
        double comp, hash, imd, tone, ec, ei, dl, dc;
        chain_character(cfg, in, dn, drate, &comp, &hash);
        imd = two_tone_imd(cfg, drate, 0.35f, cab, cn, &tone);
        printf("    %+6.2f dB      %+6.2f dB     %5.1f dB       %5.1f dB"
               "      %5.1f dB\n", lift, cut, comp, hash, imd);
        ec = cap_comp - comp;
        ei = cap_imd - imd;
        if (ec < 0.25 && ec > -0.25 && ei < 1.0 && ei > -1.0) {
            break;
        }
        dl = 0.35 * (ec * j22 - j12 * ei) / det;
        dc = 0.35 * (j11 * ei - j21 * ec) / det;
        if (dl > 1.5) { dl = 1.5; }
        if (dl < -1.5) { dl = -1.5; }
        if (dc > 1.5) { dc = 1.5; }
        if (dc < -1.5) { dc = -1.5; }
        /* Both bounded to six decibels total: past that it is a different
         * amplifier, not a match. */
        if (lift + dl > 6.0) { dl = 6.0 - lift; }
        if (lift + dl < -6.0) { dl = -6.0 - lift; }
        if (cut + dc > 6.0) { dc = 6.0 - cut; }
        if (cut + dc < -6.0) { dc = -6.0 - cut; }
        if (dl > -0.01 && dl < 0.01 && dc > -0.01 && dc < 0.01) {
            break;
        }
        lift += dl;
        cut += dc;
        for (b = 0; b < AG_AMP_VOICE_N; b++) {
            float d = 0.0f;
            if (cfg->voice[0][b].hz >= 100.0f && cfg->voice[0][b].hz <= 800.0f) {
                d += (float)dl;
            }
            if (cfg->voice[0][b].hz <= 200.0f) {
                d -= (float)dc; /* `cut` is positive when bass comes out */
            }
            cfg->voice[0][b].db += d;
            if (cfg->voice[0][b].db > 18.0f) { cfg->voice[0][b].db = 18.0f; }
            if (cfg->voice[0][b].db < -18.0f) { cfg->voice[0][b].db = -18.0f; }
        }
    }
    printf("    what the front ended up at:");
    for (b = 0; b < AG_AMP_VOICE_N; b++) {
        if (cfg->voice[0][b].hz <= 1600.0f) {
            printf("  %.0f Hz %+.1f", (double)cfg->voice[0][b].hz,
                   (double)cfg->voice[0][b].db);
        }
    }
    printf("\n    (`tube_render sens` is where those slopes come from, and what"
           " every other band\n     in front does instead; `harm` is the same"
           " thing frequency by frequency)\n");
}

/*
 * The fit itself, on signals rather than on filenames.
 *
 * It was inside mode_fit, which read two wavs and did everything.  The tone
 * matcher has a reference that was never a file - it played the capture a moment
 * ago, in memory - and running it through a temporary wav to reach this code
 * would put a 16-bit quantisation between the amplifier and the thing being
 * matched to it.  So the fit takes arrays, and mode_fit is what reads files.
 *
 * `best_out` receives the configuration it settled on, for a caller that wants to
 * go on and measure it.
 */
static void fit_banks(const float *ref, uint32_t rn, const float *in,
                      uint32_t dn, uint32_t rate, int iters, float drive,
                      float q, float cc2, const char *ref_label,
                      double cap_comp, double cap_imd, const float *cab_for_imd,
                      uint32_t cab_for_imd_n, ag_amp_cfg_t *best_out)
{
    const uint32_t drate = rate;
    ag_amp_cfg_t   cfg, best_cfg;
    double         best_rms = 1e30;
    int            best_it = 0;
    ag_amp_t      *a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    float         *out = (float *)malloc(sizeof(float) * (dn ? dn : 1u));
    double         want[SPEC_N], got[SPEC_N];
    uint32_t       sn = dn; /* how much of the render the spectrum is taken over */
    int            it, i, b;

    if (a == NULL || out == NULL) {
        free(a);
        free(out);
        return;
    }
    spectrum_of(ref, (int)rn, (float)rate, want);

    model_cfg(&cfg, (float)drate);
    cfg.drive = drive;
    cfg.ccouple2 = cc2;
    /*
     * Wider bands than the octave they sit on, so their skirts overlap.  At
     * Q 1.4 the fit left a two-and-a-half decibel trough at 250-315 Hz, in the
     * gap between the 200 and 400 Hz bands - a bank can only correct what one of
     * its filters can reach.
     */
    for (b = 0; b < AG_AMP_VOICE_N; b++) {
        cfg.voice[0][b].q = q;
        cfg.tone[b].q = q;
    }
    /*
     * THE PRE BANK FIRST, ON ITS OWN, AND THE POST BANK AFTER IT
     *
     * The two used to move together, each band assigned to one bank or the other
     * by frequency, and that is wrong for a reason that has nothing to do with
     * which frequency goes where: **the filters in front of the valves decide the
     * character of the distortion**, and the ones behind decide the tone.  Moving
     * them at once lets the post bank absorb the very error that should have been
     * driving the pre bank, so the spectrum converges while the grain of the
     * overdrive stays wrong - the same class of mistake as fitting a spectrum and
     * never measuring compression.
     *
     * So: the post bank starts at zero and stays there while the pre bank is
     * fitted against the whole error, especially in the bands that drive the hot
     * stages.  Then the pre bank is frozen and the post bank finishes the tone.
     * Any band the pre bank could not reach is still there for the second pass.
     *
     * The frequency split stays inside pass one, and it is still physics: above
     * 2 kHz a hard limiter blanks a small signal in the presence of a large one,
     * so pre-emphasis cannot deliver a top end the guitar does not have (fitted
     * that way, both top bands ran into their 18 dB limit and were 5 dB short),
     * and below 200 Hz more bass into a clipping grid is less midrange out.
     */
    for (b = 0; b < AG_AMP_VOICE_N; b++) {
        cfg.tone[b].db = 0.0f;
    }
    best_cfg = cfg;

    printf("Fitting %s in two passes: the filters in front of the valves first,\n"
           "because they decide the character of the overdrive, then the bank\n"
           "behind them for what is left of the tone.  Third-octave bands from\n"
           "63 Hz to 6.3 kHz relative to 1 kHz, %d iterations each at drive %.2f,\n"
           "Q %.2f.\n\n",
           ref_label, iters, (double)drive, (double)q);
    print_model();
    printf("\n");
    print_spec_header();
    print_spec("reference", want);

    /*
     * PASS ONE IS OFF BY DEFAULT NOW, AND `ladder` IS WHY
     *
     * What this pass did was move the *one* bank in front of the whole chain, one
     * broadband lift, against the capture's compression.  There is no longer one
     * bank in front of the whole chain: there is one in front of each valve, and
     * they are fitted by `ladder` at the level where each valve starts working,
     * which is the thing this pass was a single-parameter stand-in for.  Running
     * both means two fitters moving the same filters against different objectives,
     * and the second one to run wins by accident rather than on merit.
     *
     * So `match` is now only what its second pass always was: the bank behind the
     * valves, and then the impulse.  Set AG_FIT_PRE=1 to get the old behaviour -
     * it is the right thing for a model that has never been through the ladder,
     * because a broadband lift chosen against compression beats nothing at all.
     */
    if (cap_comp > 0.0 && getenv("AG_FIT_PRE") != NULL) {
        fit_pre_character(&cfg, in, dn, drate, cap_comp,
                          above_2k_db(ref, rn, rate), cap_imd, cab_for_imd,
                          cab_for_imd_n);
        best_cfg = cfg;
    } else if (cap_comp > 0.0) {
        printf("\n  (the front is the ladder's, so this run only fits behind the"
               " valves - AG_FIT_PRE=1 for the old single-bank pass)\n");
    } else {
        printf("\n  (no capture to compare dynamics against, so the front is"
               " left as it ships)\n");
    }

    printf("\n  PASS 2 - behind the valves, with the front frozen\n");
    for (it = 0; it <= iters; it++) {
        double err = 0.0, worst = 0.0;
        int    nb = 0;

        if (ag_amp_build(a, g_ckt, &cfg, g_tab, 0, in,
                         (int)(dn < drate * 10u ? dn : drate * 10u)) != 0) {
            printf("  build failed\n");
            break;
        }
        for (i = 0; i < (int)dn; i++) {
            out[i] = ag_amp_tick(a, in[i]);
        }
        /*
         * A cabinet if there is one to use, and none is a legitimate answer
         * rather than a failure: a head-only capture has no loudspeaker in it, so
         * putting one on this side alone would make the fit correct a speaker
         * with the voicing.  `match` decides which case this is and clears both
         * cabinet globals when it is the second one.
         */
        if (g_cab != 0 || g_cab_path != NULL) {
            if (run_cab(out, dn, drate) != 0) {
                printf("  no cabinet; point AG_CAB_IR at one\n");
                break;
            }
            sn = dn - dn % AG_IR_BLOCK;
        }
        spectrum_of(out, (int)sn, (float)drate, got);

        for (i = 0; i < SPEC_N; i++) {
            double d;
            if (k_spec_f[i] < 63.0f || got[i] < -299.0 || want[i] < -299.0) {
                continue;
            }
            d = want[i] - got[i];
            err += d * d;
            if (d > worst || -d > worst) {
                worst = d > 0.0 ? d : -d;
            }
            nb++;
        }
        /*
         * Iteration zero of pass one is the model's *shipping* pre bank with the
         * post bank taken out, not a flat chain - `model_cfg` fills the banks in.
         * The label used to say "flat", which made a pass that could not improve
         * on the stored answer look like a chain that needed no voicing at all.
         */
        printf("  %-20s", it == 0 ? "after pass 1" : "");
        if (it > 0) {
            printf("\r  iteration %-11d", it);
        }
        for (i = 0; i < SPEC_N; i++) {
            printf(" %+5.1f", got[i] > -299.0 ? got[i] : 0.0);
        }
        {
            const double rms = sqrt(err / (nb ? nb : 1));
            printf("   rms %.2f worst %.1f dB\n", rms, worst);
            /*
             * Keep the best rather than the last.  The bands below 200 Hz are not
             * in the error, so the low end keeps drifting as the 200 Hz band is
             * pushed, and past the fifth iteration the fit gets slowly worse
             * while still looking like it is converging.
             */
            if (rms < best_rms) {
                best_rms = rms;
                best_cfg = cfg;
                best_it = it;
            }
        }
        if (it == iters) {
            break;
        }

        /*
         * Move each band by a fraction of the error at its own centre.  Under
         * relaxation because the bands overlap and the nonlinearity spreads what
         * each one does; at 100% this oscillated.
         *
         * The split between the two banks is where the physics is.  Below 2 kHz
         * the guitar has enough energy to drive the valves, so what happens in
         * front of them decides what gets distorted, and that is the bank that
         * should carry it.  Above 2 kHz it does not: a hard limiter blanks a
         * small signal in the presence of a large one, so pre-emphasising a top
         * end the instrument barely has cannot get it through - fitted that way
         * both top bands ran into their eighteen decibel limit and were still
         * five decibels short.  Above 2 kHz the correction goes after the valves,
         * where a 2203 keeps its tone stack anyway.
         */
        for (b = 0; b < AG_AMP_VOICE_N; b++) {
            const float f = cfg.voice[0][b].hz;
            double      best = 1e30, d = 0.0;
            for (i = 0; i < SPEC_N; i++) {
                const double sep = (double)k_spec_f[i] / (double)f;
                const double dist = sep > 1.0 ? sep : 1.0 / sep;
                if (k_spec_f[i] < 63.0f || got[i] < -299.0) {
                    continue;
                }
                if (dist < best) {
                    best = dist;
                    d = want[i] - got[i];
                }
            }
            /*
             * Each band belongs to exactly one bank, or the two would correct the
             * same error twice.  The middle - 200 Hz to 1.6 kHz - goes in front,
             * because that is where the guitar has the energy to drive the valves
             * and so where pre-emphasis decides what gets distorted.  The top
             * goes behind, because a limiter blanks it otherwise.  And so does
             * the bottom: more bass in front of a clipping valve is not more bass
             * out of it, it is less midrange - fitted that way, 10 nF in V1b's
             * coupling capacitor matched the reference below 200 Hz and cost
             * 4.7 dB at 315.  A bass control after the distortion is what a real
             * tone stack is.
             */
            /* The front is frozen from here on; this pass is the bank behind
             * the valves, and it gets every band. */
            cfg.tone[b].db += (float)(0.8 * d);
            if (cfg.voice[0][b].db > 18.0f) {
                cfg.voice[0][b].db = 18.0f;
            }
            if (cfg.voice[0][b].db < -18.0f) {
                cfg.voice[0][b].db = -18.0f;
            }
            if (cfg.tone[b].db > 24.0f) {
                cfg.tone[b].db = 24.0f;
            }
            if (cfg.tone[b].db < -24.0f) {
                cfg.tone[b].db = -24.0f;
            }
        }
    }
    /*
     * After pass one, the number that says whether the *character* moved rather
     * than the tone: what this chain does to dynamics against what the amplifier
     * does to them.  Printed here, between the passes, because after pass two the
     * spectrum will look better whatever happened to the grain - and pass one is
     * the pass that can change it.  Both banks are linear; only the pre bank
     * changes how hard the valves are driven.
     */

    printf("\n  Best at iteration %d of pass 2, rms %.2f dB.  What the two"
           " passes arrived at:\n", best_it, best_rms);
    printf("      Hz    in front of the valves    after them\n");
    for (b = 0; b < AG_AMP_VOICE_N; b++) {
        printf("    %6.0f          %+6.2f dB          %+6.2f dB   (Q %.2f)\n",
               (double)best_cfg.voice[0][b].hz, (double)best_cfg.voice[0][b].db,
               (double)best_cfg.tone[b].db, (double)best_cfg.voice[0][b].q);
    }
    {
        int cm, any = 0;
        for (cm = 0; cm < AG_AMP_STAGES; cm++) {
            if (best_cfg.couple_mul[cm] > 0.0f &&
                best_cfg.couple_mul[cm] != 1.0f) {
                any = 1;
            }
        }
        if (any) {
            printf("\n  and the coupling capacitors it wants, as multiples of the"
                   " schematic's corner\n  (so a bigger number is a smaller"
                   " capacitor):");
            for (cm = 0; cm < AG_AMP_STAGES; cm++) {
                printf("  stage %d x%.1f", cm + 1,
                       (double)(best_cfg.couple_mul[cm] > 0.0f
                                    ? best_cfg.couple_mul[cm] : 1.0f));
            }
            printf("\n");
        }
    }
    printf("\n  As the %s block in ag_amp_model would have it:\n",
           ag_amp_model_name(g_model));
    for (b = 0; b < AG_AMP_VOICE_N; b++) {
        printf("    { %6.0ff, %+7.2ff, %.2ff },   { %6.0ff, %+7.2ff, %.2ff },\n",
               (double)best_cfg.voice[0][b].hz, (double)best_cfg.voice[0][b].db,
               (double)best_cfg.voice[0][b].q, (double)best_cfg.tone[b].hz,
               (double)best_cfg.tone[b].db, (double)best_cfg.tone[b].q);
    }
    if (best_out != NULL) {
        *best_out = best_cfg;
    }
    free(a);
    free(out);
}

static void mode_fit(int argc, char **argv)
{
    const int   iters = argc > 4 ? atoi(argv[4]) : 8;
    const float drive = argc > 5 ? (float)atof(argv[5]) : 0.5f;
    const float q = argc > 6 ? (float)atof(argv[6]) : 1.0f;
    const float cc2 = argc > 7 ? (float)atof(argv[7]) * 1e-9f : 0.0f;
    float      *ref = NULL, *in = NULL;
    uint32_t    rn = 0, rrate = 0, dn = 0, drate = 0;

    if (argc < 4) {
        printf("  usage: tube_render fit reference.wav di.wav [iterations "
               "[drive [q [ccouple2_nF]]]]\n");
        return;
    }
    ref = read_wav(argv[2], &rn, &rrate);
    in = read_wav(argv[3], &dn, &drate);
    if (ref == NULL || in == NULL) {
        free(ref);
        free(in);
        return;
    }
    if (rrate != drate) {
        printf("  %s is %u Hz and %s is %u Hz; resample one with wavrate first"
               " - or use `match`, which does it itself\n",
               argv[2], rrate, argv[3], drate);
    } else {
        fit_banks(ref, rn, in, dn, drate, iters, drive, q, cc2, argv[2], 0.0,
                  0.0, NULL, 0u, NULL);
    }
    free(ref);
    free(in);
}

/* ------------------------------------------------------------------------ */
/* attack - what blocking distortion actually does                           */
/* ------------------------------------------------------------------------ */

/*
 * The envelope of a hard pick, with the grid's charge on the coupling capacitor
 * and without it.
 *
 * This is the one thing a static curve cannot do and the reason for the whole
 * exercise: the grid conducts on the loudest part of the attack, the charge it
 * leaves holds the grid down, and the stage goes quieter for as long as the grid
 * leak takes to drain it - a hundred milliseconds on V1a, one on V1b.  On a hard
 * pick that is the note choking and then blooming, and it is what a waveshaper
 * of any shape will never do.
 *
 * Printed as five-millisecond windows rather than described, because "it
 * compresses the attack" is a claim and decibels are not.
 */
static void mode_attack(int argc, char **argv)
{
    const int    n = (int)(RATE * 0.4f); /* 400 ms */
    const int    w = (int)(RATE * 0.005f);
    ag_amp_cfg_t cfg;
    ag_amp_t    *a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    float       *in = (float *)malloc(sizeof(float) * n);
    float       *on = (float *)malloc(sizeof(float) * n);
    float       *off = (float *)malloc(sizeof(float) * n);
    float        drive = argc > 2 ? (float)atof(argv[2]) : 1.0f;
    int          i, b;
    float        vc0 = 0.0f, vc1 = 0.0f;

    if (a == NULL || in == NULL || on == NULL || off == NULL) {
        free(a);
        free(in);
        free(on);
        free(off);
        return;
    }
    ag_amp_probe_pluck(in, n, RATE);

    model_cfg(&cfg, RATE);
    cfg.drive = drive;
    cfg.blocking = 0;
    if (ag_amp_build(a, g_ckt, &cfg, g_tab, 0, in, n) == 0) {
        for (i = 0; i < n; i++) {
            off[i] = ag_amp_tick(a, in[i]);
        }
    }
    cfg.blocking = 1;
    if (ag_amp_set_voicing(a, &cfg) == 0) {
        for (i = 0; i < n; i++) {
            on[i] = ag_amp_tick(a, in[i]);
        }
        vc0 = a->tube[0].vc_peak;
        vc1 = a->tube[1].vc_peak;
    }

    printf("A hard pick at drive %.2f, five-millisecond windows, in decibels\n"
           "relative to the loudest window of each.  The grid charge peaked at\n"
           "%.2f V on V1a (drains in 100 ms) and %.2f V on V1b (1 ms).\n\n",
           (double)drive, (double)vc0, (double)vc1);
    printf("    ms   no blocking   blocking   difference\n");
    for (b = 0; b + w <= n; b += w) {
        const double r0 = rms_of(off, b, b + w);
        const double r1 = rms_of(on, b, b + w);
        if (b > (int)(RATE * 0.2f)) {
            break;
        }
        printf("  %4.0f   %+9.1f   %+9.1f   %+8.1f\n",
               1000.0 * (double)b / (double)RATE,
               db(r0 / (peak_of(off, n) + 1e-30)),
               db(r1 / (peak_of(on, n) + 1e-30)), db(r1 / (r0 + 1e-30)));
    }
    printf("\n  Level over the whole 400 ms: %+.1f dB with blocking against\n"
           "  without, so the effect is shape rather than volume.\n",
           db(rms_of(on, 0, n) / (rms_of(off, 0, n) + 1e-30)));
    free(a);
    free(in);
    free(on);
    free(off);
}

/* ------------------------------------------------------------------------ */
/* noise - whose hiss is it                                                  */
/* ------------------------------------------------------------------------ */

/*
 * A constant hiss is a different animal from a crackle, and it has a different
 * set of suspects: something whose size does not follow the signal.  Three
 * candidates, and this separates them in one run.
 *
 *   1. The recording's own noise floor, amplified and then compressed.  A high
 *      gain amplifier really does this - a 2203 hisses - and the measure of it is
 *      that the output's peak-to-floor is worse than the input's by exactly the
 *      compression.  Physically right, and possibly too much of it.
 *   2. The cabinet.  A convolution is linear, so it cannot make noise out of
 *      silence - but ag_ir's cabinet presets are synthetic, twenty milliseconds
 *      of low-passed white noise, so what they convolve the music with *is*
 *      noise.
 *   3. Arithmetic in the chain.  Digital silence rules out a constant floor;
 *      the moving-signal kind would show up as the output floor being worse than
 *      the input floor by more than the gain accounts for.
 */
/*
 * Peak and floor of a file, in a window the caller names.
 *
 * Separate from `noise` because the automatic quietest-window search is not
 * comparable between two files: run on a render from the other model it found the
 * zero padding at the end of the file and reported an infinite dynamic range,
 * while on this one it found a real gap in the playing.  Comparing two models
 * means comparing the same hundred milliseconds of the same performance.
 */
static void mode_floor(int argc, char **argv)
{
    float   *x;
    uint32_t frames = 0, rate = 0;
    double   t0 = argc > 3 ? atof(argv[3]) : 3.10;
    int      a, w;

    if (argc < 3) {
        printf("  usage: tube_render floor in.wav [start_s [dur_ms]]\n");
        return;
    }
    x = read_wav(argv[2], &frames, &rate);
    if (x == NULL) {
        return;
    }
    w = (int)((argc > 4 ? atof(argv[4]) : 100.0) * (double)rate / 1000.0);
    a = (int)(t0 * (double)rate);
    if (a < 0 || a + w > (int)frames) {
        printf("  %s: %.2f s + %d samples is past the end (%u frames at %u Hz)\n",
               argv[2], t0, w, frames, rate);
        free(x);
        return;
    }
    printf("  %-46s peak %+6.1f  floor %+6.1f  ratio %+6.1f dB\n", argv[2],
           db(peak_of(x, (int)frames)), db(rms_of(x, a, a + w)),
           db(peak_of(x, (int)frames) / rms_of(x, a, a + w)));

    /*
     * And where in the spectrum that floor sits, because "hiss" and "rumble" are
     * the same number of decibels and not the same complaint.  Four bands by
     * direct transform of the window - it is a tenth of a second, so this costs
     * nothing and needs no filter to be designed and then doubted.
     */
    {
        static const double edge[5] = { 0.0, 300.0, 1500.0, 5000.0, 1e9 };
        double              e[4] = { 0.0, 0.0, 0.0, 0.0 }, tot = 0.0;
        int                 b, k, band;
        for (b = 1; b <= w / 2; b++) {
            double re = 0.0, im = 0.0, p;
            const double f = (double)b * (double)rate / (double)w;
            for (k = 0; k < w; k++) {
                const double ww =
                    0.5 - 0.5 * cos(2.0 * PI * (double)k / (double)w);
                const double t = 2.0 * PI * (double)b * (double)k / (double)w;
                re += (double)x[a + k] * ww * cos(t);
                im -= (double)x[a + k] * ww * sin(t);
            }
            p = re * re + im * im;
            tot += p;
            for (band = 0; band < 4; band++) {
                if (f >= edge[band] && f < edge[band + 1]) {
                    e[band] += p;
                }
            }
        }
        if (tot > 1e-300) {
            printf("       that floor by band:  <300 Hz %+5.1f   0.3-1.5k %+5.1f"
                   "   1.5-5k %+5.1f   >5k %+5.1f dB of it\n",
                   10.0 * log10(e[0] / tot + 1e-30),
                   10.0 * log10(e[1] / tot + 1e-30),
                   10.0 * log10(e[2] / tot + 1e-30),
                   10.0 * log10(e[3] / tot + 1e-30));
        }
    }
    free(x);
}

static void mode_noise(int argc, char **argv)
{
    ag_amp_cfg_t cfg;
    ag_amp_t    *a;
    float       *in, *out;
    uint32_t     frames = 0, rate = 0, i;
    int          w, qa = 0, win;
    double       qr = 1e30;

    if (argc < 3) {
        printf("  usage: tube_render noise in.wav [drive]\n");
        return;
    }
    in = read_wav(argv[2], &frames, &rate);
    if (in == NULL) {
        return;
    }
    a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    out = (float *)malloc(sizeof(float) * frames);
    if (a == NULL || out == NULL) {
        free(in);
        free(a);
        free(out);
        return;
    }
    model_cfg(&cfg, (float)rate);
    if (argc > 3) {
        cfg.drive = (float)atof(argv[3]);
    }

    /* The quietest tenth of a second of the input, which is where its own floor
     * is visible and where the output's hiss will be loudest relative to music. */
    win = (int)(rate / 10u);
    for (w = 0; w + win <= (int)frames; w += win / 2) {
        const double r = rms_of(in, w, w + win);
        if (r < qr) {
            qr = r;
            qa = w;
        }
    }
    printf("Quietest %d ms of %s starts at %.2f s.\n\n", 1000 * win / (int)rate,
           argv[2], (double)qa / (double)rate);
    printf("  stage                    peak      floor    peak/floor\n");
    printf("  the recording          %+7.1f  %+7.1f  %+9.1f dB\n",
           db(peak_of(in, (int)frames)), db(qr), db(peak_of(in, (int)frames) / qr));

    /* Digital silence: anything but zero here is arithmetic. */
    if (ag_amp_build(a, g_ckt, &cfg, g_tab, 0, in, (int)(frames < rate * 10u
                                                             ? frames
                                                             : rate * 10u)) != 0) {
        printf("  build failed\n");
        free(in);
        free(a);
        free(out);
        return;
    }
    for (i = 0; i < 4410u && i < frames; i++) {
        out[i] = ag_amp_tick(a, 0.0f);
    }
    printf("  silence through it     %+7.1f  %+7.1f       (must be silent)\n",
           db(peak_of(out, 4410)), db(rms_of(out, 0, 4410)));

    /* The amplifier alone. */
    ag_amp_reset(a);
    for (i = 0; i < frames; i++) {
        out[i] = ag_amp_tick(a, in[i]);
    }
    printf("  + the amplifier        %+7.1f  %+7.1f  %+9.1f dB   (drive %.2f)\n",
           db(peak_of(out, (int)frames)), db(rms_of(out, qa, qa + win)),
           db(peak_of(out, (int)frames) / rms_of(out, qa, qa + win)),
           (double)cfg.drive);

    /* And through the cabinet. */
    if (g_cab != 0 || g_cab_path != NULL) {
        if (run_cab(out, frames, rate) == 0) {
            const uint32_t n = frames - frames % AG_IR_BLOCK;
            printf("  + the cabinet          %+7.1f  %+7.1f  %+9.1f dB\n",
                   db(peak_of(out, (int)n)), db(rms_of(out, qa, qa + win)),
                   db(peak_of(out, (int)n) / rms_of(out, qa, qa + win)));
        }
    }
    printf("\n  A high gain amplifier is supposed to lose peak-to-floor: it\n"
           "  compresses, so the quiet parts come up.  What it is not supposed to\n"
           "  do is lose more than the compression accounts for.\n");
    free(in);
    free(a);
    free(out);
}

/* ------------------------------------------------------------------------ */
/* imd - the question this design started from, answered in decibels         */
/* ------------------------------------------------------------------------ */

/*
 * Two tones in, and everything that comes out sorted into what it is.
 *
 * The design started from the question of whether a chain of static curves gives
 * intermodulation.  It does - a memoryless nonlinearity expands two tones into
 * every m*w1 +- n*w2 from the cubic term onwards, and that is algebra rather than
 * a property of a valve - but "it does" is not a number, and a number is what
 * decides whether the model is behaving like two clipping triodes or like
 * something else.
 *
 * Bins 197 and 296 of 4096 at 22.05 kHz: 1060 and 1593 Hz, near enough a fifth,
 * and 197 is prime so the harmonic series of the two do not fall on each other.
 */
#define IMD_N  4096
#define IMD_B1 197
#define IMD_B2 296

/* One bin's amplitude, Hann windowed and corrected for the window's gain. */
static double bin_db(const float *x, int n, int b)
{
    double re = 0.0, im = 0.0, a;
    int    i;
    for (i = 0; i < n; i++) {
        const double w = 0.5 - 0.5 * cos(2.0 * PI * (double)i / (double)n);
        const double t = 2.0 * PI * (double)b * (double)i / (double)n;
        re += (double)x[i] * w * cos(t);
        im -= (double)x[i] * w * sin(t);
    }
    a = 4.0 * sqrt(re * re + im * im) / (double)n;
    return 20.0 * log10(a > 1e-15 ? a : 1e-15);
}

/*
 * Named products at named bins, which is what an intermodulation figure is.
 *
 * The first version of this tried to sort the whole spectrum into "harmonic",
 * "intermodulation" and "the rest", and that does not work: with two tones 197
 * and 296 bins apart, |m*197 - n*296| reaches almost every small bin by m = 3,
 * so the intermodulation class absorbed nearly everything and reported 45% of the
 * energy as products at a drive where the first valve is barely clipping.  A
 * classifier whose classes are not disjoint is not a measurement.
 */
static void mode_imd(void)
{
    static const struct {
        const char *name;
        int         bin;
    } part[] = { { "f1            1061 Hz", IMD_B1 },
                 { "f2            1593 Hz", IMD_B2 },
                 { "2f1 - f2       529 Hz", 2 * IMD_B1 - IMD_B2 },
                 { "f2 - f1        532 Hz", IMD_B2 - IMD_B1 },
                 { "2f1           2122 Hz", 2 * IMD_B1 },
                 { "2f2 - f1      2125 Hz", 2 * IMD_B2 - IMD_B1 },
                 { "f1 + f2       2654 Hz", IMD_B1 + IMD_B2 },
                 { "3f1           3183 Hz", 3 * IMD_B1 },
                 { "2f1 + f2      3715 Hz", 2 * IMD_B1 + IMD_B2 },
                 { "3f2 - f1      4657 Hz", 3 * IMD_B2 - IMD_B1 } };
    /*
     * Down to a fiftieth, because at a fifth the answer is already flat.
     *
     * A 2203 has no volume control between its two valves, which is what makes it
     * high gain, and this model is faithful about that: the first valve's gain of
     * 71 puts ten volts on the second grid from a seventh of a volt in, and the
     * second grid's whole window is one and a half.  So the second valve is
     * saturated at every setting anyone would call a gain setting, and the useful
     * clean range is an order of magnitude lower than a guitar's own level.
     */
    static const float drives[] = { 0.02f, 0.1f, 0.5f, 2.5f };
    ag_amp_cfg_t       cfg;
    ag_amp_t          *a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    float             *buf = (float *)malloc(sizeof(float) * IMD_N);
    double             lv[4][10];
    int                d, p, i;

    if (a == NULL || buf == NULL) {
        free(a);
        free(buf);
        return;
    }
    for (d = 0; d < 4; d++) {
        model_cfg(&cfg, RATE);
        cfg.drive = drives[d];
        if (ag_amp_build(a, g_ckt, &cfg, g_tab, 0, g_probe, G_PROBE_N) != 0) {
            continue;
        }
        for (i = 0; i < 110000; i++) {
            const double t1 = 2.0 * PI * (double)IMD_B1 * (double)i / (double)IMD_N;
            const double t2 = 2.0 * PI * (double)IMD_B2 * (double)i / (double)IMD_N;
            (void)ag_amp_tick(a, (float)(0.35 * (sin(t1) + sin(t2))));
        }
        for (i = 0; i < IMD_N; i++) {
            const double t1 = 2.0 * PI * (double)IMD_B1 * (double)i / (double)IMD_N;
            const double t2 = 2.0 * PI * (double)IMD_B2 * (double)i / (double)IMD_N;
            buf[i] = ag_amp_tick(a, (float)(0.35 * (sin(t1) + sin(t2))));
        }
        for (p = 0; p < 10; p++) {
            lv[d][p] = bin_db(buf, IMD_N, part[p].bin);
        }
        /* Everything relative to the stronger fundamental. */
        {
            const double ref = lv[d][0] > lv[d][1] ? lv[d][0] : lv[d][1];
            for (p = 0; p < 10; p++) {
                lv[d][p] -= ref;
            }
        }
    }

    printf("Two tones at %.0f and %.0f Hz, equal amplitude, through the whole\n"
           "chain.  Each product relative to the stronger fundamental.\n\n"
           "This is the question the design started from, and the answer is that a\n"
           "chain of static curves intermodulates freely - it has to, because a\n"
           "memoryless nonlinearity expands two tones into every m*f1 +- n*f2 from\n"
           "the cubic term onwards.  What it lacks is memory inside the curve, not\n"
           "products.\n\n",
           (double)(IMD_B1 * RATE / IMD_N), (double)(IMD_B2 * RATE / IMD_N));
    printf("  product                  drive 0.02  0.1     0.5     2.5\n");
    for (p = 0; p < 10; p++) {
        printf("  %s  %+7.1f %+7.1f %+7.1f %+7.1f\n", part[p].name, lv[0][p],
               lv[1][p], lv[2][p], lv[3][p]);
    }
    free(a);
    free(buf);
}

/* ------------------------------------------------------------------------ */
/* res - how big the table has to be, and how much of the tail to keep       */
/* ------------------------------------------------------------------------ */

#define RES_N G_PROBE_N

/* RMS of `d` against the RMS of `r`, in dB.  Optionally through 1.5-5 kHz,
 * which is where a speaker passes everything and the ear forgives least. */
static double diff_db(const float *a, const float *b, int n, int band)
{
    /* Two independent copies of the same band, so that the difference and the
     * reference are measured through the same filter and not against each
     * other's passband. */
    ag_biq_t dh1, dh2, dl, rh1, rh2, rl;
    double   sd = 0.0, sr = 0.0;
    int      i;

    (void)ag_biq_hp1(&dh1, RATE, 1500.0f);
    (void)ag_biq_hp1(&dh2, RATE, 1500.0f);
    (void)ag_biq_lp2(&dl, RATE, 5000.0f, 0.70710678f);
    (void)ag_biq_hp1(&rh1, RATE, 1500.0f);
    (void)ag_biq_hp1(&rh2, RATE, 1500.0f);
    (void)ag_biq_lp2(&rl, RATE, 5000.0f, 0.70710678f);
    for (i = 0; i < n; i++) {
        float d = a[i] - b[i];
        float r = b[i];
        if (band) {
            d = ag_biq_tick(&dl, ag_biq_tick(&dh2, ag_biq_tick(&dh1, d)));
            r = ag_biq_tick(&rl, ag_biq_tick(&rh2, ag_biq_tick(&rh1, r)));
        }
        sd += (double)d * (double)d;
        sr += (double)r * (double)r;
    }
    if (sr < 1e-30) {
        return -300.0;
    }
    return 10.0 * log10(sd / sr + 1e-30);
}

/*
 * Axes are held still across a comparison, because the fit converges to a
 * slightly different axis for every table size and on a hard-clipped waveform
 * that moves the clipping edges by a fraction of a sample - which is worth more
 * decibels than anything being measured here.
 */
static float g_ref_lo[AG_AMP_STAGES], g_ref_hi[AG_AMP_STAGES];
static int   g_adaa = 1;
/* Where the antialiasing average stops walking cells and starts subtracting.
 * 0 keeps what the bake chose; -1 forces the whole table, which makes the walk
 * exact and slow, and the cheap form has to be measured against something. */
static int g_walk;

static int render_cfg(float *out, const float *in, int n, int tab_n, float tol,
                      float drive, int fit)
{
    ag_amp_cfg_t cfg;
    ag_amp_t    *a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    float       *tab =
        (float *)malloc(sizeof(float) * (size_t)(AG_AMP_STAGES * 3 * tab_n));
    int i, ok = 0;

    if (a != NULL && tab != NULL) {
        model_cfg(&cfg, RATE);
        cfg.axis_tol = tol;
        cfg.drive = drive;
        cfg.adaa = g_adaa;
        if (!fit) {
            for (i = 0; i < AG_AMP_STAGES; i++) {
                cfg.axis_lo[i] = g_ref_lo[i];
                cfg.axis_hi[i] = g_ref_hi[i];
            }
        }
        if (ag_amp_build(a, g_ckt, &cfg, tab, tab_n, fit ? g_probe : 0,
                         fit ? G_PROBE_N : 0) == 0) {
            if (g_walk != 0) {
                for (i = 0; i < AG_AMP_STAGES; i++) {
                    a->tube[i].walk_max = g_walk < 0 ? tab_n : g_walk;
                }
            }
            for (i = 0; i < n; i++) {
                out[i] = ag_amp_tick(a, in[i]);
            }
            if (fit) {
                for (i = 0; i < AG_AMP_STAGES; i++) {
                    g_ref_lo[i] = a->tube[i].lo;
                    g_ref_hi[i] = a->tube[i].hi;
                }
            }
            ok = 1;
        }
    }
    free(a);
    free(tab);
    return ok ? 0 : -1;
}

static void mode_res(void)
{
    static const int   ns[] = { 256, 512, 1024, 2048, 4096, 8192 };
    static const float tols[] = { 0.0005f, 0.002f, 0.01f, 0.03f, 0.08f };
    const float       *in = g_probe;
    float             *ref = (float *)malloc(sizeof(float) * RES_N);
    float             *got = (float *)malloc(sizeof(float) * RES_N);
    int                i, j;

    if (ref == NULL || got == NULL) {
        free(ref);
        free(got);
        return;
    }

    /* One fitted render to find the axes; every render after this borrows them. */
    if (render_cfg(got, in, RES_N, 4096, 0.002f, 2.0f, 1) != 0) {
        printf("  axis fit failed\n");
        free(ref);
        free(got);
        return;
    }
    printf("Two seconds of a plucked two-note decay, drive 2.0, axes held at\n"
           "%.2f..%.2f V and %.2f..%.2f V for every row.\n\n",
           (double)g_ref_lo[0], (double)g_ref_hi[0], (double)g_ref_lo[1],
           (double)g_ref_hi[1]);

    /* ---------------------------------------------------------------- */
    /* 1. How many points the curve needs.                              */
    /* ---------------------------------------------------------------- */

    /*
     * With the antialiasing off, so that this measures interpolation and nothing
     * else.  Leaving it on measured the reference's own arithmetic instead and
     * reported the same 29 dB for every table size - a metric bounded by its
     * reference, which docs/08 has a name for.
     */
    g_adaa = 0;
    printf("Points across the curve, antialiasing off so that this is\n"
           "interpolation and nothing else.  Reference is 131072 points, 128\n"
           "times the smallest candidate.\n\n");
    if (render_cfg(ref, in, RES_N, 131072, 0.002f, 2.0f, 0) == 0) {
        printf("  points   mV/cell (V1b)      all      1.5-5 kHz    KB for the pair\n");
        for (i = 0; i < (int)(sizeof(ns) / sizeof(ns[0])); i++) {
            if (render_cfg(got, in, RES_N, ns[i], 0.002f, 2.0f, 0) == 0) {
                printf("  %6d   %9.3f    %+7.1f      %+7.1f      %6.0f\n", ns[i],
                       (double)((g_ref_hi[1] - g_ref_lo[1]) * 1000.0f /
                                (float)(ns[i] - 1)),
                       diff_db(got, ref, RES_N, 0), diff_db(got, ref, RES_N, 1),
                       (double)(AG_AMP_STAGES * 3 * ns[i] * 4) / 1024.0);
            }
        }
    }

    /* ---------------------------------------------------------------- */
    /* 2. How much of the grid-current tail the axis has to keep.        */
    /* ---------------------------------------------------------------- */

    printf("\nHow much of the tail the axis keeps, at %d points, still with no\n"
           "antialiasing.  Fitted to the curve rather than to the signal, which\n"
           "is what `tol` controls; the reference is the signal-fitted axis, so\n"
           "a row that reaches the floor is a row that kept enough.\n\n",
           AG_AMP_TAB_N);
    printf("  tol      axis V1b            all      1.5-5 kHz   clamped\n");
    for (j = 0; j < (int)(sizeof(tols) / sizeof(tols[0])); j++) {
        ag_amp_cfg_t cfg;
        ag_amp_t    *a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
        float       *tab = (float *)malloc(sizeof(float) *
                                     (size_t)(AG_AMP_STAGES * 3 * AG_AMP_TAB_N));
        if (a == NULL || tab == NULL) {
            free(a);
            free(tab);
            continue;
        }
        model_cfg(&cfg, RATE);
        cfg.drive = 2.0f;
        cfg.adaa = 0;
        cfg.axis_tol = tols[j];
        if (ag_amp_build(a, g_ckt, &cfg, tab, AG_AMP_TAB_N, 0, 0) == 0) {
            int t;
            for (t = 0; t < RES_N; t++) {
                got[t] = ag_amp_tick(a, in[t]);
            }
            printf("  %.4f   %6.1f..%-8.1f  %+7.1f      %+7.1f   %7lu\n",
                   (double)tols[j], (double)a->tube[1].lo, (double)a->tube[1].hi,
                   diff_db(got, ref, RES_N, 0), diff_db(got, ref, RES_N, 1),
                   (unsigned long)ag_amp_clamped(a));
        }
        free(a);
        free(tab);
    }

    /* ---------------------------------------------------------------- */
    /* 3. What the cheap antialiasing path costs in accuracy.           */
    /* ---------------------------------------------------------------- */

    /*
     * The average over a step is exact when it walks cells and only approximate
     * when it subtracts two running integrals, so the exact answer is the same
     * code with the crossover pushed past the end of the table.  This is the
     * measurement that has to be taken on a decaying note rather than on
     * silence: the error is of constant size, so it hides completely at full
     * level and grows as the music goes away.
     */
    g_adaa = 1;
    printf("\nThe antialiasing crossover, at %d points.  Walking cells is exact and\n"
           "costs an iteration a cell; subtracting two running integrals is O(1) and\n"
           "loses precision as the step gets smaller.  Measured against the same\n"
           "code walking the whole table, on the quietest quarter of the note as\n"
           "well as the whole of it - the error is of constant size, so at full\n"
           "level it hides completely.\n\n",
           AG_AMP_TAB_N);
    g_walk = -1;
    if (render_cfg(ref, in, RES_N, AG_AMP_TAB_N, 0.002f, 2.0f, 0) == 0) {
        static const int walks[] = { 1, 2, 4, 8, 16, 32, 128 };
        printf("  walk_max   whole note   quietest quarter   ns/sample\n");
        for (i = 0; i < (int)(sizeof(walks) / sizeof(walks[0])); i++) {
            clock_t t0, t1;
            double  sd = 0.0, sr = 0.0, ns_per;
            int     t;
            const int q0 = (RES_N * 3) / 4;
            g_walk = walks[i];
            if (render_cfg(got, in, RES_N, AG_AMP_TAB_N, 0.002f, 2.0f, 0) != 0) {
                continue;
            }
            for (t = q0; t < RES_N; t++) {
                const double d = (double)got[t] - (double)ref[t];
                sd += d * d;
                sr += (double)ref[t] * (double)ref[t];
            }
            t0 = clock();
            (void)render_cfg(got, in, RES_N, AG_AMP_TAB_N, 0.002f, 2.0f, 0);
            t1 = clock();
            ns_per = 1e9 * (double)(t1 - t0) / (double)CLOCKS_PER_SEC /
                     (double)RES_N;
            printf("  %8d   %+7.1f      %+7.1f            %8.1f\n", walks[i],
                   diff_db(got, ref, RES_N, 0),
                   10.0 * log10(sd / (sr + 1e-30) + 1e-30), ns_per);
        }
    }
    g_walk = 0;

    free(ref);
    free(got);
}

/* ------------------------------------------------------------------------ */
/* preset - bake once here so the target never has to                        */
/* ------------------------------------------------------------------------ */

/*
 * Baked at the top of the drive range on purpose.
 *
 * The axes are fitted to the signal and the signal follows the gain knob: the
 * second stage's axis runs to +32 V at drive 0.5 and +147 V at drive 2.5.  A
 * preset carries one axis, so it has to be the widest the knob can ask for, or
 * turning the gain up runs the model off the end of its own table.  The price is
  * resolution, and the trade has a visible optimum.  Measured against a chain
 * baked fresh on the material itself:
 *
 *     fitted at drive 1.0 (the top of the knob)   -36.6 dB
 *     fitted at drive 2.0                         -69.2 dB
 *     fitted at drive 4.0                         -67.2 dB
 *
 * Twice the top of the range, then.  One times is not enough because the probe
 * is a synthetic pluck and a real guitar drives the second grid harder than it
 * does; four times is worse again, because by then the axis is wide enough that
 * the resolution it loses costs more than the headroom it buys.
 */
static int write_preset(const char *path, float fit_drive)
{
    ag_amp_cfg_t cfg;
    ag_amp_t    *a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    uint8_t     *blob;
    uint32_t     size, wrote;
    FILE        *f;

    if (a == NULL) {
        return -1;
    }
    print_model();
    model_cfg(&cfg, RATE);
    cfg.drive = fit_drive;
    if (ag_amp_build(a, g_ckt, &cfg, g_tab, 0, g_probe, G_PROBE_N) != 0) {
        printf("  build failed\n");
        free(a);
        return -1;
    }
    size = ag_amp_preset_size(a->n, a->tab_n);
    blob = (uint8_t *)malloc(size);
    if (blob == NULL) {
        free(a);
        return -1;
    }
    wrote = ag_amp_preset_save(a, blob, size);
    f = wrote == size ? fopen(path, "wb") : NULL;
    if (f != NULL) {
        fwrite(blob, 1, wrote, f);
        fclose(f);
        printf("  %s: %u stages, %d points, %u bytes, axes fitted at drive %.2f\n",
               path, (unsigned)a->n, a->tab_n, wrote, (double)fit_drive);
        {
            int i;
            for (i = 0; i < a->n; i++) {
                printf("    stage %d axis %.2f .. %.2f V\n", i,
                       (double)a->tube[i].lo, (double)a->tube[i].hi);
            }
        }
    } else {
        printf("  cannot write %s\n", path);
    }
    free(blob);
    free(a);
    return f != NULL ? 0 : -1;
}

/* Load a preset into `a` if AG_PRESET names one; 0 on success. */
static int load_preset(ag_amp_t *a, float *tab, float fs)
{
    const char *path = getenv("AG_PRESET");
    FILE       *f;
    uint8_t    *blob;
    long        len;
    int         rc = -1;

    if (path == NULL || (f = fopen(path, "rb")) == NULL) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    blob = (uint8_t *)malloc((size_t)(len > 0 ? len : 1));
    if (blob != NULL && fread(blob, 1, (size_t)len, f) == (size_t)len) {
        rc = ag_amp_preset_load(a, blob, (uint32_t)len, tab, fs);
        printf("  preset %s: %ld bytes, %s\n", path, len,
               rc == 0 ? "loaded" : "REFUSED");
    }
    free(blob);
    fclose(f);
    return rc;
}
/* ------------------------------------------------------------------------ */
/* render                                                                    */
/* ------------------------------------------------------------------------ */

/* Lookups over every live stage, not the two this chain used to have. */
static unsigned long stage_lookups(const ag_amp_t *a)
{
    unsigned long n = 0;
    int           i;
    for (i = 0; i < a->n; i++) {
        n += a->tube[i].samples;
    }
    return n;
}

/*
 * Is the dirt the table?
 *
 * A listening report of an unpleasant broadband rattle from 2 kHz up, worst at
 * 3-6 kHz, signal-modulated, that oversampling, the antialiasing average, the
 * blocking flag and the axis fit all leave exactly where it is - and that a live
 * render reproduces bit for bit, so it is the chain and not the tool.
 *
 * What none of those tests can see: **a piecewise-linear table has a
 * discontinuous derivative at every node**.  A corner in the derivative radiates
 * a harmonic series that falls off as 1/n^2 instead of exponentially, which is
 * what a buzz is, and it is invisible to every knob above - more oversampling
 * computes the corners more accurately, the antialiasing average integrates the
 * piecewise-linear curve exactly, and a better-fitted axis moves the corners
 * without removing them.
 *
 * So render the same take twice through the same everything, changing only the
 * number of points, and look at the difference.  32 times finer is 5 bits of
 * corner-to-corner distance, so if the corners are audible the difference is
 * loud; if the difference is 80 dB down, the table is not the problem and the
 * next hypothesis is up.
 *
 *   tube_render buzz in.wav [drive [top_hz [points]]]
 */
/*
 * How deep the humps are.
 *
 * The artefact a listener could see once the live tool recorded what it played:
 * in a third-octave band up around 4 kHz, the energy does not sit there steadily
 * - it arrives in humps, one per clipping edge, twice a cycle.  What is heard as
 * a rattle is that modulation, not the tone inside it, and it is both high and
 * low at once: the carrier is the band and the pitch of the rattle is the hump
 * rate.
 *
 * So this measures the modulation depth: band pass, envelope, and the swing of
 * that envelope inside a 30 ms window - long enough to hold several humps, short
 * enough that the note's own decay does not count as one.  Windows where the
 * band is quiet are skipped, or a fade would read as infinite depth.
 *
 * Measured against a NAM capture of a real Marshall on the same take, drive 0.5,
 * through the same cabinet:
 *
 *     the reference                 +9.0 dB
 *     this chain, two stages       +15.0
 *     the same with cfg.hf_flat 1  +10.9
 *     the dry take, no amplifier    +8.6
 *
 * A real amplifier has humps too - a clipped edge is where the harmonics come
 * from, so they cannot help arriving with it.  What makes this chain worse is
 * making all of them in **one** place: two stages generate every harmonic at the
 * same instant, so they arrive as one coherent burst.  Three stages generate them
 * in three places with filters in between and the burst spreads, which is what
 * cfg.hf_flat buys without spending a valve on it.
 *
 * The first version of this measurement smoothed the envelope over 0.4 ms and
 * took the window's extremes.  It read every row about nineteen decibels higher
 * and hardly moved when the humps did, because the carrier's own period at
 * 4.2 kHz is 0.24 ms - so it was measuring carrier ripple.  Both fixes are noted
 * where they are made below; the moral is that a metric named after a thing has
 * to be checked against a change in that thing before it is trusted.
 *
 *   tube_render humps a.wav [b.wav ...]
 */
static int cmp_float(const void *a, const void *b)
{
    const float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void mode_humps(int argc, char **argv)
{
    int a;

    if (argc < 3) {
        printf("  usage: tube_render humps file.wav [more.wav ...]\n");
        return;
    }
    printf("  Depth of the 3.5-5 kHz humps: band pass, envelope, the swing of it\n"
           "  inside 30 ms.  For scale, a NAM Marshall on the guitar take reads\n"
           "  +8.8 dB and the dry take +8.3.\n\n");
    for (a = 2; a < argc; a++) {
        uint32_t frames = 0, rate = 0, i;
        float   *x = read_wav(argv[a], &frames, &rate);
        float   *bp, *env;
        double  *sw;
        int      nsw = 0, win, k;
        if (x == NULL) {
            continue;
        }
        bp = (float *)malloc(sizeof(float) * frames);
        env = (float *)malloc(sizeof(float) * frames);
        sw = (double *)malloc(sizeof(double) * (frames / 64u + 2u));
        if (bp == NULL || env == NULL || sw == NULL) {
            free(x);
            free(bp);
            free(env);
            free(sw);
            continue;
        }
        /* Two cascaded band passes a third of an octave wide at 4.2 kHz, which
         * is the same filter tube_live's solo uses - so what this measures is
         * what that listens to. */
        {
            ag_biq_chain_t ch;
            ag_biq_t      *s;
            ag_biq_chain_init(&ch);
            if ((s = ag_biq_chain_push(&ch)) != 0) {
                (void)ag_biq_bandpass(s, (float)rate, 4200.0f, 0.4f);
            }
            if ((s = ag_biq_chain_push(&ch)) != 0) {
                (void)ag_biq_bandpass(s, (float)rate, 4200.0f, 0.4f);
            }
            for (i = 0; i < frames; i++) {
                bp[i] = ag_biq_chain_tick(&ch, x[i]);
            }
        }
        /*
         * The envelope, smoothed over 1.2 ms.
         *
         * Not 0.4, which is what this did first and which made the whole
         * measurement meaningless: the carrier's period at 4.2 kHz is 0.24 ms, so
         * a 0.4 ms average still ripples at the carrier, and the depth came out
         * of that ripple rather than out of the valleys between the humps.  1.2 ms
         * is five carrier cycles and still a third of the gap between humps.
         */
        {
            const int   kk = (int)(0.0012 * (double)rate) | 1;
            double      acc = 0.0;
            int         j;
            for (i = 0; i < frames; i++) {
                env[i] = bp[i] < 0.0f ? -bp[i] : bp[i];
            }
            for (j = 0; j < kk && (uint32_t)j < frames; j++) {
                acc += (double)env[j];
            }
            for (i = 0; i + (uint32_t)kk < frames; i++) {
                const double m = acc / (double)kk;
                acc += (double)env[i + (uint32_t)kk] - (double)env[i];
                bp[i] = (float)m; /* reuse bp for the smoothed envelope */
            }
            for (; i < frames; i++) {
                bp[i] = env[i];
            }
        }
        win = (int)(0.03 * (double)rate);
        /* The loud reference, so quiet windows can be skipped rather than
         * reading as infinite depth. */
        {
            float loud = 0.0f;
            for (i = 0; i < frames; i++) {
                if (bp[i] > loud) {
                    loud = bp[i];
                }
            }
            float *sorted = (float *)malloc(sizeof(float) * (size_t)win);
            if (sorted == NULL) {
                free(sw);
                free(env);
                free(bp);
                free(x);
                continue;
            }
            for (i = (uint32_t)win; i + (uint32_t)win < frames;
                 i += (uint32_t)(win / 2)) {
                float hi90, lo15;
                /*
                 * Percentiles rather than the window's extremes.  One near-zero
                 * sample is a zero crossing and not a valley, and the extremes
                 * read the depth off exactly that - which is the other half of
                 * why this measurement used to be meaningless.
                 */
                memcpy(sorted, bp + i, sizeof(float) * (size_t)win);
                qsort(sorted, (size_t)win, sizeof(float), cmp_float);
                hi90 = sorted[(int)(0.90 * (double)win)];
                lo15 = sorted[(int)(0.15 * (double)win)];
                if (hi90 < loud * 0.15f) {
                    continue;
                }
                sw[nsw++] =
                    20.0 * log10(((double)hi90 + 1e-9) / ((double)lo15 + 1e-9));
            }
            free(sorted);
        }
        /* The median, by a partial sort - the mean would follow the deepest
         * window rather than the typical one. */
        {
            int j;
            for (k = 0; k < nsw; k++) {
                for (j = k + 1; j < nsw; j++) {
                    if (sw[j] < sw[k]) {
                        const double t = sw[k];
                        sw[k] = sw[j];
                        sw[j] = t;
                    }
                }
            }
            printf("  %-44s %+6.1f dB   (%d windows)\n", argv[a],
                   nsw ? sw[nsw / 2] : 0.0, nsw);
        }
        free(sw);
        free(env);
        free(bp);
        free(x);
    }
}

/* ------------------------------------------------------------------------ */
/* irfit - the post-stage matching as one impulse, fitted                     */
/* ------------------------------------------------------------------------ */

/*
 * Fit the impulse response that carries the loudspeaker *and* the correction,
 * which is the only thing README.md's first section allows after the stages.
 *
 *   tube_render irfit target.wav ours.wav cab.wav out.wav [taps [iters]]
 *
 *   target   the reference through a cabinet: what it should sound like
 *   ours     our chain through the same cabinet with the post bank off
 *   cab      that cabinet's own impulse, at the same rate (see `cabify`)
 *   out      the fitted impulse: the correction convolved with the cabinet
 *
 * Why it is built this way rather than as a deconvolution.  Three reasons, and
 * the third is the one that decided it:
 *
 *   - **The cabinet's own response has to stay exact.**  A speaker falls away
 *     above 5 kHz by tens of decibels, and asking a fitted filter to synthesise
 *     that near Nyquist puts its accuracy where the bilinear transform's warping
 *     is worst.  Convolving with the real impulse keeps that part measured, and
 *     the fit then only has to explain the difference between two amplifiers,
 *     which is under ten decibels.
 *   - **Minimum phase for free.**  Every section here is minimum phase, so the
 *     cascade is, so the impulse is - no cepstrum and no Hilbert transform, and
 *     no pre-ringing in front of an edge, which ag_biq.h explains is not a
 *     preference.
 *   - **It reuses the analyser that is already trusted.**  The bands are the same
 *     third-octave bandpass pairs `spectrum_of` uses, so a number printed here
 *     means what a number printed by `fit` or `spec` means.
 *
 * The band list is wider than the analyser's, and that is the one thing this had
 * to add: `fit` stops at 6.3 kHz because that is where a guitar's own energy
 * stops mattering, while an impulse left flat above its last fitted band hands
 * back the top octave the loudspeaker is there to remove.  So it runs to 9 kHz,
 * which at 22.05 kHz is as far as 0.45*fs allows.
 */
#define IRFIT_N 24
static const float k_irfit_f[IRFIT_N] = {
    50.0f,   63.0f,   80.0f,   100.0f,  125.0f,  160.0f,  200.0f,  250.0f,
    315.0f,  400.0f,  500.0f,  630.0f,  800.0f,  1000.0f, 1250.0f, 1600.0f,
    2000.0f, 2500.0f, 3150.0f, 4000.0f, 5000.0f, 6300.0f, 8000.0f, 9000.0f
};
#define IRFIT_REF 13 /* the 1 kHz band, which everything is relative to */

/* Band levels in dB relative to 1 kHz - the same two cascaded bandpasses as
 * spectrum_of, on this file's own wider list. */
static void irfit_bands(const float *x, int n, float rate, double *out)
{
    int i;
    for (i = 0; i < IRFIT_N; i++) {
        ag_biq_t a, b;
        double   s = 0.0;
        int      k;
        if (k_irfit_f[i] >= rate * 0.45f) {
            out[i] = -300.0;
            continue;
        }
        (void)ag_biq_bandpass(&a, rate, k_irfit_f[i], 0.333f);
        (void)ag_biq_bandpass(&b, rate, k_irfit_f[i], 0.333f);
        for (k = 0; k < n; k++) {
            const float v = ag_biq_tick(&b, ag_biq_tick(&a, x[k]));
            s += (double)v * (double)v;
        }
        out[i] = 10.0 * log10(s / (double)n + 1e-30);
    }
    {
        const double ref = out[IRFIT_REF];
        for (i = 0; i < IRFIT_N; i++) {
            if (out[i] > -299.0) {
                out[i] -= ref;
            }
        }
    }
}

/*
 * The correction as an impulse: a delta through one peaking section a band.
 *
 * Q 4.0, which is about what a third-octave spacing wants: at that width
 * neighbouring sections cross three decibels down.  The same trade `fit` records
 * at Q 1 for its octave-wide bands -
 * narrower leaves gaps between the bands that nothing can reach, wider makes
 * every band fight its neighbours and the iteration oscillate.
 */
static void irfit_render(const double *g, float *h, int n, float rate)
{
    ag_biq_t s[IRFIT_N];
    int      i, k, live = 0;

    for (i = 0; i < IRFIT_N; i++) {
        if (k_irfit_f[i] < rate * 0.45f && (g[i] > 0.005 || g[i] < -0.005)) {
            (void)ag_biq_peak(&s[live], rate, k_irfit_f[i], (float)g[i], 4.0f);
            live++;
        }
    }
    for (k = 0; k < n; k++) {
        h[k] = 0.0f;
    }
    h[0] = 1.0f;
    for (i = 0; i < live; i++) {
        for (k = 0; k < n; k++) {
            h[k] = ag_biq_tick(&s[i], h[k]);
        }
    }
}

/*
 * The impulse fit itself, on signals rather than filenames - same reason
 * fit_banks is: `match` has all three of these in memory already, and writing
 * them out to reach this code would put a 16-bit quantisation between the
 * amplifier and the impulse being fitted to it.
 *
 * WHAT EACH ARGUMENT HAS TO BE, BECAUSE GETTING IT WRONG IS SILENT
 *
 * `tgt` is the reference **with the speaker on it**.  `ours` is this chain with
 * the post bank at zero and **through the same cabinet**.  `cab` is that cabinet.
 *
 * The middle one is the trap.  What this fits is the *residual* between two
 * signals that already both have a loudspeaker in them, and then it convolves the
 * residual with the cabinet to make one impulse that carries both.  Hand it a
 * render with no cabinet and the residual comes out containing the whole speaker
 * response, which then gets applied twice: measured that way, the fitted impulse
 * played +17 dB at 100 Hz and -12 dB at 4 kHz against the bank it was supposed to
 * replace, which is a 4x12 heard through a second 4x12.
 */
static float *fit_impulse(const float *tgt, uint32_t tn, const float *ours,
                          uint32_t on, const float *cab, uint32_t cn,
                          uint32_t rate, int taps, int iters,
                          const char *out_path, const char *tgt_label,
                          const char *ours_label, const char *cab_label)
{
    const uint32_t trate = rate;
    const uint32_t orate = rate;
    float         *h, *out;
    double         want[IRFIT_N], got[IRFIT_N], g[IRFIT_N], best[IRFIT_N];
    double         base[IRFIT_N], tb[IRFIT_N], ob[IRFIT_N];
    double         best_rms = 1e30;
    int            best_it = -1;
    int            i, it, k;

    h = (float *)calloc((size_t)(taps > 8 ? taps : 8), sizeof(float));
    out = (float *)calloc((size_t)taps + cn + 8u, sizeof(float));
    if (h == NULL || out == NULL) {
        /* The three inputs belong to the caller now that this takes arrays. */
        free(h);
        free(out);
        return NULL;
    }

    printf("Fitting one impulse to carry the cabinet and the correction.\n\n"
           "  target  %s (%u frames)\n  ours    %s (%u frames)\n"
           "  cabinet %s (%u taps)\n  %u Hz, %d taps out, %d iterations\n\n",
           tgt_label, tn, ours_label, on, cab_label, cn, trate, taps,
           iters);

    /*
     * The two spectra stay in their own arrays and `want` holds the *difference*,
     * which is what the loop below is fitting.
     *
     * Written out like this because the first version was not: `want` held the
     * target's own spectrum and only the initial gains got the difference, so
     * every pass after the first was fitting the impulse to the shape of a guitar
     * through a Bogner rather than to the correction between two amplifiers.  It
     * converged, reported 1.29 dB rms against its own idea of the target, and the
     * render came out 6.65 dB off - worse than no correction at all, which is the
     * only reason it was caught.  A fit can always agree with itself.
     */
    irfit_bands(tgt, (int)tn, (float)trate, tb);
    irfit_bands(ours, (int)on, (float)orate, ob);
    printf("      Hz   target     ours   wanted\n");
    for (i = 0; i < IRFIT_N; i++) {
        const double w = (tb[i] < -299.0 || ob[i] < -299.0) ? -300.0
                                                            : tb[i] - ob[i];
        want[i] = w;
        g[i] = w < -299.0 ? 0.0 : (w > 18.0 ? 18.0 : (w < -18.0 ? -18.0 : w));
        best[i] = g[i];
        printf("  %6.0f  %+6.1f   %+6.1f   %+6.1f%s\n", (double)k_irfit_f[i],
               tb[i], ob[i], w < -299.0 ? 0.0 : w,
               (w > -299.0 && w != g[i]) ? "  clamped" : "");
    }
    /* What the fit is being asked to close, before it starts: the difference
     * itself, with its own mean taken out for the reason given below. */
    {
        double s = 0.0, mean = 0.0;
        int    nb = 0;
        for (i = 0; i < IRFIT_N; i++) {
            if (want[i] > -299.0) {
                mean += want[i];
                nb++;
            }
        }
        mean = nb ? mean / (double)nb : 0.0;
        for (i = 0; i < IRFIT_N; i++) {
            if (want[i] > -299.0) {
                s += (want[i] - mean) * (want[i] - mean);
            }
        }
        printf("\n  before: %.2f dB rms over %d bands\n", sqrt(s / (nb ? nb : 1)),
               nb);
    }

    /*
     * The baseline: what a bare delta measures on these bands.
     *
     * Not a detail, and it cost a wasted fit to find.  A third-octave band is
     * proportionally wide, so a *flat* impulse carries three decibels more energy
     * in each octave than in the one below - measure a delta and the analyser
     * reports a rising response that is not there.  For two signals the tilt
     * cancels in the difference, which is why `fit` never had to know; for an
     * impulse it does not, and the iteration spent its first attempt trying to
     * equalise the analyser instead of the filter (+20 dB at 100 Hz, alternating
     * sign between neighbours, 3.35 dB rms and going nowhere).
     */
    {
        double zero[IRFIT_N];
        for (i = 0; i < IRFIT_N; i++) {
            zero[i] = 0.0;
        }
        irfit_render(zero, h, taps, (float)trate);
        irfit_bands(h, taps, (float)trate, base);
    }

    /*
     * Iterated for the reason `fit` is iterated: the sections overlap, so a band
     * asked for three decibels does not arrive as three.  Under-relaxed at 0.6,
     * and each pass measures the impulse it has just built rather than trusting
     * the gains it set.  The best pass is kept, not the last - both of the
     * voicing fits overshot their own minimum and this one can too.
     */
    for (it = 0; it < iters; it++) {
        double s = 0.0, worst = 0.0;
        int    nb = 0;
        irfit_render(g, h, taps, (float)trate);
        irfit_bands(h, taps, (float)trate, got);
        /* The analyser's own tilt out of the way first - see the note on `base`. */
        for (i = 0; i < IRFIT_N; i++) {
            if (want[i] > -299.0 && got[i] > -299.0) {
                got[i] -= base[i];
            }
        }
        /* And the score is about the *shape*: the mean error is a level, the
         * impulse is normalised on the way out, so scoring it would rank two
         * identical filters differently for the volume they were written at. */
        {
            double mean = 0.0;
            for (i = 0; i < IRFIT_N; i++) {
                if (want[i] > -299.0 && got[i] > -299.0) {
                    mean += want[i] - got[i];
                    nb++;
                }
            }
            mean = nb ? mean / (double)nb : 0.0;
            for (i = 0; i < IRFIT_N; i++) {
                double e;
                if (want[i] < -299.0 || got[i] < -299.0) {
                    continue;
                }
                e = want[i] - got[i] - mean;
                s += e * e;
                if (e > worst || -e > worst) {
                    worst = e > 0.0 ? e : -e;
                }
            }
        }
        s = sqrt(s / (nb ? nb : 1));
        if (s < best_rms) {
            best_rms = s;
            best_it = it;
            for (i = 0; i < IRFIT_N; i++) {
                best[i] = g[i];
            }
        }
        printf("  pass %2d: %.2f dB rms, worst %.1f dB\n", it, s, worst);
        /*
         * The common part of the error is dropped before it is applied, and
         * without that this diverges.
         *
         * Both spectra are measured relative to their own 1 kHz, so a correction
         * that lifts the bands *around* 1 kHz lifts 1 kHz too through their
         * skirts - which reads as every other band having fallen, so every gain
         * goes up, which lifts 1 kHz further.  Measured: it improved to 1.82 dB
         * rms by pass 12 and was back at 8.07 by pass 29, with +20 dB of gain in
         * the low midrange and climbing.  The overall level of an impulse means
         * nothing here - it is normalised on the way out and the render trims to
         * -1 dBFS after that - so the mean error is not information and acting on
         * it is the whole instability.
         */
        {
            double mean = 0.0;
            int    nlive = 0;
            for (i = 0; i < IRFIT_N; i++) {
                if (want[i] > -299.0 && got[i] > -299.0) {
                    mean += want[i] - got[i];
                    nlive++;
                }
            }
            mean = nlive ? mean / (double)nlive : 0.0;
            for (i = 0; i < IRFIT_N; i++) {
                if (want[i] < -299.0 || got[i] < -299.0) {
                    continue;
                }
                g[i] += 0.6 * (want[i] - got[i] - mean);
                if (g[i] > 24.0) {
                    g[i] = 24.0;
                }
                if (g[i] < -24.0) {
                    g[i] = -24.0;
                }
            }
        }
        /*
         * And smoothed across the bands, which is the whole difference between a
         * filter and a set of numbers that satisfies a measurement.
         *
         * Twenty-four peaking sections a third of an octave apart overlap heavily,
         * so the band errors do not determine the gains: without this the fit
         * found +6.8 dB at 50 Hz, -11.1 at 63 and +12.5 at 80, three neighbours
         * cancelling each other into the right band energies and a response
         * between the centres that nobody measured.  A three-tap smoothing on the
         * gain vector removes that family of solutions and leaves the one that is
         * also smooth - which is what a loudspeaker and an amplifier both are.
         */
        {
            double sm[IRFIT_N];
            for (i = 0; i < IRFIT_N; i++) {
                const double lo = i > 0 ? g[i - 1] : g[i];
                const double hi = i + 1 < IRFIT_N ? g[i + 1] : g[i];
                sm[i] = 0.25 * lo + 0.5 * g[i] + 0.25 * hi;
            }
            for (i = 0; i < IRFIT_N; i++) {
                g[i] = sm[i];
            }
        }
    }
    printf("\n  best at pass %d, %.2f dB rms.  What it settled on, and what that\n"
           "  impulse then measures - the last two columns are the check that\n"
           "  matters, because a fit can only ever agree with its own analyser:\n",
           best_it, best_rms);
    irfit_render(best, h, taps, (float)trate);
    irfit_bands(h, taps, (float)trate, got);
    printf("      Hz    gain    analyser    realised    wanted\n");
    for (i = 0; i < IRFIT_N; i++) {
        if (k_irfit_f[i] < (float)trate * 0.45f) {
            printf("  %6.0f  %+6.2f    %+7.2f    %+8.2f  %+8.2f\n",
                   (double)k_irfit_f[i], best[i], base[i], got[i] - base[i],
                   want[i]);
        }
    }

    /* And through the cabinet, which is where the speaker's own response and its
     * phase come from rather than from anything fitted. */
    for (k = 0; k < taps; k++) {
        uint32_t j;
        if (h[k] == 0.0f) {
            continue;
        }
        for (j = 0; j < cn; j++) {
            out[k + (int)j] += h[k] * cab[j];
        }
    }
    /*
     * Truncated to `taps`, and the cost of that is printed rather than assumed:
     * everything past the window is energy the render will not have.
     */
    {
        double kept = 0.0, lost = 0.0, pk = 0.0;
        for (k = 0; k < taps; k++) {
            const double a = out[k] < 0.0f ? -(double)out[k] : (double)out[k];
            kept += (double)out[k] * (double)out[k];
            if (a > pk) {
                pk = a;
            }
        }
        for (k = taps; k < taps + (int)cn; k++) {
            lost += (double)out[k] * (double)out[k];
        }
        printf("\n  tail past %d taps: %.1f dB of the total\n", taps,
               10.0 * log10(lost / (kept + lost + 1e-300) + 1e-300));
        /* A short fade rather than a cliff, so the truncation is not a step. */
        for (k = taps - 128; k < taps; k++) {
            if (k > 0) {
                const double w = 0.5 * (1.0 + cos(3.14159265358979 *
                                                  (double)(k - (taps - 128)) /
                                                  128.0));
                out[k] = (float)((double)out[k] * w);
            }
        }
        if (pk > 1e-12) {
            const float gg = (float)(0.891 / pk);
            for (k = 0; k < taps; k++) {
                out[k] *= gg;
            }
        }
    }
    (void)write_wav(out_path, out, (uint32_t)taps, trate);
    printf("  wrote %s (%d taps)\n", out_path, taps);

    free(h);
    /* Handed back rather than dropped: the caller is about to render through it
     * and say whether it beat the bank. */
    return out;
}

static void mode_irfit(int argc, char **argv)
{
    /* 200 ms at 22.05 kHz, and 0 or a negative means the same: a caller who
     * wants the default should not have to know what it is. */
    const int taps_in = argc > 6 ? atoi(argv[6]) : 0;
    const int taps = taps_in > 8 ? taps_in : 4410;
    const int iters_in = argc > 7 ? atoi(argv[7]) : 0;
    const int iters = iters_in > 0 ? iters_in : 20;
    float    *tgt, *ours, *cab;
    uint32_t  tn = 0, on = 0, cn = 0, trate = 0, orate = 0, crate = 0;

    if (argc < 6) {
        printf("  usage: tube_render irfit target.wav ours.wav cab.wav out.wav"
               " [taps [iters]]\n"
               "    target: the reference, with a speaker on it\n"
               "    ours:   this chain with the post bank at zero, *through"
               " cab.wav* - `render` writes\n"
               "            that one as <out>_cab.wav\n"
               "    or let `match` do the whole thing, which has all three"
               " without files\n");
        return;
    }
    tgt = read_wav(argv[2], &tn, &trate);
    ours = read_wav(argv[3], &on, &orate);
    cab = read_wav(argv[4], &cn, &crate);
    if (tgt == NULL || ours == NULL || cab == NULL) {
        free(tgt);
        free(ours);
        free(cab);
        return;
    }
    if (trate != orate || trate != crate) {
        printf("  %u, %u and %u Hz: put them on one rate first\n", trate, orate,
               crate);
    } else {
        free(fit_impulse(tgt, tn, ours, on, cab, cn, trate, taps, iters, argv[5],
                         argv[2], argv[3], argv[4]));
    }
    free(tgt);
    free(ours);
    free(cab);
}
/* ------------------------------------------------------------------------ */
/* tonefilt - the post-valve bank on its own, so it can be moved             */
/* ------------------------------------------------------------------------ */

/*
 * A file through nothing but `cfg.tone`, the bank that sits after the valves.
 *
 * It exists to answer one question with a measurement instead of an argument:
 * **post-stage matching has to live somewhere, and there are two candidate
 * places** - seven biquads in the audio path, which is where it is now, or folded
 * into the cabinet impulse, which is the only position after the stages that the
 * architecture in this file's first section allows.
 *
 * Everything from the last plate onwards is linear, so `chain -> eq -> cab` and
 * `chain -> cab -> eq` are the same filter on paper, and `cab (*) eq` as one
 * impulse is the same again.  On paper.  The convolution is int16 in and
 * block-float inside, with a partition every 256 taps, so what this makes
 * possible is measuring what the arithmetic does to a boost of sixteen decibels
 * at 3 kHz depending on which side of it that boost is applied.
 *
 * Feed it the cabinet's own impulse response and the output is the combined
 * impulse:
 *
 *   tube_render cabify delta.wav cab_ir.wav        the speaker alone
 *   AG_MODEL=bogner tube_render tonefilt cab_ir.wav cab_plus_eq.wav
 *   AG_NO_TONE=1 AG_CAB_IR=cab_plus_eq.wav tube_render render ...
 */
static void mode_tonefilt(int argc, char **argv)
{
    static const float probe_hz[] = { 100.0f, 400.0f, 1000.0f, 2000.0f,
                                      3150.0f, 5000.0f, 8000.0f };
    ag_amp_cfg_t       cfg;
    ag_amp_t          *a;
    float             *x;
    uint32_t           frames = 0, rate = 0, i;
    double             pk = 0.0;

    if (argc < 4) {
        printf("  usage: tube_render tonefilt in.wav out.wav\n");
        return;
    }
    x = read_wav(argv[2], &frames, &rate);
    a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    if (x == NULL || a == NULL) {
        free(x);
        free(a);
        return;
    }
    print_model();
    /*
     * Built rather than designed by hand, and at the file's own rate: that way
     * the sections are bit for bit the ones the audio path would run, instead of
     * a second implementation of the same designers that can drift from them.
     * The bake it costs is thrown away.
     */
    model_cfg(&cfg, (float)rate);
    if (ag_amp_build(a, g_ckt, &cfg, g_tab, 0, g_probe, G_PROBE_N) != 0) {
        printf("  build failed\n");
        free(x);
        free(a);
        return;
    }
    printf("  %s: %u frames at %u Hz, %d sections in the post bank\n", argv[2],
           frames, rate, a->tone.n);
    printf("  what it applies:");
    for (i = 0; i < (uint32_t)(sizeof(probe_hz) / sizeof(probe_hz[0])); i++) {
        if (probe_hz[i] < 0.45f * (float)rate) {
            printf("  %.0f Hz %+.1f dB", (double)probe_hz[i],
                   (double)ag_biq_chain_mag_db(&a->tone, probe_hz[i],
                                               (float)rate));
        }
    }
    printf("\n");

    for (i = 0; i < frames; i++) {
        const float y = ag_biq_chain_tick(&a->tone, x[i]);
        const double m = y < 0.0f ? -(double)y : (double)y;
        x[i] = y;
        if (m > pk) {
            pk = m;
        }
    }
    /* Normalised, because sixteen decibels of presence on an impulse that was
     * already at -1 dBFS does not fit in the file, and an impulse's absolute
     * level means nothing: ag_ir normalises it again on load. */
    if (pk > 1e-9) {
        const float g = (float)(0.891 / pk);
        for (i = 0; i < frames; i++) {
            x[i] *= g;
        }
        printf("  scaled %+.1f dB to -1 dBFS\n", 20.0 * log10((double)g));
    }
    (void)write_wav(argv[3], x, frames, rate);
    printf("  wrote %s\n", argv[3]);
    free(x);
    free(a);
}

/* ------------------------------------------------------------------------ */
/* cabify - somebody else's render through our speaker                       */
/* ------------------------------------------------------------------------ */

/*
 * A file in, the same file through the cabinet out, and nothing else.
 *
 * The gap this fills appeared the moment a capture arrived without a speaker in
 * it.  A NAM capture whose `gear_type` is "amp" rather than "amp_cab" is a
 * preamp and power amp with no loudspeaker - measured on the two in the tree,
 * the top two octaves sit 3 to 9 dB under the midrange where a real 4x12 puts
 * them 24 to 42 dB under - so it cannot be listened to beside our chain, and it
 * cannot be *fitted* against either: `fit` compares band by band, and comparing
 * a chain that has a speaker with a reference that has none measures the
 * speaker.
 *
 * With this, both sides get the same one.  That is a better position than the
 * Marshall reference ever allowed: there the capture had a cabinet baked in and
 * ours had to be an impulse extracted from it, while here the choice of speaker
 * cancels out of the comparison exactly.
 */
static void mode_cabify(int argc, char **argv)
{
    float   *x;
    uint32_t frames = 0, rate = 0;

    if (argc < 4) {
        printf("  usage: tube_render cabify in.wav out.wav\n"
               "  (AG_CAB_IR picks the impulse; the same one the renders use)\n");
        return;
    }
    x = read_wav(argv[2], &frames, &rate);
    if (x == NULL) {
        return;
    }
    printf("  %s: %u frames at %u Hz\n", argv[2], frames, rate);
    if (run_cab(x, frames, rate) != 0) {
        printf("  no cabinet; point AG_CAB_IR at one\n");
        free(x);
        return;
    }
    /*
     * And normalised to -1 dBFS, the same as the render path does to its own
     * cabinet output.
     *
     * Not cosmetic.  A capture is trained at whatever level its author played at:
     * measured on the two here, after the same speaker the Bogner came out 4.6 dB
     * under full scale and the 5150 12.1 dB under, against our chain's 1.0.  Rake
     * 19 in docs/08 is what that does to a listening test - two files at different
     * levels are told apart by their levels, and the difference gets attributed to
     * whichever difference the listener was looking for.  The fit does not care,
     * because it works in bands relative to 1 kHz; ears do.
     */
    {
        uint32_t k;
        double   pk = 0.0;
        for (k = 0; k < frames; k++) {
            const double a = x[k] < 0.0f ? -(double)x[k] : (double)x[k];
            if (a > pk) {
                pk = a;
            }
        }
        if (pk > 1e-9) {
            const float g = (float)(0.891 / pk);
            for (k = 0; k < frames; k++) {
                x[k] *= g;
            }
            printf("  trimmed %+.1f dB to -1 dBFS\n", 20.0 * log10((double)g));
        }
    }
    /* run_cab works in whole blocks, so the tail that does not fill one is not
     * written rather than written half-convolved. */
    (void)write_wav(argv[3], x, frames - frames % AG_IR_BLOCK, rate);
    printf("  wrote %s (%u frames)\n", argv[3], frames - frames % AG_IR_BLOCK);
    free(x);
}

static void mode_buzz(int argc, char **argv)
{
    const int    fine = argc > 5 ? atoi(argv[5]) : 65536;
    ag_amp_cfg_t cfg;
    ag_amp_t    *a;
    float       *in, *coarse_out, *fine_out, *tab_c, *tab_f;
    uint32_t     frames = 0, rate = 0, i;
    int          pass;

    if (argc < 3) {
        printf("  usage: tube_render buzz in.wav [drive [top_hz [points]]]\n");
        return;
    }
    in = read_wav(argv[2], &frames, &rate);
    if (in == NULL) {
        return;
    }
    a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    coarse_out = (float *)malloc(sizeof(float) * frames);
    fine_out = (float *)malloc(sizeof(float) * frames);
    tab_c = (float *)malloc(sizeof(float) * AG_AMP_STAGES * 3 * AG_AMP_TAB_N);
    tab_f = (float *)malloc(sizeof(float) * (size_t)AG_AMP_STAGES * 3u *
                            (size_t)fine);
    if (a == NULL || coarse_out == NULL || fine_out == NULL || tab_c == NULL ||
        tab_f == NULL) {
        printf("  out of memory\n");
        goto done;
    }
    model_cfg(&cfg, (float)rate);
    if (argc > 3) {
        cfg.drive = (float)atof(argv[3]);
    }
    if (argc > 4) {
        cfg.top_hz = (float)atof(argv[4]);
    }
    printf("  %s at drive %.2f, top %.0f Hz, %dx%s: %d points against %d\n",
           argv[2], (double)cfg.drive, (double)cfg.top_hz, cfg.os,
           cfg.adaa ? "+adaa" : "", AG_AMP_TAB_N, fine);

    /*
     * Both passes fit their axes to the same signal and then *freeze* them, so
     * that the two tables span exactly the same range.  Without that the fit
     * converges slightly differently at each size and moves the clipping edges
     * by a fraction of a sample, which swamps what is being measured - the same
     * rake mode_res stepped on.
     */
    for (pass = 0; pass < 2; pass++) {
        const int n = pass == 0 ? AG_AMP_TAB_N : fine;
        float    *tab = pass == 0 ? tab_c : tab_f;
        float    *out = pass == 0 ? coarse_out : fine_out;
        const uint32_t pn = frames < rate * 10u ? frames : rate * 10u;
        if (ag_amp_build(a, g_ckt, &cfg, tab, n, in, (int)pn) != 0) {
            printf("  build failed at %d points\n", n);
            goto done;
        }
        if (pass == 0) {
            int k;
            for (k = 0; k < AG_AMP_STAGES; k++) {
                cfg.axis_lo[k] = a->tube[k].lo;
                cfg.axis_hi[k] = a->tube[k].hi;
            }
        }
        ag_amp_reset(a);
        for (i = 0; i < frames; i++) {
            out[i] = ag_amp_tick(a, in[i]);
        }
    }

    /* Band by band, the difference against the signal that is there. */
    {
        static const struct {
            double lo, hi;
        } bands[] = { { 200.0, 1500.0 },
                      { 1500.0, 2500.0 },
                      { 2500.0, 3500.0 },
                      { 3500.0, 6000.0 },
                      { 6000.0, 9000.0 },
                      { 9000.0, 11000.0 } };
        const int nb = (int)(sizeof(bands) / sizeof(bands[0]));
        double   *se = (double *)calloc((size_t)nb, sizeof(double));
        double   *ss = (double *)calloc((size_t)nb, sizeof(double));
        const int N = 8192;
        float    *w = (float *)malloc(sizeof(float) * (size_t)N);
        int       b, wins = 0;
        uint32_t  c;
        if (se == NULL || ss == NULL || w == NULL) {
            free(se);
            free(ss);
            free(w);
            goto done;
        }
        for (b = 0; b < N; b++) {
            w[b] = (float)(0.5 - 0.5 * cos(2.0 * PI * (double)b / (double)N));
        }
        for (c = 0; c + (uint32_t)N < frames; c += (uint32_t)N) {
            int k;
            for (b = 0; b < nb; b++) {
                const int k0 = (int)(bands[b].lo * (double)N / (double)rate);
                const int k1 = (int)(bands[b].hi * (double)N / (double)rate);
                double    es = 0.0, sg = 0.0;
                for (k = k0; k < k1; k++) {
                    double sr = 0.0, si = 0.0, er = 0.0, ei = 0.0;
                    int    t;
                    for (t = 0; t < N; t++) {
                        const double al =
                            -2.0 * PI * (double)k * (double)t / (double)N;
                        const double cs = cos(al), sn = sin(al);
                        const double sv = (double)(fine_out[c + (uint32_t)t] *
                                                   w[t]);
                        const double ev =
                            (double)((coarse_out[c + (uint32_t)t] -
                                      fine_out[c + (uint32_t)t]) *
                                     w[t]);
                        sr += sv * cs;
                        si += sv * sn;
                        er += ev * cs;
                        ei += ev * sn;
                    }
                    sg += sr * sr + si * si;
                    es += er * er + ei * ei;
                }
                ss[b] += sg;
                se[b] += es;
            }
            wins++;
            if (wins >= 6) {
                break; /* six windows is enough and this DFT is O(N^2) */
            }
        }
        printf("\n  band          signal   table error   error/signal\n");
        for (b = 0; b < nb; b++) {
            printf("  %5.0f-%5.0f Hz  %+7.1f  %+11.1f  %+13.1f dB\n",
                   bands[b].lo, bands[b].hi, db(sqrt(ss[b])), db(sqrt(se[b])),
                   db(sqrt(se[b] / (ss[b] + 1e-30))));
        }
        free(se);
        free(ss);
        free(w);
    }

    /* And the three files, because the difference is the thing to listen to. */
    {
        char p[512];
        double pk = peak_of(fine_out, (int)frames);
        float  g;
        if (pk < 1e-9) {
            pk = 1.0;
        }
        /* All three scaled by the same constant, so the difference file's level
         * is the difference's real level and not a normalised picture of it. */
        g = (float)(0.9 / pk);
        for (i = 0; i < frames; i++) {
            in[i] = (coarse_out[i] - fine_out[i]) * g;
            coarse_out[i] *= g;
            fine_out[i] *= g;
        }
        snprintf(p, sizeof(p), "build/listen/buzz_coarse.wav");
        (void)write_wav(p, coarse_out, frames, rate);
        snprintf(p, sizeof(p), "build/listen/buzz_fine.wav");
        (void)write_wav(p, fine_out, frames, rate);
        snprintf(p, sizeof(p), "build/listen/buzz_diff.wav");
        (void)write_wav(p, in, frames, rate);
        printf("\n  wrote build/listen/buzz_{coarse,fine,diff}.wav\n");
    }

done:
    free(tab_f);
    free(tab_c);
    free(fine_out);
    free(coarse_out);
    free(a);
    free(in);
}

static void mode_render(int argc, char **argv)
{
    ag_amp_cfg_t cfg;
    ag_amp_t    *a;
    float       *in, *out;
    uint32_t     frames = 0, rate = 0, i;

    if (argc < 4) {
        printf("  usage: tube_render render in.wav out.wav [drive [os [adaa]]]\n");
        return;
    }
    in = read_wav(argv[2], &frames, &rate);
    if (in == NULL) {
        return;
    }
    a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    out = (float *)malloc(sizeof(float) * frames);
    if (a == NULL || out == NULL) {
        free(in);
        free(a);
        free(out);
        return;
    }
    model_cfg(&cfg, (float)rate);
    if (argc > 4) {
        cfg.drive = (float)atof(argv[4]);
    }
    if (argc > 5) {
        cfg.os = atoi(argv[5]);
    }
    if (argc > 6) {
        cfg.adaa = atoi(argv[6]);
    }
    if (argc > 7) {
        g_cab = atoi(argv[7]);
    }
    if (argc > 8) {
        cfg.g12 = (float)atof(argv[8]);
    }
    if (argc > 9) {
        cfg.blocking = atoi(argv[9]);
    }
    if (argc > 10) {
        cfg.ccouple2 = (float)atof(argv[10]) * 1e-9f;
    }
    if (argc > 11) {
        cfg.n_stages = atoi(argv[11]);
    }
    print_model();
    if (cfg.drive > 1.0f) {
        printf("  note: drive %.2f is past the top of the range; 1.0 is the"
               " maximum and 0.5 the working setting\n",
               (double)cfg.drive);
    }
    /* The two voicing knobs tube_live has and this argument list does not.
     * Environment rather than an eleventh positional argument, because they are
     * for reproducing a listening report, not for everyday renders. */
    {
        const char *e;
        if ((e = getenv("AG_TOP_HZ")) != NULL) {
            cfg.top_hz = (float)atof(e);
            printf("  AG_TOP_HZ: top cut %.0f Hz\n", (double)cfg.top_hz);
        }
        if ((e = getenv("AG_MID_DB")) != NULL) {
            cfg.mid_db = (float)atof(e);
            printf("  AG_MID_DB: mid %.1f dB\n", (double)cfg.mid_db);
        }
        /*
         * The divider in front of the third stage, which g12 is for the second.
         * On a three-valve chain this is the control that decides whether the
         * last stage is a valve or a comparator: 150 V of plate swing reaches a
         * grid that cuts off at -6 V, so what it is divided by is the whole
         * character of the stage.  Named for the two stages it sits between.
         */
        if ((e = getenv("AG_G23")) != NULL) {
            cfg.gain[2] = (float)atof(e);
            printf("  AG_G23: %.3f into the third stage\n", (double)cfg.gain[2]);
        }
        /*
         * The two fitted banks, off - which is how the bare nonlinearity is
         * compared against ckt_exact, since that reference has no voicing.  Also
         * the only way to ask how much of what is heard in a band is what the
         * valves made and how much is what the fit did to it afterwards.
         */
        if (getenv("AG_TONE_SHELF") != NULL) {
            cfg.tone_shelf = 1;
            printf("  AG_TONE_SHELF: the tone stack's top bands as shelves\n");
        }
        if ((e = getenv("AG_TONE_Q")) != NULL) {
            int b;
            for (b = 0; b < AG_AMP_VOICE_N; b++) {
                cfg.tone[b].q = (float)atof(e);
            }
            printf("  AG_TONE_Q: %.2f\n", atof(e));
        }
        if (getenv("AG_NO_TONE") != NULL) {
            int b;
            for (b = 0; b < AG_AMP_VOICE_N; b++) {
                cfg.tone[b].db = 0.0f;
            }
            printf("  AG_NO_TONE: the post bank is off\n");
        }
        /*
         * The first valve back to stock, which is the measurement that says
         * whether the hash at 3-6 kHz is the circuit or the hot-rodding of it.
         */
        if (getenv("AG_V1A_STOCK") != NULL) {
            cfg.rcath1 = 2700.0f;
            cfg.rplate1 = 100.0e3f;
            printf("  AG_V1A_STOCK: V1a at 2k7 / 100k instead of 820R / 220k\n");
        }
        if ((e = getenv("AG_RCATH1")) != NULL) {
            cfg.rcath1 = (float)atof(e);
        }
        if ((e = getenv("AG_RPLATE1")) != NULL) {
            cfg.rplate1 = (float)atof(e);
        }
        if (getenv("AG_NO_VOICE") != NULL) {
            int b;
            for (b = 0; b < AG_AMP_VOICE_N; b++) {
                cfg.voice[0][b].db = 0.0f;
            }
            cfg.mid_db = 0.0f;
            printf("  AG_NO_VOICE: the pre bank and the mid lift are off\n");
        }
    }
    /*
     * The axes are fitted to this recording, not to the synthetic probe.
     *
     * That is not a nicety.  Fitted to the probe, the second valve's axis came
     * out -3.8..+32.5 V while the take drove it to -36.6..+51.6 - so the model
     * spent the loudest peaks sitting on the end of its table.  A synthetic sum
     * of harmonics has neither the spectrum nor the dynamics of a guitar, and
     * docs/08 records the same mistake twice: an axis fitted to one signal and
     * used on another.
     *
     * Up to ten seconds of it, which is enough to see the range and short enough
     * that three fitting passes stay quick.
     */
    if (load_preset(a, g_tab, (float)rate) != 0) {
        const uint32_t pn = frames < rate * 10u ? frames : rate * 10u;
        if (ag_amp_build(a, g_ckt, &cfg, g_tab, 0, in, (int)pn) != 0) {
            printf("  build failed\n");
            free(in);
            free(a);
            free(out);
            return;
        }
    } else {
        /* The preset carries its own voicing and its own axes; only the knobs
         * the command line named are pushed on top of it. */
        ag_amp_cfg_t k = a->cfg;
        k.drive = cfg.drive;
        k.os = cfg.os;
        k.adaa = cfg.adaa;
        k.g12 = cfg.g12;
        k.blocking = cfg.blocking;
        (void)ag_amp_set_voicing(a, &k);
    }
    printf("  %s: %u frames at %u Hz, drive %.2f, %dx%s\n", argv[2], frames, rate,
           (double)cfg.drive, cfg.os, cfg.adaa ? " + adaa" : "");
    for (i = 0; i < frames; i++) {
        out[i] = ag_amp_tick(a, in[i]);
    }
    printf("  peak out %.3f, clamped %lu of %lu lookups\n", (double)a->peak_out,
           (unsigned long)ag_amp_clamped(a),
           (unsigned long)stage_lookups(a));
    if (a->peak_out > 1.0e-6f) {
        printf("  master 1/%.0f would have peaked at -1 dBFS (it is 1/%.0f)\n",
               (double)(1.0f / (a->cfg.master * 0.891f / a->peak_out)),
               (double)(1.0f / a->cfg.master));
    }
    /*
     * Every live stage, not the first two - a third one whose axis is off by
     * three volts is invisible in a two-stage report, and "how far off is the
     * axis" is the number this whole line exists for.  Read the two ranges
     * together: seen wider than the axis means the run spent time on the end of
     * the table, which the clamped count above prices.
     */
    for (i = 0; i < (uint32_t)a->n; i++) {
        printf("  %s saw %.3f..%.3f V of an axis of %.3f..%.3f\n",
               valve_name((int)i),
               (double)a->tube[i].seen_lo, (double)a->tube[i].seen_hi,
               (double)a->tube[i].lo, (double)a->tube[i].hi);
    }
    (void)write_wav(argv[3], out, frames, rate);

    /* And the same through the cabinet, which is where the chain actually ends. */
    if (run_cab(out, frames, rate) == 0) {
        char path[512];
        int  n = 0;
        /*
         * A cabinet impulse has whatever gain it has - this one is +5 dB in the
         * low end - so the level after it is not the level before it, and the
         * first render through a working cabinet came out 0.7 dB over full scale.
         * Trimmed to -1 dBFS and the trim printed, because it belongs to the
         * impulse rather than to the amplifier.
         */
        {
            const double pk = peak_of(out, (int)(frames - frames % AG_IR_BLOCK));
            if (pk > 1e-9) {
                const float g = (float)(0.891 / pk);
                uint32_t    k;
                for (k = 0; k < frames; k++) {
                    out[k] *= g;
                }
                printf("  cabinet output trimmed %+.1f dB to -1 dBFS\n",
                       20.0 * log10((double)g));
            }
        }
        while (argv[3][n] != '\0' && n < 500) {
            path[n] = argv[3][n];
            n++;
        }
        if (n > 4) { /* drop ".wav" */
            n -= 4;
        }
        path[n] = '\0';
        strcat(path, "_cab.wav");
        (void)write_wav(path, out, frames - frames % AG_IR_BLOCK, rate);
    }
    free(in);
    free(out);
    free(a);
}

/* ------------------------------------------------------------------------ */
/* match - a capture in, a fitted voicing out                                */
/* ------------------------------------------------------------------------ */

/*
 * The whole tone match in one command:
 *
 *   AG_MODEL=bogner AG_CAB_IR=cab.wav tube_render match capture.nam di.wav
 *
 * It plays the capture over the take to make the reference, fits both voicing
 * banks to it, and then measures the one thing a spectrum fit cannot see.
 *
 * WHY THE REFERENCE IS MADE HERE
 *
 * It used to be three commands: wavrate up to 48 kHz, a Go binary that lived in a
 * temporary directory, wavrate back down.  Each of those wrote a 16-bit file, so
 * the amplifier being matched to arrived with two quantisations on it, and the
 * step in the middle was the most losable thing in the whole exercise.  Now the
 * take goes up, through the capture and back down in float, in this process.
 *
 * WHY THE COMPRESSION LINE IS PRINTED WHETHER OR NOT ANYBODY ASKED
 *
 * Because a third-octave fit will confidently match the spectrum of an amplifier
 * whose dynamics are nothing like the model's, and that mistake has been made in
 * this tree twice.  A voicing was fitted to a Marshall capture to 1.29 dB rms
 * while the chain compressed 10 dB less than the amplifier did: it measures well
 * and it does not play like the amplifier, because most of what a high gain
 * amplifier *is* is its dynamics.  So both are measured the same way - the same
 * take twice, twenty decibels apart, and the shortfall in what came back is the
 * compression - and the two numbers are printed side by side.
 *
 * If they differ by more than about two decibels, the fit above is decoration:
 * what has to change is the circuit - a stage's bias, a cathode bypass, the
 * divider in front of a cold clipper - and not the filters.
 */

/*
 * Third-octave rms between two spectra, over the bands the fit is judged on.
 *
 * Below 63 Hz is left out for the same reason `fit` leaves it out: down there the
 * cabinet decides everything and a guitar has almost nothing, so a decibel of
 * error at 50 Hz is not a decibel anybody hears.
 */
static double spec_rms(const double *a, const double *b)
{
    double err = 0.0;
    int    i, n = 0;
    for (i = 0; i < SPEC_N; i++) {
        if (k_spec_f[i] < 63.0f || a[i] < -299.0 || b[i] < -299.0) {
            continue;
        }
        err += (a[i] - b[i]) * (a[i] - b[i]);
        n++;
    }
    return sqrt(err / (double)(n ? n : 1));
}

/*
 * Energy of `a` over energy of `b` inside one third-octave band, in dB.
 *
 * Unnormalised on purpose: `spectrum_of` divides everything by its own 1 kHz band,
 * which is exactly right for comparing two *signals* and exactly wrong for asking
 * how big an error is next to the signal it sits on - do that with normalised
 * spectra and the 1 kHz column reads 0.0 dB no matter what.  (It did.)
 */
static double band_ratio_db(const float *a, const float *b, uint32_t n,
                            float rate, float f)
{
    ag_biq_t a1, a2, b1, b2;
    double   ea = 0.0, eb = 0.0;
    uint32_t i;

    if (f >= rate * 0.45f) {
        return -300.0;
    }
    (void)ag_biq_bandpass(&a1, rate, f, 0.333f);
    (void)ag_biq_bandpass(&a2, rate, f, 0.333f);
    (void)ag_biq_bandpass(&b1, rate, f, 0.333f);
    (void)ag_biq_bandpass(&b2, rate, f, 0.333f);
    for (i = 0; i < n; i++) {
        const double va = (double)ag_biq_tick(&a2, ag_biq_tick(&a1, a[i]));
        const double vb = (double)ag_biq_tick(&b2, ag_biq_tick(&b1, b[i]));
        ea += va * va;
        eb += vb * vb;
    }
    return 10.0 * log10(ea / (eb + 1e-300) + 1e-300);
}

/* Energy in dB, which is all either measurement needs. */
static double energy_db(const float *x, uint32_t n)
{
    double   e = 0.0;
    uint32_t i;
    for (i = 0; i < n; i++) {
        e += (double)x[i] * (double)x[i];
    }
    return 10.0 * log10(e / (double)(n ? n : 1u) + 1e-300);
}

/*
 * The take through the capture at a given input gain, at the capture's own rate.
 *
 * A fresh model for each pass rather than one played twice: the state carries
 * across a call, and while a take is long enough that a warmed-up start makes no
 * measurable difference, "makes no measurable difference" is exactly the sort of
 * thing that is easier to avoid than to defend.
 */
static float *capture_render(const char *path, const float *in48, uint32_t n48,
                             float gain, int verbose)
{
    nam_model_t *m = nam_load(path, 0, verbose);
    float       *x, *y;
    uint32_t     i;

    if (m == NULL) {
        printf("  %s: %s\n", path, nam_err());
        return NULL;
    }
    x = (float *)malloc(sizeof(float) * (n48 ? n48 : 1u));
    y = (float *)malloc(sizeof(float) * (n48 ? n48 : 1u));
    if (x == NULL || y == NULL) {
        nam_free(m);
        free(x);
        free(y);
        return NULL;
    }
    for (i = 0; i < n48; i++) {
        x[i] = in48[i] * gain;
    }
    for (i = 0; i + (uint32_t)NAM_BLOCK <= n48; i += (uint32_t)NAM_BLOCK) {
        nam_process(m, x + i, y + i, NAM_BLOCK);
    }
    if (i < n48) {
        nam_process(m, x + i, y + i, (int)(n48 - i));
    }
    nam_free(m);
    free(x);
    return y;
}

/*
 * THE THIRD CHARACTER NUMBER: SUBSONIC INTERMODULATION, WHICH IS WHAT FARTING IS
 *
 * Two tones a fourth apart - 147 and 196 Hz, an ordinary low double stop - and the
 * level of their difference product at 49 Hz, relative to the louder tone.  No
 * single tone can show this and no third-octave spectrum can separate it from the
 * music, which is why it had to be its own probe.
 *
 * It is the number that says whether a low chord farts, and it is the one where
 * this chain is worst against its captures: measured with `harm`, the Bogner puts
 * 49 Hz 42 to 58 dB under the tones and *quieter* as it is driven harder, while
 * this chain put it 33 to 39 dB down and *louder* with level.  The mechanism a real
 * design uses against it is a small bass cut before each hot stage rather than one
 * big cut in front - `cfg.couple_mul` is where that lives now, and this is how the
 * two arrangements are told apart.
 */
static double two_tone_imd(const ag_amp_cfg_t *cfg, uint32_t rate, float level,
                           const float *cab, uint32_t cn, double *tone_out)
{
    const double f1 = 146.83, f2 = 196.0, prod = f2 - f1;
    const uint32_t edge = (uint32_t)(0.02 * (double)rate);
    const uint32_t body = (uint32_t)(0.30 * (double)rate);
    const uint32_t n = 2u * edge + body;
    ag_amp_t      *a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    float         *x = (float *)malloc(sizeof(float) * n);
    float         *y = (float *)malloc(sizeof(float) * n);
    ag_amp_cfg_t   c = *cfg;
    double         out = -300.0;
    uint32_t       i;
    int            b;

    for (b = 0; b < AG_AMP_VOICE_N; b++) {
        c.tone[b].db = 0.0f; /* the same invariance the other two have */
    }
    if (tone_out != NULL) {
        *tone_out = -300.0;
    }
    if (a == NULL || x == NULL || y == NULL) {
        free(a); free(x); free(y);
        return out;
    }
    for (i = 0; i < n; i++) {
        const double t = (double)i / (double)rate;
        const double env = i < edge ? 0.5 - 0.5 * cos(PI * (double)i /
                                                      (double)edge)
                           : (i > edge + body
                                  ? 0.5 - 0.5 * cos(PI * (double)(n - i) /
                                                    (double)edge)
                                  : 1.0);
        x[i] = (float)(0.5 * (double)level * env *
                       (sin(2.0 * PI * f1 * t) + sin(2.0 * PI * f2 * t)));
    }
    if (ag_amp_build(a, g_ckt, &c, g_tab, 0, x, (int)n) == 0) {
        double tone;
        for (i = 0; i < n; i++) {
            y[i] = ag_amp_tick(a, x[i]);
        }
        /*
         * A cabinet if the thing being compared against has one, and none if it
         * does not.  At 49 Hz against 196 a loudspeaker is not a detail: it eats
         * the product and leaves the tone, so measuring one side through a speaker
         * and the other without it would be a comparison of speakers again.
         */
        if (cab != NULL && cn > 8u) {
            float *w = convolve_f(y, n, cab, cn);
            if (w != NULL) {
                free(y);
                y = w;
            }
        }
        tone = goertzel_db(y + edge, body, f2, (double)rate);
        out = goertzel_db(y + edge, body, prod, (double)rate) - tone;
        /*
         * The reference tone's own level goes back too, and it has to.
         *
         * The ratio alone is ambiguous: raising a coupling corner attenuates the
         * *tone* on its way into the stage, while the 49 Hz product is made after
         * that corner and meets only what is downstream of it - so the ratio can
         * get worse while the absolute product gets better.  Two numbers make the
         * two cases distinguishable; one number made them one confusing result,
         * which is how the coupling rows first read.
         */
        if (tone_out != NULL) {
            *tone_out = tone;
        }
    }
    free(a);
    free(x);
    free(y);
    return out;
}

/*
 * THE CHARACTER OF THE OVERDRIVE, IN TWO NUMBERS, NEITHER OF WHICH A FILTER
 * BEHIND THE VALVES CAN MOVE
 *
 * `comp` is what twenty decibels less input comes back as: the shortfall is the
 * compression.  `hash` is how much of the output sits above 2 kHz, where the DI has
 * almost nothing, so it is a measure of what the distortion *made* rather than of
 * what a filter passed.
 *
 * **The post bank is zeroed before either is measured, and that is a bug fix.**
 * Compression looks like it should be invariant to a linear filter after the
 * valves - the same gain applies to both levels and cancels - and it is not,
 * because the two levels do not have the same spectrum: the loud pass has
 * harmonics the quiet one does not, so a filter that is not flat weights them
 * differently.  Measured: +6 dB at 200 Hz *behind* the valves moved this number by
 * 0.7 dB, exactly as much as +6 dB in front of them did, which would have made the
 * one metric that is supposed to separate the two indifferent to the difference.
 *
 * What remains, and it cannot be helped: the *capture's* number is measured through
 * the real amplifier's own tone stack, because there is no way to take it out. So
 * the comparison is indicative, and a decibel of it is not worth an argument; four
 * are.
 */
static void chain_character(const ag_amp_cfg_t *cfg, const float *in, uint32_t n,
                            uint32_t rate, double *comp, double *hash)
{
    ag_amp_t    *a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    float       *out = (float *)malloc(sizeof(float) * (n ? n : 1u));
    ag_amp_cfg_t c = *cfg;
    double       loud = 0.0, quiet = 0.0;
    uint32_t     i;
    int          pass, b;

    *comp = 0.0;
    *hash = -300.0;
    for (b = 0; b < AG_AMP_VOICE_N; b++) {
        c.tone[b].db = 0.0f;
    }
    if (a == NULL || out == NULL) {
        free(a);
        free(out);
        return;
    }
    if (ag_amp_build(a, g_ckt, &c, g_tab, 0, in,
                     (int)(n < rate * 10u ? n : rate * 10u)) != 0) {
        free(a);
        free(out);
        return;
    }
    for (pass = 0; pass < 2; pass++) {
        const float g = pass == 0 ? 1.0f : 0.1f;
        ag_amp_reset(a);
        for (i = 0; i < n; i++) {
            out[i] = ag_amp_tick(a, in[i] * g);
        }
        if (pass == 0) {
            loud = energy_db(out, n);
            *hash = above_2k_db(out, n, rate);
        } else {
            quiet = energy_db(out, n);
        }
    }
    free(a);
    free(out);
    *comp = 20.0 - (loud - quiet);
}

/* No cabinet in either: it is linear and after everything, so it cancels in the
 * ratio, and leaving it out keeps this about the valves. */
static double chain_compression(const ag_amp_cfg_t *cfg, const float *in,
                                uint32_t n, uint32_t rate)
{
    double comp, hash;
    chain_character(cfg, in, n, rate, &comp, &hash);
    (void)hash;
    return comp;
}

/*
 * The cabinet, taken from the capture itself when nothing else was named.
 *
 * A capture of an "amp_cab" profile has the loudspeaker in it, and our chain
 * needs one of its own or the comparison is a bare plate against a speaker - a
 * twenty decibel difference in the top octave, which the fit would then spend
 * its whole budget on.  There is no separate impulse for most of these
 * amplifiers, so the impulse is taken from the capture the same way
 * build/nam/imp_mars.wav was: a delta in, 200 ms of what came out.
 *
 * It is honestly not a cabinet.  A delta through a *nonlinear* model is that
 * model's response at one level, and the smaller the delta the closer that comes
 * to being the linear part - at the cost of how far the useful tail reaches
 * before it is the model's own noise.  A tenth of full scale is the compromise,
 * and it is written into a file so that anybody can look at what was used.
 */
static float *cab_from_capture(const char *cap, uint32_t crate, const char *out,
                              uint32_t *out_n)
{
    const uint32_t n = (uint32_t)(0.2 * (double)crate);
    float         *d = (float *)calloc(n ? n : 1u, sizeof(float));
    float         *y;
    double         pk = 0.0;
    uint32_t       i;

    *out_n = 0;
    if (d == NULL) {
        return NULL;
    }
    d[0] = 0.1f;
    y = capture_render(cap, d, n, 1.0f, 0);
    free(d);
    if (y == NULL) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        const double a = y[i] < 0.0f ? -(double)y[i] : (double)y[i];
        if (a > pk) {
            pk = a;
        }
    }
    if (pk > 1e-12) {
        for (i = 0; i < n; i++) {
            y[i] = (float)((double)y[i] / pk * 0.95);
        }
    }
    if (write_wav(out, y, n, crate) != 0) {
        free(y);
        return NULL;
    }
    *out_n = n;
    return y;
}

/* This chain, rendered dry - no cabinet, and optionally with the post bank at
 * zero, which is what an impulse fitted to replace that bank has to be fitted
 * against. */
static float *chain_render_dry(const ag_amp_cfg_t *cfg, const float *in,
                              uint32_t n, uint32_t rate, int post_off)
{
    ag_amp_t    *a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    float       *out = (float *)malloc(sizeof(float) * (n ? n : 1u));
    ag_amp_cfg_t c = *cfg;
    uint32_t     i;

    if (post_off) {
        for (i = 0; i < (uint32_t)AG_AMP_VOICE_N; i++) {
            c.tone[i].db = 0.0f;
        }
    }
    if (a == NULL || out == NULL ||
        ag_amp_build(a, g_ckt, &c, g_tab, 0, in,
                     (int)(n < rate * 10u ? n : rate * 10u)) != 0) {
        free(a);
        free(out);
        return NULL;
    }
    for (i = 0; i < n; i++) {
        out[i] = ag_amp_tick(a, in[i]);
    }
    free(a);
    return out;
}

/*
 * Exact convolution in double, by FFT, because this is the analysis path.
 *
 * WHY NOT ag_ir, WHICH IS RIGHT THERE
 *
 * Because ag_ir is int16 and this is where a filter gets *fitted*.  The engine's
 * convolution is exact - a delta through it comes back at 0.00 dB in every
 * third-octave band, and so does pink noise - but the level going into it is
 * staged with one scale for the whole take, so a decaying note enters eleven bits
 * down and comes back with flat quantisation noise on it.  Normalised to 1 kHz
 * that measures as a 2 dB bass deficit and 1.18 dB rms of nothing.  A fit that
 * sees it will correct it, and there is nothing there to correct.
 *
 * WHY FFT AND NOT THE OBVIOUS DOUBLE LOOP
 *
 * The obvious loop was here first and cost 770 million multiplies on an eight
 * second take; the fit does one per iteration and the verdicts do four more, so it
 * was minutes.  Radix-2, one transform of the impulse reused across the whole
 * signal, overlap-add in blocks of half the transform.  Own code rather than
 * ag_fft because that one is fixed point for the chip, which is the thing being
 * measured.
 */
static void fft_radix2(double *re, double *im, uint32_t n, int inverse)
{
    uint32_t i, j = 0, len;

    for (i = 1; i < n; i++) { /* bit reversal */
        uint32_t bit = n >> 1;
        for (; (j & bit) != 0u; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * PI / (double)len * (inverse ? 1.0 : -1.0);
        const double wr = cos(ang), wi = sin(ang);
        for (i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            uint32_t k;
            for (k = 0; k < len / 2u; k++) {
                const double ur = re[i + k], ui = im[i + k];
                const double vr = re[i + k + len / 2u] * cr -
                                  im[i + k + len / 2u] * ci;
                const double vi = re[i + k + len / 2u] * ci +
                                  im[i + k + len / 2u] * cr;
                double nr;
                re[i + k] = ur + vr;
                im[i + k] = ui + vi;
                re[i + k + len / 2u] = ur - vr;
                im[i + k + len / 2u] = ui - vi;
                nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
    if (inverse) {
        for (i = 0; i < n; i++) {
            re[i] /= (double)n;
            im[i] /= (double)n;
        }
    }
}

static float *convolve_f(const float *x, uint32_t n, const float *h,
                        uint32_t hn)
{
    uint32_t nfft = 1u, blk, pos, i;
    double  *hr = NULL, *hi = NULL, *xr = NULL, *xi = NULL;
    float   *y = (float *)calloc(n ? n : 1u, sizeof(float));

    if (y == NULL || x == NULL || h == NULL || hn == 0u) {
        free(y);
        return NULL;
    }
    /* A transform four times the impulse: blocks of 3/4 of it would work, half is
     * simpler and the cost of the extra transforms is nothing here. */
    while (nfft < 4u * hn) {
        nfft <<= 1;
    }
    blk = nfft / 2u;
    hr = (double *)calloc(nfft, sizeof(double));
    hi = (double *)calloc(nfft, sizeof(double));
    xr = (double *)calloc(nfft, sizeof(double));
    xi = (double *)calloc(nfft, sizeof(double));
    if (hr == NULL || hi == NULL || xr == NULL || xi == NULL) {
        free(hr); free(hi); free(xr); free(xi); free(y);
        return NULL;
    }
    for (i = 0; i < hn; i++) {
        hr[i] = (double)h[i];
    }
    fft_radix2(hr, hi, nfft, 0);

    for (pos = 0; pos < n; pos += blk) {
        const uint32_t m = (n - pos < blk) ? n - pos : blk;
        for (i = 0; i < nfft; i++) {
            xr[i] = (i < m) ? (double)x[pos + i] : 0.0;
            xi[i] = 0.0;
        }
        fft_radix2(xr, xi, nfft, 0);
        for (i = 0; i < nfft; i++) {
            const double ar = xr[i], ai = xi[i];
            xr[i] = ar * hr[i] - ai * hi[i];
            xi[i] = ar * hi[i] + ai * hr[i];
        }
        fft_radix2(xr, xi, nfft, 1);
        /* Overlap-add: m + hn - 1 samples come out of a block of m. */
        for (i = 0; i < m + hn - 1u && pos + i < n; i++) {
            y[pos + i] += (float)xr[i];
        }
    }
    free(hr); free(hi); free(xr); free(xi);
    return y;
}

/*
 * The held-out take: `AG_EVAL_DI`, or the other DI take in the tree, or nothing.
 *
 * `fit_di` is the take the fit used, and it is refused here - held out means held
 * out, and the one way this check can quietly become worthless is by being run on
 * the same performance.
 */
static const char *eval_take(const char *fit_di)
{
    static const char *const candidates[3] = {
        NULL, "build/listen/e2_di_22050.wav", "build/listen/tube_di_22050.wav"
    };
    size_t i;

    for (i = 0; i < 3; i++) {
        const char *c = (i == 0) ? getenv("AG_EVAL_DI") : candidates[i];
        FILE       *f;
        if (c == NULL || (fit_di != NULL && strcmp(c, fit_di) == 0)) {
            continue;
        }
        f = fopen(c, "rb");
        if (f != NULL) {
            (void)fclose(f);
            return c;
        }
    }
    return NULL;
}

static void eval_held_out(const char *cap, const char *fit_di, uint32_t crate,
                          uint32_t drate, const float *cab, uint32_t cn,
                          const float *imp, const float *imp_bank, uint32_t taps,
                          int has_cab, float drive)
{
    const char  *path = eval_take(fit_di);
    float       *in = NULL, *in_c = NULL, *wet = NULL, *ref = NULL, *tgt = NULL;
    float       *dry = NULL, *bank = NULL, *fit = NULL;
    uint32_t     n = 0, rate = 0, nc = 0, rn = 0;
    ag_amp_cfg_t shipped;

    if (path == NULL) {
        printf("\n  no held-out take, so every number above is this fit"
               " reporting on its own\n  material.  Point AG_EVAL_DI at another"
               " DI take of the same rate.\n");
        return;
    }
    in = read_wav(path, &n, &rate);
    if (in == NULL || n < drate) {
        free(in);
        return;
    }
    if (rate != drate) {
        printf("\n  held-out take %s is %u Hz and the fit ran at %u; skipped\n",
               path, rate, drate);
        free(in);
        return;
    }
    printf("\n  Held out: %s, %.1f s, which neither of the two has seen.\n",
           path, (double)n / (double)rate);

    /* The amplifier's own answer on this take, through the same speaker. */
    if (crate == drate) {
        in_c = in;
        nc = n;
    } else {
        in_c = wr_resample_f(in, n, drate, crate, &nc, 0);
    }
    wet = in_c != NULL ? capture_render(cap, in_c, nc, 1.0f, 0) : NULL;
    if (wet != NULL) {
        if (crate == drate) {
            ref = wet;
            rn = nc;
            wet = NULL;
        } else {
            ref = wr_resample_f(wet, nc, crate, drate, &rn, 0);
        }
    }
    if (ref != NULL) {
        tgt = has_cab ? ref : convolve_f(ref, rn, cab, cn);
    }

    /* And ours, both ways, exactly as the two sides of `z` are built. */
    model_cfg(&shipped, (float)drate);
    shipped.drive = drive;
    dry = chain_render_dry(&shipped, in, n, drate, 1);
    bank = chain_render_dry(&shipped, in, n, drate, 0);
    if (dry != NULL && bank != NULL && cab != NULL) {
        float *raw = convolve_f(bank, n, cab, cn);
        float *m1 = convolve_f(bank, n, imp_bank, taps);
        fit = convolve_f(dry, n, imp, taps);
        if (tgt != NULL && raw != NULL && m1 != NULL && fit != NULL) {
            double rs[SPEC_N], as_[SPEC_N], b1[SPEC_N], b2[SPEC_N];
            double r0, r1, r2;
            spectrum_of(tgt, (int)rn, (float)drate, rs);
            spectrum_of(raw, (int)n, (float)drate, as_);
            spectrum_of(m1, (int)n, (float)drate, b1);
            spectrum_of(fit, (int)n, (float)drate, b2);
            r0 = spec_rms(rs, as_);
            r1 = spec_rms(rs, b1);
            r2 = spec_rms(rs, b2);
            printf("    as it ships: the bank into a raw cabinet  %5.2f dB\n"
                   "    mode 1: the bank and a fitted impulse     %5.2f dB\n"
                   "    mode 2: the fitted impulse alone          %5.2f dB\n",
                   r0, r1, r2);
            /*
             * The only conclusion this measurement can carry: whether the
             * fitting survives material it has never heard.  A mode that wins
             * the fitted take and loses here has spent its degrees of freedom
             * on that take.
             */
            if (r1 > r0 && r2 > r0) {
                printf("  Both modes lose to the unfitted cabinet here, which"
                       " means the impulse fit did not\n  generalise at all on"
                       " this material - suspect the take before the method.\n");
            } else {
                printf("  The better of the two here is %s, by %.2f dB.\n",
                       r1 < r2 ? "mode 1, the bank plus an impulse"
                               : "mode 2, the impulse alone",
                       r1 < r2 ? r2 - r1 : r1 - r2);
            }
        } else {
            printf("    (could not render the held-out comparison)\n");
        }
        free(raw);
        free(m1);
    }
    if (in_c != in) {
        free(in_c);
    }
    if (tgt != ref) {
        free(tgt);
    }
    free(in);
    free(wet);
    free(ref);
    free(dry);
    free(bank);
    free(fit);
}

/* ------------------------------------------------------------------------ */
/* sens - what a filter in front of the valves actually does                  */
/* ------------------------------------------------------------------------ */

/*
 * One band at a time, +6 dB, and what came out.
 *
 *   AG_MODEL=bogner tube_render sens build/listen/tube_di_22050.wav [drive [dB]]
 *
 * WHY THIS EXISTS
 *
 * Because "the filters in front of the valves decide the character of the
 * overdrive" is a true statement that nothing in this tree had measured, and a fit
 * cannot be constrained sensibly by a statement.  Raising the bass in front of a
 * clipper does something to the harmonics; raising the treble does something else;
 * and the only honest way to know which is which - and how far it reaches into the
 * 0-5 kHz range anybody listens to - is to move one band and look.
 *
 * The last row is the control, and it is the row that shows what "a different kind
 * of filter" means: the same +6 dB applied *behind* the valves instead. It moves
 * the spectrum by what a filter moves and leaves the two character numbers where
 * they were, to the decibel. In front, the same 6 dB rearranges what gets made.
 *
 * No cabinet: this is about the valves, and a loudspeaker only takes the top away
 * again.
 */
static void mode_sens(int argc, char **argv)
{
    const char  *path = argc > 2 ? argv[2] : NULL;
    const float  drive = argc > 3 ? (float)atof(argv[3]) : 0.5f;
    const float  step = argc > 4 ? (float)atof(argv[4]) : 6.0f;
    float       *in = NULL, *out = NULL;
    uint32_t     n = 0, rate = 0;
    ag_amp_t    *a = NULL;
    double       base[SPEC_N], got[SPEC_N];
    double       base_comp, base_hash, base_imd, base_tone = 0.0;
    int          i, b, row;

    if (path == NULL) {
        printf("  usage: tube_render sens di.wav [drive [dB]]\n");
        return;
    }
    in = read_wav(path, &n, &rate);
    a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    out = (float *)malloc(sizeof(float) * (n ? n : 1u));
    if (in == NULL || a == NULL || out == NULL || n == 0u) {
        free(in);
        free(a);
        free(out);
        return;
    }

    print_model();
    printf("  take: %s, %.1f s at %u Hz, drive %.2f, one band at a time %+.0f dB,"
           " no cabinet\n\n", path, (double)n / (double)rate, rate, (double)drive,
           (double)step);

    /* The baseline, and the two numbers that say what kind of change a row is. */
    {
        ag_amp_cfg_t cfg;
        model_cfg(&cfg, (float)rate);
        cfg.drive = drive;
        if (ag_amp_build(a, g_ckt, &cfg, g_tab, 0, in,
                         (int)(n < rate * 10u ? n : rate * 10u)) != 0) {
            printf("  build failed\n");
            free(in);
            free(a);
            free(out);
            return;
        }
        for (i = 0; i < (int)n; i++) {
            out[i] = ag_amp_tick(a, in[i]);
        }
        spectrum_of(out, (int)n, (float)rate, base);
        chain_character(&cfg, in, n, rate, &base_comp, &base_hash);
        base_imd = two_tone_imd(&cfg, rate, 0.35f, NULL, 0u, &base_tone);
        printf("  as it ships, relative to its own 1 kHz:\n");
        print_spec_header();
        print_spec("the chain", base);
        printf("    it compresses %.1f dB, makes %.1f dB of its output above"
               " 2 kHz, and puts the\n    49 Hz difference of two low tones"
               " %.1f dB under them\n\n", base_comp, base_hash, base_imd);
    }

    printf("  each row: that band %+.0f dB, and the change in every band"
           " (dB)\n", (double)step);
    print_spec_header();
    /*
     * The rows: every band in front, one behind as the control, then the coupling
     * corners - each stage on its own, and then all of them together.
     *
     * The last group is the one that answers "cut a lot before the first stage, or
     * a little before each".  Same total bass removed, different place: the
     * per-stage rows raise one corner by 2x and the last row raises every corner
     * by the amount that removes about as much overall, so the two can be compared
     * on what they do to the harmonics rather than on how much bass is left.
     */
    for (row = 0; row <= AG_AMP_VOICE_N + AG_AMP_STAGES + 1 + 1 + AG_AMP_STAGES;
         row++) {
        ag_amp_cfg_t cfg;
        char         label[48];
        double       comp, hash;

        model_cfg(&cfg, (float)rate);
        cfg.drive = drive;
        /*
         * The last group is not a knob at all: it is the chain taken apart, so
         * that a number like the 49 Hz difference product can be traced to where
         * it is made.  Blocking distortion off, then one stage at a time - if the
         * product appears with the second stage it is made there, and if it
         * appears without blocking it is the static curve rather than the grid.
         */
        if (row > AG_AMP_VOICE_N + AG_AMP_STAGES + 1) {
            const int which = row - (AG_AMP_VOICE_N + AG_AMP_STAGES + 2);
            if (which == 0) {
                cfg.blocking = 0;
                (void)snprintf(label, sizeof(label), "blocking OFF");
            } else {
                if (which > cfg.n_stages) {
                    continue;
                }
                cfg.n_stages = which;
                (void)snprintf(label, sizeof(label), "only %d stage%s", which,
                               which > 1 ? "s" : "");
            }
        } else if (row > AG_AMP_VOICE_N) {
            const int which = row - AG_AMP_VOICE_N - 1;
            int       st;
            if (which < AG_AMP_STAGES) {
                if (which >= cfg.n_stages) {
                    continue;
                }
                cfg.couple_mul[which] = 2.0f;
                (void)snprintf(label, sizeof(label),
                               "stage %d coupling x2", which + 1);
            } else {
                /* All of them, by the cube/square root so that the total bass
                 * removed is close to one stage at 2x rather than 2x per stage. */
                const float each =
                    (float)pow(2.0, 1.0 / (double)(cfg.n_stages > 0
                                                       ? cfg.n_stages : 1));
                for (st = 0; st < cfg.n_stages; st++) {
                    cfg.couple_mul[st] = each;
                }
                (void)snprintf(label, sizeof(label),
                               "every stage x%.2f (same total)", (double)each);
            }
        } else if (row < AG_AMP_VOICE_N) {
            cfg.voice[0][row].db += step;
            (void)snprintf(label, sizeof(label), "in front, %.0f Hz",
                           (double)cfg.voice[0][row].hz);
        } else {
            /* The control: the same lift behind the valves.  200 Hz because that
             * is the band with the most signal in it and so the one where a
             * pre-valve lift has the most to work with. */
            for (b = 0; b < AG_AMP_VOICE_N; b++) {
                if (cfg.tone[b].hz > 150.0f && cfg.tone[b].hz < 300.0f) {
                    cfg.tone[b].db += step;
                }
            }
            (void)snprintf(label, sizeof(label), "BEHIND, 200 Hz");
        }
        if (ag_amp_build(a, g_ckt, &cfg, g_tab, 0, in,
                         (int)(n < rate * 10u ? n : rate * 10u)) != 0) {
            continue;
        }
        for (i = 0; i < (int)n; i++) {
            out[i] = ag_amp_tick(a, in[i]);
        }
        spectrum_of(out, (int)n, (float)rate, got);
        chain_character(&cfg, in, n, rate, &comp, &hash);
        printf("  %-22s", label);
        for (i = 0; i < SPEC_N; i++) {
            printf(" %+5.1f", (got[i] > -299.0 && base[i] > -299.0)
                                  ? got[i] - base[i] : 0.0);
        }
        {
            double       tone = 0.0;
            const double im = two_tone_imd(&cfg, rate, 0.35f, NULL, 0u, &tone);
            printf("   comp %+.1f  above2k %+.1f  imd49 %+.1f (tone %+.1f)\n",
                   comp - base_comp, hash - base_hash, im - base_imd,
                   tone - base_tone);
        }
    }
    printf("\n  Everything is relative to each render's own 1 kHz, so a row of"
           " zeros with a\n  positive `above2k` means the band moved what the"
           " valves *make* without moving\n  the shape - and a row that moves"
           " the shape while `comp` and `above2k` stay put\n  is a tone control,"
           " wherever it happens to sit.\n");

    free(in);
    free(a);
    free(out);
}

/* ------------------------------------------------------------------------ */
/* irnoise - what the chip's int16 convolution costs each of the two modes    */
/* ------------------------------------------------------------------------ */

/*
 *   AG_MODEL=bogner tube_render irnoise build/listen/tube_di_22050.wav
 *
 * The two post-valve modes are equally accurate in exact arithmetic - measured,
 * within 0.33 dB of each other and 0.21 dB on material neither was fitted on - so
 * the choice between them is not about tone.  The one argument that remains is
 * arithmetic: mode 2 asks its impulse to carry the whole correction, and `ag_ir` is
 * int16, so an impulse holding a 19 dB tilt spends its dynamic range on the tilt.
 * Mode 1 keeps that tilt in seven biquads, where the arithmetic is exact, and asks
 * the impulse only for the remainder.
 *
 * So: each mode's own render, convolved with its own impulse twice - once exactly
 * and once through the engine the chip runs - and the difference between those two
 * is the cost.  Levels are matched by least squares first, because a gain is not an
 * error.
 */
static void mode_irnoise(int argc, char **argv)
{
    const char  *path = argc > 2 ? argv[2] : NULL;
    const float  drive = argc > 3 ? (float)atof(argv[3]) : 0.5f;
    float       *in = NULL;
    uint32_t     n = 0, rate = 0;
    int          mode;

    if (path == NULL) {
        printf("  usage: tube_render irnoise di.wav [drive]\n");
        return;
    }
    in = read_wav(path, &n, &rate);
    if (in == NULL || n < 1024u) {
        free(in);
        return;
    }
    print_model();
    printf("  take: %s, %.1f s at %u Hz, drive %.2f\n\n", path,
           (double)n / (double)rate, rate, (double)drive);
    printf("  mode                       impulse            error to signal"
           "    1 kHz   5 kHz      8 kHz\n");

    for (mode = 1; mode <= 2; mode++) {
        char         irp[256];
        ag_amp_cfg_t shipped;
        float       *dry = NULL, *exact = NULL, *engine = NULL, *imp = NULL;
        uint32_t     hn = 0, hrate = 0, i;
        double       num = 0.0, den = 0.0, g;

        (void)snprintf(irp, sizeof(irp), "build/listen/ir_%s_%s.wav",
                       ag_amp_model_name(g_model), mode == 1 ? "bank" : "fitted");
        imp = read_wav(irp, &hn, &hrate);
        if (imp == NULL) {
            printf("  mode %d: no %s - run `argon match` first\n", mode, irp);
            continue;
        }
        if (hrate != rate) {
            float *r = wr_resample_f(imp, hn, hrate, rate, &hn, 0);
            free(imp);
            imp = r;
        }
        model_cfg(&shipped, (float)rate);
        shipped.drive = drive;
        /* Mode 1 keeps the post bank, mode 2 does not: that is the difference. */
        dry = chain_render_dry(&shipped, in, n, rate, mode == 2);
        if (imp == NULL || dry == NULL || hn < 8u) {
            free(imp);
            free(dry);
            continue;
        }
        exact = convolve_f(dry, n, imp, hn);
        engine = (float *)malloc(sizeof(float) * n);
        if (exact == NULL || engine == NULL) {
            free(imp); free(dry); free(exact); free(engine);
            continue;
        }
        for (i = 0; i < n; i++) {
            engine[i] = dry[i];
        }
        /* The same impulse through the chip's engine, by pointing the cabinet
         * loader at it. */
        {
            const char *save = g_cab_path;
            const int   save_cab = g_cab;
            g_cab_path = irp;
            g_cab = 0;
            if (run_cab(engine, n, rate) != 0) {
                printf("  mode %d: the engine would not load %s\n", mode, irp);
                g_cab_path = save;
                g_cab = save_cab;
                free(imp); free(dry); free(exact); free(engine);
                continue;
            }
            g_cab_path = save;
            g_cab = save_cab;
        }
        /*
         * Neither a gain nor a delay is an error, and the delay had to be looked
         * for: differencing the two straight gave -22 dB, which would have been a
         * catastrophic engine, and the whole of it was one sample of latency.  So
         * the best lag is searched over first and the gain fitted at that lag.
         */
        {
            int    best_lag = 0, lag;
            double best_res = 1e300;
            /* calloc, not malloc: the loops below leave the first and last
             * sixteen samples alone and the band filters read the whole array -
             * with malloc that showed up as band ratios of +394 dB. */
            float *err = (float *)calloc(n, sizeof(float));
            if (err == NULL) {
                free(imp); free(dry); free(exact); free(engine);
                continue;
            }
            for (lag = -8; lag <= 8; lag++) {
                double nu = 0.0, de = 0.0, res = 0.0, gg;
                uint32_t j;
                for (j = 16; j + 16u < n; j++) {
                    const double e = (double)exact[j];
                    const double m = (double)engine[j + (uint32_t)lag];
                    nu += e * m;
                    de += e * e;
                }
                gg = de > 1e-30 ? nu / de : 1.0;
                for (j = 16; j + 16u < n; j++) {
                    const double d = (double)engine[j + (uint32_t)lag] -
                                     gg * (double)exact[j];
                    res += d * d;
                }
                if (res < best_res) {
                    best_res = res;
                    best_lag = lag;
                    g = gg;
                }
            }
            num = 0.0;
            den = 0.0;
            for (i = 16; i + 16u < n; i++) {
                const double d = (double)engine[i + (uint32_t)best_lag] -
                                 g * (double)exact[i];
                err[i] = (float)d;
                num += d * d;
                den += (double)engine[i + (uint32_t)best_lag] *
                       (double)engine[i + (uint32_t)best_lag];
            }
            /* Where it sits matters more than how much of it there is: a flat
             * quantisation error under a loudspeaker that falls off a cliff is
             * loudest exactly where the music is quietest. */
            printf("  %d %-24s %-18s %+8.1f dB %8.1f %6.1f %10.1f  (lag %d)\n",
                   mode, mode == 1 ? "the bank plus impulse"
                                   : "the impulse alone",
                   irp + 14, 10.0 * log10(num / (den + 1e-300) + 1e-300),
                   band_ratio_db(err, engine, n, (float)rate, 1000.0f),
                   band_ratio_db(err, engine, n, (float)rate, 5000.0f),
                   band_ratio_db(err, engine, n, (float)rate, 8000.0f),
                   best_lag);
            free(err);
        }
        free(imp);
        free(dry);
        free(exact);
        free(engine);
    }
    printf("\n  The last three columns are the error relative to the signal in"
           " that band, so they are\n  the numbers that matter: a flat"
           " quantisation error under a loudspeaker that falls off\n  a cliff is"
           " loudest where the music is quietest.\n");
    free(in);
}

/* ------------------------------------------------------------------------ */
/* selftest - the arithmetic this tool's own conclusions rest on             */
/* ------------------------------------------------------------------------ */

/*
 *   tube_render selftest
 *
 * `convolve_f` is an FFT overlap-add written for this tool, and every verdict it
 * prints - which post-valve mode is closer to the amplifier, what the held-out
 * take says, what the chip's int16 engine costs - is a difference between two
 * things it produced.  An error in it would not look like an error; it would look
 * like a result.  So it is checked against the naive double loop it replaced, on
 * random data, at several lengths including the ones the matcher actually uses.
 */
static void mode_selftest(void)
{
    static const int hns[4] = { 7, 64, 441, 4410 };
    uint32_t         seed = 12345u;
    int              k, bad = 0;

    printf("\n  convolve_f against a direct convolution in double:\n");
    for (k = 0; k < 4; k++) {
        const uint32_t hn = (uint32_t)hns[k];
        const uint32_t n = 3u * hn + 137u;
        float         *x = (float *)malloc(sizeof(float) * n);
        float         *h = (float *)malloc(sizeof(float) * hn);
        float         *fast;
        double         es = 0.0, ss = 0.0, worst = 0.0;
        uint32_t       i, j;

        if (x == NULL || h == NULL) {
            free(x);
            free(h);
            continue;
        }
        for (i = 0; i < n; i++) {
            seed = seed * 1103515245u + 12345u;
            x[i] = (float)((double)(seed >> 8) / 8388608.0 - 1.0);
        }
        for (i = 0; i < hn; i++) {
            seed = seed * 1103515245u + 12345u;
            h[i] = (float)((double)(seed >> 8) / 8388608.0 - 1.0) *
                   (float)exp(-3.0 * (double)i / (double)hn);
        }
        fast = convolve_f(x, n, h, hn);
        if (fast == NULL) {
            printf("    %5u taps: no memory\n", hn);
            free(x);
            free(h);
            continue;
        }
        for (i = 0; i < n; i++) {
            double acc = 0.0, d;
            const uint32_t m = (i + 1u < hn) ? i + 1u : hn;
            for (j = 0; j < m; j++) {
                acc += (double)h[j] * (double)x[i - j];
            }
            d = (double)fast[i] - acc;
            es += d * d;
            ss += acc * acc;
            if (d * d > worst) {
                worst = d * d;
            }
        }
        printf("    %5u taps, %6u samples: error %.1f dB, worst sample %.1f dB\n",
               hn, n, 10.0 * log10(es / (ss + 1e-300) + 1e-300),
               10.0 * log10(worst / (ss / (double)n + 1e-300) + 1e-300));
        if (10.0 * log10(es / (ss + 1e-300) + 1e-300) > -100.0) {
            bad++;
        }
        free(fast);
        free(x);
        free(h);
    }
    printf("  %s\n\n", bad ? "  ^ SOMETHING IS WRONG: this should be at the"
                             " floor of float, about -140 dB"
                           : "all at the floor of float, as they have to be");
}

/* ------------------------------------------------------------------------ */
/* harm - the harmonic fingerprint, ours against the amplifier's              */
/* ------------------------------------------------------------------------ */

/*
 *   AG_MODEL=bogner tube_render harm "assets/audio/guitar-di/Bogner.nam" [drive]
 *
 * WHY A GUITAR TAKE IS THE WRONG PROBE FOR THIS
 *
 * A third-octave spectrum of a guitar performance cannot separate what the
 * distortion *made* from what a filter *passed*: the take is a harmonic comb to
 * begin with, so the two land in the same bands.  Everything this tool has fitted
 * so far has been fitted through that ambiguity, and the one number that tried to
 * see past it - energy above 2 kHz - moves when any filter moves.
 *
 * A sine burst has no such ambiguity.  Feed 82 Hz in and every decibel at 165, 247
 * and 330 was made by the chain.  Feed *two* tones and the products at f2-f1 and
 * 2f1-f2 are intermodulation, which is what a listener means by "farting" on a low
 * chord and which no single tone can show.  And the capture is a black box that
 * will play anything, so the same probe gives the real amplifier's fingerprint.
 *
 * WHAT TO DISTRUST IN IT
 *
 * A NAM capture is trained on guitar, so a steady full-scale tone is outside what
 * it has ever seen and it may extrapolate badly.  Two guards: the bursts have a
 * guitar-like envelope and sit at playing levels, and each frequency is run at
 * three levels - if the ladder moves smoothly with level, the model is
 * interpolating; if it jumps, it is guessing and the row should not be trusted.
 *
 * The linear part is matched before comparing, or the ladder would be reporting
 * the cabinet: our side plays through the fitted impulse when there is one, which
 * is this tree's best model of everything the capture has after its last valve.
 */

#define HARM_F 6
#define HARM_L 3
#define HARM_N 8 /* harmonics 1..8 */

static const double k_harm_f[HARM_F] = { 82.41, 110.0, 146.83, 196.0, 261.63,
                                         349.23 };
static const double k_harm_l[HARM_L] = { 0.10, 0.35, 0.90 };

/* One frequency out of a stretch of samples, by Goertzel: exact for a bin, and it
 * costs one multiply per sample instead of a transform per harmonic. */
static double goertzel_db(const float *x, uint32_t n, double f, double rate)
{
    const double w = 2.0 * PI * f / rate;
    const double c = 2.0 * cos(w);
    double       s1 = 0.0, s2 = 0.0;
    uint32_t     i;

    for (i = 0; i < n; i++) {
        const double s0 = (double)x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    /* |X|^2 from the last two states, and the window's own gain taken out. */
    return 10.0 * log10((s1 * s1 + s2 * s2 - c * s1 * s2) /
                        ((double)n * (double)n / 4.0) + 1e-300);
}

/*
 * The probe: bursts of one tone, then bursts of two, then noise.  Generated at
 * whatever rate it is asked for, so the capture gets it at 48 kHz and the chain at
 * 22.05 without a resampler between either of them and the analysis.
 */
#define HARM_BURST 0.30 /* steady part */
#define HARM_EDGE  0.02 /* raised-cosine attack and release */
#define HARM_GAP   0.10

static uint32_t harm_layout(uint32_t rate, uint32_t *starts, uint32_t *lens,
                            int max)
{
    const uint32_t burst = (uint32_t)((HARM_BURST + 2.0 * HARM_EDGE) *
                                      (double)rate);
    const uint32_t gap = (uint32_t)(HARM_GAP * (double)rate);
    uint32_t       pos = gap, i;
    int            k = 0;

    for (i = 0; i < (uint32_t)(HARM_F * HARM_L + 2 * HARM_L + 1); i++) {
        if (k < max) {
            starts[k] = pos;
            lens[k] = burst;
            k++;
        }
        pos += burst + gap;
    }
    return pos;
}

static float *harm_probe(uint32_t rate, uint32_t *out_n)
{
    uint32_t starts[HARM_F * HARM_L + 2 * HARM_L + 1];
    uint32_t lens[HARM_F * HARM_L + 2 * HARM_L + 1];
    const int slots = HARM_F * HARM_L + 2 * HARM_L + 1;
    const uint32_t n = harm_layout(rate, starts, lens, slots);
    float   *x = (float *)calloc(n ? n : 1u, sizeof(float));
    uint32_t seed = 22222u;
    int      slot = 0, fi, li;

    if (x == NULL) {
        *out_n = 0;
        return NULL;
    }
    for (fi = 0; fi < HARM_F; fi++) {
        for (li = 0; li < HARM_L; li++, slot++) {
            const uint32_t s = starts[slot], m = lens[slot];
            uint32_t       j;
            for (j = 0; j < m; j++) {
                const double t = (double)j / (double)rate;
                const double env =
                    t < HARM_EDGE ? 0.5 - 0.5 * cos(PI * t / HARM_EDGE)
                    : (t > HARM_BURST + HARM_EDGE
                           ? 0.5 - 0.5 * cos(PI * (HARM_BURST + 2.0 * HARM_EDGE -
                                                   t) / HARM_EDGE)
                           : 1.0);
                x[s + j] = (float)(k_harm_l[li] * env *
                                   sin(2.0 * PI * k_harm_f[fi] * t));
            }
        }
    }
    /* Two tones, equal halves so the peak is the same level as a single one. */
    for (fi = 0; fi < 2; fi++) {
        const double f1 = fi == 0 ? k_harm_f[0] : k_harm_f[2];
        const double f2 = fi == 0 ? k_harm_f[1] : k_harm_f[3];
        for (li = 0; li < HARM_L; li++, slot++) {
            const uint32_t s = starts[slot], m = lens[slot];
            uint32_t       j;
            if (fi >= 2) {
                break;
            }
            for (j = 0; j < m; j++) {
                const double t = (double)j / (double)rate;
                const double env =
                    t < HARM_EDGE ? 0.5 - 0.5 * cos(PI * t / HARM_EDGE)
                    : (t > HARM_BURST + HARM_EDGE
                           ? 0.5 - 0.5 * cos(PI * (HARM_BURST + 2.0 * HARM_EDGE -
                                                   t) / HARM_EDGE)
                           : 1.0);
                x[s + j] = (float)(0.5 * k_harm_l[li] * env *
                                   (sin(2.0 * PI * f1 * t) +
                                    sin(2.0 * PI * f2 * t)));
            }
        }
        if (li == HARM_L && fi == 1) {
            break;
        }
    }
    /* And a noise burst, for the one thing the tones cannot say: whether the
     * capture has a loudspeaker in it. */
    {
        const uint32_t s = starts[slots - 1], m = lens[slots - 1];
        uint32_t       j;
        for (j = 0; j < m; j++) {
            seed = seed * 1103515245u + 12345u;
            x[s + j] = (float)(0.3 * ((double)(seed >> 8) / 8388608.0 - 1.0));
        }
    }
    *out_n = n;
    return x;
}

static void mode_harm(int argc, char **argv)
{
    const char  *cap = argc > 2 ? argv[2] : NULL;
    const float  drive = argc > 3 ? (float)atof(argv[3]) : 0.5f;
    const uint32_t rate = 22050u;
    uint32_t     crate = 48000u;
    float       *probe = NULL, *cprobe = NULL, *cap_out = NULL, *cap_at = NULL;
    float       *ours = NULL, *imp = NULL;
    uint32_t     n = 0, cn = 0, con = 0, hn = 0, hrate = 0;
    uint32_t     starts[HARM_F * HARM_L + 2 * HARM_L + 1];
    uint32_t     lens[HARM_F * HARM_L + 2 * HARM_L + 1];
    const int    slots = HARM_F * HARM_L + 2 * HARM_L + 1;
    ag_amp_cfg_t shipped;
    char         irp[256];
    int          fi, li, slot;

    if (cap == NULL) {
        printf("  usage: tube_render harm capture.nam [drive]\n");
        return;
    }
    {
        nam_model_t *m = nam_load(cap, 0, 0);
        if (m == NULL) {
            printf("  %s: %s\n", cap, nam_err());
            return;
        }
        if (nam_sample_rate(m) > 0) {
            crate = (uint32_t)nam_sample_rate(m);
        }
        nam_free(m);
    }
    print_model();
    printf("  capture: %s at %u Hz; the chain at %u Hz, drive %.2f\n", cap,
           crate, rate, (double)drive);

    probe = harm_probe(rate, &n);
    cprobe = harm_probe(crate, &cn);
    if (probe == NULL || cprobe == NULL) {
        free(probe);
        free(cprobe);
        return;
    }
    (void)harm_layout(rate, starts, lens, slots);

    /* The amplifier's answer, at its own rate, then brought to ours. */
    cap_out = capture_render(cap, cprobe, cn, 1.0f, 0);
    if (cap_out == NULL) {
        free(probe);
        free(cprobe);
        return;
    }
    if (crate == rate) {
        cap_at = cap_out;
        con = cn;
        cap_out = NULL;
    } else {
        cap_at = wr_resample_f(cap_out, cn, crate, rate, &con, 0);
    }

    /*
     * BEFORE ANYTHING IS COMPARED: DOES THE CAPTURE HAVE A LOUDSPEAKER IN IT
     *
     * The noise burst at the end of the probe answers it, by the same rule `match`
     * uses on a guitar take - 6.3 kHz against 1 kHz, and a twelve-inch speaker is
     * 15 to 25 dB down there while a preamp is not.  It matters more here than
     * anywhere else in this tool: our side plays through an impulse that *carries*
     * a cabinet, so against a head-only capture every high harmonic on our side
     * arrives 20 dB down and the whole ladder is a comparison of loudspeakers.
     * Measured before this was fixed: the capture read +4.1 dB up there and ours
     * -20.5.
     *
     * So a head-only capture gets our cabinet put on it, which is the same
     * position `match` takes: each side gets a speaker or neither does.
     */
    {
        const uint32_t s = starts[slots - 1] +
                           (uint32_t)(HARM_EDGE * (double)rate);
        const uint32_t m = (uint32_t)(HARM_BURST * (double)rate);
        double         tilt = 0.0;
        if (s + m <= con) {
            tilt = goertzel_db(cap_at + s, m, 6300.0, (double)rate) -
                   goertzel_db(cap_at + s, m, 1000.0, (double)rate);
        }
        printf("  the capture's noise burst: %+.1f dB at 6.3 kHz relative to"
               " 1 kHz, so it %s\n", tilt,
               tilt < -8.0 ? "has a loudspeaker in it" : "is HEAD ONLY");
        if (tilt >= -8.0) {
            char     cabp[256];
            uint32_t kn = 0, krate = 0;
            float   *k;
            (void)snprintf(cabp, sizeof(cabp), "build/listen/match_cab_%s.wav",
                           ag_amp_model_name(g_model));
            k = read_wav(cabp, &kn, &krate);
            if (k != NULL && krate != rate) {
                float *r = wr_resample_f(k, kn, krate, rate, &kn, 0);
                free(k);
                k = r;
            }
            if (k != NULL && kn > 8u) {
                float *w = convolve_f(cap_at, con, k, kn);
                if (w != NULL) {
                    free(cap_at);
                    cap_at = w;
                    printf("  so %s goes on the capture too, or the ladder"
                           " would be a comparison of speakers\n", cabp);
                }
            } else {
                printf("  WARNING no %s to put on it - run `argon match`"
                       " first, or the high harmonics below are speakers\n",
                       cabp);
            }
            free(k);
        }
    }

    /*
     * And ours, through the same **cabinet** the capture side got - not through the
     * fitted impulse.
     *
     * That distinction is a bug fix.  The fitted impulse is correction convolved
     * with cabinet, so putting it on our side while the capture side got the bare
     * cabinet compares two different filters, and the correction's own bass lift
     * then shows up as intermodulation this chain does not make: measured both
     * ways, the 49 Hz product moved by 11 dB and the sign of the comparison
     * reversed.  One filter, both sides.
     *
     * The post bank stays out for the same reason it stays out of the other two
     * character numbers: what is being compared is what the valves *make*.
     */
    model_cfg(&shipped, (float)rate);
    shipped.drive = drive;
    (void)snprintf(irp, sizeof(irp), "build/listen/match_cab_%s.wav",
                   ag_amp_model_name(g_model));
    imp = read_wav(irp, &hn, &hrate);
    if (imp != NULL && hrate != rate) {
        float *r = wr_resample_f(imp, hn, hrate, rate, &hn, 0);
        free(imp);
        imp = r;
    }
    {
        float *dry = chain_render_dry(&shipped, probe, n, rate, 1);
        if (dry == NULL) {
            free(probe); free(cprobe); free(cap_out); free(cap_at); free(imp);
            return;
        }
        if (imp != NULL) {
            ours = convolve_f(dry, n, imp, hn);
            free(dry);
            printf("  ours through %s, post bank at zero - the same speaker as"
                   " above\n", irp);
        } else {
            ours = dry;
            printf("  no %s, so ours is the bare chain\n", irp);
        }
    }
    if (ours == NULL || cap_at == NULL) {
        free(probe); free(cprobe); free(cap_out); free(cap_at); free(imp);
        free(ours);
        return;
    }

    /* --------------------------------------------------------------- the ladder */
    printf("\n  HARMONICS, dB below the fundamental of the same burst"
           " (capture / ours)\n");
    printf("    Hz   level        H2              H3              H4"
           "              H5\n");
    slot = 0;
    for (fi = 0; fi < HARM_F; fi++) {
        for (li = 0; li < HARM_L; li++, slot++) {
            const uint32_t s = starts[slot] +
                               (uint32_t)(HARM_EDGE * (double)rate);
            const uint32_t m = (uint32_t)(HARM_BURST * (double)rate);
            double         c1, o1;
            int            h;
            if (s + m > con || s + m > n) {
                continue;
            }
            c1 = goertzel_db(cap_at + s, m, k_harm_f[fi], (double)rate);
            o1 = goertzel_db(ours + s, m, k_harm_f[fi], (double)rate);
            printf("  %5.0f   %4.2f  ", k_harm_f[fi], k_harm_l[li]);
            for (h = 2; h <= 5; h++) {
                const double f = k_harm_f[fi] * (double)h;
                if (f > 0.45 * (double)rate) {
                    printf("      -    -      ");
                    continue;
                }
                printf(" %6.1f /%6.1f ",
                       goertzel_db(cap_at + s, m, f, (double)rate) - c1,
                       goertzel_db(ours + s, m, f, (double)rate) - o1);
            }
            printf("\n");
        }
    }

    /* ------------------------------------------------------------------- IMD */
    printf("\n  TWO TONES, dB below the louder tone (capture / ours) - this is"
           " the one that\n  says whether a low chord farts\n");
    printf("    tones      level    f2-f1          2f1-f2         2f2-f1"
           "         f1+f2\n");
    for (fi = 0; fi < 2; fi++) {
        const double f1 = fi == 0 ? k_harm_f[0] : k_harm_f[2];
        const double f2 = fi == 0 ? k_harm_f[1] : k_harm_f[3];
        for (li = 0; li < HARM_L; li++, slot++) {
            const uint32_t s = starts[slot] +
                               (uint32_t)(HARM_EDGE * (double)rate);
            const uint32_t m = (uint32_t)(HARM_BURST * (double)rate);
            const double   prod[4] = { f2 - f1, 2.0 * f1 - f2, 2.0 * f2 - f1,
                                       f1 + f2 };
            double         c1, o1;
            int            k;
            if (s + m > con || s + m > n) {
                continue;
            }
            c1 = goertzel_db(cap_at + s, m, f1, (double)rate);
            o1 = goertzel_db(ours + s, m, f1, (double)rate);
            printf("  %4.0f+%-4.0f    %4.2f ", f1, f2, k_harm_l[li]);
            for (k = 0; k < 4; k++) {
                printf(" %6.1f /%6.1f ",
                       goertzel_db(cap_at + s, m, prod[k], (double)rate) - c1,
                       goertzel_db(ours + s, m, prod[k], (double)rate) - o1);
            }
            printf("\n");
        }
    }

    /* ------------------------------------------------- the noise burst, linear */
    {
        const uint32_t s = starts[slots - 1] +
                           (uint32_t)(HARM_EDGE * (double)rate);
        const uint32_t m = (uint32_t)(HARM_BURST * (double)rate);
        if (s + m <= con && s + m <= n) {
            const double c1k = goertzel_db(cap_at + s, m, 1000.0, (double)rate);
            const double c6k = goertzel_db(cap_at + s, m, 6300.0, (double)rate);
            const double o1k = goertzel_db(ours + s, m, 1000.0, (double)rate);
            const double o6k = goertzel_db(ours + s, m, 6300.0, (double)rate);
            printf("\n  NOISE BURST after the speaker business above,"
                   " 6.3 kHz relative to 1 kHz:\n    capture %+.1f dB, ours"
                   " %+.1f dB - these two should now be close, and what is left"
                   "\n    is the linear error the impulse did not close.\n",
                   c6k - c1k, o6k - o1k);
        }
    }
    printf("\n  Read the ladder down each column: if a row's three levels do not"
           " move smoothly,\n  the capture is extrapolating and that row is not"
           " evidence.\n");

    free(probe);
    free(cprobe);
    free(cap_out);
    free(cap_at);
    free(ours);
    free(imp);
}

/* ------------------------------------------------------------------------ */
/* knee - which valve is working, level by level                              */
/* ------------------------------------------------------------------------ */

/*
 *   AG_MODEL=slo tube_render knee "assets/audio/guitar-di/5150red.nam"
 *
 * THE IDEA, WHICH IS NOT MINE
 *
 * In a cascade the *last* valve sees the biggest signal, so it is the one that
 * distorts first.  Start quiet enough and only it is working; raise the level and
 * the one before it joins; raise it again and so on back to the first.  So a level
 * sweep separates stages that a single loud take mixes together - and each stage can
 * be judged, and adjusted, in the level range where it is the only new thing.
 *
 * HOW THE SEPARATION IS MADE VISIBLE
 *
 * Not by guessing which stage is clipping, but by *taking the chain apart*: at each
 * level the same tone is rendered through one stage, then two, then three, then
 * four, and what each column adds is what that stage contributed.  A column that
 * matches the one before it at a given level is a stage that is not doing anything
 * yet; the level where it starts to differ is its knee.
 *
 * The amplifier's own column is beside them, from the same tone at the same level,
 * so the comparison at each level is with the real thing rather than between our own
 * stages only.  147 Hz because it is a real note, low enough for several harmonics to
 * fit under the top and high enough to be clear of the coupling corners.
 *
 * H3 is printed rather than H2 for a reason worth keeping: third harmonic is what
 * symmetric clipping makes, so it tracks *how hard* a stage is working; second
 * harmonic tracks how lopsided it is, which is a different question and the one
 * `harm` is for.
 */
static void mode_knee(int argc, char **argv)
{
    static const float levels[6] = { 0.02f, 0.05f, 0.12f, 0.28f, 0.55f, 0.95f };
    const char  *cap = argc > 2 ? argv[2] : NULL;
    const float  drive = argc > 3 ? (float)atof(argv[3]) : 0.5f;
    const uint32_t rate = 22050u;
    const double f0 = 146.83;
    const uint32_t edge = (uint32_t)(0.02 * (double)rate);
    const uint32_t body = (uint32_t)(0.30 * (double)rate);
    const uint32_t n = 2u * edge + body;
    uint32_t     crate = 48000u;
    float       *x = NULL, *y = NULL, *xc = NULL;
    ag_amp_t    *a = NULL;
    int          li, st, max_st;

    if (cap == NULL) {
        printf("  usage: tube_render knee capture.nam [drive]\n");
        return;
    }
    {
        nam_model_t *m = nam_load(cap, 0, 0);
        if (m == NULL) {
            printf("  %s: %s\n", cap, nam_err());
            return;
        }
        if (nam_sample_rate(m) > 0) {
            crate = (uint32_t)nam_sample_rate(m);
        }
        nam_free(m);
    }
    a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    x = (float *)malloc(sizeof(float) * n);
    y = (float *)malloc(sizeof(float) * n);
    if (a == NULL || x == NULL || y == NULL) {
        free(a); free(x); free(y);
        return;
    }
    print_model();
    {
        ag_amp_cfg_t c0;
        model_cfg(&c0, (float)rate);
        max_st = c0.n_stages;
    }
    printf("  %.0f Hz, third harmonic in dB below the fundamental, no cabinet and"
           " no post bank\n"
           "  ours through one stage, then two, and so on - a column that repeats"
           " the one before\n  it is a valve that is not working yet\n\n"
           "   level      capture", f0);
    for (st = 1; st <= max_st; st++) {
        printf("   %d stage%s", st, st > 1 ? "s" : " ");
    }
    printf("\n");

    /* The capture's column, at its own rate: one render per level. */
    {
        const uint32_t cedge = (uint32_t)(0.02 * (double)crate);
        const uint32_t cbody = (uint32_t)(0.30 * (double)crate);
        const uint32_t cn = 2u * cedge + cbody;
        xc = (float *)malloc(sizeof(float) * cn);
        if (xc == NULL) {
            free(a); free(x); free(y);
            return;
        }
        for (li = 0; li < 6; li++) {
            double   capv = -300.0;
            uint32_t i;
            float   *cy;
            for (i = 0; i < cn; i++) {
                const double t = (double)i / (double)crate;
                const double env =
                    i < cedge ? 0.5 - 0.5 * cos(PI * (double)i / (double)cedge)
                    : (i > cedge + cbody
                           ? 0.5 - 0.5 * cos(PI * (double)(cn - i) /
                                             (double)cedge)
                           : 1.0);
                xc[i] = (float)((double)levels[li] * env *
                                sin(2.0 * PI * f0 * t));
            }
            cy = capture_render(cap, xc, cn, 1.0f, 0);
            if (cy != NULL) {
                capv = goertzel_db(cy + cedge, cbody, 3.0 * f0, (double)crate) -
                       goertzel_db(cy + cedge, cbody, f0, (double)crate);
                free(cy);
            }
            printf("   %5.2f     %6.1f dB", (double)levels[li], capv);

            /* And ours, one stage count at a time. */
            for (st = 1; st <= max_st; st++) {
                ag_amp_cfg_t cfg;
                double       v = -300.0;
                int          b;
                uint32_t     j;
                model_cfg(&cfg, (float)rate);
                cfg.drive = drive;
                cfg.n_stages = st;
                for (b = 0; b < AG_AMP_VOICE_N; b++) {
                    cfg.tone[b].db = 0.0f;
                }
                for (j = 0; j < n; j++) {
                    const double t = (double)j / (double)rate;
                    const double env =
                        j < edge ? 0.5 - 0.5 * cos(PI * (double)j / (double)edge)
                        : (j > edge + body
                               ? 0.5 - 0.5 * cos(PI * (double)(n - j) /
                                                 (double)edge)
                               : 1.0);
                    x[j] = (float)((double)levels[li] * env *
                                   sin(2.0 * PI * f0 * t));
                }
                if (ag_amp_build(a, g_ckt, &cfg, g_tab, 0, x, (int)n) == 0) {
                    for (j = 0; j < n; j++) {
                        y[j] = ag_amp_tick(a, x[j]);
                    }
                    v = goertzel_db(y + edge, body, 3.0 * f0, (double)rate) -
                        goertzel_db(y + edge, body, f0, (double)rate);
                }
                printf("  %7.1f", v);
            }
            printf("\n");
        }
    }
    printf("\n  Read down a column for the knee of that stage, and across a row for"
           " which stage is\n  the last one doing anything at that level.  Where"
           " the capture's column sits against\n  ours at the *quiet* end is the"
           " last valve on its own; the loud end is all of them.\n");
    free(a);
    free(x);
    free(y);
    free(xc);
}

/* ------------------------------------------------------------------------ */
/* ladder - the block in front of each valve, fitted where that valve works   */
/* ------------------------------------------------------------------------ */

/*
 *   AG_MODEL=slo tube_render ladder "assets/audio/guitar-di/5150red.nam" \
 *                                   build/listen/gc1_di_22050.wav [passes]
 *
 * THE PROCEDURE, WHICH IS NOT MINE AND IS WRITTEN DOWN HERE BECAUSE IT MATTERS
 *
 * Start with a signal small enough that nothing is distorting and raise it until
 * something is.  In a cascade the thing that goes first is the **last** valve,
 * because it sees the biggest signal - so at that level any difference in
 * harmonic structure from the capture belongs to the last valve, and the thing to
 * adjust is the tone block in front of it.  Raise the level again until the valve
 * before it joins in; the new difference is that valve's, and its own block is
 * what corrects it.  Carry on back to the first.
 *
 * Two rules come with it and both are constraints on this code rather than
 * remarks.  **The amplifier's own settings do not move**: drive, g12, gain[],
 * every capacitor and every resistor are the circuit, and the circuit is assumed
 * right.  What moves is the matching layer, cfg.voice[stage][] and
 * cfg.vtrim[stage].  And **absolute tone is not the target here**: what is being
 * matched is what each valve is fed and therefore what kind of distortion comes
 * out of it.  The output bank and the impulse close the tone at the end, which is
 * what `match` is for.
 *
 * If, when every stage is done, the chain as a whole compresses by the wrong
 * amount, the fix is not a new shape anywhere: it is the same trim on every stage,
 * which is the last step below.
 *
 * WHAT IS MEASURED
 *
 * Five notes, low E to middle C, one after another, at one level.  For each note
 * the second through sixth harmonic as decibels below its own fundamental - that
 * is twenty-five numbers, and they are what "the same kind of distortion" means,
 * because dividing by the fundamental takes the level out of it.  Beside them the
 * third-octave spectrum of the whole probe from 63 Hz to 5 kHz, normalised at
 * 1 kHz, at half weight: the "roughly the same spectrum up to 5 kHz" half of the
 * rule, and half weight because the tone is not this step's job.
 *
 * No cabinet on either side and the output bank off, for the same reason the other
 * character measurements do it: what is being compared is valves.
 *
 * WHICH LEVEL BELONGS TO WHICH STAGE IS MEASURED, NOT ASSUMED
 *
 * The chain is taken apart the way `knee` takes it apart - rendered through one
 * stage, then two, then three - and stage k's level is the quietest one on the
 * ladder where adding it puts more than 3 dB of harmonics on top of what the
 * stages before it were already making.  That is "raise it until this valve starts
 * working", measured.  It is recomputed on every pass, because fitting a block
 * moves the knee behind it.
 */

#define LAD_NOTE_N 5
#define LAD_HARM_N 5 /* H2 through H6 */
#define LAD_SIG_N  (LAD_NOTE_N * LAD_HARM_N)
#define LAD_LVL_N  12
/*
 * ONE MORE SEGMENT, AND IT IS THE ONE THE HARMONICS CANNOT SEE
 *
 * A double stop a fourth apart - 147 and 196 Hz - and the level of their 49 Hz
 * difference product.  No single note shows this, which is why a fit judged on
 * twenty-five harmonic ratios can drive a chain deep into blocking distortion and
 * report that the character improved: measured on the four-valve model, a ladder
 * that matched harmonics to within a decibel came out **12 dB above the capture on
 * the 49 Hz product**, which is the sound of a low chord farting.
 *
 * It goes in the probe rather than in a separate tool because it has to be inside
 * the objective to change what the objective chooses.  The whole reason the
 * pre-stage blocks exist is to take a little bass out in front of each hot valve,
 * and this is the number that says whether they did.
 */
#define LAD_SEG_N (LAD_NOTE_N + 1)

/* E2, A2, D3, G3, C4 - low enough for six harmonics to fit under 5 kHz, high
 * enough to be clear of every coupling corner in the chain. */
static const double k_lad_note[LAD_NOTE_N] = {
    82.41, 110.0, 146.83, 196.0, 261.63
};

/*
 * 0.004 to 1.0 in eleven steps of 4.9 dB.
 *
 * The bottom is that low for a measured reason: on both high-gain models the last
 * valve is **already saturated** at a hundredth of full scale - the three-stage
 * column reads -25 dB of harmonics where two stages read -50 - so a ladder that
 * starts at 0.012 has no rung on which that valve is working gently, which is the
 * one rung the procedure is about.  48 dB of range is a soft fingerpick to a hard
 * strum and then some.
 */
static double lad_level(int i)
{
    return 0.004 * pow(1.0 / 0.004, (double)i / (double)(LAD_LVL_N - 1));
}

static uint32_t lad_seg(uint32_t rate)
{
    return (uint32_t)(0.32 * (double)rate);
}

static uint32_t lad_n(uint32_t rate)
{
    return (uint32_t)LAD_SEG_N * lad_seg(rate);
}

/* The double stop, at half amplitude each so the segment peaks like a note. */
static const double k_lad_imd[2] = { 146.83, 196.0 };

static void lad_probe(float *x, uint32_t rate, double level)
{
    const uint32_t seg = lad_seg(rate);
    const uint32_t edge = (uint32_t)(0.02 * (double)rate);
    int            k;
    uint32_t       i;

    for (k = 0; k < LAD_SEG_N; k++) {
        for (i = 0; i < seg; i++) {
            const double t = (double)i / (double)rate;
            double       env = 1.0, v;
            if (i < edge) {
                env = 0.5 - 0.5 * cos(PI * (double)i / (double)edge);
            } else if (i + edge > seg) {
                env = 0.5 - 0.5 * cos(PI * (double)(seg - i) / (double)edge);
            }
            if (k < LAD_NOTE_N) {
                v = sin(2.0 * PI * k_lad_note[k] * t);
            } else {
                v = 0.5 * sin(2.0 * PI * k_lad_imd[0] * t) +
                    0.5 * sin(2.0 * PI * k_lad_imd[1] * t);
            }
            x[(uint32_t)k * seg + i] = (float)(level * env * v);
        }
    }
}

/*
 * THE PROBE THE LINEAR RESPONSE IS MEASURED WITH, AND WHY IT IS NOT THE NOTES
 *
 * The five-note probe has energy at five frequencies and nowhere else, so a render
 * of it at a level too low to distort has *nothing* in the bands where the
 * harmonics land - the analyser reads its own floor there and the correction below
 * comes out as noise.  Measured: the capture's corrected harmonic average came back
 * at **+14 dB**, harmonics above the fundamental, which is not a thing an amplifier
 * does and was entirely the response of empty bands.
 *
 * An exponential sweep from 60 Hz to two fifths of the rate has energy in every
 * band, the same energy on both sides of the comparison, and no randomness to
 * average out.
 */
static void lad_chirp(float *x, uint32_t rate, double level)
{
    const uint32_t n = lad_n(rate);
    const uint32_t edge = (uint32_t)(0.02 * (double)rate);
    const double   f0 = 60.0, f1 = 0.4 * (double)rate;
    const double   t_end = (double)n / (double)rate;
    const double   k = log(f1 / f0);
    uint32_t       i;

    for (i = 0; i < n; i++) {
        const double t = (double)i / (double)rate;
        const double ph = 2.0 * PI * f0 * t_end / k * (exp(k * t / t_end) - 1.0);
        double       env = 1.0;
        if (i < edge) {
            env = 0.5 - 0.5 * cos(PI * (double)i / (double)edge);
        } else if (i + edge > n) {
            env = 0.5 - 0.5 * cos(PI * (double)(n - i) / (double)edge);
        }
        x[i] = (float)(level * env * sin(ph));
    }
}

/*
 * The device's own small-signal response at one frequency, in dB, interpolated
 * along the third-octave centres.  Used to take the linear network back out of a
 * harmonic ratio - see lad_sig.
 */
static double lad_lin_at(const double *lin, double f)
{
    int i;
    if (lin == NULL) {
        return 0.0;
    }
    if (f <= (double)k_spec_f[0]) {
        return lin[0];
    }
    for (i = 1; i < SPEC_N; i++) {
        if (f <= (double)k_spec_f[i]) {
            const double a = log(f / (double)k_spec_f[i - 1]);
            const double b = log((double)k_spec_f[i] / (double)k_spec_f[i - 1]);
            if (lin[i] < -299.0 || lin[i - 1] < -299.0) {
                return 0.0;
            }
            return lin[i - 1] + (lin[i] - lin[i - 1]) * (a / b);
        }
    }
    return lin[SPEC_N - 1];
}

/*
 * Twenty-five harmonic ratios and one normalised spectrum, from one render.
 *
 * WHY THE HARMONIC RATIOS ARE DIVIDED BY THE DEVICE'S OWN LINEAR RESPONSE
 *
 * Because a plain H2-over-H1 taken at the output is not a measurement of
 * distortion: it is a measurement of distortion times whatever the linear network
 * behind the valves does to those two frequencies.  A Marshall tone stack at noon
 * has its dip at 400 Hz, so on a 349 Hz note it takes eight decibels off the
 * fundamental and almost nothing off the second harmonic, and the ratio reads
 * eight decibels of distortion that is not there.  Measured on this chain: H2 came
 * out at -3.4 dB on that note against the capture's -20.2, a seventeen decibel
 * disagreement, most of it two tone stacks set differently.
 *
 * So each ratio has the device's own small-signal response at both frequencies
 * taken out of it: what is compared is what the *valves* made.  `lin` is that
 * response, measured on the same device by rendering the same probe far too quietly
 * to distort - which is a render per evaluation and worth it, because without it
 * the ladder spends its pre-stage filters correcting the output tone, and that is
 * the exact mistake the whole per-stage arrangement exists to stop.
 *
 * The spectrum in `sp` gets the same treatment, and that took a second look to get
 * right.  Left uncorrected it compares two *tones*, and the two sides do not have
 * the same one for a reason that has nothing to do with character: the Marshall
 * capture has a loudspeaker in it (-17.7 dB at 6.3 kHz) and our chain in this mode
 * has none, so five decibels of that term was a missing speaker.  Corrected, it
 * says where each device's nonlinearity *adds* energy relative to what a linear
 * pass through the same device would have given - which is cabinet-blind,
 * tone-stack-blind, and about character, which is what this step is for.  The
 * output bank and the impulse are what match the tone, later.
 */
static void lad_sig(const float *y, uint32_t rate, double *h, double *sp,
                    const double *lin, double *imd)
{
    const uint32_t seg = lad_seg(rate);
    const uint32_t edge = (uint32_t)(0.02 * (double)rate);
    const uint32_t body = seg - 2u * edge;
    int            k, m;

    for (k = 0; k < LAD_NOTE_N; k++) {
        const float *w = y + (uint32_t)k * seg + edge;
        const double f0 = k_lad_note[k];
        const double h1 = goertzel_db(w, body, f0, (double)rate);
        const double l1 = lad_lin_at(lin, f0);
        for (m = 0; m < LAD_HARM_N; m++) {
            const double f = f0 * (double)(m + 2);
            double       v = -90.0;
            if (f < 0.45 * (double)rate) {
                v = goertzel_db(w, body, f, (double)rate) - h1 -
                    (lad_lin_at(lin, f) - l1);
                /* Floored and capped: below -90 dB it is the render's own noise
                 * and a search that chases it is chasing nothing. */
                if (v < -90.0) {
                    v = -90.0;
                }
                if (v > 20.0) {
                    v = 20.0;
                }
            }
            h[k * LAD_HARM_N + m] = v;
        }
    }
    spectrum_of(y, (int)lad_n(rate), (float)rate, sp);
    if (lin != NULL) {
        int i;
        for (i = 0; i < SPEC_N; i++) {
            if (sp[i] > -299.0 && lin[i] > -299.0) {
                sp[i] -= lin[i];
            }
        }
    }
    /* The last segment: the difference product against the louder of the two
     * tones, with each frequency's own linear response divided out like the
     * harmonics above. */
    if (imd != NULL) {
        const float *w = y + (uint32_t)LAD_NOTE_N * seg + edge;
        const double body2 = (double)body;
        const double f1 = k_lad_imd[0], f2 = k_lad_imd[1], fd = f2 - f1;
        const double a1 = goertzel_db(w, body, f1, (double)rate);
        const double a2 = goertzel_db(w, body, f2, (double)rate);
        const double ad = goertzel_db(w, body, fd, (double)rate);
        const double ref = a1 > a2 ? a1 : a2;
        const double fref = a1 > a2 ? f1 : f2;
        double       v;
        (void)body2;
        v = ad - ref - (lad_lin_at(lin, fd) - lad_lin_at(lin, fref));
        if (v < -80.0) {
            v = -80.0;
        }
        if (v > 20.0) {
            v = 20.0;
        }
        *imd = v;
    }
}

static double lad_err(const double *ha, const double *sa, const double *hb,
                      const double *sb, double *h_out, double *s_out)
{
    double eh = 0.0, es = 0.0;
    int    i, ns = 0;

    for (i = 0; i < LAD_SIG_N; i++) {
        const double d = ha[i] - hb[i];
        eh += d * d;
    }
    eh = sqrt(eh / (double)LAD_SIG_N);
    for (i = 0; i < SPEC_N; i++) {
        if (k_spec_f[i] < 63.0f || k_spec_f[i] > 5000.0f ||
            sa[i] < -299.0 || sb[i] < -299.0) {
            continue;
        }
        es += (sa[i] - sb[i]) * (sa[i] - sb[i]);
        ns++;
    }
    es = sqrt(es / (double)(ns ? ns : 1));
    if (h_out != NULL) {
        *h_out = eh;
    }
    if (s_out != NULL) {
        *s_out = es;
    }
    return eh + 0.5 * es;
}

/* Mean harmonic-to-fundamental ratio over the whole probe, in dB: one number for
 * "how much distortion is there", which is what a knee is read off. */
static double lad_thd(const double *h)
{
    double s = 0.0;
    int    i;
    for (i = 0; i < LAD_SIG_N; i++) {
        s += pow(10.0, h[i] / 10.0);
    }
    return 10.0 * log10(s / (double)LAD_SIG_N + 1e-12);
}

/* Re-voice and render at one level, from the unit probe.  No rebake: the tables
 * belong to the valves and nothing here touches a valve, which is the whole reason
 * a search this size is affordable. */
static int lad_render(ag_amp_t *a, const ag_amp_cfg_t *cfg, const float *u,
                      float *y, uint32_t n, double level)
{
    uint32_t i;
    if (ag_amp_set_voicing(a, cfg) != 0) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        y[i] = ag_amp_tick(a, (float)((double)u[i] * level));
    }
    return 0;
}

/*
 * WHICH RUNGS A STAGE IS JUDGED ON, AND WHY IT IS NOT ONLY ITS OWN
 *
 * Its own rung is where the stage is the newest thing working, so that is where
 * the difference is attributable to it - that much is the procedure.  But the
 * block being fitted is in the signal path at *every* level, and a version of this
 * that scored only the stage's own rung found answers that were better there and
 * catastrophic above: the three-stage chain came back with harmonics **above** the
 * fundamental at half scale, +7 dB, having been improved by two decibels at a
 * two-hundredth of it.  A quiet-end fit that destroys the loud end is not a fit.
 *
 * So the score is its own rung at double weight and three more spread from there
 * to full scale.  The stage is still fitted where it starts working; it is simply
 * not allowed to buy that with the rest of the range.
 */
static int lad_rungs(int li0, int *rungs, double *wts)
{
    const int top = LAD_LVL_N - 1;
    int       cand[4], i, j, nr = 0;

    cand[0] = li0;
    cand[1] = li0 + 4 < top ? li0 + 4 : top;
    cand[2] = li0 + 8 < top ? li0 + 8 : top;
    cand[3] = top;
    /* A stage whose own rung *is* the top has nothing above it to protect, and
     * the four candidates collapse to one - which is a single-rung objective, the
     * thing this function exists to prevent.  Look down instead. */
    if (li0 >= top) {
        cand[1] = top - 3 > 0 ? top - 3 : 0;
        cand[2] = top - 6 > 0 ? top - 6 : 0;
        cand[3] = cand[2];
    }
    for (i = 0; i < 4; i++) {
        int dup = 0;
        for (j = 0; j < nr; j++) {
            if (rungs[j] == cand[i]) {
                dup = 1;
            }
        }
        if (!dup) {
            rungs[nr] = cand[i];
            wts[nr] = (cand[i] == li0) ? 2.0 : 1.0;
            nr++;
        }
    }
    return nr;
}

/*
 * The levels the two linear responses are measured at, and they differ on purpose.
 *
 * Ours can be as quiet as we like - a decade under the bottom rung puts a
 * ninety-decibel chain's own harmonics seventy decibels down, which is the network
 * and nothing else.  The capture cannot: it is a high-gain amplifier and it is
 * still making -35 dB of harmonics at a two-hundred-and-fiftieth of full scale,
 * which is the quietest honest reading available from it.  That is fine, because
 * what each level buys is *that device's* response, and each device's response is
 * divided out of that device's own harmonics.
 */
#define LAD_LIN_LEVEL     4.0e-4
#define LAD_CAP_LIN_LEVEL 4.0e-3

static double lad_obj(ag_amp_t *a, const ag_amp_cfg_t *cfg, const float *u,
                      const float *uch, float *y, uint32_t n, uint32_t rate,
                      const int *rungs, const double *wts, int nr,
                      const double *cap_h, const double *cap_sp,
                      const double *cap_imd, double *h_out, double *s_out,
                      double *i_out)
{
    double eh = 0.0, es = 0.0, ei = 0.0, wsum = 0.0;
    double lin[SPEC_N];
    int    r;

    if (lad_render(a, cfg, uch, y, n, LAD_LIN_LEVEL) != 0) {
        return 1.0e9;
    }
    spectrum_of(y, (int)n, (float)rate, lin);
    for (r = 0; r < nr; r++) {
        double h[LAD_SIG_N], sp[SPEC_N], e1 = 0.0, e2 = 0.0, im = -80.0, d;
        if (lad_render(a, cfg, u, y, n, lad_level(rungs[r])) != 0) {
            return 1.0e9;
        }
        lad_sig(y, rate, h, sp, lin, &im);
        (void)lad_err(h, sp, cap_h + (size_t)rungs[r] * LAD_SIG_N,
                      cap_sp + (size_t)rungs[r] * SPEC_N, &e1, &e2);
        /*
         * One-sided on purpose.  Less subsonic intermodulation than the amplifier
         * is not a fault anybody has ever complained about; more of it is the
         * farting the whole bass-cut argument is about.  A two-sided term would
         * let the search *add* it to score a point.
         */
        d = im - cap_imd[rungs[r]];
        if (d < 0.0) {
            d = 0.0;
        }
        eh += wts[r] * e1 * e1;
        es += wts[r] * e2 * e2;
        ei += wts[r] * d * d;
        wsum += wts[r];
    }
    eh = sqrt(eh / (wsum > 0.0 ? wsum : 1.0));
    es = sqrt(es / (wsum > 0.0 ? wsum : 1.0));
    ei = sqrt(ei / (wsum > 0.0 ? wsum : 1.0));
    if (h_out != NULL) {
        *h_out = eh;
    }
    if (s_out != NULL) {
        *s_out = es;
    }
    if (i_out != NULL) {
        *i_out = ei;
    }
    return eh + 0.5 * es + 0.5 * ei;
}

/*
 * WHERE THE WORK IS DONE, AT CONSTANT GAIN
 *
 * The ladder above fixes what each valve is fed.  What it cannot reach is how the
 * drive is *shared*, and on both high-gain models that is the biggest single thing
 * wrong: measured with `knee`, one valve does all the clipping and the ones in
 * front of it are linear amplifiers.  Two of the differences a listener hears come
 * straight out of that.
 *
 * The chain makes 8 dB more harmonic content than the capture once both are
 * saturated - and at saturation the amount no longer depends on level, so no drive
 * change fixes it; what fixes it is fewer stages clipping as hard.
 *
 * And the second harmonic is 15 to 25 dB high while the third is within two.  That
 * is asymmetry, and asymmetry is what a cascade of *inverting* stages cancels: each
 * valve flattens the half its predecessor left alone, so several valves sharing the
 * work come out more symmetric than one valve doing all of it.  Ours has one valve
 * doing all of it, so nothing cancels.
 *
 * So the drive is tilted from the back of the chain to the front, by one parameter,
 * **with the sum held at zero**.  Sum-zero is the whole difference between this and
 * the `gain[3]` attenuator that was tried and reverted: that one cut the last stage
 * and gave nothing back, so the model's small-signal gain fell below the model
 * below it and `test_model_gain_order` caught it.  A tilt takes the same decibels
 * off the back and puts them on the front, so the gain the chain has is the gain it
 * had - it is only earned in a different place.
 */
static double lad_tilt_db(int i, int n, double r)
{
    return r * (0.5 * (double)(n - 1) - (double)i);
}

/* The whole ladder in one number: the same error, unweighted, over five rungs
 * spanning the range.  Used to choose the tilt, because a tilt is not one stage's
 * business and cannot be judged on one stage's rung. */
static double lad_global(ag_amp_t *a, const ag_amp_cfg_t *cfg, const float *u,
                         const float *uch, float *y, uint32_t n, uint32_t rate,
                         const double *cap_h, const double *cap_sp,
                         const double *cap_imd, double *h_out, double *s_out,
                         double *i_out)
{
    static const int    rungs[5] = { 0, 3, 6, 9, LAD_LVL_N - 1 };
    static const double wts[5] = { 1.0, 1.0, 1.0, 1.0, 1.0 };
    return lad_obj(a, cfg, u, uch, y, n, rate, rungs, wts, 5, cap_h, cap_sp,
                   cap_imd, h_out, s_out, i_out);
}

static void mode_ladder(int argc, char **argv)
{
    const char    *cap = argc > 2 ? argv[2] : NULL;
    const char    *dipath = argc > 3 ? argv[3] : NULL;
    const int      passes = argc > 4 ? atoi(argv[4]) : 2;
    const float    drive = argc > 5 ? (float)atof(argv[5]) : 0.5f;
    const uint32_t rate = 22050u;
    uint32_t       crate = 48000u;
    ag_amp_cfg_t   cfg, work;
    ag_amp_t      *a = NULL;
    float         *x = NULL, *y = NULL, *xc = NULL, *xch = NULL;
    /* The capture's answer at every level, computed once - it does not move. */
    double        *cap_h = NULL, *cap_sp = NULL;
    double         cap_thd[LAD_LVL_N], cap_imd[LAD_LVL_N];
    double         cap_lin[SPEC_N];
    int            have_cap[LAD_LVL_N];
    uint32_t       n, cn;
    int            li, st, pass, b, k, max_st;
    double         lvl_used[AG_AMP_STAGES];
    int            lvl_idx[AG_AMP_STAGES];

    if (cap == NULL) {
        printf("  usage: tube_render ladder capture.nam di.wav [passes"
               " [drive]]\n");
        return;
    }
    {
        nam_model_t *m = nam_load(cap, 0, 0);
        if (m == NULL) {
            printf("  %s: %s\n", cap, nam_err());
            return;
        }
        if (nam_sample_rate(m) > 0) {
            crate = (uint32_t)nam_sample_rate(m);
        }
        nam_free(m);
    }
    n = lad_n(rate);
    cn = lad_n(crate);
    a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    x = (float *)malloc(sizeof(float) * n);
    y = (float *)malloc(sizeof(float) * n);
    xc = (float *)malloc(sizeof(float) * cn);
    xch = (float *)malloc(sizeof(float) * n);
    cap_h = (double *)malloc(sizeof(double) * LAD_LVL_N * LAD_SIG_N);
    cap_sp = (double *)malloc(sizeof(double) * LAD_LVL_N * SPEC_N);
    if (a == NULL || x == NULL || y == NULL || xc == NULL || xch == NULL ||
        cap_h == NULL || cap_sp == NULL) {
        goto done;
    }
    /* One unit chirp, scaled at render time like the note probe. */
    lad_chirp(xch, rate, 1.0);
    for (k = 0; k < AG_AMP_STAGES; k++) {
        lvl_idx[k] = LAD_LVL_N - 1;
        lvl_used[k] = lad_level(LAD_LVL_N - 1);
    }
    print_model();
    model_cfg(&cfg, (float)rate);
    cfg.drive = drive;
    max_st = cfg.n_stages;

    /*
     * Start from nothing in front of any stage.
     *
     * Not from whatever is stored: the stored answer for two of these models is a
     * single bank fitted against the whole take at once, and the whole claim of
     * the ladder is that that bank is in the wrong place.  Starting from it would
     * make this a refinement of the thing it is meant to replace.
     */
    for (st = 0; st < AG_AMP_STAGES; st++) {
        cfg.vtrim[st] = 0.0f;
        for (b = 0; b < AG_AMP_VOICE_N; b++) {
            cfg.voice[st][b].db = 0.0f;
        }
    }
    /* And with the output bank off throughout, because it is not this step's. */
    for (b = 0; b < AG_AMP_VOICE_N; b++) {
        cfg.tone[b].db = 0.0f;
    }

    /*
     * The capture's linear response first, from the same probe far too quietly to
     * distort, because every harmonic ratio below is divided by it.
     */
    {
        float *cy;
        lad_chirp(xc, crate, LAD_CAP_LIN_LEVEL);
        cy = capture_render(cap, xc, cn, 1.0f, 0);
        if (cy == NULL) {
            goto done;
        }
        spectrum_of(cy, (int)cn, (float)crate, cap_lin);
        free(cy);
        printf("  the capture's own response at %.0e: 100 Hz %+.1f, 400 %+.1f,"
               " 1k %+.1f, 3.15k %+.1f dB\n", LAD_CAP_LIN_LEVEL, cap_lin[3],
               cap_lin[9], cap_lin[13], cap_lin[18]);
    }

    printf("  playing the capture at %d levels from %.3f to 1.0, five notes"
           " each\n", LAD_LVL_N, lad_level(0));
    for (li = 0; li < LAD_LVL_N; li++) {
        float *cy;
        have_cap[li] = 0;
        cap_thd[li] = -300.0;
        cap_imd[li] = -80.0;
        lad_probe(xc, crate, lad_level(li));
        cy = capture_render(cap, xc, cn, 1.0f, 0);
        if (cy == NULL) {
            continue;
        }
        lad_sig(cy, crate, cap_h + (size_t)li * LAD_SIG_N,
                cap_sp + (size_t)li * SPEC_N, cap_lin, &cap_imd[li]);
        cap_thd[li] = lad_thd(cap_h + (size_t)li * LAD_SIG_N);
        have_cap[li] = 1;
        free(cy);
    }

    for (pass = 0; pass < (passes > 0 ? passes : 1); pass++) {
        double thd[AG_AMP_STAGES + 1][LAD_LVL_N];

        printf("\n  pass %d\n", pass + 1);
        /*
         * The knee table: the chain through one stage, then two, and so on, at
         * every level.  This is the only place n_stages moves, and it moves the
         * bake with it, so it is a build per stage count rather than a re-voice.
         */
        for (li = 0; li < LAD_LVL_N; li++) {
            thd[0][li] = -300.0;
        }
        for (st = 1; st <= max_st; st++) {
            work = cfg;
            work.n_stages = st;
            for (li = 0; li < LAD_LVL_N; li++) {
                double   h[LAD_SIG_N], sp[SPEC_N];
                uint32_t i;
                thd[st][li] = -300.0;
                lad_probe(x, rate, lad_level(li));
                if (li == 0) {
                    if (ag_amp_build(a, g_ckt, &work, g_tab, 0, x, (int)n) != 0) {
                        break;
                    }
                } else if (ag_amp_set_voicing(a, &work) != 0) {
                    break;
                }
                ag_amp_reset(a);
                for (i = 0; i < n; i++) {
                    y[i] = ag_amp_tick(a, x[i]);
                }
                lad_sig(y, rate, h, sp, NULL, NULL);
                thd[st][li] = lad_thd(h);
            }
        }
        printf("    mean harmonic level, dB under the fundamental\n"
               "     level   capture");
        for (st = 1; st <= max_st; st++) {
            printf("   %d st%s", st, st > 1 ? "" : " ");
        }
        printf("\n");
        for (li = 0; li < LAD_LVL_N; li++) {
            printf("     %5.3f  %7.1f", lad_level(li), cap_thd[li]);
            for (st = 1; st <= max_st; st++) {
                printf("  %6.1f", thd[st][li]);
            }
            printf("\n");
        }

        /*
         * Stage k's level: the quietest one where it adds more than 3 dB over the
         * stages in front of it.
         *
         * The first stage has nothing in front of it to add over, and a version of
         * this that compared it against a floor of -300 dB put it on the bottom
         * rung of the ladder every time - the one place it is provably not
         * working.  For that one the rule is absolute instead: the quietest level
         * at which one valve on its own makes -40 dB of harmonics, which is where
         * a first valve starts to be more than a gain block.
         */
        for (k = max_st - 1; k >= 0; k--) {
            int pick = -1;
            int j;
            for (li = 0; li < LAD_LVL_N; li++) {
                const double add = (k == 0) ? thd[1][li] + 40.0
                                            : thd[k + 1][li] - thd[k][li];
                if (thd[k + 1][li] > -299.0 && add > 3.0) {
                    pick = li;
                    break;
                }
            }
            /*
             * A STAGE THAT NEVER ADDS 3 dB HAS NO RUNG, AND MUST NOT BE GIVEN ONE
             *
             * The four-valve model's last stage is a recovery stage: across the
             * whole ladder it adds nothing - at the bottom rung it comes out half a
             * decibel *quieter* in harmonics than three stages alone.  The first
             * version handed it the top rung as a default and then the "never below
             * the stage behind it" clamp dragged the other three up to the top with
             * it, so all four were fitted at full scale, on one rung, with the
             * guard rungs collapsed into it.  What came back was +30 dB of matching
             * gain and ten decibels too much distortion.
             *
             * So a stage with no knee is left alone - its block stays at zero,
             * which is the honest answer for a valve that is not distorting - and it
             * is excluded from the clamp, which only orders the stages that do work.
             */
            lvl_idx[k] = pick;
            if (pick < 0) {
                lvl_used[k] = -1.0;
                continue;
            }
            /* Never below the nearest working stage behind it: the ladder only
             * goes up. */
            for (j = k + 1; j < max_st; j++) {
                if (lvl_idx[j] >= 0) {
                    if (pick < lvl_idx[j]) {
                        pick = lvl_idx[j];
                    }
                    break;
                }
            }
            lvl_idx[k] = pick;
            lvl_used[k] = lad_level(pick);
        }

        /*
         * And the fit itself, last stage first.  Coordinate descent on the seven
         * bands, three rounds of halving steps, bounded at +-8 dB - which is the
         * "reasonable, not absurd" rule as a number.  A band that wants more than
         * eight decibels in front of a valve is asking the matching layer to
         * supply something the circuit does not have, and that is a netlist
         * question.
         */
        for (k = max_st - 1; k >= 0; k--) {
            const int li_k = lvl_idx[k];
            double    best, e0, eh = 0.0, es = 0.0, ei = 0.0, wts[4];
            int       rungs[4], nr, round;

            if (li_k < 0) {
                printf("    stage %d adds nothing at any level, so its block"
                       " stays at zero\n", k + 1);
                continue;
            }
            if (!have_cap[li_k]) {
                continue;
            }
            work = cfg;
            work.n_stages = max_st;
            nr = lad_rungs(li_k, rungs, wts);
            lad_probe(x, rate, 1.0);
            if (ag_amp_build(a, g_ckt, &work, g_tab, 0, x, (int)n) != 0) {
                break;
            }
            e0 = lad_obj(a, &work, x, xch, y, n, rate, rungs, wts, nr, cap_h,
                         cap_sp, cap_imd, &eh, &es, &ei);
            best = e0;

            /*
             * The eighth coordinate is the flat trim, and it is not optional.
             *
             * On the crunch model the chain makes **9.4 dB more harmonic content
             * than the amplifier at every level from a hundredth of full scale
             * up**, and seven peaking sections cannot take away a broadband
             * amount - they can only move it about.  A search without the trim
             * reached 11 dB of harmonic error and stopped, which is what "the
             * shape is right and there is too much of it" looks like from inside a
             * shape-only search.
             *
             * This is the same physical quantity the rejected `gain[3]`
             * attenuator was, and the difference is the whole reason this one is
             * allowed: it is per stage and fitted, so a cut in front of the last
             * valve is paid for by a lift in front of the first, and the chain's
             * small-signal gain is not quietly thrown away.  It also lives in the
             * matching layer rather than in `gain[]`, which keeps the schematic
             * readable as the schematic.
             */
            for (round = 0; round < 3; round++) {
                const float step = 3.0f / (float)(1 << round);
                for (b = 0; b <= AG_AMP_VOICE_N; b++) {
                    const int   trim = (b == AG_AMP_VOICE_N);
                    const float lim = trim ? 10.0f : 8.0f;
                    int         dir;
                    for (dir = 0; dir < 2; dir++) {
                        float      *cell = trim ? &work.vtrim[k]
                                                : &work.voice[k][b].db;
                        const float was = *cell;
                        const float try_db = was + (dir == 0 ? step : -step);
                        double      e;
                        if (try_db > lim || try_db < -lim) {
                            continue;
                        }
                        *cell = try_db;
                        e = lad_obj(a, &work, x, xch, y, n, rate, rungs, wts,
                                    nr, cap_h, cap_sp, cap_imd, NULL, NULL,
                                    NULL);
                        if (e < best - 1e-6) {
                            best = e;
                        } else {
                            *cell = was;
                        }
                    }
                }
                /*
                 * RE-CENTRED AFTER EVERY ROUND: THE BANDS CARRY SHAPE, THE TRIM
                 * CARRIES LEVEL
                 *
                 * Seven peaking sections and a flat trim are redundant - the
                 * bands can make a broadband change by all moving together - and
                 * a search handed redundant parameters will use them.  It did:
                 * the two-valve model came back with -7.5 dB at 3.15 and 5 kHz in
                 * front of the first valve and +7.5 dB at the same two in front of
                 * the second, which is a level shift written as fourteen decibels
                 * of tone that cancels, with four bands pinned at their bound and
                 * no shape left to fit.
                 *
                 * So each round ends by taking the bank's mean out of the bank and
                 * putting it in the trim.  The response is the same to within what
                 * seven skirts do between their centres, which is why `best` is
                 * measured again rather than assumed; what changes is that the
                 * bounds now mean something - +-8 dB of *shape* around the stage's
                 * own level, and the level itself in one number a reader can see.
                 */
                {
                    double mean = 0.0;
                    for (b = 0; b < AG_AMP_VOICE_N; b++) {
                        mean += (double)work.voice[k][b].db;
                    }
                    mean /= (double)AG_AMP_VOICE_N;
                    if (mean > 0.05 || mean < -0.05) {
                        for (b = 0; b < AG_AMP_VOICE_N; b++) {
                            work.voice[k][b].db -= (float)mean;
                        }
                        work.vtrim[k] += (float)mean;
                        best = lad_obj(a, &work, x, xch, y, n, rate, rungs, wts,
                                       nr, cap_h, cap_sp, cap_imd, NULL, NULL,
                                       NULL);
                    }
                }
            }
            /* The winner's own two numbers, for the report. */
            (void)lad_obj(a, &work, x, xch, y, n, rate, rungs, wts, nr, cap_h,
                          cap_sp, cap_imd, &eh, &es, &ei);
            for (b = 0; b < AG_AMP_VOICE_N; b++) {
                cfg.voice[k][b].db = work.voice[k][b].db;
            }
            cfg.vtrim[k] = work.vtrim[k];
            printf("    stage %d at level %.3f: %.2f -> %.2f"
                   "  (harmonics %.2f, spectrum %.2f, 49 Hz excess %.2f dB,"
                   " trim %+.1f dB)\n",
                   k + 1, lvl_used[k], e0, best, eh, es, ei,
                   (double)cfg.vtrim[k]);
            printf("      ");
            for (b = 0; b < AG_AMP_VOICE_N; b++) {
                printf(" %.0f:%+.1f", (double)cfg.voice[k][b].hz,
                       (double)cfg.voice[k][b].db);
            }
            printf("\n");
        }

        /*
         * THE PER-STAGE TRIMS REDISTRIBUTE DRIVE; THEY DO NOT SUPPLY IT
         *
         * Each stage's trim was chosen where that stage works, and nothing in that
         * search knows or cares what the *sum* comes to.  Left alone it came to
         * +30 dB on the four-valve model: thirty decibels of gain manufactured in
         * the matching layer, which is not a matching correction at all - it is the
         * statement that the circuit's gain is wrong, and that belongs in the
         * netlist.
         *
         * So the vector is brought back to zero mean here.  What that removes is a
         * uniform level change across every stage - which is precisely the one
         * global move the procedure does sanction, and it is made a few lines
         * below, from the compression measurement, where it can be justified by a
         * number instead of falling out of a search.
         */
        {
            double mean = 0.0;
            for (st = 0; st < max_st; st++) {
                mean += (double)cfg.vtrim[st];
            }
            mean /= (double)max_st;
            if (mean > 0.05 || mean < -0.05) {
                for (st = 0; st < max_st; st++) {
                    cfg.vtrim[st] -= (float)mean;
                }
                printf("    the trims summed to %+.1f dB of gain; taken out,"
                       " they are", mean * (double)max_st);
                for (st = 0; st < max_st; st++) {
                    printf(" %+.1f", (double)cfg.vtrim[st]);
                }
                printf(" dB\n");
            }
        }

        /*
         * THE TILT, ONCE, AFTER THE FIRST PASS OF BANDS - see lad_tilt_db.
         *
         * After rather than before, because it is judged on the whole ladder and
         * the whole ladder is easier to read once each stage is fed something
         * sensible; and only once, so that the second pass of bands is fitted
         * with the drive already where it belongs rather than being asked to
         * absorb a change made after it.
         *
         * Coarse scan and then a refinement rather than a descent: the objective
         * is not smooth in this parameter - moving three decibels of drive from
         * the last valve to the first changes *which* valves clip at all - and a
         * local step stops at the first flat spot.
         */
        if (pass == 0 && max_st > 1) {
            double best_r = 0.0, best_e, e, eh = 0.0, es = 0.0, ei = 0.0;
            double base[AG_AMP_STAGES];
            int    i;

            for (st = 0; st < max_st; st++) {
                base[st] = (double)cfg.vtrim[st];
            }
            work = cfg;
            work.n_stages = max_st;
            lad_probe(x, rate, 1.0);
            if (ag_amp_build(a, g_ckt, &work, g_tab, 0, x, (int)n) == 0) {
                best_e = lad_global(a, &work, x, xch, y, n, rate, cap_h, cap_sp,
                                    cap_imd, &eh, &es, &ei);
                printf("\n    tilting the drive from the back of the chain to"
                       " the front, sum held at zero\n"
                       "      r = %4.1f dB: %.2f  (harmonics %.2f,"
                       " spectrum %.2f, 49 Hz %.2f)\n", 0.0, best_e, eh, es,
                       ei);
                for (i = 1; i <= 12; i++) {
                    const double r = (i <= 7) ? 2.0 * (double)i
                                              : best_r + 0.5 * (double)(i - 10);
                    if (r < -14.0 || r > 14.0 ||
                        (i > 7 && (r == best_r || r <= 0.0))) {
                        continue;
                    }
                    for (st = 0; st < max_st; st++) {
                        work.vtrim[st] =
                            (float)(base[st] + lad_tilt_db(st, max_st, r));
                    }
                    e = lad_global(a, &work, x, xch, y, n, rate, cap_h, cap_sp,
                                   cap_imd, &eh, &es, &ei);
                    printf("      r = %4.1f dB: %.2f  (harmonics %.2f,"
                           " spectrum %.2f, 49 Hz %.2f)\n", r, e, eh, es, ei);
                    if (e < best_e) {
                        best_e = e;
                        best_r = r;
                    }
                }
                for (st = 0; st < max_st; st++) {
                    cfg.vtrim[st] =
                        (float)(base[st] + lad_tilt_db(st, max_st, best_r));
                }
                printf("      kept r = %.1f dB, so the trims are", best_r);
                for (st = 0; st < max_st; st++) {
                    printf(" %+.1f", (double)cfg.vtrim[st]);
                }
                printf(" dB\n");
            }
        }
    }

    /*
     * THE LAST STEP: THE SAME TRIM ON EVERY STAGE, AGAINST COMPRESSION
     *
     * Every stage now makes the right kind of distortion at the level where it is
     * the one working.  What that does not settle is how much of it the chain does
     * as a whole, and the number for that is compression: the same take through
     * the chain twice, twenty decibels apart.  If it is off, the fix is not a new
     * shape on one stage - it is the same trim on all of them, which is the one
     * thing seven peaking sections cannot make.
     *
     * Bisection rather than descent, because it is monotone by construction: less
     * into every valve is less clipping in every valve.
     */
    if (dipath != NULL) {
        float   *di = NULL, *dic = NULL, *wet = NULL, *quiet = NULL;
        uint32_t dn = 0, drate = 0, dnc = 0;

        di = read_wav(dipath, &dn, &drate);
        if (di != NULL && drate == rate && dn > rate) {
            /* Three seconds is plenty for an energy ratio and keeps this step
             * from costing more than the whole ladder above it. */
            if (dn > 3u * rate) {
                dn = 3u * rate;
            }
            if (crate == rate) {
                dic = di;
                dnc = dn;
            } else {
                dic = wr_resample_f(di, dn, rate, crate, &dnc, 0);
            }
            wet = dic != NULL ? capture_render(cap, dic, dnc, 1.0f, 0) : NULL;
            quiet = dic != NULL ? capture_render(cap, dic, dnc, 0.1f, 0) : NULL;
            if (wet != NULL && quiet != NULL) {
                const double cap_comp =
                    20.0 - (energy_db(wet, dnc) - energy_db(quiet, dnc));
                double best_t = 0.0, best_e, c0, c1;
                int    it;

                work = cfg;
                c0 = chain_compression(&work, di, dn, rate);
                best_e = fabs(c0 - cap_comp);
                printf("\n  compression: the capture %.1f dB, the chain"
                       " %.1f dB\n", cap_comp, c0);
                /*
                 * SCANNED AND BOUNDED, NOT BISECTED - AND IT MAY DECLINE
                 *
                 * Bisection assumes the thing it is solving for is monotone and
                 * reachable.  Compression is monotone in drive but on a chain that
                 * is already saturated it is very nearly *flat*: the four-valve
                 * model was 1.2 dB short and the bisection answered "+8.9 dB on
                 * every stage", which is thirty-six decibels of gain bought to move
                 * one - and it landed on the bound, which is a search reporting
                 * that it could not do the job while sounding as if it had.
                 *
                 * So: a bounded scan, the smallest trim that gets closest, and if
                 * nothing inside the bound is at least a third of a decibel better
                 * than doing nothing, do nothing and say so.  A compression gap
                 * that a reasonable trim cannot close is a fact about the circuit,
                 * and the output impulse is what closes the rest.
                 */
                for (it = -12; it <= 12; it++) {
                    const double t = 0.5 * (double)it;
                    double       e;
                    if (t == 0.0) {
                        continue;
                    }
                    for (st = 0; st < max_st; st++) {
                        work.vtrim[st] = cfg.vtrim[st] + (float)t;
                    }
                    e = fabs(chain_compression(&work, di, dn, rate) - cap_comp);
                    if (e < best_e - 1.0e-6) {
                        best_e = e;
                        best_t = t;
                    }
                }
                if (best_t != 0.0 && fabs(c0 - cap_comp) - best_e > 0.3) {
                    for (st = 0; st < max_st; st++) {
                        cfg.vtrim[st] += (float)best_t;
                        work.vtrim[st] = cfg.vtrim[st];
                    }
                    c1 = chain_compression(&work, di, dn, rate);
                    printf("  %+.2f dB on every stage brings it to %.1f dB\n",
                           best_t, c1);
                } else {
                    printf("  no trim inside +-6 dB brings it closer than %.1f dB,"
                           " so none is applied\n", fabs(c0 - cap_comp));
                }
            }
            if (dic != di) {
                free(dic);
            }
            free(wet);
            free(quiet);
        } else if (di != NULL) {
            printf("\n  %s is %u Hz and the ladder runs at %u; compression"
                   " step skipped\n", dipath, drate, rate);
        }
        free(di);
    }

    /* And the answer, in the form it has to be pasted in. */
    printf("\n  paste into ag_amp.c, %s:\n\n", ag_amp_model_name(g_model));
    for (k = 0; k < max_st; k++) {
        printf("        static const ag_amp_band_t pre%d[AG_AMP_VOICE_N] = {\n",
               k);
        for (b = 0; b < AG_AMP_VOICE_N; b++) {
            printf("%s{ %.1ff, %.2ff, 1.0f }%s",
                   (b % 2 == 0) ? "            " : " ",
                   (double)cfg.voice[k][b].hz, (double)cfg.voice[k][b].db,
                   b == AG_AMP_VOICE_N - 1 ? "\n"
                                           : ((b % 2 == 1) ? ",\n" : ","));
        }
        printf("        };\n");
    }
    printf("        static const float vtrim[%d] = {", max_st);
    for (k = 0; k < max_st; k++) {
        printf(" %.2ff%s", (double)cfg.vtrim[k], k == max_st - 1 ? " " : ",");
    }
    printf("};\n");

done:
    free(a);
    free(x);
    free(y);
    free(xc);
    free(xch);
    free(cap_h);
    free(cap_sp);
}

/* ------------------------------------------------------------------------ */
/* step1 - put our clipping onset on the amplifier's, with one flat trim      */
/* ------------------------------------------------------------------------ */

/*
 *   AG_MODEL=slo tube_render step1 "assets/audio/guitar-di/5150red.nam" [drive [target]]
 *
 * THE FIRST STEP OF THE MATCH, AND THE ONLY THING IT IS ALLOWED TO CARE ABOUT
 *
 * Find the input voltage at which each side makes a light overdrive, and move ours
 * onto the amplifier's with a flat gain in the tone block in front of the first
 * stage.  Nothing else: not the spectrum, not the harmonic structure, not the
 * compression figure.  Only the level at which the same *amount* of overdrive
 * appears.
 *
 * "The same amount" is the products-to-notes ratio - all the energy that is not one
 * of the two notes, over the energy that is.  Not the compression figure, and that
 * is a measurement rather than a preference: this chain compresses like a hard
 * limiter (two decibels for every two of input, so the output is frozen) while the
 * amplifier compresses gradually, so at any one input the two compression figures
 * differ by eighteen decibels and cannot be a shared yardstick.  The products ratio
 * has the same plateau on both sides - -8.4 dB against -8.9 - so it is a scale
 * marked identically at both ends.  -20 dB is 10% and is the default target.
 *
 * THE SAME TRIM ON EVERY PRE-STAGE BLOCK, WHICH MAKES IT A SEARCH
 *
 * Not `cfg.vtrim[0]` alone.  The architecture has one tone block in front of every
 * stage, so a flat coefficient means the same number of decibels in all of them -
 * that is what "a flat coefficient of the tone blocks" means, and it is the thing
 * that keeps the correction from being a single input attenuator wearing a
 * different name.
 *
 * That costs the arithmetic.  A trim in front of the first valve slides our whole
 * curve along the voltage axis by exactly that many decibels, because everything
 * ahead of it is linear.  A trim in front of the *third* valve does not: what
 * reaches that valve has already been through two clipping stages, so taking three
 * decibels off it moves that stage's own onset by three and the chain's by
 * something else.  The total leverage is therefore neither 1x nor n x the per-stage
 * value, and it has to be measured.  So: bisect on one per-stage number until our
 * crossing lands on the amplifier's.
 *
 * The report then gives the other thresholds.  If our curve has the same slope as
 * the amplifier's, aligning one point aligns all of them.  If it does not, the
 * other rows say so, and that is the next step's problem rather than something to
 * hide by choosing a different target.
 */

#define ST1_N 41 /* 0.05 mV to 0.5 V, 2 dB a step: 80 dB */
#define ST1_K 121

/*
 * The two-tone probe into a buffer.  Its own function because it has to exist
 * *before* ag_amp_build sees the buffer, and the first version of these two modes
 * did not do that: the build fits each stage's table axes to the probe it is handed,
 * and it was handed a freshly malloc'd buffer that nothing had written yet.  Two
 * identical runs then behaved twenty decibels apart, because the axes had been
 * fitted to whatever was in that memory.  A build takes a *signal*, and the signal
 * has to be the loudest one the run will render, or the axes are short.
 */
static void st1_probe(float *x, uint32_t n, uint32_t edge, uint32_t rate,
                      double vin, double phase)
{
    const double fe = 2.0 * 41.205, fb = 3.0 * 41.205;
    uint32_t     i;
    for (i = 0; i < n; i++) {
        const double tt = (double)i / (double)rate;
        double       env = 1.0;
        if (i < edge) {
            env = 0.5 - 0.5 * cos(PI * (double)i / (double)edge);
        } else if (i + edge > n) {
            env = 0.5 - 0.5 * cos(PI * (double)(n - i) / (double)edge);
        }
        x[i] = (float)(0.5 * vin * env *
                       (sin(2.0 * PI * fe * tt) +
                        sin(2.0 * PI * fb * tt + phase)));
    }
}

/* Products-to-notes ratio in dB at one level, averaged over two phases of the
 * second note.  `cap` renders the amplifier; NULL renders our chain. */
static double st1_point(ag_amp_t *a, const char *cap, float *x, uint32_t n,
                        uint32_t edge, uint32_t body, uint32_t rate, double vin,
                        double *out_db)
{
    const double fd = 41.205, fe = 2.0 * 41.205, fb = 3.0 * 41.205;
    double       notes = 0.0, prod = 0.0, en = 0.0;
    int          ph, k;
    uint32_t     i;

    for (ph = 0; ph < 2; ph++) {
        const double phase = (ph == 0) ? 0.0 : PI * 0.5;
        float       *y;
        st1_probe(x, n, edge, rate, vin, phase);
        if (cap != NULL) {
            y = capture_render(cap, x, n, 1.0f, 0);
            if (y == NULL) {
                return 1.0e9;
            }
        } else {
            y = (float *)malloc(sizeof(float) * n);
            if (y == NULL) {
                return 1.0e9;
            }
            ag_amp_reset(a);
            for (i = 0; i < n; i++) {
                y[i] = ag_amp_tick(a, x[i]);
            }
        }
        for (k = 1; k <= ST1_K; k++) {
            const double f = fd * (double)k;
            double       e;
            if (f >= 0.45 * (double)rate) {
                continue;
            }
            e = pow(10.0, goertzel_db(y + edge, body, f, (double)rate) / 10.0);
            if (k == 2 || k == 3) {
                notes += e;
            } else {
                prod += e;
            }
        }
        for (i = edge; i < edge + body; i++) {
            en += (double)y[i] * (double)y[i];
        }
        free(y);
    }
    if (out_db != NULL) {
        *out_db = 10.0 * log10(en / (double)(2u * body) + 1e-30);
    }
    return 10.0 * log10(prod / (notes > 0.0 ? notes : 1e-30) + 1e-30);
}

/* Where a curve first reaches `thr`, interpolated in log voltage. */
static double st1_cross(const double *lv, const double *d, int n, double thr)
{
    int i;
    for (i = 1; i < n; i++) {
        if (d[i - 1] < thr && d[i] >= thr) {
            const double f = (thr - d[i - 1]) / (d[i] - d[i - 1]);
            return lv[i - 1] * pow(lv[i] / lv[i - 1], f);
        }
    }
    return -1.0;
}

static void mode_step1(int argc, char **argv)
{
    static const double thr[4] = { -26.0, -20.0, -14.0, -11.0 };
    const char    *cap = argc > 2 ? argv[2] : NULL;
    const float    drive = argc > 3 ? (float)atof(argv[3]) : 0.5f;
    const double   target = argc > 4 ? atof(argv[4]) : -20.0;
    const uint32_t rate = 22050u;
    uint32_t       crate = 48000u;
    ag_amp_cfg_t   cfg;
    ag_amp_t      *a = NULL;
    float         *x = NULL, *xc = NULL;
    double         lv[ST1_N], od[ST1_N], ad[ST1_N], od2[ST1_N];
    double         oo[ST1_N], ao[ST1_N];
    double         vo, va, trim = 0.0;
    uint32_t       n, cn, edge, body, cedge, cbody;
    int            li, t;

    if (cap == NULL) {
        printf("  usage: tube_render step1 capture.nam [drive [target dB]]\n");
        return;
    }
    {
        nam_model_t *m = nam_load(cap, 0, 0);
        if (m == NULL) {
            printf("  %s: %s\n", cap, nam_err());
            return;
        }
        if (nam_sample_rate(m) > 0) {
            crate = (uint32_t)nam_sample_rate(m);
        }
        nam_free(m);
    }
    edge = (uint32_t)(0.02 * (double)rate);
    body = (uint32_t)(20.0 / 41.205 * (double)rate);
    n = 2u * edge + body;
    cedge = (uint32_t)(0.02 * (double)crate);
    cbody = (uint32_t)(20.0 / 41.205 * (double)crate);
    cn = 2u * cedge + cbody;

    a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    x = (float *)malloc(sizeof(float) * n);
    xc = (float *)malloc(sizeof(float) * cn);
    if (a == NULL || x == NULL || xc == NULL) {
        goto done;
    }
    print_model();
    model_cfg(&cfg, (float)rate);
    cfg.drive = drive;
    {
        int st, b;
        for (st = 0; st < AG_AMP_STAGES; st++) {
            cfg.vtrim[st] = 0.0f;
            for (b = 0; b < AG_AMP_VOICE_N; b++) {
                cfg.voice[st][b].db = 0.0f;
            }
        }
        for (b = 0; b < AG_AMP_VOICE_N; b++) {
            cfg.tone[b].db = 0.0f;
        }
        cfg.mid_db = 0.0f;
    }
    for (li = 0; li < ST1_N; li++) {
        lv[li] = 0.00005 * pow(10.0, (double)li * 4.0 / 40.0);
    }
    printf("  target: products / notes = %.1f dB.  %.5f to %.3f V at the first"
           " grid, 2 dB a step;\n  drive %.2f, take amplitude V / drive.  No"
           " cabinet either side, every matching filter\n  linear, energies"
           " averaged over two phases of the second note.\n\n", target, lv[0],
           lv[ST1_N - 1], (double)drive);

    /* The two curves as they stand.  The build is handed the loudest probe of the
     * sweep, because that is what its axes have to cover. */
    {
        int rc = 0;
        st1_probe(x, n, edge, rate,
                  lv[ST1_N - 1] / (double)(drive > 0.0f ? drive : 1.0f), 0.0);
        if (ag_amp_build(a, g_ckt, &cfg, g_tab, 0, x, (int)n) != 0) {
            goto done;
        }
        for (li = 0; li < ST1_N; li++) {
            const double vin = lv[li] / (double)(drive > 0.0f ? drive : 1.0f);
            od[li] = st1_point(a, NULL, x, n, edge, body, rate, vin, &oo[li]);
            ad[li] = st1_point(a, cap, xc, cn, cedge, cbody, crate, vin, &ao[li]);
            if (od[li] > 1.0e8 || ad[li] > 1.0e8) {
                rc = -1;
                break;
            }
        }
        if (rc != 0) {
            goto done;
        }
    }

    printf("  BEFORE - products / notes, dB\n");
    printf("     V       in dBV     ours    amplifier\n");
    for (li = 0; li < ST1_N; li++) {
        printf("   %.5f   %6.1f   %7.1f    %7.1f\n", lv[li],
               20.0 * log10(lv[li]), od[li], ad[li]);
    }

    vo = st1_cross(lv, od, ST1_N, target);
    va = st1_cross(lv, ad, ST1_N, target);
    if (vo <= 0.0 || va <= 0.0) {
        printf("\n  one of the two never crosses %.1f dB inside the sweep - widen"
               " it or pick another\n  target.\n", target);
        goto done;
    }
    trim = 20.0 * log10(vo / va);
    printf("\n  %.1f dB of products: ours at %.5f V, the amplifier at %.5f V -"
           " we are %.1f dB early.\n", target, vo, va, -trim);

    /*
     * Bisect on one per-stage number.  More negative trim means less into every
     * valve, which means our crossing moves to a higher voltage, so the residual
     * below is monotone decreasing in `t` and a bisection is honest.
     *
     * Fifteen decibels a stage is the bound on each side.  If the answer wants
     * more than that, the trim is not what is wrong and the search saying so is
     * the useful output.
     */
    {
        double lo = -15.0, hi = 2.0, t = 0.0;
        int    it, st;
        printf("\n  bisecting one per-stage trim over %d blocks", cfg.n_stages);
        for (it = 0; it < 11; it++) {
            double v;
            t = 0.5 * (lo + hi);
            for (st = 0; st < cfg.n_stages; st++) {
                cfg.vtrim[st] = (float)t;
            }
            if (ag_amp_set_voicing(a, &cfg) != 0) {
                goto done;
            }
            for (li = 0; li < ST1_N; li++) {
                const double vin = lv[li] / (double)(drive > 0.0f ? drive : 1.0f);
                od2[li] = st1_point(a, NULL, x, n, edge, body, rate, vin, NULL);
            }
            v = st1_cross(lv, od2, ST1_N, target);
            if (v <= 0.0) {
                /* Never crosses inside the sweep: too much attenuation. */
                lo = t;
                continue;
            }
            if (v > va) {
                lo = t;
            } else {
                hi = t;
            }
            printf(".");
        }
        printf("\n  **%+.2f dB on each of the %d blocks** puts our %.1f dB point at"
               " %.5f V against the\n  amplifier's %.5f V.  Total through the"
               " chain: %+.1f dB.\n", t, cfg.n_stages, target,
               st1_cross(lv, od2, ST1_N, target), va, t * (double)cfg.n_stages);
        trim = t;
    }

    printf("\n  AFTER %+.2f dB on every block - where each side crosses, volts at"
           " the first grid\n", trim);
    printf("    products / notes     ours        amplifier     ours is\n");
    for (t = 0; t < 4; t++) {
        const double v1 = st1_cross(lv, od2, ST1_N, thr[t]);
        const double v2 = st1_cross(lv, ad, ST1_N, thr[t]);
        printf("      %5.0f dB", thr[t]);
        if (v1 > 0.0) {
            printf("        %8.5f V", v1);
        } else {
            printf("           -     ");
        }
        if (v2 > 0.0) {
            printf("   %8.5f V", v2);
        } else {
            printf("      -     ");
        }
        if (v1 > 0.0 && v2 > 0.0) {
            printf("   %+5.1f dB %s\n", 20.0 * log10(v1 / v2),
                   v1 < v2 ? "early" : "late ");
        } else {
            printf("      -\n");
        }
    }
    /*
     * The plateau, and it does move - which the first version of this line claimed
     * it could not.  A trim in front of the *first* stage is a pure input gain and
     * cannot change what an already saturated chain makes; the same trim in front of
     * every stage is not, because it changes what each valve gets relative to the
     * one before it.  Measured on `slo`: -7.9 dB became -6.7 when 7.84 dB went on
     * all four blocks, against the amplifier's -8.9.
     */
    printf("\n  plateau, where both are fully saturated: ours %.1f dB, the"
           " amplifier %.1f dB.\n  A trim on every block is not a pure input gain,"
           " so this can move - and does.\n", od2[ST1_N - 1], ad[ST1_N - 1]);
    /*
     * And how much of the onset each block actually bought, because on a
     * four-stage chain one of the four is a recovery stage that never clips: a
     * trim in front of it takes level out of the output and moves no onset at all.
     * Printed as the crossing with that one block's trim removed.
     */
    {
        int    st;
        double v_all, v_less;
        for (st = 0; st < cfg.n_stages; st++) {
            cfg.vtrim[st] = (float)trim;
        }
        if (ag_amp_set_voicing(a, &cfg) == 0) {
            for (li = 0; li < ST1_N; li++) {
                const double vin = lv[li] / (double)(drive > 0.0f ? drive : 1.0f);
                od2[li] = st1_point(a, NULL, x, n, edge, body, rate, vin, NULL);
            }
            v_all = st1_cross(lv, od2, ST1_N, target);
            printf("\n  what each block bought, %.1f dB crossing with that one"
                   " block back at 0 dB:\n", target);
            for (st = 0; st < cfg.n_stages; st++) {
                cfg.vtrim[st] = 0.0f;
                if (ag_amp_set_voicing(a, &cfg) == 0) {
                    for (li = 0; li < ST1_N; li++) {
                        const double vin =
                            lv[li] / (double)(drive > 0.0f ? drive : 1.0f);
                        od2[li] = st1_point(a, NULL, x, n, edge, body, rate, vin,
                                            NULL);
                    }
                    v_less = st1_cross(lv, od2, ST1_N, target);
                    if (v_less > 0.0 && v_all > 0.0) {
                        printf("    block %d: %.5f V -> %.5f V, so it is worth"
                               " %.1f dB of onset\n", st + 1, v_less, v_all,
                               20.0 * log10(v_all / v_less));
                    }
                }
                cfg.vtrim[st] = (float)trim;
            }
        }
    }

done:
    free(a);
    free(x);
    free(xc);
}

/* ------------------------------------------------------------------------ */
/* onset - the input voltage where clipping starts, ours and the amplifier's   */
/* ------------------------------------------------------------------------ */

/*
 *   AG_MODEL=slo tube_render onset "assets/audio/guitar-di/5150red.nam" [drive]
 *
 * The same two notes a fifth apart as `duo`, swept over **60 dB** - 0.5 mV to 0.5 V
 * at the first grid, two decibels a step - because the previous sweep started at
 * 0.1 V and the compression column said the output had already stopped moving
 * there: +0.1 dB out for +14 dB in.  Everything interesting is below where that one
 * began.
 *
 * Two numbers per step, for each side:
 *
 *   products / notes - the energy in every line that is not one of the two notes,
 *                      over the energy in the two notes.  Harmonics and
 *                      intermodulation together, which is what distortion is when
 *                      two notes are playing.  -40 dB is 1%, -26 dB is 5%.
 *   compression      - how far the output fell short of tracking the input, taken
 *                      against the quietest step.  0 dB is a linear amplifier.
 *
 * Every energy is averaged over **two phases** of the second note, 0 and 90
 * degrees.  High-order products are sums of many combinations that add with signs
 * depending on the phase between the tones, so a single phase gives a number that
 * moves by decibels for no physical reason; two phases average that out.
 *
 * No cabinet either side (this capture is head-only) and every matching filter
 * linear - both banks, all per-stage banks, all trims, the mid lift.  The circuit
 * and the knobs only.
 */

#define ONS_N 31 /* 0.5 mV to 0.5 V, 2 dB a step */
#define ONS_K 121

static void mode_onset(int argc, char **argv)
{
    const double   fd = 41.205;
    const double   fe = 2.0 * 41.205, fb = 3.0 * 41.205;
    const char    *cap = argc > 2 ? argv[2] : NULL;
    const float    drive = argc > 3 ? (float)atof(argv[3]) : 0.5f;
    const uint32_t rate = 22050u;
    uint32_t       crate = 48000u;
    ag_amp_cfg_t   cfg;
    ag_amp_t      *a = NULL;
    float         *x = NULL, *y = NULL, *xc = NULL;
    double         lv[ONS_N];
    double         od[ONS_N], ad[ONS_N]; /* products over notes, dB */
    double         oo[ONS_N], ao[ONS_N]; /* output level, dB */
    uint32_t       n, cn, edge, body, cedge, cbody;
    int            li, k, ph;

    if (cap == NULL) {
        printf("  usage: tube_render onset capture.nam [drive]\n");
        return;
    }
    {
        nam_model_t *m = nam_load(cap, 0, 0);
        if (m == NULL) {
            printf("  %s: %s\n", cap, nam_err());
            return;
        }
        if (nam_sample_rate(m) > 0) {
            crate = (uint32_t)nam_sample_rate(m);
        }
        nam_free(m);
    }
    edge = (uint32_t)(0.02 * (double)rate);
    body = (uint32_t)(20.0 / fd * (double)rate);
    n = 2u * edge + body;
    cedge = (uint32_t)(0.02 * (double)crate);
    cbody = (uint32_t)(20.0 / fd * (double)crate);
    cn = 2u * cedge + cbody;

    a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    x = (float *)malloc(sizeof(float) * n);
    y = (float *)malloc(sizeof(float) * n);
    xc = (float *)malloc(sizeof(float) * cn);
    if (a == NULL || x == NULL || y == NULL || xc == NULL) {
        goto done;
    }
    print_model();
    model_cfg(&cfg, (float)rate);
    cfg.drive = drive;
    {
        int st, b;
        for (st = 0; st < AG_AMP_STAGES; st++) {
            cfg.vtrim[st] = 0.0f;
            for (b = 0; b < AG_AMP_VOICE_N; b++) {
                cfg.voice[st][b].db = 0.0f;
            }
        }
        for (b = 0; b < AG_AMP_VOICE_N; b++) {
            cfg.tone[b].db = 0.0f;
        }
        cfg.mid_db = 0.0f;
    }
    for (li = 0; li < ONS_N; li++) {
        lv[li] = 0.0005 * pow(10.0, (double)li * 3.0 / 30.0);
    }
    printf("  E2 %.2f Hz + a just fifth %.2f Hz, %.4f to %.3f V at the first grid,"
           " 2 dB a step.\n  drive %.2f, so the take amplitude is V / drive.  No"
           " cabinet either side; every\n  matching filter linear.  Energies"
           " averaged over two phases of the second note.\n\n",
           fe, fb, lv[0], lv[ONS_N - 1], (double)drive);

    for (li = 0; li < ONS_N; li++) {
        const double vin = lv[li] / (double)(drive > 0.0f ? drive : 1.0f);
        double       on = 0.0, op = 0.0, an = 0.0, ap = 0.0;
        double       oe = 0.0, ae = 0.0;
        uint32_t     i;

        for (ph = 0; ph < 2; ph++) {
            const double phase = (ph == 0) ? 0.0 : PI * 0.5;
            float       *cy;

            for (i = 0; i < n; i++) {
                const double tt = (double)i / (double)rate;
                double       env = 1.0;
                if (i < edge) {
                    env = 0.5 - 0.5 * cos(PI * (double)i / (double)edge);
                } else if (i + edge > n) {
                    env = 0.5 - 0.5 * cos(PI * (double)(n - i) / (double)edge);
                }
                x[i] = (float)(0.5 * vin * env *
                               (sin(2.0 * PI * fe * tt) +
                                sin(2.0 * PI * fb * tt + phase)));
            }
            /*
             * Built once, and **at the loudest step of the sweep**.  It used to be
             * built here at li == 0, which is the quietest: the bake fits each
             * stage's table axes to the probe it is handed, so axes sized for half a
             * millivolt were then asked about half a volt and clamped flat all the
             * way up.  What that looked like was a chain whose products floored at
             * -51 dB, jumped twenty-one decibels in one two-decibel step, and
             * reported fifty-five decibels of compression - none of it real.
             */
            if (li == 0 && ph == 0) {
                st1_probe(x, n, edge, rate,
                          lv[ONS_N - 1] / (double)(drive > 0.0f ? drive : 1.0f),
                          0.0);
                if (ag_amp_build(a, g_ckt, &cfg, g_tab, 0, x, (int)n) != 0) {
                    goto done;
                }
            }
            ag_amp_reset(a);
            for (i = 0; i < n; i++) {
                y[i] = ag_amp_tick(a, x[i]);
            }
            for (k = 1; k <= ONS_K; k++) {
                const double f = fd * (double)k;
                double       e;
                if (f >= 0.45 * (double)rate) {
                    continue;
                }
                e = pow(10.0, goertzel_db(y + edge, body, f, (double)rate) / 10.0);
                if (k == 2 || k == 3) {
                    on += e;
                } else {
                    op += e;
                }
            }
            for (i = edge; i < edge + body; i++) {
                oe += (double)y[i] * (double)y[i];
            }

            for (i = 0; i < cn; i++) {
                const double tt = (double)i / (double)crate;
                double       env = 1.0;
                if (i < cedge) {
                    env = 0.5 - 0.5 * cos(PI * (double)i / (double)cedge);
                } else if (i + cedge > cn) {
                    env = 0.5 - 0.5 * cos(PI * (double)(cn - i) / (double)cedge);
                }
                xc[i] = (float)(0.5 * vin * env *
                                (sin(2.0 * PI * fe * tt) +
                                 sin(2.0 * PI * fb * tt + phase)));
            }
            cy = capture_render(cap, xc, cn, 1.0f, 0);
            if (cy == NULL) {
                goto done;
            }
            for (k = 1; k <= ONS_K; k++) {
                const double f = fd * (double)k;
                double       e;
                if (f >= 0.45 * (double)crate) {
                    continue;
                }
                e = pow(10.0,
                        goertzel_db(cy + cedge, cbody, f, (double)crate) / 10.0);
                if (k == 2 || k == 3) {
                    an += e;
                } else {
                    ap += e;
                }
            }
            for (i = cedge; i < cedge + cbody; i++) {
                ae += (double)cy[i] * (double)cy[i];
            }
            free(cy);
        }
        od[li] = 10.0 * log10(op / (on > 0.0 ? on : 1e-30) + 1e-30);
        ad[li] = 10.0 * log10(ap / (an > 0.0 ? an : 1e-30) + 1e-30);
        oo[li] = 10.0 * log10(oe / (double)(2u * body) + 1e-30);
        ao[li] = 10.0 * log10(ae / (double)(2u * cbody) + 1e-30);
    }

    printf("                      OURS                    THE AMPLIFIER\n");
    printf("     V     in dBV   prod/notes   compr      prod/notes   compr\n");
    for (li = 0; li < ONS_N; li++) {
        const double din = 20.0 * log10(lv[li] / lv[0]);
        printf("   %.4f  %6.1f    %7.1f    %6.2f       %7.1f    %6.2f\n", lv[li],
               20.0 * log10(lv[li]), od[li], din - (oo[li] - oo[0]), ad[li],
               din - (ao[li] - ao[0]));
    }

    /*
     * And where each side crosses the thresholds worth naming, by interpolating in
     * log voltage between the two steps that straddle it.  Printed rather than
     * argued about: "light overdrive" is a decision, and this is the table it is
     * made from.
     */
    {
        static const double thr[4] = { -40.0, -30.0, -26.0, -20.0 };
        static const double cthr[3] = { 1.0, 3.0, 6.0 };
        int                 t;
        printf("\n  WHERE EACH SIDE CROSSES, volts at the first grid\n");
        printf("    products / notes      ours      amplifier\n");
        for (t = 0; t < 4; t++) {
            double vo = -1.0, va = -1.0;
            for (li = 1; li < ONS_N; li++) {
                if (vo < 0.0 && od[li - 1] < thr[t] && od[li] >= thr[t]) {
                    const double f = (thr[t] - od[li - 1]) / (od[li] - od[li - 1]);
                    vo = lv[li - 1] * pow(lv[li] / lv[li - 1], f);
                }
                if (va < 0.0 && ad[li - 1] < thr[t] && ad[li] >= thr[t]) {
                    const double f = (thr[t] - ad[li - 1]) / (ad[li] - ad[li - 1]);
                    va = lv[li - 1] * pow(lv[li] / lv[li - 1], f);
                }
            }
            printf("      %5.0f dB", thr[t]);
            if (thr[t] == -40.0) {
                printf(" (1%%)  ");
            } else if (thr[t] == -26.0) {
                printf(" (5%%)  ");
            } else if (thr[t] == -20.0) {
                printf(" (10%%) ");
            } else {
                printf(" (3%%)  ");
            }
            if (vo > 0.0) {
                printf("  %7.4f V", vo);
            } else {
                printf("        -  ");
            }
            if (va > 0.0) {
                printf("    %7.4f V\n", va);
            } else {
                printf("          -  \n");
            }
        }
        printf("    compression           ours      amplifier\n");
        for (t = 0; t < 3; t++) {
            double vo = -1.0, va = -1.0;
            for (li = 1; li < ONS_N; li++) {
                const double c0 = 20.0 * log10(lv[li - 1] / lv[0]) -
                                  (oo[li - 1] - oo[0]);
                const double c1 = 20.0 * log10(lv[li] / lv[0]) - (oo[li] - oo[0]);
                const double d0 = 20.0 * log10(lv[li - 1] / lv[0]) -
                                  (ao[li - 1] - ao[0]);
                const double d1 = 20.0 * log10(lv[li] / lv[0]) - (ao[li] - ao[0]);
                if (vo < 0.0 && c0 < cthr[t] && c1 >= cthr[t]) {
                    vo = lv[li - 1] *
                         pow(lv[li] / lv[li - 1], (cthr[t] - c0) / (c1 - c0));
                }
                if (va < 0.0 && d0 < cthr[t] && d1 >= cthr[t]) {
                    va = lv[li - 1] *
                         pow(lv[li] / lv[li - 1], (cthr[t] - d0) / (d1 - d0));
                }
            }
            printf("      %5.0f dB        ", cthr[t]);
            if (vo > 0.0) {
                printf("  %7.4f V", vo);
            } else {
                printf("        -  ");
            }
            if (va > 0.0) {
                printf("    %7.4f V\n", va);
            } else {
                printf("          -  \n");
            }
        }
    }

done:
    free(a);
    free(x);
    free(y);
    free(xc);
}

/* ------------------------------------------------------------------------ */
/* step2 - the tone block in front of the last stage, at step one's level      */
/* ------------------------------------------------------------------------ */

/*
 *   AG_MODEL=slo tube_render step2 "assets/audio/guitar-di/5150red.nam" \
 *                                  [drive [trim [volts]]]
 *
 * Step one put the level where a light overdrive starts on the amplifier's, with the
 * same flat trim on every pre-stage block.  Step two shapes the block in front of
 * the **last** stage, at that level, to get closer to the reference.  Closer, not
 * identical - and the report says how close it got and where it stopped.
 *
 * WHAT IS COMPARED
 *
 * Every line in the output, on the grid of the difference tone, so harmonics and
 * intermodulation products sit together and none is counted twice.  Below 700 Hz the
 * lines are strong and far apart, so they are compared one at a time.  Above it they
 * are not: many combinations land on the same line and add with signs that depend on
 * the phase between the two notes, so a single line can differ by ten decibels with
 * the same total energy.  Up there the lines are summed into third-octave groups and
 * the groups are compared.  Every energy is averaged over two phases of the second
 * note as well.
 *
 * Lines more than 40 dB under the amplifier's loudest are left out entirely: an
 * error on something that quiet is not worth a decibel of error on something loud.
 *
 * WHAT IT MAY NOT DO
 *
 * The block is held at **zero mean**.  Its average is a flat gain, and a flat gain
 * is step one's answer - letting the shape search move it would quietly undo the
 * onset alignment and report a better spectrum for having done so.  The onset is
 * measured again at the end to prove it did not move.
 */

#define ST2_K   121
#define ST2_LOW 17 /* k=17 is 700.5 Hz: individual below, grouped above */

/* One rendered point: the level of every line, phase-averaged, in dB. */
static void st2_lines(ag_amp_t *a, const char *cap, float *x, uint32_t n,
                      uint32_t edge, uint32_t body, uint32_t rate, double vin,
                      double *out)
{
    const double fd = 41.205, fe = 2.0 * 41.205, fb = 3.0 * 41.205;
    double       acc[ST2_K + 1];
    int          ph, k;
    uint32_t     i;

    for (k = 0; k <= ST2_K; k++) {
        acc[k] = 0.0;
    }
    for (ph = 0; ph < 2; ph++) {
        const double phase = (ph == 0) ? 0.0 : PI * 0.5;
        float       *y;
        st1_probe(x, n, edge, rate, vin, phase);
        if (cap != NULL) {
            y = capture_render(cap, x, n, 1.0f, 0);
        } else {
            y = (float *)malloc(sizeof(float) * n);
            if (y != NULL) {
                ag_amp_reset(a);
                for (i = 0; i < n; i++) {
                    y[i] = ag_amp_tick(a, x[i]);
                }
            }
        }
        if (y == NULL) {
            for (k = 1; k <= ST2_K; k++) {
                out[k] = -300.0;
            }
            return;
        }
        for (k = 1; k <= ST2_K; k++) {
            const double f = fd * (double)k;
            if (f >= 0.45 * (double)rate) {
                continue;
            }
            acc[k] += pow(10.0,
                          goertzel_db(y + edge, body, f, (double)rate) / 10.0);
        }
        free(y);
    }
    for (k = 1; k <= ST2_K; k++) {
        out[k] = 10.0 * log10(0.5 * acc[k] + 1e-30);
    }
}

/*
 * The comparison set: individual lines under 700 Hz, third-octave groups over it.
 * Returns the number of entries and fills `fo`/`fa` with our and the amplifier's
 * level for each, in dB.
 */
static int st2_set(const double *o, const double *am, double *fo, double *fa,
                   double *fhz)
{
    const double fd = 41.205;
    int          k, m = 0, bi;

    for (k = 1; k <= ST2_LOW; k++) {
        if (o[k] < -299.0 || am[k] < -299.0) {
            continue;
        }
        fo[m] = o[k];
        fa[m] = am[k];
        fhz[m] = fd * (double)k;
        m++;
    }
    for (bi = 0; bi < SPEC_N; bi++) {
        const double c = (double)k_spec_f[bi];
        const double lo = c / 1.122462, hi = c * 1.122462;
        double       eo = 0.0, ea = 0.0;
        int          got = 0;
        if (c < 700.0 || c > 5000.0) {
            continue;
        }
        for (k = ST2_LOW + 1; k <= ST2_K; k++) {
            const double f = fd * (double)k;
            if (f < lo || f >= hi || o[k] < -299.0 || am[k] < -299.0) {
                continue;
            }
            eo += pow(10.0, o[k] / 10.0);
            ea += pow(10.0, am[k] / 10.0);
            got++;
        }
        if (got == 0) {
            continue;
        }
        fo[m] = 10.0 * log10(eo + 1e-30);
        fa[m] = 10.0 * log10(ea + 1e-30);
        fhz[m] = c;
        m++;
    }
    return m;
}

/* rms of the difference over the entries the amplifier puts within 40 dB of its
 * loudest, after removing one common gain. */
static double st2_err(const double *fo, const double *fa, int m, double *gain_out)
{
    double top = -300.0, g = 0.0, e = 0.0;
    int    i, nn = 0;

    for (i = 0; i < m; i++) {
        if (fa[i] > top) {
            top = fa[i];
        }
    }
    for (i = 0; i < m; i++) {
        if (fa[i] < top - 40.0) {
            continue;
        }
        g += fo[i] - fa[i];
        nn++;
    }
    g = nn > 0 ? g / (double)nn : 0.0;
    for (i = 0; i < m; i++) {
        if (fa[i] < top - 40.0) {
            continue;
        }
        e += (fo[i] - fa[i] - g) * (fo[i] - fa[i] - g);
    }
    if (gain_out != NULL) {
        *gain_out = g;
    }
    return sqrt(e / (double)(nn > 0 ? nn : 1));
}

static void mode_step2(int argc, char **argv)
{
    const char    *cap = argc > 2 ? argv[2] : NULL;
    const float    drive = argc > 3 ? (float)atof(argv[3]) : 0.5f;
    const float    trim = argc > 4 ? (float)atof(argv[4]) : -7.84f;
    const double   volts = argc > 5 ? atof(argv[5]) : 0.00315;
    const int      want_blk = argc > 6 ? atoi(argv[6]) : 0;
    const uint32_t rate = 22050u;
    uint32_t       crate = 48000u;
    ag_amp_cfg_t   cfg, best;
    ag_amp_t      *a = NULL;
    float         *x = NULL, *xc = NULL;
    double        *ol = NULL, *al = NULL;
    double         fo[64], fa[64], fhz[64], fo0[64];
    double         e0, e1, g0, g1;
    uint32_t       n, cn, edge, body, cedge, cbody;
    int            m, last, b, round, i;

    if (cap == NULL) {
        printf("  usage: tube_render step2 capture.nam [drive [trim [volts]]]\n");
        return;
    }
    {
        nam_model_t *mm = nam_load(cap, 0, 0);
        if (mm == NULL) {
            printf("  %s: %s\n", cap, nam_err());
            return;
        }
        if (nam_sample_rate(mm) > 0) {
            crate = (uint32_t)nam_sample_rate(mm);
        }
        nam_free(mm);
    }
    edge = (uint32_t)(0.02 * (double)rate);
    body = (uint32_t)(20.0 / 41.205 * (double)rate);
    n = 2u * edge + body;
    cedge = (uint32_t)(0.02 * (double)crate);
    cbody = (uint32_t)(20.0 / 41.205 * (double)crate);
    cn = 2u * cedge + cbody;

    a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    x = (float *)malloc(sizeof(float) * n);
    xc = (float *)malloc(sizeof(float) * cn);
    ol = (double *)malloc(sizeof(double) * (ST2_K + 1));
    al = (double *)malloc(sizeof(double) * (ST2_K + 1));
    if (a == NULL || x == NULL || xc == NULL || ol == NULL || al == NULL) {
        goto done;
    }
    print_model();
    model_cfg(&cfg, (float)rate);
    cfg.drive = drive;
    {
        int st;
        for (st = 0; st < AG_AMP_STAGES; st++) {
            cfg.vtrim[st] = (st < cfg.n_stages) ? trim : 0.0f;
            for (b = 0; b < AG_AMP_VOICE_N; b++) {
                cfg.voice[st][b].db = 0.0f;
            }
        }
        for (b = 0; b < AG_AMP_VOICE_N; b++) {
            cfg.tone[b].db = 0.0f;
        }
        cfg.mid_db = 0.0f;
    }
    /*
     * Which block, and the default is not always the right question.  The procedure
     * says "the block in front of the last stage" because in a cascade the last
     * valve sees the biggest signal and distorts first.  On this model that is not
     * true: the fourth stage is a *recovery* stage and barely clips - step one
     * measured its block as worth 2.4 dB of onset against 7.8 for each of the other
     * three - so its block is very nearly a linear filter at the output.  The last
     * stage that actually clips is the third.  A sixth argument says which block.
     */
    last = (want_blk >= 1 && want_blk <= cfg.n_stages) ? want_blk - 1
                                                       : cfg.n_stages - 1;
    printf("  step one's answer carried in: %+.2f dB on each of the %d blocks."
           "  Shaping block %d, at\n  %.5f V where the amplifier makes a light"
           " overdrive.  The block is held at zero mean,\n  which stops a flat drift"
           " but not a shape one, so the onset is re-measured at the end.\n\n",
           (double)trim, cfg.n_stages, last + 1, volts);

    /* The amplifier, once. */
    st2_lines(a, cap, xc, cn, cedge, cbody, crate,
              volts / (double)(drive > 0.0f ? drive : 1.0f), al);

    /*
     * The build sees a real probe, and a loud one: the onset check at the end of
     * this mode sweeps up to 0.01 V, so the axes are fitted there rather than at
     * the working level.
     */
    st1_probe(x, n, edge, rate, 0.01 / (double)(drive > 0.0f ? drive : 1.0f), 0.0);
    if (ag_amp_build(a, g_ckt, &cfg, g_tab, 0, x, (int)n) != 0) {
        goto done;
    }
    st2_lines(a, NULL, x, n, edge, body, rate,
              volts / (double)(drive > 0.0f ? drive : 1.0f), ol);
    m = st2_set(ol, al, fo, fa, fhz);
    e0 = st2_err(fo, fa, m, &g0);
    for (i = 0; i < m; i++) {
        fo0[i] = fo[i];
    }
    printf("  before: %.2f dB rms over %d comparisons\n", e0, m);

    /* Coordinate descent on the seven bands, zero mean kept after every round. */
    best = cfg;
    e1 = e0;
    for (round = 0; round < 3; round++) {
        const float step = 3.0f / (float)(1 << round);
        for (b = 0; b < AG_AMP_VOICE_N; b++) {
            int dir;
            for (dir = 0; dir < 2; dir++) {
                const float was = cfg.voice[last][b].db;
                const float try_db = was + (dir == 0 ? step : -step);
                double      e;
                if (try_db > 12.0f || try_db < -12.0f) {
                    continue;
                }
                cfg.voice[last][b].db = try_db;
                if (ag_amp_set_voicing(a, &cfg) != 0) {
                    cfg.voice[last][b].db = was;
                    continue;
                }
                st2_lines(a, NULL, x, n, edge, body, rate,
                          volts / (double)(drive > 0.0f ? drive : 1.0f), ol);
                (void)st2_set(ol, al, fo, fa, fhz);
                e = st2_err(fo, fa, m, NULL);
                if (e < e1 - 1.0e-6) {
                    e1 = e;
                    best = cfg;
                } else {
                    cfg.voice[last][b].db = was;
                }
            }
        }
        /* Zero mean: the average is a flat gain and belongs to step one. */
        {
            double mean = 0.0;
            for (b = 0; b < AG_AMP_VOICE_N; b++) {
                mean += (double)cfg.voice[last][b].db;
            }
            mean /= (double)AG_AMP_VOICE_N;
            if (mean > 0.02 || mean < -0.02) {
                for (b = 0; b < AG_AMP_VOICE_N; b++) {
                    cfg.voice[last][b].db -= (float)mean;
                }
                if (ag_amp_set_voicing(a, &cfg) == 0) {
                    st2_lines(a, NULL, x, n, edge, body, rate,
                              volts / (double)(drive > 0.0f ? drive : 1.0f), ol);
                    (void)st2_set(ol, al, fo, fa, fhz);
                    e1 = st2_err(fo, fa, m, NULL);
                    best = cfg;
                }
            }
        }
        printf("  round %d, step %.2f dB: %.2f dB rms\n", round + 1,
               (double)step, e1);
    }

    cfg = best;
    if (ag_amp_set_voicing(a, &cfg) != 0) {
        goto done;
    }
    st2_lines(a, NULL, x, n, edge, body, rate,
              volts / (double)(drive > 0.0f ? drive : 1.0f), ol);
    (void)st2_set(ol, al, fo, fa, fhz);
    e1 = st2_err(fo, fa, m, &g1);

    printf("\n  block %d, in front of the last stage:", last + 1);
    for (b = 0; b < AG_AMP_VOICE_N; b++) {
        printf(" %.0f:%+.2f", (double)cfg.voice[last][b].hz,
               (double)cfg.voice[last][b].db);
    }
    printf("\n  %.2f dB rms -> %.2f dB.  One common gain of %+.1f dB is removed"
           " (was %+.1f).\n\n", e0, e1, g1, g0);

    {
        double top = -300.0;
        for (i = 0; i < m; i++) {
            if (fa[i] > top) {
                top = fa[i];
            }
        }
        printf("     Hz     what        before    after    cap level\n");
        for (i = 0; i < m; i++) {
            if (fa[i] < top - 40.0) {
                continue;
            }
            printf("   %6.1f  %-9s   %+6.1f   %+6.1f    %6.1f\n", fhz[i],
                   fhz[i] < 700.0 ? "one line" : "1/3 oct",
                   fo0[i] - fa[i] - g0, fo[i] - fa[i] - g1, fa[i] - top);
        }
    }

    /* And the onset, to prove the zero mean held it. */
    {
        static const double lvs[9] = { 0.0016, 0.0020, 0.0025, 0.0032, 0.0040,
                                       0.0050, 0.0063, 0.0079, 0.0100 };
        double d[9];
        int    li;
        printf("\n  the onset, to show it did not drift: products / notes at"
               " step one's level\n");
        for (li = 0; li < 9; li++) {
            double junk;
            d[li] = st1_point(a, NULL, x, n, edge, body, rate,
                              lvs[li] / (double)(drive > 0.0f ? drive : 1.0f),
                              &junk);
        }
        printf("    ");
        for (li = 0; li < 9; li++) {
            printf(" %.4f", lvs[li]);
        }
        printf("\n    ");
        for (li = 0; li < 9; li++) {
            printf("  %5.1f", d[li]);
        }
        printf("\n");
    }

done:
    free(a);
    free(x);
    free(xc);
    free(ol);
    free(al);
}

/* ------------------------------------------------------------------------ */
/* stepn - raise the level, then shape one block: every step after the first  */
/* ------------------------------------------------------------------------ */

/*
 *   AG_MODEL=slo tube_render stepn "assets/audio/guitar-di/5150red.nam" \
 *                                  <products dB> <block> [drive]
 *
 * Steps three and four, and five and six, and so on: they are all the same pair of
 * questions.  **Raise the input until the amplifier makes the amount of overdrive
 * asked for** - that is the step-three half, and the answer is a voltage read off
 * the amplifier, not off us.  **Then shape the block in front of the named stage at
 * that voltage** - the step-four half.
 *
 *   stepn cap.nam -14 3     20% of products, the block in front of stage 3
 *   stepn cap.nam -20 4     10%, the block in front of stage 4 (which is step 2)
 *
 * Unlike `step1` and `step2` this reads the model as it stands: the trims and every
 * pre-stage block come from `ag_amp_model`, so each step builds on the ones already
 * written in.  Only the *output* bank and the mid lift are forced to zero, because
 * the walk has not reached them.
 *
 * The block is held at zero mean.  Its average is a flat gain and a flat gain is
 * step one's answer; letting the shape search move it would quietly undo the onset
 * alignment and then report a better spectrum for having done so.  That is not
 * enough on its own - a shape change still alters what reaches the valves - so the
 * onset is measured again at the end and printed.
 */

static void mode_stepn(int argc, char **argv)
{
    const char    *cap = argc > 2 ? argv[2] : NULL;
    const double   target = argc > 3 ? atof(argv[3]) : -14.0;
    const int      blk = argc > 4 ? atoi(argv[4]) : 3;
    const float    drive = argc > 5 ? (float)atof(argv[5]) : 0.5f;
    const uint32_t rate = 22050u;
    uint32_t       crate = 48000u;
    ag_amp_cfg_t   cfg, best;
    ag_amp_t      *a = NULL;
    float         *x = NULL, *xc = NULL;
    double        *ol = NULL, *al = NULL;
    double         fo[64], fa[64], fhz[64], fo0[64];
    double         lv[26], ad[26], od[26];
    double         va, vo, e0, e1, g0, g1;
    uint32_t       n, cn, edge, body, cedge, cbody;
    int            m, last, b, round, i, li;

    if (cap == NULL) {
        printf("  usage: tube_render stepn capture.nam <products dB> <block>"
               " [drive]\n");
        return;
    }
    {
        nam_model_t *mm = nam_load(cap, 0, 0);
        if (mm == NULL) {
            printf("  %s: %s\n", cap, nam_err());
            return;
        }
        if (nam_sample_rate(mm) > 0) {
            crate = (uint32_t)nam_sample_rate(mm);
        }
        nam_free(mm);
    }
    edge = (uint32_t)(0.02 * (double)rate);
    body = (uint32_t)(20.0 / 41.205 * (double)rate);
    n = 2u * edge + body;
    cedge = (uint32_t)(0.02 * (double)crate);
    cbody = (uint32_t)(20.0 / 41.205 * (double)crate);
    cn = 2u * cedge + cbody;

    a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    x = (float *)malloc(sizeof(float) * n);
    xc = (float *)malloc(sizeof(float) * cn);
    ol = (double *)malloc(sizeof(double) * (ST2_K + 1));
    al = (double *)malloc(sizeof(double) * (ST2_K + 1));
    if (a == NULL || x == NULL || xc == NULL || ol == NULL || al == NULL) {
        goto done;
    }
    print_model();
    model_cfg(&cfg, (float)rate);
    cfg.drive = drive;
    for (b = 0; b < AG_AMP_VOICE_N; b++) {
        cfg.tone[b].db = 0.0f; /* the walk has not reached the output */
    }
    cfg.mid_db = 0.0f;
    last = (blk >= 1 && blk <= cfg.n_stages) ? blk - 1 : cfg.n_stages - 1;

    printf("  the model as it stands: trims");
    for (i = 0; i < cfg.n_stages; i++) {
        printf(" %+.2f", (double)cfg.vtrim[i]);
    }
    printf(" dB; output bank and mid forced to zero.\n"
           "  step A: find where the AMPLIFIER makes %.1f dB of products."
           "  step B: shape block %d there.\n\n", target, last + 1);

    /* 0.5 mV to 0.15 V, 2 dB a step: wide enough for 10% to 30%. */
    for (li = 0; li < 26; li++) {
        lv[li] = 0.0005 * pow(10.0, (double)li * 2.5 / 25.0);
    }
    st1_probe(x, n, edge, rate,
              lv[25] / (double)(drive > 0.0f ? drive : 1.0f), 0.0);
    if (ag_amp_build(a, g_ckt, &cfg, g_tab, 0, x, (int)n) != 0) {
        goto done;
    }
    printf("     V      amplifier   ours\n");
    for (li = 0; li < 26; li++) {
        const double vin = lv[li] / (double)(drive > 0.0f ? drive : 1.0f);
        double       junk;
        ad[li] = st1_point(a, cap, xc, cn, cedge, cbody, crate, vin, &junk);
        od[li] = st1_point(a, NULL, x, n, edge, body, rate, vin, &junk);
        printf("   %.5f   %7.1f   %7.1f\n", lv[li], ad[li], od[li]);
    }
    va = st1_cross(lv, ad, 26, target);
    vo = st1_cross(lv, od, 26, target);
    if (va <= 0.0) {
        printf("\n  the amplifier never reaches %.1f dB inside this sweep.\n",
               target);
        goto done;
    }
    printf("\n  STEP A: the amplifier makes %.1f dB of products at **%.5f V**"
           " at the first grid.\n", target, va);
    if (vo > 0.0) {
        printf("  we reach the same amount at %.5f V, so at the amplifier's level"
               " we are %+.1f dB\n  of products %s it.\n", vo,
               st1_cross(lv, od, 26, target) > 0.0
                   ? 20.0 * log10(vo / va) : 0.0,
               vo < va ? "ahead of" : "behind");
    }

    /* Step B: the block, at the amplifier's level. */
    st2_lines(a, cap, xc, cn, cedge, cbody, crate,
              va / (double)(drive > 0.0f ? drive : 1.0f), al);
    st2_lines(a, NULL, x, n, edge, body, rate,
              va / (double)(drive > 0.0f ? drive : 1.0f), ol);
    m = st2_set(ol, al, fo, fa, fhz);
    e0 = st2_err(fo, fa, m, &g0);
    for (i = 0; i < m; i++) {
        fo0[i] = fo[i];
    }
    printf("\n  STEP B: block %d at %.5f V - before, %.2f dB rms over %d"
           " comparisons\n", last + 1, va, e0, m);

    best = cfg;
    e1 = e0;
    for (round = 0; round < 3; round++) {
        const float step = 3.0f / (float)(1 << round);
        for (b = 0; b < AG_AMP_VOICE_N; b++) {
            int dir;
            for (dir = 0; dir < 2; dir++) {
                const float was = cfg.voice[last][b].db;
                const float try_db = was + (dir == 0 ? step : -step);
                double      e;
                if (try_db > 12.0f || try_db < -12.0f) {
                    continue;
                }
                cfg.voice[last][b].db = try_db;
                if (ag_amp_set_voicing(a, &cfg) != 0) {
                    cfg.voice[last][b].db = was;
                    continue;
                }
                st2_lines(a, NULL, x, n, edge, body, rate,
                          va / (double)(drive > 0.0f ? drive : 1.0f), ol);
                (void)st2_set(ol, al, fo, fa, fhz);
                e = st2_err(fo, fa, m, NULL);
                if (e < e1 - 1.0e-6) {
                    e1 = e;
                    best = cfg;
                } else {
                    cfg.voice[last][b].db = was;
                }
            }
        }
        {
            double mean = 0.0;
            for (b = 0; b < AG_AMP_VOICE_N; b++) {
                mean += (double)cfg.voice[last][b].db;
            }
            mean /= (double)AG_AMP_VOICE_N;
            if (mean > 0.02 || mean < -0.02) {
                for (b = 0; b < AG_AMP_VOICE_N; b++) {
                    cfg.voice[last][b].db -= (float)mean;
                }
                if (ag_amp_set_voicing(a, &cfg) == 0) {
                    st2_lines(a, NULL, x, n, edge, body, rate,
                              va / (double)(drive > 0.0f ? drive : 1.0f), ol);
                    (void)st2_set(ol, al, fo, fa, fhz);
                    /* Kept only if the re-centring did not cost anything: the mean
                     * is not exactly a flat gain, so it can. */
                    if (st2_err(fo, fa, m, NULL) <= e1 + 0.05) {
                        e1 = st2_err(fo, fa, m, NULL);
                        best = cfg;
                    } else {
                        cfg = best;
                    }
                }
            }
        }
        printf("    round %d, step %.2f dB: %.2f dB rms\n", round + 1,
               (double)step, e1);
    }

    cfg = best;
    if (ag_amp_set_voicing(a, &cfg) != 0) {
        goto done;
    }
    st2_lines(a, NULL, x, n, edge, body, rate,
              va / (double)(drive > 0.0f ? drive : 1.0f), ol);
    (void)st2_set(ol, al, fo, fa, fhz);
    e1 = st2_err(fo, fa, m, &g1);

    printf("\n  block %d:", last + 1);
    for (b = 0; b < AG_AMP_VOICE_N; b++) {
        printf(" %.0f:%+.2f", (double)cfg.voice[last][b].hz,
               (double)cfg.voice[last][b].db);
    }
    printf("\n  %.2f dB rms -> %.2f dB.  One common gain of %+.1f dB removed"
           " (was %+.1f).\n\n", e0, e1, g1, g0);
    {
        double top = -300.0;
        for (i = 0; i < m; i++) {
            if (fa[i] > top) {
                top = fa[i];
            }
        }
        printf("     Hz     what        before    after    cap level\n");
        for (i = 0; i < m; i++) {
            if (fa[i] < top - 40.0) {
                continue;
            }
            printf("   %6.1f  %-9s   %+6.1f   %+6.1f    %6.1f\n", fhz[i],
                   fhz[i] < 700.0 ? "one line" : "1/3 oct",
                   fo0[i] - fa[i] - g0, fo[i] - fa[i] - g1, fa[i] - top);
        }
    }

    /* Every earlier step, re-measured: this is where a later step is caught
     * undoing an earlier one. */
    {
        static const double chk[3] = { -20.0, -14.0, -11.0 };
        printf("\n  what the earlier steps look like now, volts at the first"
               " grid\n    products    amplifier     ours      ours is\n");
        for (li = 0; li < 26; li++) {
            const double vin = lv[li] / (double)(drive > 0.0f ? drive : 1.0f);
            double       junk;
            od[li] = st1_point(a, NULL, x, n, edge, body, rate, vin, &junk);
        }
        for (i = 0; i < 3; i++) {
            const double v1 = st1_cross(lv, ad, 26, chk[i]);
            const double v2 = st1_cross(lv, od, 26, chk[i]);
            printf("     %5.0f dB", chk[i]);
            if (v1 > 0.0) {
                printf("   %8.5f V", v1);
            } else {
                printf("       -     ");
            }
            if (v2 > 0.0) {
                printf("  %8.5f V", v2);
            } else {
                printf("      -     ");
            }
            if (v1 > 0.0 && v2 > 0.0) {
                printf("   %+5.1f dB\n", 20.0 * log10(v2 / v1));
            } else {
                printf("      -\n");
            }
        }
    }

done:
    free(a);
    free(x);
    free(xc);
    free(ol);
    free(al);
}

/* ------------------------------------------------------------------------ */
/* iter - the walk, round after round, until it stops improving               */
/* ------------------------------------------------------------------------ */

/*
 *   AG_MODEL=slo tube_render iter "assets/audio/guitar-di/5150red.nam" [rounds [drive]]
 *
 * THE PROCEDURE, GENERALISED TO ANY NUMBER OF STAGES
 *
 * The metric throughout is **products over notes**: two notes a just fifth apart, and
 * the energy in every line that is not one of them over the energy in the two.
 * Harmonics and intermodulation together, on one grid of the 41.205 Hz difference
 * tone so that nothing is counted twice, every energy averaged over two phases of the
 * second note.  Percent is amplitude, so 10% is -20 dB of that energy ratio.
 *
 * One round is:
 *
 *   1. **The ceiling.**  What the amplifier saturates at, half a decibel under the
 *      plateau so the top rung is reachable rather than asymptotic.  A ten percent
 *      ladder walks off the end of a long chain - this amplifier only ever makes 36%,
 *      so a four-stage chain has no 40% rung.
 *   2. **The rung**, `min(10%, ceiling / n)`.  8.2% on this four-stage chain; the full
 *      ten on a two-stage one, where there is room.
 *   3. **Step one**: bisect one flat trim, the same on **every** pre-stage block, so
 *      that our bottom rung sits on the amplifier's.  A trim in front of a later
 *      stage does not slide our curve by its own value - what reaches that stage has
 *      already been clipped twice - so this is measured, not arithmetic.
 *   4. **One rung per block, last to first.**  Rung k's voltage is read off the
 *      *amplifier*; the block in front of that stage is then shaped at that voltage,
 *      held at zero mean so its average cannot undo step one.
 *
 * And then the whole thing again, starting from the blocks the last round left.
 *
 * TWO NUMBERS PER ROUND, NOT ONE
 *
 * `spec` is the mean of the per-rung spectral rms - how well each block matched where
 * it was fitted.  `level` is the rms of the level misalignment across the rungs - how
 * far our "this much overdrive" voltages sit from the amplifier's.
 *
 * Both, because they pull against each other and the first one alone is a trap.
 * Iteration 1 improved every rung's spectrum - 4.08 to 1.83 dB on the last block, and
 * so on - while pushing the rungs themselves from exact to 3, 8 and 10 dB out.  A
 * search watching only `spec` would have called that progress and kept going.
 */

/*
 * The sweep the rounds are measured on: 0.05 mV to 0.16 V, two decibels a step.
 *
 * It starts four decades down for a reason found the hard way.  A chain that is too
 * hot is already past the bottom rung at the quietest step, so its curve never
 * *rises* through the target inside the sweep and the crossing search returns
 * nothing - which is the same answer it gives for a chain too cold to ever reach the
 * target.  The two need opposite corrections, so they have to be told apart, and a
 * sweep this wide makes "nothing" almost always mean too cold.
 */
#define ITER_N 36

static void mode_iter(int argc, char **argv)
{
    const char    *cap = argc > 2 ? argv[2] : NULL;
    const int      rounds = argc > 3 ? atoi(argv[3]) : 4;
    const float    drive = argc > 4 ? (float)atof(argv[4]) : 0.5f;
    /*
     * Step 1b off by default, and that is a measured choice rather than caution.
     * The tilt is a real mechanism - `kneew` shows it moving the knee from 14 dB
     * wide to 36 - but as a *sequential* step inside the loop it destabilises the
     * whole walk: with it the best round comes out at spec 2.71 dB and level
     * 6.65 against 1.69 and 6.15 without, and the tilt itself flips from +4 dB to
     * -8 between rounds.  The reason is in the numbers it prints: once the trims
     * are spread over twelve decibels, a band change on one block moves the bottom
     * rung by twelve decibels instead of one, so every step after it is no longer
     * a small correction to the one before.  The sequential structure needs the
     * steps to be nearly independent, and the tilt breaks that.  It belongs in a
     * joint fit, which is iteration 2.
     */
    const int      use_tilt = argc > 5 ? atoi(argv[5]) : 0;
    const uint32_t rate = 22050u;
    uint32_t       crate = 48000u;
    ag_amp_cfg_t   cfg, best, keep;
    ag_amp_t      *a = NULL;
    float         *x = NULL, *xc = NULL;
    double        *ol = NULL, *al = NULL;
    double         fo[64], fa[64], fhz[64];
    double         lv[ITER_N], ad[ITER_N], od[ITER_N];
    double         plateau, rung = 10.0, av_hi = -1.0, tilt = 0.0;
    double         spec_best = 1.0e9, sum_best = 1.0e9;
    double         made[AG_AMP_STAGES], baseline[AG_AMP_STAGES];
    int            order[AG_AMP_STAGES];
    uint32_t       n, cn, edge, body, cedge, cbody;
    int            m, b, round, i, li, k, step, it, best_round = -1;

    if (cap == NULL) {
        printf("  usage: tube_render iter capture.nam [rounds [drive]]\n");
        return;
    }
    {
        nam_model_t *mm = nam_load(cap, 0, 0);
        if (mm == NULL) {
            printf("  %s: %s\n", cap, nam_err());
            return;
        }
        if (nam_sample_rate(mm) > 0) {
            crate = (uint32_t)nam_sample_rate(mm);
        }
        nam_free(mm);
    }
    edge = (uint32_t)(0.02 * (double)rate);
    body = (uint32_t)(20.0 / 41.205 * (double)rate);
    n = 2u * edge + body;
    cedge = (uint32_t)(0.02 * (double)crate);
    cbody = (uint32_t)(20.0 / 41.205 * (double)crate);
    cn = 2u * cedge + cbody;

    a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    x = (float *)malloc(sizeof(float) * n);
    xc = (float *)malloc(sizeof(float) * cn);
    ol = (double *)malloc(sizeof(double) * (ST2_K + 1));
    al = (double *)malloc(sizeof(double) * (ST2_K + 1));
    if (a == NULL || x == NULL || xc == NULL || ol == NULL || al == NULL) {
        goto done;
    }
    print_model();
    model_cfg(&cfg, (float)rate);
    cfg.drive = drive;
    for (b = 0; b < AG_AMP_VOICE_N; b++) {
        cfg.tone[b].db = 0.0f; /* the walk has not reached the output */
    }
    cfg.mid_db = 0.0f;

    for (li = 0; li < ITER_N; li++) {
        lv[li] = 0.00005 * pow(10.0, (double)li * 2.0 / 20.0);
    }
    st1_probe(x, n, edge, rate,
              lv[ITER_N - 1] / (double)(drive > 0.0f ? drive : 1.0f), 0.0);
    if (ag_amp_build(a, g_ckt, &cfg, g_tab, 0, x, (int)n) != 0) {
        goto done;
    }
    /* The amplifier once: it does not move, and it is the expensive side. */
    for (li = 0; li < ITER_N; li++) {
        double junk;
        ad[li] = st1_point(a, cap, xc, cn, cedge, cbody, crate,
                           lv[li] / (double)(drive > 0.0f ? drive : 1.0f), &junk);
    }
    plateau = ad[ITER_N - 1];
    av_hi = st1_cross(lv, ad, ITER_N, plateau - 1.0);
    {
        const double ceil_pct = 100.0 * pow(10.0, (plateau - 0.5) / 20.0);
        rung = ceil_pct / (double)cfg.n_stages;
        if (rung > 10.0) {
            rung = 10.0;
        }
        printf("  the amplifier saturates at %.1f dB of products (%.0f%%); half a"
               " decibel under that\n  is %.1f%%, and over %d stages the rung is"
               " %.1f%%%s.  %d rounds.\n\n", plateau,
               100.0 * pow(10.0, plateau / 20.0), ceil_pct, cfg.n_stages, rung,
               ceil_pct / (double)cfg.n_stages > 10.0 ? " (capped at 10)" : "",
               rounds > 0 ? rounds : 1);
    }

    keep = cfg;
    for (it = 0; it < (rounds > 0 ? rounds : 1); it++) {
        double spec_sum = 0.0, lvl_sq = 0.0, va_bottom;
        int    spec_n = 0, lvl_n = 0;

        printf("  ================ round %d\n", it + 1);

        /* --- step one: one flat trim on every block ---------------------- */
        va_bottom = st1_cross(lv, ad, ITER_N, 20.0 * log10(rung / 100.0));
        if (va_bottom <= 0.0) {
            printf("    the amplifier never reaches the bottom rung; stopping\n");
            break;
        }
        {
            double lo = -15.0, hi = 15.0, t = 0.0, v = -1.0;
            for (i = 0; i < 11; i++) {
                t = 0.5 * (lo + hi);
                for (k = 0; k < cfg.n_stages; k++) {
                    cfg.vtrim[k] = (float)t;
                }
                if (ag_amp_set_voicing(a, &cfg) != 0) {
                    goto done;
                }
                for (li = 0; li < ITER_N; li++) {
                    double junk;
                    od[li] = st1_point(a, NULL, x, n, edge, body, rate,
                                       lv[li] /
                                           (double)(drive > 0.0f ? drive : 1.0f),
                                       &junk);
                }
                /*
                 * Which way to go, and the three cases are not two.
                 *
                 * More trim is more drive, so our crossing moves to a *lower*
                 * voltage as `t` rises: v is decreasing in t.  When there is no
                 * crossing at all the sign has to come from somewhere else, and the
                 * first version guessed "too cold" every time - so a chain already
                 * past the rung at the quietest step was given *more* gain, every
                 * round, until the trim sat on its +15 dB bound with no crossing
                 * anywhere and every number below it was meaningless.  Which end of
                 * the sweep the curve is on says which case it is.
                 */
                v = st1_cross(lv, od, ITER_N, 20.0 * log10(rung / 100.0));
                if (v > 0.0) {
                    if (v > va_bottom) {
                        lo = t;
                    } else {
                        hi = t;
                    }
                } else if (od[0] >= 20.0 * log10(rung / 100.0)) {
                    hi = t; /* already over the rung at the quietest step */
                } else {
                    lo = t; /* never reaches it at the loudest */
                }
            }
            printf("    step 1: %+.2f dB on each of the %d blocks puts the %.1f%%"
                   " point at %.5f V\n            against the amplifier's"
                   " %.5f V\n", t, cfg.n_stages, rung, v, va_bottom);
            for (k = 0; k < cfg.n_stages; k++) {
                baseline[k] = (double)cfg.vtrim[k];
            }
        }

        /*
         * STEP 1b: THE TILT, AND ITS TARGET IS THE SPREAD, NOT A WIDTH
         *
         * The same trim on every block slides the whole curve and cannot change its
         * width - which is what I first said the tone blocks could not do at all,
         * and that was only half the case.  What sets the width is *at what input
         * each stage starts clipping*, and moving decibels from the back of the
         * chain to the front at constant sum moves those arrival levels relative to
         * each other.  `kneew` measures it: the width goes from 14 dB to 36 dB
         * across a tilt scan while the bottom rung moves by a tenth of a decibel, so
         * this and step one are very nearly independent knobs.
         *
         * What it must **not** be aimed at is a width defined by the amplifier's
         * plateau.  That was the first version and it made everything worse - spec
         * 1.69 dB to 3.63, level 6.15 to 11.22, and the tilt flipping from +4 to -6
         * between rounds.  The reason is that our plateau sits 3.4 dB above the
         * amplifier's, so "one decibel under the amplifier's plateau" lands in the
         * middle of a long shallow crawl on our curve and the width read off it is
         * not a width.
         *
         * The honest target is the thing the tilt is for: the **spread** of the rung
         * misalignments, that is the rms of them after the mean is taken out.  The
         * mean is the common trim's business and is set right afterwards; what is
         * left is the shape, and that is what the tilt can move.
         */
        if (cfg.n_stages > 1 && use_tilt) {
            double best_r = 0.0, best_spread = 1.0e9;
            for (i = -4; i <= 8; i++) {
                const double r = 2.0 * (double)i;
                double       d[AG_AMP_STAGES], mean = 0.0, sq = 0.0;
                int          got = 0;
                for (k = 0; k < cfg.n_stages; k++) {
                    cfg.vtrim[k] = (float)(baseline[k] +
                                           r * (0.5 * (double)(cfg.n_stages - 1) -
                                                (double)k));
                }
                if (ag_amp_set_voicing(a, &cfg) != 0) {
                    continue;
                }
                for (li = 0; li < ITER_N; li++) {
                    double junk;
                    od[li] = st1_point(a, NULL, x, n, edge, body, rate,
                                       lv[li] /
                                           (double)(drive > 0.0f ? drive : 1.0f),
                                       &junk);
                }
                for (k = 1; k <= cfg.n_stages; k++) {
                    const double want = 20.0 * log10(rung * (double)k / 100.0);
                    const double v1 = st1_cross(lv, ad, ITER_N, want);
                    const double v2 = st1_cross(lv, od, ITER_N, want);
                    if (v1 > 0.0 && v2 > 0.0) {
                        d[got] = 20.0 * log10(v2 / v1);
                        mean += d[got];
                        got++;
                    }
                }
                if (got < 2) {
                    continue;
                }
                mean /= (double)got;
                for (k = 0; k < got; k++) {
                    sq += (d[k] - mean) * (d[k] - mean);
                }
                sq = sqrt(sq / (double)got);
                if (sq < best_spread) {
                    best_spread = sq;
                    best_r = r;
                }
            }
            tilt = best_r;
            for (k = 0; k < cfg.n_stages; k++) {
                cfg.vtrim[k] = (float)(baseline[k] +
                                       tilt * (0.5 * (double)(cfg.n_stages - 1) -
                                               (double)k));
            }
            if (ag_amp_set_voicing(a, &cfg) != 0) {
                goto done;
            }
            printf("    step 1b: tilt %+.1f dB leaves %.2f dB of spread across the"
                   " rungs; trims are", tilt, best_spread);
            for (k = 0; k < cfg.n_stages; k++) {
                printf(" %+.1f", (double)cfg.vtrim[k]);
            }
            printf("\n");
        }

        /* And the common trim again, on top of the tilt, to put the mean back where
         * step one had it: the tilt only fixed the shape. */
        if (use_tilt) {
            double lo = -15.0, hi = 15.0, t = 0.0, v = -1.0;
            for (k = 0; k < cfg.n_stages; k++) {
                baseline[k] = (double)cfg.vtrim[k];
            }
            for (i = 0; i < 11; i++) {
                t = 0.5 * (lo + hi);
                for (k = 0; k < cfg.n_stages; k++) {
                    cfg.vtrim[k] = (float)(baseline[k] + t);
                }
                if (ag_amp_set_voicing(a, &cfg) != 0) {
                    goto done;
                }
                for (li = 0; li < ITER_N; li++) {
                    double junk;
                    od[li] = st1_point(a, NULL, x, n, edge, body, rate,
                                       lv[li] /
                                           (double)(drive > 0.0f ? drive : 1.0f),
                                       &junk);
                }
                v = st1_cross(lv, od, ITER_N, 20.0 * log10(rung / 100.0));
                if (v > 0.0) {
                    if (v > va_bottom) {
                        lo = t;
                    } else {
                        hi = t;
                    }
                } else if (od[0] >= 20.0 * log10(rung / 100.0)) {
                    hi = t;
                } else {
                    lo = t;
                }
            }
            printf("    step 1 again: %+.2f dB more on every block, %.1f%% now at"
                   " %.5f V\n", t, rung, v);
        }

        /*
         * WHICH BLOCK FIRST, AND IT IS NOT SIMPLY THE LAST ONE
         *
         * The rungs go with the *stages that make the distortion*, not with their
         * position: the procedure says start from the stage that is working and end
         * at the most linear one.  On this model the last stage is a recovery stage
         * that barely clips, so walking backwards by position hands the lowest rung
         * to a valve that is not doing anything.
         *
         * So each stage's contribution is measured - the chain rendered through k
         * stages against k-1, at the middle rung, and what the k-th one adds - and
         * the stages are sorted by it.  The most overdriven gets the lowest rung.
         */
        {
            const double vmid = st1_cross(lv, ad, ITER_N,
                                          20.0 * log10(rung * 0.5 *
                                                       (double)cfg.n_stages /
                                                       100.0));
            ag_amp_cfg_t probe = cfg;
            double       prev = -300.0;
            int          st2;
            for (k = 0; k < cfg.n_stages; k++) {
                double junk, v;
                probe.n_stages = k + 1;
                if (ag_amp_build(a, g_ckt, &probe, g_tab, 0, x, (int)n) != 0) {
                    break;
                }
                v = st1_point(a, NULL, x, n, edge, body, rate,
                              (vmid > 0.0 ? vmid : lv[ITER_N / 2]) /
                                  (double)(drive > 0.0f ? drive : 1.0f), &junk);
                made[k] = (k == 0) ? v : v - prev;
                prev = v;
                order[k] = k;
            }
            probe.n_stages = cfg.n_stages;
            if (ag_amp_build(a, g_ckt, &probe, g_tab, 0, x, (int)n) != 0) {
                goto done;
            }
            for (k = 0; k < cfg.n_stages; k++) {
                for (st2 = k + 1; st2 < cfg.n_stages; st2++) {
                    if (made[order[st2]] > made[order[k]]) {
                        const int tmp = order[k];
                        order[k] = order[st2];
                        order[st2] = tmp;
                    }
                }
            }
            printf("    stages by how much each adds at the middle rung"
                   " (for iteration 2):");
            for (k = 0; k < cfg.n_stages; k++) {
                printf(" %d(%+.1f)", order[k] + 1, made[order[k]]);
            }
            printf("\n");
        }

        /* --- one rung per block, last to first --------------------------- */
        /* Last to first, by position.  The ranking printed above is measured for
         * iteration 2, where the order follows how much each stage distorts; in
         * iteration 1 the order is the chain's own. */
        for (step = 0; step < cfg.n_stages; step++) {
            const int    blk = cfg.n_stages - 1 - step;
            const double pct = rung * (double)(step + 1);
            const double want = 20.0 * log10(pct / 100.0);
            const double va = st1_cross(lv, ad, ITER_N, want);
            double       e0, e1;

            if (va <= 0.0) {
                printf("    block %d, %.1f%%: the amplifier never gets there;"
                       " left alone\n", blk + 1, pct);
                continue;
            }
            st2_lines(a, cap, xc, cn, cedge, cbody, crate,
                      va / (double)(drive > 0.0f ? drive : 1.0f), al);
            if (ag_amp_set_voicing(a, &cfg) != 0) {
                goto done;
            }
            st2_lines(a, NULL, x, n, edge, body, rate,
                      va / (double)(drive > 0.0f ? drive : 1.0f), ol);
            m = st2_set(ol, al, fo, fa, fhz);
            e0 = st2_err(fo, fa, m, NULL);
            best = cfg;
            e1 = e0;
            for (round = 0; round < 3; round++) {
                const float st = 3.0f / (float)(1 << round);
                for (b = 0; b < AG_AMP_VOICE_N; b++) {
                    int dir;
                    for (dir = 0; dir < 2; dir++) {
                        const float was = cfg.voice[blk][b].db;
                        const float try_db = was + (dir == 0 ? st : -st);
                        double      e;
                        if (try_db > 12.0f || try_db < -12.0f) {
                            continue;
                        }
                        cfg.voice[blk][b].db = try_db;
                        if (ag_amp_set_voicing(a, &cfg) != 0) {
                            cfg.voice[blk][b].db = was;
                            continue;
                        }
                        st2_lines(a, NULL, x, n, edge, body, rate,
                                  va / (double)(drive > 0.0f ? drive : 1.0f),
                                  ol);
                        (void)st2_set(ol, al, fo, fa, fhz);
                        e = st2_err(fo, fa, m, NULL);
                        if (e < e1 - 1.0e-6) {
                            e1 = e;
                            best = cfg;
                        } else {
                            cfg.voice[blk][b].db = was;
                        }
                    }
                }
                {
                    double mean = 0.0;
                    for (b = 0; b < AG_AMP_VOICE_N; b++) {
                        mean += (double)cfg.voice[blk][b].db;
                    }
                    mean /= (double)AG_AMP_VOICE_N;
                    if (mean > 0.02 || mean < -0.02) {
                        for (b = 0; b < AG_AMP_VOICE_N; b++) {
                            cfg.voice[blk][b].db -= (float)mean;
                        }
                        if (ag_amp_set_voicing(a, &cfg) == 0) {
                            double e;
                            st2_lines(a, NULL, x, n, edge, body, rate,
                                      va / (double)(drive > 0.0f ? drive : 1.0f),
                                      ol);
                            (void)st2_set(ol, al, fo, fa, fhz);
                            e = st2_err(fo, fa, m, NULL);
                            if (e <= e1 + 0.05) {
                                e1 = e;
                                best = cfg;
                            } else {
                                cfg = best;
                            }
                        }
                    }
                }
            }
            cfg = best;
            spec_sum += e1;
            spec_n++;
            printf("    block %d at %.1f%% (%.5f V): %.2f -> %.2f dB rms\n",
                   blk + 1, pct, va, e0, e1);
        }

        /* --- the two convergence numbers -------------------------------- */
        if (ag_amp_set_voicing(a, &cfg) != 0) {
            goto done;
        }
        for (li = 0; li < ITER_N; li++) {
            double junk;
            od[li] = st1_point(a, NULL, x, n, edge, body, rate,
                               lv[li] / (double)(drive > 0.0f ? drive : 1.0f),
                               &junk);
        }
        printf("      rung   amplifier      ours      ours is\n");
        for (i = 1; i <= cfg.n_stages; i++) {
            const double pct = rung * (double)i;
            const double v1 = st1_cross(lv, ad, ITER_N, 20.0 * log10(pct / 100.0));
            const double v2 = st1_cross(lv, od, ITER_N, 20.0 * log10(pct / 100.0));
            printf("     %4.1f%%", pct);
            if (v1 > 0.0 && v2 > 0.0) {
                const double d = 20.0 * log10(v2 / v1);
                lvl_sq += d * d;
                lvl_n++;
                printf("  %8.5f V  %8.5f V   %+5.1f dB\n", v1, v2, d);
            } else {
                printf("      -           -           -\n");
            }
        }
        {
            const double spec = spec_sum / (double)(spec_n ? spec_n : 1);
            const double lvl = sqrt(lvl_sq / (double)(lvl_n ? lvl_n : 1));
            printf("    round %d: spec %.2f dB, level %.2f dB, sum %.2f\n\n",
                   it + 1, spec, lvl, spec + lvl);
            if (spec + lvl < sum_best) {
                sum_best = spec + lvl;
                spec_best = spec;
                keep = cfg;
                best_round = it + 1;
            }
        }
    }

    printf("  best round was %d, spec %.2f dB, sum %.2f\n", best_round,
           spec_best, sum_best);
    cfg = keep;
    printf("\n  paste into ag_amp.c:\n\n");
    printf("        static const ag_amp_band_t pre[%d][AG_AMP_VOICE_N] = {\n",
           cfg.n_stages);
    for (k = 0; k < cfg.n_stages; k++) {
        printf("            { /* stage %d */\n", k + 1);
        for (b = 0; b < AG_AMP_VOICE_N; b += 2) {
            printf("                { %.1ff, %.2ff, 1.0f }",
                   (double)cfg.voice[k][b].hz, (double)cfg.voice[k][b].db);
            if (b + 1 < AG_AMP_VOICE_N) {
                printf(", { %.1ff, %.2ff, 1.0f }",
                       (double)cfg.voice[k][b + 1].hz,
                       (double)cfg.voice[k][b + 1].db);
            }
            printf("%s\n", b + 2 < AG_AMP_VOICE_N ? "," : "");
        }
        printf("            }%s\n", k + 1 < cfg.n_stages ? "," : "");
    }
    printf("        };\n        static const float vtrim[%d] = {",
           cfg.n_stages);
    for (k = 0; k < cfg.n_stages; k++) {
        printf(" %.2ff%s", (double)cfg.vtrim[k],
               k + 1 < cfg.n_stages ? "," : " ");
    }
    printf("};\n");

done:
    free(a);
    free(x);
    free(xc);
    free(ol);
    free(al);
}

/* ------------------------------------------------------------------------ */
/* kneew - can the knee be stretched from the tone blocks, or only the netlist */
/* ------------------------------------------------------------------------ */

/*
 *   AG_MODEL=slo tube_render kneew "assets/audio/guitar-di/5150red.nam" [drive]
 *
 * THE QUESTION, AND IT IS NOT THE ONE I FIRST ANSWERED
 *
 * The amplifier takes about 25 dB of input to go from "just broke up" to saturated;
 * this chain takes about 10.  I said a tone block could not change that and gave a
 * reason that only covers half the case, so here is the whole of it.
 *
 * A trim in front of a stage cancels that stage's excess *gain* exactly - that is
 * step one, and it works.  What sets the width of the transition is something else:
 * **at what input each stage starts clipping**.  Stage k clips when its own grid
 * reaches its own threshold, so it clips at an input of (threshold / the gain from
 * the input to that grid).  If all four stages arrive at their thresholds at nearly
 * the same input, the chain's transition is as narrow as one valve's.  If they are
 * spread over twenty decibels of input, the transition is spread over twenty.
 *
 * And that spread is exactly what **differing** trims move.  The same trim on every
 * block - step one's answer - divides every stage's arrival level by the same
 * number and slides the whole curve without changing its width.  Trim the back of
 * the chain down and the front up, at constant sum, and the arrival levels move
 * *relative to each other*: the width changes.  The rule I imposed on the walk -
 * every block held at zero mean - forbade precisely that, so the procedure could
 * not stretch the knee because I had taken the freedom away, not because the
 * freedom does not exist.
 *
 * So: scan one tilt parameter, decibels moved from the back of the chain to the
 * front with the sum held at zero, and measure the width each time.  If the width
 * moves, the tone blocks can do this and iteration 2 should be allowed to.  If it
 * does not, it is the netlist.
 *
 * Width is measured between two rungs the amplifier also has: the bottom rung
 * (products at min(10%, ceiling/n)) and one decibel under the plateau.
 */

#define KNW_N 36

static void mode_kneew(int argc, char **argv)
{
    const char    *cap = argc > 2 ? argv[2] : NULL;
    const float    drive = argc > 3 ? (float)atof(argv[3]) : 0.5f;
    const uint32_t rate = 22050u;
    uint32_t       crate = 48000u;
    ag_amp_cfg_t   cfg;
    ag_amp_t      *a = NULL;
    float         *x = NULL, *xc = NULL;
    double         lv[KNW_N], ad[KNW_N], od[KNW_N];
    double         base[AG_AMP_STAGES];
    double         plateau, rung, lo_want, hi_want, av_lo, av_hi;
    uint32_t       n, cn, edge, body, cedge, cbody;
    int            li, i, st;

    if (cap == NULL) {
        printf("  usage: tube_render kneew capture.nam [drive]\n");
        return;
    }
    {
        nam_model_t *mm = nam_load(cap, 0, 0);
        if (mm == NULL) {
            printf("  %s: %s\n", cap, nam_err());
            return;
        }
        if (nam_sample_rate(mm) > 0) {
            crate = (uint32_t)nam_sample_rate(mm);
        }
        nam_free(mm);
    }
    edge = (uint32_t)(0.02 * (double)rate);
    body = (uint32_t)(20.0 / 41.205 * (double)rate);
    n = 2u * edge + body;
    cedge = (uint32_t)(0.02 * (double)crate);
    cbody = (uint32_t)(20.0 / 41.205 * (double)crate);
    cn = 2u * cedge + cbody;

    a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    x = (float *)malloc(sizeof(float) * n);
    xc = (float *)malloc(sizeof(float) * cn);
    if (a == NULL || x == NULL || xc == NULL) {
        goto done;
    }
    print_model();
    model_cfg(&cfg, (float)rate);
    cfg.drive = drive;
    for (i = 0; i < AG_AMP_VOICE_N; i++) {
        cfg.tone[i].db = 0.0f;
    }
    cfg.mid_db = 0.0f;
    for (st = 0; st < cfg.n_stages; st++) {
        base[st] = (double)cfg.vtrim[st];
    }
    for (li = 0; li < KNW_N; li++) {
        lv[li] = 0.00005 * pow(10.0, (double)li * 2.0 / 20.0);
    }
    st1_probe(x, n, edge, rate,
              lv[KNW_N - 1] / (double)(drive > 0.0f ? drive : 1.0f), 0.0);
    if (ag_amp_build(a, g_ckt, &cfg, g_tab, 0, x, (int)n) != 0) {
        goto done;
    }
    for (li = 0; li < KNW_N; li++) {
        double junk;
        ad[li] = st1_point(a, cap, xc, cn, cedge, cbody, crate,
                           lv[li] / (double)(drive > 0.0f ? drive : 1.0f), &junk);
    }
    plateau = ad[KNW_N - 1];
    rung = 100.0 * pow(10.0, (plateau - 0.5) / 20.0) / (double)cfg.n_stages;
    if (rung > 10.0) {
        rung = 10.0;
    }
    lo_want = 20.0 * log10(rung / 100.0);
    hi_want = plateau - 1.0;
    av_lo = st1_cross(lv, ad, KNW_N, lo_want);
    av_hi = st1_cross(lv, ad, KNW_N, hi_want);
    printf("  the amplifier: bottom rung %.1f%% at %.1f dB, one decibel under its"
           " %.1f dB plateau\n  at %.1f dB.  %.5f V to %.5f V, so its knee is"
           " **%.1f dB wide**.\n\n", rung, lo_want, plateau, hi_want, av_lo,
           av_hi, 20.0 * log10(av_hi / av_lo));

    printf("  r is decibels taken off the back of the chain and put on the front,"
           " sum held at zero.\n");
    printf("     r      trims                        bottom      plateau-1"
           "     width\n");
    for (i = -3; i <= 8; i++) {
        const double r = 2.0 * (double)i;
        double       v1, v2;
        for (st = 0; st < cfg.n_stages; st++) {
            cfg.vtrim[st] =
                (float)(base[st] +
                        r * (0.5 * (double)(cfg.n_stages - 1) - (double)st));
        }
        if (ag_amp_set_voicing(a, &cfg) != 0) {
            break;
        }
        for (li = 0; li < KNW_N; li++) {
            double junk;
            od[li] = st1_point(a, NULL, x, n, edge, body, rate,
                               lv[li] / (double)(drive > 0.0f ? drive : 1.0f),
                               &junk);
        }
        v1 = st1_cross(lv, od, KNW_N, lo_want);
        v2 = st1_cross(lv, od, KNW_N, hi_want);
        printf("   %5.1f  ", r);
        for (st = 0; st < cfg.n_stages; st++) {
            printf("%+6.1f", (double)cfg.vtrim[st]);
        }
        printf("   ");
        if (v1 > 0.0) {
            printf("  %8.5f V", v1);
        } else {
            printf("      -     ");
        }
        if (v2 > 0.0) {
            printf("  %8.5f V", v2);
        } else {
            printf("      -     ");
        }
        if (v1 > 0.0 && v2 > 0.0) {
            printf("   %5.1f dB\n", 20.0 * log10(v2 / v1));
        } else {
            printf("      -\n");
        }
    }
    printf("\n  our plateau at the top of the sweep: %.1f dB against the"
           " amplifier's %.1f.\n", od[KNW_N - 1], plateau);

done:
    free(a);
    free(x);
    free(xc);
}

/* ------------------------------------------------------------------------ */
/* duo - two notes a fifth apart, every line in the output, level by level    */
/* ------------------------------------------------------------------------ */

/*
 *   AG_MODEL=slo tube_render duo "assets/audio/guitar-di/5150red.nam" [drive]
 *
 * E2 and a fifth above it, together, swept from 0.1 to 0.5 volts at the first grid
 * in tenths.  **Volts**, because that is the unit the circuit is in: `cfg.drive` is
 * volts at the first grid per unit of input, so the take amplitude the capture is
 * fed is V / drive, and both sides get that same number.  Both are printed.
 *
 * No cabinet on either side, because the 5150 capture is head-only.  A model whose
 * capture has a speaker in it needs the same speaker on both sides instead.
 *
 * **Every matching filter is linear here** - both banks, every per-stage bank, every
 * trim, the mid lift.  What is left is the circuit and the knobs: coupling
 * capacitors, cathode shelves, the tone stack at noon.  So this measures the chain
 * before anything has been fitted to anything.
 *
 * SEPARATING THE TWO NOTES' HARMONICS IS MEANINGLESS AND THIS DOES NOT DO IT
 *
 * Two tones through a nonlinearity make harmonics *and* intermodulation products,
 * and they land on each other: E2's sixth harmonic and the fifth's fourth are the
 * same 494 Hz, and a table with a column for each note reports that one line twice
 * and calls it two different things.  So the fifth is taken **just** rather than
 * equal-tempered - 3/2 exactly - which puts every harmonic and every combination
 * product on exact multiples of the difference tone, 41.205 Hz.  One grid, every
 * line on it, including the difference tone itself, which is the fart and which no
 * single note shows.
 *
 * One gain is measured once - on the strongest line at the quietest step - and held
 * for every number, so a row that moves is the chain moving and not a level.
 */

#define DUO_LVL 5
#define DUO_K   121 /* 121 * 41.205 = 4986 Hz, just under 5 kHz */

static void mode_duo(int argc, char **argv)
{
    static const double lv[DUO_LVL] = { 0.1, 0.2, 0.3, 0.4, 0.5 };
    const double   fd = 41.205;           /* the difference tone */
    const double   fe = 2.0 * 41.205;     /* E2, 82.41 Hz */
    const double   fb = 3.0 * 41.205;     /* a just fifth above, 123.615 Hz */
    const char    *cap = argc > 2 ? argv[2] : NULL;
    const float    drive = argc > 3 ? (float)atof(argv[3]) : 0.5f;
    const uint32_t rate = 22050u;
    uint32_t       crate = 48000u;
    ag_amp_cfg_t   cfg;
    ag_amp_t      *a = NULL;
    float         *x = NULL, *y = NULL, *xc = NULL;
    double        *ol = NULL, *al = NULL;  /* [level][k], dB */
    double         orms[DUO_LVL], arms[DUO_LVL];
    double         osp[DUO_LVL][SPEC_N], asp[DUO_LVL][SPEC_N];
    double         align = 0.0;
    uint32_t       n, cn, edge, body, cedge, cbody;
    int            li, k, kk;

    if (cap == NULL) {
        printf("  usage: tube_render duo capture.nam [drive]\n");
        return;
    }
    {
        nam_model_t *m = nam_load(cap, 0, 0);
        if (m == NULL) {
            printf("  %s: %s\n", cap, nam_err());
            return;
        }
        if (nam_sample_rate(m) > 0) {
            crate = (uint32_t)nam_sample_rate(m);
        }
        nam_free(m);
    }
    /* Whole periods of the difference tone in the window, so no line leaks into
     * its neighbour: 0.4854 s is exactly 20 periods of 41.205 Hz. */
    edge = (uint32_t)(0.02 * (double)rate);
    body = (uint32_t)(20.0 / fd * (double)rate);
    n = 2u * edge + body;
    cedge = (uint32_t)(0.02 * (double)crate);
    cbody = (uint32_t)(20.0 / fd * (double)crate);
    cn = 2u * cedge + cbody;

    a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    x = (float *)malloc(sizeof(float) * n);
    y = (float *)malloc(sizeof(float) * n);
    xc = (float *)malloc(sizeof(float) * cn);
    ol = (double *)malloc(sizeof(double) * DUO_LVL * (DUO_K + 1));
    al = (double *)malloc(sizeof(double) * DUO_LVL * (DUO_K + 1));
    if (a == NULL || x == NULL || y == NULL || xc == NULL || ol == NULL ||
        al == NULL) {
        goto done;
    }
    print_model();
    model_cfg(&cfg, (float)rate);
    cfg.drive = drive;
    {
        int st, b;
        for (st = 0; st < AG_AMP_STAGES; st++) {
            cfg.vtrim[st] = 0.0f;
            for (b = 0; b < AG_AMP_VOICE_N; b++) {
                cfg.voice[st][b].db = 0.0f;
            }
        }
        for (b = 0; b < AG_AMP_VOICE_N; b++) {
            cfg.tone[b].db = 0.0f;
        }
        cfg.mid_db = 0.0f;
    }
    printf("  E2 %.2f Hz + a just fifth %.2f Hz, %.2f to %.2f V at the first grid;"
           " drive %.2f, so the\n  take amplitude is V / drive.  No cabinet either"
           " side - this capture is head-only.\n"
           "  Every matching filter is LINEAR: both banks, every per-stage bank,"
           " every trim, the mid\n  lift.  The tone stack and the coupling networks"
           " are the circuit and stay in.\n"
           "  Every line is an exact multiple of the difference tone %.3f Hz, so"
           " harmonics and\n  intermodulation products sit on one grid and none of"
           " them is counted twice.\n\n",
           fe, fb, lv[0], lv[DUO_LVL - 1], (double)drive, fd);

    for (li = 0; li < DUO_LVL; li++) {
        const double vin = lv[li] / (double)(drive > 0.0f ? drive : 1.0f);
        float       *cy;
        uint32_t     i;
        double       acc;

        for (i = 0; i < n; i++) {
            const double tt = (double)i / (double)rate;
            double       env = 1.0;
            if (i < edge) {
                env = 0.5 - 0.5 * cos(PI * (double)i / (double)edge);
            } else if (i + edge > n) {
                env = 0.5 - 0.5 * cos(PI * (double)(n - i) / (double)edge);
            }
            x[i] = (float)(0.5 * vin * env *
                           (sin(2.0 * PI * fe * tt) + sin(2.0 * PI * fb * tt)));
        }
        /* Built once at the loudest step, not the first - see mode_onset for what
         * axes fitted to the quiet end do to every number below them. */
        if (li == 0) {
            st1_probe(x, n, edge, rate,
                      lv[DUO_LVL - 1] / (double)(drive > 0.0f ? drive : 1.0f),
                      0.0);
            if (ag_amp_build(a, g_ckt, &cfg, g_tab, 0, x, (int)n) != 0) {
                goto done;
            }
        }
        ag_amp_reset(a);
        for (i = 0; i < n; i++) {
            y[i] = ag_amp_tick(a, x[i]);
        }
        for (k = 1; k <= DUO_K; k++) {
            const double f = fd * (double)k;
            ol[li * (DUO_K + 1) + k] =
                f < 0.45 * (double)rate
                    ? goertzel_db(y + edge, body, f, (double)rate)
                    : -300.0;
        }
        acc = 0.0;
        for (i = edge; i < edge + body; i++) {
            acc += (double)y[i] * (double)y[i];
        }
        orms[li] = 10.0 * log10(acc / (double)body + 1e-30);
        spectrum_of(y, (int)n, (float)rate, osp[li]);

        for (i = 0; i < cn; i++) {
            const double tt = (double)i / (double)crate;
            double       env = 1.0;
            if (i < cedge) {
                env = 0.5 - 0.5 * cos(PI * (double)i / (double)cedge);
            } else if (i + cedge > cn) {
                env = 0.5 - 0.5 * cos(PI * (double)(cn - i) / (double)cedge);
            }
            xc[i] = (float)(0.5 * vin * env *
                            (sin(2.0 * PI * fe * tt) + sin(2.0 * PI * fb * tt)));
        }
        cy = capture_render(cap, xc, cn, 1.0f, 0);
        if (cy == NULL) {
            goto done;
        }
        for (k = 1; k <= DUO_K; k++) {
            const double f = fd * (double)k;
            al[li * (DUO_K + 1) + k] =
                f < 0.45 * (double)crate
                    ? goertzel_db(cy + cedge, cbody, f, (double)crate)
                    : -300.0;
        }
        acc = 0.0;
        for (i = cedge; i < cedge + cbody; i++) {
            acc += (double)cy[i] * (double)cy[i];
        }
        arms[li] = 10.0 * log10(acc / (double)cbody + 1e-30);
        spectrum_of(cy, (int)cn, (float)crate, asp[li]);
        free(cy);
    }

    align = ol[0 * (DUO_K + 1) + 2] - al[0 * (DUO_K + 1) + 2];
    printf("  one gain taken out of every line below: ours is %+.1f dB over the"
           " capture on E2 at\n  %.2f V, where neither is distorting much.\n\n",
           align, lv[0]);

    /* ------------------------------------------------------------------ */
    printf("  GAIN AND COMPRESSION - output of the pair, dB, and what it did for"
           " 14 dB of input\n");
    printf("     V     in dBV    ours     capture   ours-cap\n");
    for (li = 0; li < DUO_LVL; li++) {
        printf("   %.2f   %7.1f  %7.1f   %7.1f   %+7.1f\n", lv[li],
               20.0 * log10(lv[li]), orms[li] - align, arms[li],
               (orms[li] - align) - arms[li]);
    }
    printf("   0.1 -> 0.5 V is +14.0 dB in: ours came back %+.1f dB, the capture"
           " %+.1f dB,\n   so compression is %.1f dB against %.1f dB - ours is"
           " %.1f dB %s compressed.\n",
           orms[DUO_LVL - 1] - orms[0], arms[DUO_LVL - 1] - arms[0],
           14.0 - (orms[DUO_LVL - 1] - orms[0]),
           14.0 - (arms[DUO_LVL - 1] - arms[0]),
           fabs((arms[DUO_LVL - 1] - arms[0]) - (orms[DUO_LVL - 1] - orms[0])),
           (orms[DUO_LVL - 1] - orms[0]) > (arms[DUO_LVL - 1] - arms[0])
               ? "LESS" : "MORE");

    /* ------------------------------------------------------------------ */
    /*
     * Which lines are worth printing: everything the *amplifier* puts within 50 dB
     * of its own loudest line at the middle step.  Chosen from the capture rather
     * than from ours so that the row list does not change when our chain does.
     */
    {
        const int mid = 2;
        double    top = -300.0;
        printf("\n  EVERY LINE IN THE SIGNAL, ours minus the capture in dB.  k is"
               " the multiple of\n  %.3f Hz; k=2 is E2 and k=3 is the fifth, every"
               " other row is a harmonic or an\n  intermodulation product and this"
               " does not distinguish them.\n", fd);
        for (k = 1; k <= DUO_K; k++) {
            const double v = al[mid * (DUO_K + 1) + k];
            if (v > top) {
                top = v;
            }
        }
        printf("\n     k     Hz    what        0.10   0.20   0.30   0.40   0.50"
               "     cap@0.3\n");
        for (k = 1; k <= DUO_K; k++) {
            const char *what;
            if (al[mid * (DUO_K + 1) + k] < top - 50.0) {
                continue;
            }
            what = (k == 1) ? "f2-f1" : (k == 2) ? "E2" : (k == 3) ? "fifth" : "";
            printf("   %3d  %6.1f  %-9s", k, fd * (double)k, what);
            for (li = 0; li < DUO_LVL; li++) {
                const double o = ol[li * (DUO_K + 1) + k];
                const double c = al[li * (DUO_K + 1) + k];
                if (o < -299.0 || c < -299.0) {
                    printf("    -  ");
                } else {
                    printf(" %+6.1f", o - c - align);
                }
            }
            printf("   %7.1f\n", al[mid * (DUO_K + 1) + k] - top);
        }
        printf("\n  The last column is where that line sits under the amplifier's"
               " loudest one at 0.30 V,\n  so a large error on a quiet line matters"
               " less than the same error on a loud one.\n");
    }

    /* ------------------------------------------------------------------ */
    {
        int    c5, c2;
        double e5, e2;
        printf("\n  TONE, third-octave rms of the difference (each spectrum"
               " normalised at its own 1 kHz)\n     V      0-5 kHz   0-2 kHz\n");
        for (li = 0; li < DUO_LVL; li++) {
            e5 = 0.0;
            e2 = 0.0;
            c5 = 0;
            c2 = 0;
            for (kk = 0; kk < SPEC_N; kk++) {
                if (osp[li][kk] < -299.0 || asp[li][kk] < -299.0) {
                    continue;
                }
                if (k_spec_f[kk] <= 5000.0f) {
                    e5 += (osp[li][kk] - asp[li][kk]) * (osp[li][kk] - asp[li][kk]);
                    c5++;
                }
                if (k_spec_f[kk] <= 2000.0f) {
                    e2 += (osp[li][kk] - asp[li][kk]) * (osp[li][kk] - asp[li][kk]);
                    c2++;
                }
            }
            printf("   %.2f   %7.2f   %7.2f\n", lv[li],
                   sqrt(e5 / (double)(c5 ? c5 : 1)),
                   sqrt(e2 / (double)(c2 ? c2 : 1)));
        }
    }

done:
    free(a);
    free(x);
    free(y);
    free(xc);
    free(ol);
    free(al);
}

static void mode_match(int argc, char **argv)
{
    const int    iters = argc > 4 ? atoi(argv[4]) : 8;
    const float  drive = argc > 5 ? (float)atof(argv[5]) : 0.5f;
    const float  q = argc > 6 ? (float)atof(argv[6]) : 1.0f;
    const char  *cap = argc > 2 ? argv[2] : NULL;
    float       *in = NULL, *in48 = NULL, *wet48 = NULL, *quiet48 = NULL;
    float       *ref = NULL;
    uint32_t     dn = 0, drate = 0, n48 = 0, rn = 0;
    uint32_t     crate = 48000u;
    /* The cabinet, at the capture's rate, and whether the capture had one. */
    float       *cabc = NULL;
    uint32_t     cabc_n = 0;
    int          has_cab = 0;
    /* The same cabinet brought to the take's rate, which is what everything below
     * the fit uses. */
    float       *cabd = NULL;
    uint32_t     cabdn = 0;
    double       cap_imd = -300.0;
    double       cap_comp = 0.0, chain_comp = 0.0;
    ag_amp_cfg_t best;
    char         path[256];

    if (argc < 4) {
        printf("  usage: tube_render match capture.nam di.wav [iterations"
               " [drive [q]]]\n"
               "    AG_MODEL picks the amplifier, AG_CAB_IR the cabinet this"
               " chain plays through\n");
        return;
    }
    in = read_wav(argv[3], &dn, &drate);
    if (in == NULL || dn == 0) {
        free(in);
        return;
    }
    print_model();
    printf("  take: %s, %u frames at %u Hz, %.2f s\n", argv[3], dn, drate,
           (double)dn / (double)(drate ? drate : 1u));

    /*
     * The capture's own rate, which is not negotiable: every filter it learned
     * sits at a fixed fraction of it, so playing it at another rate is a
     * different amplifier.  The take goes up to meet it and the answer comes
     * back down.
     */
    {
        nam_model_t *probe = nam_load(cap, 0, 0);
        if (probe == NULL) {
            printf("  %s: %s\n", cap, nam_err());
            free(in);
            return;
        }
        if (nam_sample_rate(probe) > 0) {
            crate = (uint32_t)nam_sample_rate(probe);
        }
        nam_free(probe);
    }
    if (crate == drate) {
        in48 = in;
        n48 = dn;
    } else {
        in48 = wr_resample_f(in, dn, drate, crate, &n48, 1);
        if (in48 == NULL) {
            free(in);
            return;
        }
    }

    printf("\n  playing the capture over the take, twice - at the level the fit"
           " uses and 20 dB\n  down, which is what says how much it"
           " compresses\n");
    wet48 = capture_render(cap, in48, n48, 1.0f, 1);
    quiet48 = capture_render(cap, in48, n48, 0.1f, 0);
    if (wet48 == NULL || quiet48 == NULL) {
        goto done;
    }
    cap_comp = 20.0 - (energy_db(wet48, n48) - energy_db(quiet48, n48));
    free(quiet48);
    quiet48 = NULL;

    if (crate == drate) {
        ref = wet48;
        rn = n48;
        wet48 = NULL;
    } else {
        ref = wr_resample_f(wet48, n48, crate, drate, &rn, 1);
        if (ref == NULL) {
            goto done;
        }
    }

    /* Kept, because everything after the fit wants it: irfit takes it as its
     * target, and it is the file to listen to beside a render. */
    (void)snprintf(path, sizeof(path), "build/listen/match_ref_%s.wav",
                   ag_amp_model_name(g_model));
    if (write_wav(path, ref, rn, drate) == 0) {
        printf("  reference written to %s\n", path);
    }

    /*
     * IS THERE A LOUDSPEAKER IN THIS CAPTURE?
     *
     * It decides the whole comparison and it is not written down anywhere
     * reliable - TONE3000 files carry a "gear_type" and half of the ones here
     * carry no metadata at all - so it is measured.  A twelve-inch guitar speaker
     * is fifteen to twenty-five decibels down at 6.3 kHz relative to 1 kHz; a
     * preamp is not.  On the captures in this tree the two cases do not overlap
     * anywhere near the threshold: Mars Gain 8 measures -17.7 dB there, and
     * Bogner and 5150red measure -0.5 and +0.1, which is to say **they are
     * head-only captures with no cabinet in them at all**.
     *
     * Getting this wrong is not a small error.  Fitting a chain that plays
     * through a cabinet against a capture that has none puts the whole of a
     * speaker's top-octave rolloff into the voicing as a boost - twenty decibels
     * of it - and what that sounds like is the fizz that a listener reported and
     * that took a week to find.  So each side gets a speaker or neither does.
     */
    {
        double rs[SPEC_N];
        spectrum_of(ref, (int)rn, (float)drate, rs);
        has_cab = rs[SPEC_N - 1] < -8.0;
        printf("\n  the capture at 6.3 kHz: %+.1f dB relative to 1 kHz, so it"
               " %s\n", rs[SPEC_N - 1],
               has_cab ? "has a loudspeaker in it"
                       : "is HEAD ONLY - no cabinet");
        if (getenv("AG_CAB_IR") != NULL) {
            printf("  cabinet: %s, from AG_CAB_IR\n", getenv("AG_CAB_IR"));
            if (!has_cab) {
                printf("  WARNING the capture has no speaker in it and this"
                       " chain is about to play through\n          one, so the"
                       " fit will try to correct the difference with the"
                       " voicing.\n          Unset AG_CAB_IR, or add the same"
                       " impulse to the capture's render.\n");
            }
        } else if (!has_cab) {
            /* Neither side gets one.  The cabinet is then a free choice made
             * later, and the voicing fitted here is the amplifier alone. */
            g_cab = 0;
            g_cab_path = NULL;
            printf("  cabinet: none on either side, which is what matching a"
                   " head means\n");
        } else {
            static char cabp[256];
            (void)snprintf(cabp, sizeof(cabp), "build/listen/match_cab_%s.wav",
                           ag_amp_model_name(g_model));
            cabc = cab_from_capture(cap, crate, cabp, &cabc_n);
            if (cabc != NULL) {
                g_cab_path = cabp;
                g_cab = 0;
                printf("  cabinet: taken from the capture itself, 200 ms,"
                       " written to %s\n", cabp);
            } else {
                printf("  cabinet: could not take one from the capture, using"
                       " %s\n", g_cab_path != NULL ? g_cab_path
                                                   : "an ag_ir preset");
            }
        }
    }

    /*
     * The cabinet at the take's rate, obtained here rather than further down
     * because pass one needs it: the 49 Hz intermodulation product is measured
     * through a loudspeaker when the capture has one, and a speaker eats that
     * product while leaving the tones.
     *
     * Order: whatever AG_CAB_IR named, then the one taken from the capture, then
     * the impulse tube_live loads by default.
     */
    if (g_cab_path != NULL) {
        uint32_t rr = 0, fn = 0;
        float   *f = read_wav(g_cab_path, &fn, &rr);
        if (f != NULL && rr == drate) {
            cabd = f;
            cabdn = fn;
        } else if (f != NULL) {
            cabd = wr_resample_f(f, fn, rr, drate, &cabdn, 0);
            free(f);
        }
    } else if (cabc != NULL) {
        cabd = wr_resample_f(cabc, cabc_n, crate, drate, &cabdn, 0);
    } else {
        uint32_t rr = 0, fn = 0;
        float   *f = read_wav(AG_CAB_MATCHED, &fn, &rr);
        if (f == NULL) {
            f = read_wav(AG_CAB_DEFAULT, &fn, &rr);
        }
        if (f != NULL && rr == drate) {
            cabd = f;
            cabdn = fn;
        } else if (f != NULL) {
            cabd = wr_resample_f(f, fn, rr, drate, &cabdn, 0);
            free(f);
        }
    }
    /* 200 ms, for the reason in run_cab: past that, an impulse taken from a model
     * is the model's own noise floor. */
    if (cabdn > (uint32_t)(0.2 * (double)drate)) {
        cabdn = (uint32_t)(0.2 * (double)drate);
    }

    /* And the amplifier's own subsonic intermodulation, which is pass one's third
     * target.  Through its own speaker if it has one, so ours gets the same. */
    cap_imd = capture_two_tone_imd(cap, crate, 0.35f);

    printf("\n");
    fit_banks(ref, rn, in, dn, drate, iters, drive, q, 0.0f, cap, cap_comp,
              cap_imd, has_cab ? cabd : NULL, has_cab ? cabdn : 0u, &best);
    chain_comp = chain_compression(&best, in, dn, drate);

    /*
     * The measurement the fit cannot make, printed after it rather than before,
     * so that a good rms figure is never the last thing on the screen.
     */
    printf("\n  Compression - the same take 20 dB down, and how much of that"
           " came back:\n"
           "    the capture       %5.1f dB\n"
           "    this chain        %5.1f dB   (%+.1f dB)\n",
           cap_comp, chain_comp, chain_comp - cap_comp);
    if (chain_comp - cap_comp > 2.0 || cap_comp - chain_comp > 2.0) {
        printf("\n  MORE THAN 2 dB APART.  The voicing above matches the"
               " spectrum of an amplifier\n  whose dynamics this chain does not"
               " have, and no filter repairs that: what has\n  to change is the"
               " circuit - a stage bias, a cathode bypass, the divider in"
               " front\n  of a cold clipper.\n");
    } else {
        printf("\n  Within 2 dB, so the two compress by about the same amount"
               " and the voicing above\n  is a fit to an amplifier this chain"
               " can be.\n");
    }

    /*
     * TWO WAYS TO MATCH WHAT COMES AFTER THE LAST STAGE, BOTH FITTED
     *
     * The post-valve correction can live in biquads, in the impulse, or in both,
     * and this writes **two finished configurations** rather than one:
     *
     *   1. `ir_<model>_bank.wav`   - the bank stays in, and the impulse is fitted
     *                                to whatever the bank did not manage.
     *   2. `ir_<model>_fitted.wav` - the bank at zero, and the impulse carries the
     *                                whole correction on its own.
     *
     * That is what `z` in `tube_live` swaps between, and getting it wrong is what
     * made that key misleading at first: state 1 used to be the bank into a *raw*
     * cabinet, with nothing fitted after it, so the key was comparing a matched
     * chain against an unmatched one and the difference came out large on two of
     * three models.  Both sides are matched now, and the difference between them
     * is the thing actually in question - which is small, as it should be.
     *
     * Why two at all, given that both are linear and after the last plate, so
     * they can realise the same total response: on the chip they cannot.  The
     * impulse is int16 in `ag_ir`, so an impulse asked to carry a 20 dB tilt
     * spends its dynamic range on that tilt (measured: 2.5 dB of noise floor),
     * while a bank in front of it is exact arithmetic and free of that. Splitting
     * the work - gross shaping in seven biquads, fine detail in the impulse - is
     * the arithmetically cheap position, and it is also the only one where a knob
     * can still move something at runtime.
     *
     * The speaker is the same on every side, and that is the one thing needing
     * care.  A capture with a cabinet in it already has one; a head-only capture
     * gets ours put on the reference first, because an impulse fitted against a
     * reference with no speaker would have to undo one.
     */
    {
        char   irp[256], irp_bank[256];
        float *cab = cabd, *tgt = NULL, *ours = NULL, *ours_dry = NULL;
        float *bank_dry = NULL, *bank_wet = NULL;
        uint32_t cn = cabdn;

        (void)snprintf(irp, sizeof(irp), "build/listen/ir_%s_fitted.wav",
                       ag_amp_model_name(g_model));
        (void)snprintf(irp_bank, sizeof(irp_bank), "build/listen/ir_%s_bank.wav",
                       ag_amp_model_name(g_model));
        if (0) {
            uint32_t rr = 0, fn = 0;
            float   *f = read_wav(g_cab_path, &fn, &rr);
            if (f != NULL && rr == drate) {
                cab = f;
                cn = fn;
            } else if (f != NULL) {
                cab = wr_resample_f(f, fn, rr, drate, &cn, 0);
                free(f);
            }
        } else if (cabc != NULL) {
            cab = wr_resample_f(cabc, cabc_n, crate, drate, &cn, 0);
        } else {
            uint32_t rr = 0, fn = 0;
            float   *f = read_wav(AG_CAB_MATCHED, &fn, &rr);
            if (f == NULL) {
                f = read_wav(AG_CAB_DEFAULT, &fn, &rr);
            }
            if (f != NULL && rr == drate) {
                cab = f;
                cn = fn;
            } else if (f != NULL) {
                cab = wr_resample_f(f, fn, rr, drate, &cn, 0);
                free(f);
            }
        }
        /*
         * And it is written out under the name both tools agree on, even when it
         * came from a default rather than from the capture.
         *
         * This is what makes the A/B in tube_live a comparison: that key plays
         * the bank through a cabinet and the impulse instead of both, and if the
         * two carry *different* speakers then what it demonstrates is the
         * difference between two loudspeakers.  One file, named after the model,
         * used by whichever tool needs it.
         */
        if (cab != NULL && cn > 8u && !has_cab) {
            char cabp[256];
            (void)snprintf(cabp, sizeof(cabp), "build/listen/match_cab_%s.wav",
                           ag_amp_model_name(g_model));
            if (write_wav(cabp, cab, cn, drate) == 0) {
                printf("\n  the cabinet this used, for tube_live to load as the"
                       " other side of `z`: %s\n", cabp);
            }
        }
        /*
         * Fitted against the chain **as it ships**, not against the voicing the
         * fit above just proposed.
         *
         * Those are two different answers to the same question and only one of
         * them is in the tree: the impulse is there to be A/B'd against the bank
         * that is compiled in today, which is what `tube_live` plays.  Adopt the
         * numbers printed above and re-run this, and the impulse follows.
         */
        {
            ag_amp_cfg_t shipped;
            model_cfg(&shipped, (float)drate);
            shipped.drive = drive;
            /* The same chain twice: without the post bank, which is what mode 2
             * fits an impulse for, and with it, which is what mode 1 does. */
            ours_dry = chain_render_dry(&shipped, in, dn, drate, 1);
            bank_dry = chain_render_dry(&shipped, in, dn, drate, 0);
        }
        if (cab != NULL && cn > 8u && ours_dry != NULL) {
            /*
             * Two versions of the same render, and keeping them apart is the
             * whole of it: the *fit* wants our side through the cabinet, because
             * it is fitting the residual between two signals that both have a
             * speaker; the *render through the finished impulse* wants our side
             * dry, because the impulse carries the speaker itself.  Using one for
             * both applies the cabinet twice, which is how this was wrong twice in
             * one afternoon - once in the fit and once in the verdict below.
             */
            ours = convolve_f(ours_dry, dn, cab, cn);
            bank_wet = bank_dry != NULL ? convolve_f(bank_dry, dn, cab, cn)
                                        : NULL;
            if (has_cab) {
                tgt = ref;
            } else {
                printf("\n  putting the same cabinet on the reference too,"
                       " since the capture has none -\n  otherwise the impulse"
                       " would have to undo a speaker\n");
                tgt = convolve_f(ref, rn, cab, cn);
                /*
                 * And kept, because this is the file to *listen* to: the
                 * amplifier through the same speaker our chain plays through,
                 * which is the only form in which the two can be compared by
                 * ear.  `tube_live` loads it for its `y` key.
                 */
                if (tgt != NULL) {
                    char rp[256];
                    (void)snprintf(rp, sizeof(rp),
                                   "build/listen/match_ref_%s_cab.wav",
                                   ag_amp_model_name(g_model));
                    if (write_wav(rp, tgt, rn, drate) == 0) {
                        printf("  the reference through that cabinet: %s"
                               " - tube_live's `y` plays it\n", rp);
                    }
                }
            }
        }
        if (tgt != NULL && ours != NULL && bank_wet != NULL) {
            const int taps = (int)(0.2 * (double)drate);
            float    *imp, *imp_bank;
            printf("\n  MODE 1: the bank stays in, the impulse takes the"
                   " remainder\n\n");
            imp_bank = fit_impulse(tgt, rn, bank_wet, dn, cab, cn, drate, taps,
                                   20, irp_bank, path,
                                   "this chain as it ships, post bank IN,"
                                   " through the cabinet",
                                   g_cab_path != NULL ? g_cab_path
                                                      : "the default cabinet");
            printf("\n  MODE 2: the post bank at zero, the impulse carries all"
                   " of it\n\n");
            imp = fit_impulse(tgt, rn, ours, dn, cab, cn, drate, taps, 20, irp,
                              path,
                              "this chain as it ships, post bank at zero,"
                              " through the cabinet",
                              g_cab_path != NULL ? g_cab_path
                                                 : "the default cabinet");
            printf("\n  `z` in tube_live swaps those two: %s with the bank,"
                   "\n  or %s without it.\n", irp_bank, irp);
            /*
             * AND WHICH OF THE TWO IS ACTUALLY CLOSER TO THE AMPLIFIER
             *
             * Both architectures rendered on the same take and both measured
             * against the same reference, because "the impulse is
             * architecturally right" and "the impulse sounds more like the
             * amplifier" are different claims and only the second one is worth
             * shipping on.  It is a spectrum measure and it says nothing about
             * dynamics - the compression figures above are that - but it settles
             * the part it does cover instead of leaving it to arithmetic done by
             * hand.
             */
            if (imp != NULL && imp_bank != NULL) {
                float *m1 = convolve_f(bank_dry, dn, imp_bank, (uint32_t)taps);
                float *m2 = convolve_f(ours_dry, dn, imp, (uint32_t)taps);
                if (m1 != NULL && m2 != NULL) {
                    double rs[SPEC_N], bs[SPEC_N], b1[SPEC_N], b2[SPEC_N];
                    spectrum_of(tgt, (int)rn, (float)drate, rs);
                    spectrum_of(bank_wet, (int)dn, (float)drate, bs);
                    spectrum_of(m1, (int)dn, (float)drate, b1);
                    spectrum_of(m2, (int)dn, (float)drate, b2);
                    /* Three rows: what ships today, and the two modes.  The
                     * first is there because it is the baseline both modes are
                     * supposed to beat. */
                    printf("\n  Against the same reference, third-octave rms"
                           " from 63 Hz to 6.3 kHz:\n"
                           "    as it ships: the bank into a raw cabinet  %5.2f dB\n"
                           "    mode 1: the bank and a fitted impulse     %5.2f dB\n"
                           "    mode 2: the fitted impulse alone          %5.2f dB\n",
                           spec_rms(rs, bs), spec_rms(rs, b1), spec_rms(rs, b2));
                }
                free(m1);
                free(m2);
            }
            /*
             * AND THE SAME TWO ON A TAKE NEITHER OF THEM WAS FITTED ON
             *
             * Without this, every number above is a fit reporting on itself.  The
             * impulse has 24 free bands and the bank has seven fixed sections, so
             * of course the impulse wins on the take it was fitted to - the
             * question that decides which one to ship is whether it still wins on
             * material it has never seen, and the two answers are not the same.
             * Measured: on the fitting take the impulse wins on all three models,
             * and on a 31-second take it has never heard it wins on one of three.
             * That is the signature of degrees of freedom, not of accuracy.
             *
             * It costs a capture render and two long convolutions - most of a
             * minute - and it is not optional, for the same reason the compression
             * line is not: a tool that prints only the flattering measurement will
             * be believed.
             */
            eval_held_out(cap, argv[3], crate, drate, cab, cn, imp, imp_bank,
                          (uint32_t)taps, has_cab, drive);
            free(imp);
            free(imp_bank);
        } else {
            printf("\n  no impulse written: no cabinet to build one around.\n");
        }
        if (tgt != ref) {
            free(tgt);
        }
        free(ours);
        free(ours_dry);
        free(bank_dry);
        free(bank_wet);
    }

done:
    free(cabc);
    free(cabd);
    if (in48 != in) {
        free(in48);
    }
    free(in);
    free(wet48);
    free(quiet48);
    free(ref);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "curve";

    {
        const char *m = getenv("AG_MODEL");
        if (m != NULL) {
            const int id = ag_amp_model_by_name(m);
            if (id < 0) {
                printf("  AG_MODEL=%s is not a model.  Known: jcm800 bogner"
                       " slo\n",
                       m);
                return 1;
            }
            g_model = id;
        }
    }
    g_cab_path = getenv("AG_CAB_IR");
    if (g_cab_path == NULL) {
        static const char *const try[2] = { AG_CAB_MATCHED, AG_CAB_DEFAULT };
        int                      k;
        for (k = 0; k < 2 && g_cab_path == NULL; k++) {
            FILE *f = fopen(try[k], "rb");
            if (f != NULL) {
                fclose(f);
                g_cab_path = try[k];
            }
        }
    }

    if (alloc_all() != 0) {
        printf("  out of memory\n");
        return 1;
    }
    if (strcmp(mode, "curve") == 0) {
        mode_curve();
    } else if (strcmp(mode, "resp") == 0) {
        mode_resp();
    } else if (strcmp(mode, "alias") == 0) {
        mode_alias();
    } else if (strcmp(mode, "spec") == 0) {
        mode_spec(argc, argv);
    } else if (strcmp(mode, "fit") == 0) {
        mode_fit(argc, argv);
    } else if (strcmp(mode, "attack") == 0) {
        mode_attack(argc, argv);
    } else if (strcmp(mode, "noise") == 0) {
        mode_noise(argc, argv);
    } else if (strcmp(mode, "floor") == 0) {
        mode_floor(argc, argv);
    } else if (strcmp(mode, "preset") == 0) {
        /*
         * The model as an argument as well as an environment variable, and it
         * wins: a preset is a file somebody will still have next month, and the
         * one mistake worth designing against here is a blob named after one
         * amplifier holding another.
         */
        if (argc > 4) {
            const int id = ag_amp_model_by_name(argv[4]);
            if (id < 0) {
                printf("  %s is not a model.  Known: jcm800 bogner slo\n",
                       argv[4]);
                return 1;
            }
            g_model = id;
        }
        {
            char def[64];
            (void)snprintf(def, sizeof(def), "build/listen/%s.preset",
                           ag_amp_model_name(g_model));
            (void)write_preset(argc > 2 ? argv[2] : def,
                               argc > 3 ? (float)atof(argv[3]) : 2.0f);
        }
    } else if (strcmp(mode, "cab") == 0) {
        cab_response((uint32_t)RATE);
    } else if (strcmp(mode, "imd") == 0) {
        mode_imd();
    } else if (strcmp(mode, "res") == 0) {
        mode_res();
    } else if (strcmp(mode, "humps") == 0) {
        mode_humps(argc, argv);
    } else if (strcmp(mode, "irfit") == 0) {
        mode_irfit(argc, argv);
    } else if (strcmp(mode, "tonefilt") == 0) {
        mode_tonefilt(argc, argv);
    } else if (strcmp(mode, "cabify") == 0) {
        mode_cabify(argc, argv);
    } else if (strcmp(mode, "buzz") == 0) {
        mode_buzz(argc, argv);
    } else if (strcmp(mode, "render") == 0) {
        mode_render(argc, argv);
    } else if (strcmp(mode, "match") == 0) {
        mode_match(argc, argv);
    } else if (strcmp(mode, "sens") == 0) {
        mode_sens(argc, argv);
    } else if (strcmp(mode, "irnoise") == 0) {
        mode_irnoise(argc, argv);
    } else if (strcmp(mode, "selftest") == 0) {
        mode_selftest();
    } else if (strcmp(mode, "harm") == 0) {
        mode_harm(argc, argv);
    } else if (strcmp(mode, "knee") == 0) {
        mode_knee(argc, argv);
    } else if (strcmp(mode, "ladder") == 0) {
        mode_ladder(argc, argv);
    } else if (strcmp(mode, "duo") == 0) {
        mode_duo(argc, argv);
    } else if (strcmp(mode, "onset") == 0) {
        mode_onset(argc, argv);
    } else if (strcmp(mode, "step1") == 0) {
        mode_step1(argc, argv);
    } else if (strcmp(mode, "step2") == 0) {
        mode_step2(argc, argv);
    } else if (strcmp(mode, "stepn") == 0) {
        mode_stepn(argc, argv);
    } else if (strcmp(mode, "iter") == 0) {
        mode_iter(argc, argv);
    } else if (strcmp(mode, "kneew") == 0) {
        mode_kneew(argc, argv);
    } else {
        printf("modes: curve resp alias imd res buzz humps render fit match"
               " sens harm knee ladder duo onset step1 step2 stepn iter kneew"
               " irnoise"
               " selftest"
               " preset"
               " cabify"
               " irfit"
               " tonefilt\n"
               "  match capture.nam di.wav is the whole tone match: it plays"
               " the capture, fits\n"
               "  the voicing to it, and measures what both of them do to"
               " dynamics\n"
               "  AG_MODEL=jcm800|bogner|slo picks the amplifier; preset takes"
               " it as its third argument\n");
    }
    free(g_ckt);
    free(g_tab);
    return 0;
}
