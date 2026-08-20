/*
 * ArgonOS - host tests for the one-dimensional valve model (apps/common/tube).
 *
 * The model is a static curve with linear filters around it, and each of those
 * two halves fails in a way the other cannot show:
 *
 * 1. The curve is baked by sweeping the same ag_ckt solver over DC, so what has
 *    to be checked is not the valve equation - test_ckt.c does that - but that
 *    the sweep produced a valve: an operating point in the right place,
 *    inversion, gain of tens, asymmetric clipping, flat tails, and no grid point
 *    where Newton gave up.  A bake that silently produced a straight line would
 *    give a clean channel that never distorts, which sounds like a working amp
 *    with the gain down.
 *
 * 2. The antialiasing is where the arithmetic is delicate.  The two-axis model
 *    in ag_stage learned this the expensive way: the textbook form of the
 *    formula divides a difference of two nearly equal integrals by a number that
 *    passes through zero twice a period, and the result is a noise of constant
 *    size that grows relative to the music as the music decays.  It was found by
 *    ear, not by measurement, because silence and full-scale signal-to-noise
 *    both looked fine.  So: silence must be bit-exact, and the oversampling
 *    filters must be measured rather than assumed - a decimator with its corner
 *    in the wrong place measured 8 dB *more* aliasing than none at all.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "ag_amp.h"
#include "ag_biq.h"
#include "ag_ckt.h"
#include "ag_os.h"
#include "ag_tone.h"
#include "ag_tube.h"
#include "ckt_circuits.h"

#define TFS 22050.0f

/* ------------------------------------------------------------------------ */
/* The component values exist twice.  This is what stops them drifting.      */
/* ------------------------------------------------------------------------ */

/*
 * Every matching filter out of the way: both banks, every stage, every trim.
 *
 * Four tests wanted this and each spelled it out as a loop over one bank, which
 * was the whole story when there was one bank in front of the chain.  There is now
 * one in front of every valve plus a per-stage trim, so those loops silently left
 * most of the matching layer switched on - a test called "the voicing out of the
 * way: this is about one high-pass corner" measuring three banks and four trims.
 */
static void amp_no_voicing(ag_amp_cfg_t *cfg)
{
    int st, b;
    for (st = 0; st < AG_AMP_STAGES; st++) {
        cfg->vtrim[st] = 0.0f;
        for (b = 0; b < AG_AMP_VOICE_N; b++) {
            cfg->voice[st][b].db = 0.0f;
        }
    }
    for (b = 0; b < AG_AMP_VOICE_N; b++) {
        cfg->tone[b].db = 0.0f;
    }
    /*
     * `mid_db` is deliberately *not* touched.  It is the model's own mid lift in
     * front of the last valve - taste, but the model's taste - rather than a
     * matching filter, and folding it in here silently changed what the stage-count
     * test was measuring: without it a fourth hot valve on a chain three already
     * saturate adds no rms, and the test that says each valve adds gain failed for
     * a reason that had nothing to do with the valves.  Callers that mean to
     * flatten it say so.
     */
}

static void test_spec_matches_bench(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        ag_tube_spec_t   t;
        ckt_stage_spec_t c;
        ag_tube_spec_jcm800(&t, i);
        ckt_jcm800_stage(&c, i);
        AG_CHECK(t.rsrc == c.rsrc);
        AG_CHECK(t.ccouple == c.ccouple);
        AG_CHECK(t.rgrid == c.rgrid);
        AG_CHECK(t.rstop == c.rstop);
        AG_CHECK(t.vsupply == c.vsupply);
        AG_CHECK(t.rplate == c.rplate);
        AG_CHECK(t.rcath == c.rcath);
        AG_CHECK(t.ccath == c.ccath);
        AG_CHECK(t.cload == c.cload);
        AG_CHECK(t.rload == c.rload);
    }
}

/* ------------------------------------------------------------------------ */
/* The curve                                                                */
/* ------------------------------------------------------------------------ */

static void test_curve_is_a_valve(void)
{
    ag_ckt_t      *k = (ag_ckt_t *)malloc(sizeof(ag_ckt_t));
    float         *tab = (float *)malloc(sizeof(float) * 2048 * 3);
    ag_tube_spec_t sp;
    ag_tube_t      tb;
    float          gain, slope_lo, slope_hi, up, dn;

    if (k == 0 || tab == 0) {
        AG_CHECK(0);
        free(k);
        free(tab);
        return;
    }
    ag_tube_spec_jcm800(&sp, 0);
    ag_tube_init(&tb);
    AG_CHECK_INT(ag_tube_bake(&tb, k, &sp, tab, tab + 2048, tab + 4096, 2048, 1.0f,
                              0.0f, 0.01f), 0);

    /* A 12AX7 on a 330 V rail through 220k, biased by 820R: the plate lands
     * well under half the rail and the cathode under a volt and a half. */
    AG_CHECK(tb.vq_plate > 80.0f && tb.vq_plate < 250.0f);
    AG_CHECK(tb.vq_cath > 0.2f && tb.vq_cath < 1.5f);
    /* Every grid point converged.  The two-axis bake reported all 2304 of its
     * points as failures once, while holding answers good to eight digits,
     * because its tolerance was below what a float can represent at 200 V. */
    AG_CHECK_INT(tb.bake_noncvg, 0);

    /* No signal in, no signal out: the curve carries the operating point as its
     * zero, so an idle amplifier is silent rather than sitting at 130 volts. */
    AG_CHECK(fabsf(ag_tube_curve(&tb, 0.0f)) < 1.0e-3f);

    /* Inverting, with gain of tens.  A stage that came out non-inverting would
     * still sound like something, and it would clip the wrong way round. */
    gain = (ag_tube_curve(&tb, 0.01f) - ag_tube_curve(&tb, -0.01f)) / 0.02f;
    AG_CHECK(gain < -20.0f && gain > -140.0f);

    /*
     * Asymmetric clipping, which is the whole reason for preferring a valve to a
     * waveshaper: the grid conducts on one side and the valve cuts off on the
     * other, and those two limits are nothing like each other.
     *
     * Taken at two volts, not one.  At one volt V1a is only four percent
     * asymmetric - the two limits are still ahead of it - and a threshold set
     * from an expectation rather than from a measurement failed here first.
     * Measured: +112.4 V against -78.1 V, so thirty percent.
     */
    up = ag_tube_curve(&tb, -2.0f);
    dn = ag_tube_curve(&tb, 2.0f);
    AG_CHECK(fabsf(fabsf(up) - fabsf(dn)) > 0.20f * fabsf(up));

    /*
     * The tails are flat, which is what makes reading off the end of this table
     * the physical answer rather than a guess - unlike the two-axis table, where
     * the same thing means the model is extrapolating a load line it never saw.
     *
     * Flat is not equally flat at both ends, and the numbers say why: cut-off
     * arrives, so the top of the axis measures -0.01 against a gain of 71, while
     * grid conduction only asymptotes, so the bottom is still at -4.8.
     */
    slope_lo = (ag_tube_curve(&tb, tb.lo + 0.05f) - ag_tube_curve(&tb, tb.lo)) /
               0.05f;
    slope_hi = (ag_tube_curve(&tb, tb.hi) - ag_tube_curve(&tb, tb.hi - 0.05f)) /
               0.05f;
    AG_CHECK(fabsf(slope_lo) < 0.12f * fabsf(gain));
    AG_CHECK(fabsf(slope_hi) < 0.05f * fabsf(gain));

    /* Monotone: more grid is always less plate.  A fold in the curve would make
     * the lookup ambiguous and the antialiasing average meaningless. */
    {
        int   i, bad = 0;
        float prev = tab[0];
        for (i = 1; i < 2048; i++) {
            if (tab[i] > prev + 1.0e-4f) {
                bad++;
            }
            prev = tab[i];
        }
        AG_CHECK_INT(bad, 0);
    }

    /* The cathode shelf, derived from the two small-signal gains the bake took
     * rather than dialled: 820R against a 12AX7 is a few decibels. */
    {
        float fz = 0.0f, db = 0.0f;
        ag_tube_shelf(&tb, &sp, &fz, &db);
        AG_CHECK(fz > 250.0f && fz < 320.0f); /* 1/(2*pi*820*0.68u) = 285 Hz */
        AG_CHECK(db < -1.0f && db > -14.0f);
    }
    /* V1a: 100 nF into 10k + 1M is 1.6 Hz, and 22 nF into the 470k it drives is
     * 15.4 Hz.  (V1b's own output capacitor sees a megohm and corners at 7.2.) */
    AG_CHECK(fabsf(ag_tube_couple_hz(&sp) - 1.58f) < 0.1f);
    AG_CHECK(fabsf(ag_tube_load_hz(&sp) - 15.39f) < 0.3f);

    free(k);
    free(tab);
}

/* ------------------------------------------------------------------------ */
/* Antialiasing arithmetic                                                  */
/* ------------------------------------------------------------------------ */

static void test_adaa_arithmetic(void)
{
    ag_ckt_t      *k = (ag_ckt_t *)malloc(sizeof(ag_ckt_t));
    float         *tab = (float *)malloc(sizeof(float) * 2048 * 3);
    ag_tube_spec_t sp;
    ag_tube_t      tb;
    int            i;

    if (k == 0 || tab == 0) {
        AG_CHECK(0);
        free(k);
        free(tab);
        return;
    }
    ag_tube_spec_jcm800(&sp, 1);
    ag_tube_init(&tb);
    AG_CHECK_INT(ag_tube_bake(&tb, k, &sp, tab, tab + 2048, tab + 4096, 2048, 1.0f,
                              0.0f, 0.01f), 0);
    AG_CHECK(ag_tube_set_adaa(&tb, 1) != 0);

    /*
     * Silence must be bit exact.  This is the check that would have caught the
     * two-axis model's crackle, and it is cheap: the naive formula gives about
     * -45 dB here, the half-fixed one -92, and the form that never subtracts
     * gives exactly zero.
     */
    for (i = 0; i < 1024; i++) {
        AG_CHECK(ag_tube_tick(&tb, 0.0f) == 0.0f);
    }

    /* A held input is a zero-width span, whose average is the curve itself. */
    ag_tube_reset(&tb);
    for (i = 0; i < 4; i++) {
        (void)ag_tube_tick(&tb, 0.3f);
    }
    AG_CHECK(fabsf(ag_tube_tick(&tb, 0.3f) - ag_tube_curve(&tb, 0.3f)) < 1.0e-4f);

    /*
     * A slow ramp: the average over a step is bounded by the curve at its two
     * ends, and it must be, because it is an average.  This catches the sign and
     * indexing mistakes that a smooth-looking waveform hides.
     */
    ag_tube_reset(&tb);
    {
        float prev = -2.0f;
        for (i = 0; i <= 400; i++) {
            const float v = -2.0f + 4.0f * (float)i / 400.0f;
            const float y = ag_tube_tick(&tb, v);
            const float a = ag_tube_curve(&tb, prev);
            const float b = ag_tube_curve(&tb, v);
            const float lo = a < b ? a : b;
            const float hi = a < b ? b : a;
            AG_CHECK(y >= lo - 1.0e-3f && y <= hi + 1.0e-3f);
            prev = v;
        }
    }

    /*
     * A jump that crosses more cells than the walk will do, so the cumulative
     * antiderivative path runs.  Same bound, and it is the path where the
     * subtraction lives - if the two paths disagree, the seam between them is a
     * discontinuity that the ear finds long before this test would.
     */
    ag_tube_reset(&tb);
    for (i = 0; i < 200; i++) {
        const float v = (i & 1) ? 2.5f : -2.5f;
        const float y = ag_tube_tick(&tb, v);
        AG_CHECK(y >= ag_tube_curve(&tb, 2.5f) - 1.0f);
        AG_CHECK(y <= ag_tube_curve(&tb, -2.5f) + 1.0f);
    }

    /* Off the end of the table on purpose: the curve is extended flat, so the
     * average of a span entirely outside is the edge value exactly. */
    ag_tube_reset(&tb);
    for (i = 0; i < 8; i++) {
        (void)ag_tube_tick(&tb, tb.hi + 40.0f);
    }
    AG_CHECK(fabsf(ag_tube_tick(&tb, tb.hi + 41.0f) - ag_tube_curve(&tb, tb.hi)) <
             1.0e-3f);
    AG_CHECK(tb.clamped > 0u);

    free(k);
    free(tab);
}

/* ------------------------------------------------------------------------ */
/* Blocking distortion                                                       */
/* ------------------------------------------------------------------------ */

/*
 * The memory a static curve cannot have: grid current charging the input
 * coupling capacitor, which holds the grid down and biases the stage toward
 * cut-off until the grid leak drains it.
 *
 * Four things have to be true, and the fourth is the one that could have gone
 * wrong quietly.  The charge has to appear only when the grid conducts, it has to
 * drain with the time constant the components say, silence must stay silence,
 * and the loop must not ring - the grid current is integrated explicitly, and at
 * 1x the faster of the two stages has a loop time constant of only two samples.
 */
static void test_blocking(void)
{
    ag_ckt_t      *k = (ag_ckt_t *)malloc(sizeof(ag_ckt_t));
    float         *tab = (float *)malloc(sizeof(float) * 2048 * 3);
    ag_tube_spec_t sp;
    ag_tube_t      tb;
    int            i, s;

    if (k == 0 || tab == 0) {
        AG_CHECK(0);
        free(k);
        free(tab);
        return;
    }

    for (s = 0; s < 2; s++) {
        const float tau = 0.0f; /* filled below from the components */
        (void)tau;
        ag_tube_spec_jcm800(&sp, s);
        ag_tube_init(&tb);
        AG_CHECK_INT(ag_tube_bake(&tb, k, &sp, tab, tab + 2048, tab + 4096, 2048,
                                  1.0f, 0.0f, 0.01f),
                     0);
        AG_CHECK(tb.g != 0);
        /* Zero at and below the operating point, or the capacitor charges on
         * silence and an idle amplifier is not idle. */
        AG_CHECK(ag_tube_curve(&tb, 0.0f) == 0.0f);
        AG_CHECK(tb.g[(int)tb.zero_p] == 0.0f);
        AG_CHECK(tb.g[0] == 0.0f);
        /* And it does conduct somewhere, or none of this does anything. */
        AG_CHECK(tb.g[tb.n - 1] > 1.0e-6f);

        AG_CHECK(ag_tube_set_blocking(&tb, &sp, TFS, 1) != 0);

        /* Silence leaves it alone, exactly. */
        for (i = 0; i < 2000; i++) {
            AG_CHECK(ag_tube_tick(&tb, 0.0f) == 0.0f);
        }
        AG_CHECK(tb.vc == 0.0f);

        /* A tone that never reaches the grid leaves it alone too. */
        ag_tube_reset(&tb);
        for (i = 0; i < 2000; i++) {
            (void)ag_tube_tick(&tb, (float)(0.2 * sin(0.05 * (double)i)));
        }
        AG_CHECK(tb.vc_peak < 1.0e-4f);

        /*
         * A hard burst charges it, and the charge holds the grid down: the stage
         * is quieter afterwards than it was before, which is the whole audible
         * effect.
         */
        ag_tube_reset(&tb);
        for (i = 0; i < 4000; i++) {
            (void)ag_tube_tick(&tb, (float)(8.0 * sin(0.3 * (double)i)));
        }
        AG_CHECK(tb.vc > 0.5f);

        /*
         * Then it drains with the time constant the components have, and nothing
         * else: rgrid * ccouple is 100 ms for V1a and 1.0 ms for V1b, which is
         * why a high gain amplifier puts a small capacitor in front of the valve
         * that clips.
         */
        {
            const double rc = (double)sp.rgrid * (double)sp.ccouple;
            const int    n = (int)(rc * (double)TFS + 0.5);
            const float  v0 = tb.vc;
            for (i = 0; i < n; i++) {
                (void)ag_tube_tick(&tb, 0.0f);
            }
            /* One time constant is 1/e = 0.368 of it. */
            AG_CHECK(tb.vc > 0.30f * v0 && tb.vc < 0.44f * v0);
        }

        /*
         * And it does not ring.  A step straight into heavy grid conduction is
         * the worst case for an explicitly integrated current, and V1b's loop -
         * ccouple times (rsrc + rstop + the grid's own resistance) - is about two
         * samples long at this rate.  Monotone approach, no overshoot past the
         * settled value.
         */
        ag_tube_reset(&tb);
        {
            float prev = 0.0f, top = 0.0f;
            int   backwards = 0;
            for (i = 0; i < 3000; i++) {
                (void)ag_tube_tick(&tb, 30.0f);
                if (tb.vc > top) {
                    top = tb.vc;
                }
                if (i > 2 && tb.vc < prev - 1.0e-4f) {
                    backwards++;
                }
                prev = tb.vc;
            }
            printf("  tube: V1%c blocking settles %.2f V into a 30 V step,"
                   " recovers in %.1f ms\n",
                   s ? 'b' : 'a', (double)top,
                   (double)(sp.rgrid * sp.ccouple * 1000.0f));
            AG_CHECK_INT(backwards, 0);
            /*
             * It settles where the grid current the leak can carry balances the
             * current the grid draws - ig(30 - vc) = vc/rgrid - which is several
             * volts short of the drive, not at it.  Both stages land between a
             * half and nine tenths of the way.
             */
            AG_CHECK(top > 15.0f && top < 29.5f);
        }
    }
    free(k);
    free(tab);
}

/* ------------------------------------------------------------------------ */
/* The oversampling filters, measured rather than assumed                   */
/* ------------------------------------------------------------------------ */

/* Amplitude of `f` in `x`, by projection onto one bin.  Enough for a single
 * tone, and no FFT needed. */
static double tone_amp(const float *x, int n, double f, double fs)
{
    double re = 0.0, im = 0.0;
    int    i;
    for (i = 0; i < n; i++) {
        const double w = 2.0 * 3.14159265358979 * f * (double)i / fs;
        re += (double)x[i] * cos(w);
        im -= (double)x[i] * sin(w);
    }
    return 2.0 * sqrt(re * re + im * im) / (double)n;
}

static void test_oversampler(void)
{
    static float buf[4096];
    ag_os2_t     os;
    int          i;

    ag_os2_init(&os, AG_OS_M);

    /* Unity at DC through up and down together.  A gain error here is a level
     * change that follows the oversampling flag, which is exactly the kind of
     * thing that gets mistaken for a tone difference. */
    for (i = 0; i < 4096; i++) {
        float u[2];
        ag_os2_up(&os, 1.0f, u);
        buf[i] = ag_os2_down(&os, u[0], u[1]);
    }
    AG_CHECK(fabsf(buf[4095] - 1.0f) < 1.0e-3f);

    /* Passband: a kilohertz must come through at its own amplitude.  Five is
     * where a Hamming halfband of this length starts to droop, so it is checked
     * loosely and printed by the tool. */
    {
        const double f = 1000.0;
        ag_os2_reset(&os);
        for (i = 0; i < 4096; i++) {
            float u[2];
            const float x =
                (float)sin(2.0 * 3.14159265358979 * f * (double)i / (double)TFS);
            ag_os2_up(&os, x, u);
            buf[i] = ag_os2_down(&os, u[0], u[1]);
        }
        AG_CHECK(fabs(tone_amp(buf + 512, 3072, f, (double)TFS) - 1.0) < 0.02);
    }

    /*
     * Rejection.  Feed the decimator alone something above the base Nyquist -
     * which is what the nonlinearity generates and what folds if it is not
     * removed - and see it gone.  This is the check that the corner is at a
     * quarter of the oversampled rate: at 0.22 instead of 0.25 the filter cuts
     * at 42 kHz rather than 22, removes nothing, and the whole exercise measures
     * worse than not doing it (rake 8 in docs/08).
     */
    {
        const double fos = 2.0 * (double)TFS;
        const double f = 15000.0; /* above 11.025 kHz, so it must not survive */
        double       amp;
        ag_os2_reset(&os);
        for (i = 0; i < 4096; i++) {
            const double t0 = 2.0 * 3.14159265358979 * f * (double)(2 * i) / fos;
            const double t1 =
                2.0 * 3.14159265358979 * f * (double)(2 * i + 1) / fos;
            buf[i] = ag_os2_down(&os, (float)sin(t0), (float)sin(t1));
        }
        /* It comes back at |22050 - 15000| = 7050 Hz if it comes back at all. */
        amp = tone_amp(buf + 512, 3072, 7050.0, (double)TFS);
        AG_CHECK(amp < 0.01); /* -40 dB */
    }

    /* Four times: same two properties, since it is the same filter twice. */
    {
        ag_os4_t os4;
        ag_os4_init(&os4);
        for (i = 0; i < 4096; i++) {
            float u[4], w[4];
            int   j;
            ag_os4_up(&os4, 1.0f, u);
            for (j = 0; j < 4; j++) {
                w[j] = u[j];
            }
            buf[i] = ag_os4_down(&os4, w);
        }
        AG_CHECK(fabsf(buf[4095] - 1.0f) < 1.0e-3f);
    }
}

/* ------------------------------------------------------------------------ */
/* Section designers                                                        */
/* ------------------------------------------------------------------------ */

static void test_sections(void)
{
    ag_biq_t s;
    float    f;

    /* First order high pass: -3.01 dB at the corner, 6 dB an octave below. */
    f = ag_biq_hp1(&s, TFS, 150.0f);
    AG_CHECK(fabsf(f - 150.0f) < 0.01f);
    AG_CHECK(fabsf(ag_biq_mag_db(&s, 150.0f, TFS) + 3.01f) < 0.1f);
    AG_CHECK(fabsf(ag_biq_mag_db(&s, 2400.0f, TFS)) < 0.1f);
    AG_CHECK(fabsf(ag_biq_mag_db(&s, 37.5f, TFS) + 12.3f) < 0.4f);

    /* Second order low pass at Butterworth Q. */
    f = ag_biq_lp2(&s, TFS, 5000.0f, 0.70710678f);
    AG_CHECK(fabsf(ag_biq_mag_db(&s, 5000.0f, TFS) + 3.01f) < 0.1f);
    AG_CHECK(fabsf(ag_biq_mag_db(&s, 200.0f, TFS)) < 0.1f);

    /*
     * A corner above Nyquist is a question about the sample rate, and the
     * designer answers it with the frequency it used instead of pretending.  The
     * top cut this amplifier was asked for is 12 kHz at a 22.05 kHz rate.
     */
    f = ag_biq_lp2(&s, TFS, 12000.0f, 0.70710678f);
    AG_CHECK(fabsf(f - TFS * 0.45f) < 1.0f);

    /* Peaking: the gain at the centre is the gain that was asked for. */
    f = ag_biq_peak(&s, TFS, 700.0f, 5.0f, 0.8f);
    AG_CHECK(fabsf(ag_biq_mag_db(&s, 700.0f, TFS) - 5.0f) < 0.05f);
    AG_CHECK(fabsf(ag_biq_mag_db(&s, 40.0f, TFS)) < 0.3f);
    AG_CHECK(fabsf(ag_biq_mag_db(&s, 9000.0f, TFS)) < 0.3f);

    /* First order shelf: flat above the zero, down by `db` below it. */
    (void)ag_biq_shelf1(&s, TFS, 285.0f, -6.0f);
    AG_CHECK(fabsf(ag_biq_mag_db(&s, 5.0f, TFS) + 6.0f) < 0.2f);
    AG_CHECK(fabsf(ag_biq_mag_db(&s, 8000.0f, TFS)) < 0.2f);
    /* The corner of the cathode network is the shelf's zero, so the response
     * there is halfway up in decibels give or take the pole's contribution. */
    AG_CHECK(ag_biq_mag_db(&s, 285.0f, TFS) > -5.0f);
    AG_CHECK(ag_biq_mag_db(&s, 285.0f, TFS) < -1.0f);
}

/* ------------------------------------------------------------------------ */
/* The chain, and the number the whole design turns on                      */
/* ------------------------------------------------------------------------ */

/*
 * Energy that is not a harmonic of the input tone.
 *
 * N and the tone are chosen together: at bin 99 of 2048, every folded harmonic
 * lands 31 bins or more away from a real one, so aliasing cannot hide inside the
 * spectrum it came from.  Picking that badly is how a measurement comes back
 * saying an artefact is not there.
 *
 * Bins within four of a multiple of `bin` are excluded, which means folded
 * products that do land there are missed - so this is a lower bound.  It is the
 * same lower bound for every configuration it is applied to, which is all a
 * comparison needs.
 */
#define NA_N   2048
#define NA_BIN 99

static double nonharmonic_db(const float *x, int n, int bin)
{
    double tot = 0.0, junk = 0.0;
    int    b;
    for (b = 1; b <= n / 2; b++) {
        double re = 0.0, im = 0.0, p;
        int    i, d, harm = 0;
        for (i = 0; i < n; i++) {
            /* Hann, so that a bin-centred tone does not smear over the bins the
             * aliases are being counted in. */
            const double w =
                0.5 - 0.5 * cos(2.0 * 3.14159265358979 * (double)i / (double)n);
            const double a =
                2.0 * 3.14159265358979 * (double)b * (double)i / (double)n;
            re += (double)x[i] * w * cos(a);
            im -= (double)x[i] * w * sin(a);
        }
        p = re * re + im * im;
        tot += p;
        d = b % bin;
        if (d > bin / 2) {
            d = bin - d;
        }
        if (d <= 4) {
            harm = 1;
        }
        if (!harm) {
            junk += p;
        }
    }
    if (tot < 1e-300 || junk < 1e-300) {
        return -300.0;
    }
    return 10.0 * log10(junk / tot);
}

static void render_tone(ag_amp_t *a, float *out, int n, double amp)
{
    int i;
    ag_amp_reset(a);
    /* Long enough for the 1.6 Hz input high pass to stop moving, which it does
     * not do quickly - and a drifting DC in front of a clipper changes which way
     * it clips. */
    for (i = 0; i < 8192; i++) {
        const double t = 2.0 * 3.14159265358979 * (double)NA_BIN *
                         (double)i / (double)NA_N;
        (void)ag_amp_tick(a, (float)(amp * sin(t)));
    }
    for (i = 0; i < n; i++) {
        const double t = 2.0 * 3.14159265358979 * (double)NA_BIN *
                         (double)(i + 8192) / (double)NA_N;
        out[i] = ag_amp_tick(a, (float)(amp * sin(t)));
    }
}

#define PROBE_N 22050

static void test_amp_chain(void)
{
    ag_ckt_t    *k = (ag_ckt_t *)malloc(sizeof(ag_ckt_t));
    float       *tab = (float *)malloc(sizeof(float) * AG_AMP_TAB_FLOATS);
    ag_amp_t    *a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    float       *buf = (float *)malloc(sizeof(float) * NA_N);
    float       *probe = (float *)malloc(sizeof(float) * PROBE_N);
    ag_amp_cfg_t cfg;
    double       junk1, junk4;

    if (k == 0 || tab == 0 || a == 0 || buf == 0 || probe == 0) {
        AG_CHECK(0);
        free(k);
        free(tab);
        free(a);
        free(buf);
        free(probe);
        return;
    }
    ag_amp_probe_pluck(probe, PROBE_N, TFS);

    ag_amp_defaults(&cfg, TFS);
    cfg.os = 1;
    cfg.adaa = 0;
    AG_CHECK_INT(ag_amp_build(a, k, &cfg, tab, 0, probe, PROBE_N), 0);

    /* The filters landed where the component values put them. */
    AG_CHECK(fabsf(a->couple_hz[0] - 1.58f) < 0.1f);
    AG_CHECK(fabsf(a->couple_hz[1] - 142.0f) < 3.0f);
    AG_CHECK(fabsf(a->load_hz - 7.23f) < 0.2f);
    AG_CHECK(a->shelf_db[0] < -1.0f && a->shelf_db[1] < -1.0f);
    /* 12 kHz asked for, 0.45*fs available. */
    AG_CHECK(fabsf(a->top_hz_used - TFS * 0.45f) < 1.0f);
    AG_CHECK(fabsf(a->mid_hz_used - 700.0f) < 1.0f);

    /*
     * Two inverting stages make a non-inverting amplifier.
     *
     * Two ways not to check that, both of which were tried.  A DC step fails
     * because three high passes stand in the way and what it measures is the
     * fastest one's tail, which points the other way.  A tone measured after two
     * hundred milliseconds fails for a subtler reason: the input coupling
     * capacitor is 1.58 Hz, a hundred millisecond time constant, so at two
     * hundred milliseconds its startup transient is still a fifth of the way
     * through and it swamps a small signal.  Measured, it read -1.4 where the
     * steady state is about +20 - not marginal, just early.
     *
     * So: settle for a whole second, ten of that time constant, then correlate
     * over an exact number of periods of a 500 Hz tone.  Small signal, so nothing
     * clips and no grid conducts, and the sign is the sign.
     */
    {
        int    i;
        double acc = 0.0;
        ag_amp_reset(a);
        for (i = 0; i < 22050 + 4410; i++) {
            const double s = sin(2.0 * 3.14159265358979 * 500.0 * (double)i /
                                 (double)TFS);
            const float  y = ag_amp_tick(a, (float)(0.002 * s));
            if (i >= 22050) {
                acc += (double)y * s;
            }
        }
        AG_CHECK(acc > 0.0);
    }

    /* Silence in, silence out, exactly - there is no dither and no dc anywhere,
     * so anything else is a state variable that did not start at rest. */
    {
        int i;
        ag_amp_reset(a);
        for (i = 0; i < 512; i++) {
            AG_CHECK(ag_amp_tick(a, 0.0f) == 0.0f);
        }
    }

    /*
     * And the number the whole design turns on.  Same signal, same curves, one
     * flag apart: without oversampling the harmonics that two clipping valves
     * make above 11 kHz come back inside the band at the wrong frequencies; with
     * four times and the antialiasing average they are computed where they
     * belong and then filtered away.
     */
    render_tone(a, buf, NA_N, 0.5);
    junk1 = nonharmonic_db(buf, NA_N, NA_BIN);

    cfg.os = 4;
    cfg.adaa = 1;
    AG_CHECK_INT(ag_amp_set_voicing(a, &cfg), 0);
    render_tone(a, buf, NA_N, 0.5);
    junk4 = nonharmonic_db(buf, NA_N, NA_BIN);

    printf("  tube: non-harmonic products  1x %.1f dB   4x+adaa %.1f dB\n", junk1,
           junk4);
    AG_CHECK(junk4 < junk1 - 15.0);
    AG_CHECK(junk4 < -35.0);

    /* Driven this hard the curve is used, but not run off the end more than
     * occasionally - and if it is, the tails are flat, so it is level rather
     * than shape that is lost.  The count is printed either way. */
    printf("  tube: clamped %lu of %lu, v1 saw %.2f..%.2f V, v2 %.1f..%.1f V\n",
           (unsigned long)ag_amp_clamped(a), (unsigned long)a->tube[0].samples,
           (double)a->tube[0].seen_lo, (double)a->tube[0].seen_hi,
           (double)a->tube[1].seen_lo, (double)a->tube[1].seen_hi);
    AG_CHECK(a->tube[0].seen_hi > 0.1f); /* the curve is actually being used */

    /*
     * The drive knob has to survive a re-voice.  The gains are cached out of the
     * config when the chain is built, so ag_amp_set_voicing has to refresh them
     * too - it did not, and the symptom was a preset that played at whatever
     * drive it was baked with whatever the knob said.  Nothing failed; it just
     * ignored you.
     */
    {
        int   i;
        float loud[2];
        int   d;
        ag_amp_defaults(&cfg, TFS);
        cfg.os = 1;
        cfg.adaa = 0;
        if (ag_amp_build(a, k, &cfg, tab, 0, probe, PROBE_N) == 0) {
            for (d = 0; d < 2; d++) {
                double acc = 0.0;
                cfg.drive = d == 0 ? 0.25f : 1.0f;
                AG_CHECK_INT(ag_amp_set_voicing(a, &cfg), 0);
                for (i = 0; i < PROBE_N; i++) {
                    const float y = ag_amp_tick(a, probe[i]);
                    acc += (double)y * (double)y;
                }
                loud[d] = (float)acc;
            }
            AG_CHECK(loud[1] > loud[0] * 1.5);
        }
    }

    free(probe);
    free(k);
    free(tab);
    free(a);
    free(buf);
}

/* ------------------------------------------------------------------------ */
/* However many stages, and stages that are not curves at all                */
/* ------------------------------------------------------------------------ */

/*
 * The chain holds one to four stages, and a stage is a stage: the runtime never
 * asks whether the table in front of it came from a triode, a diode pair or a
 * transistor - only whether there is one.  A stage with no table is its gain and
 * its filter block and nothing else.
 *
 * What has to hold: every count builds and runs, more stages means more gain,
 * a linear stage is exactly a multiply, and silence stays silence whatever the
 * topology.
 */
static void test_stage_counts(void)
{
    ag_ckt_t    *k = (ag_ckt_t *)malloc(sizeof(ag_ckt_t));
    float       *tab = (float *)malloc(sizeof(float) * AG_AMP_TAB_FLOATS);
    ag_amp_t    *a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    float       *probe = (float *)malloc(sizeof(float) * PROBE_N);
    ag_amp_cfg_t cfg;
    double       loud[5];
    int          ns, i;

    if (k == 0 || tab == 0 || a == 0 || probe == 0) {
        AG_CHECK(0);
        free(k);
        free(tab);
        free(a);
        free(probe);
        return;
    }
    ag_amp_probe_pluck(probe, PROBE_N, TFS);

    for (ns = 1; ns <= 4; ns++) {
        double acc = 0.0;
        ag_amp_defaults(&cfg, TFS);
        cfg.n_stages = ns;
        cfg.os = 1;
        cfg.adaa = 0;
        cfg.drive = 0.2f;
        /*
         * The matching layer out, because the claim below is about valves.
         *
         * With it in, the two-valve model arrives with +3.1 and +7.9 dB of trim in
         * front of its stages - so by three valves the chain is fully saturated and
         * a fourth one takes rms *off* rather than adding to it, exactly as the
         * four-valve model's real recovery stage does.  That is a true statement
         * about a slammed chain and it is not what "each valve is another sixty of
         * gain" is testing.
         */
        amp_no_voicing(&cfg);
        AG_CHECK_INT(ag_amp_build(a, k, &cfg, tab, 0, probe, PROBE_N), 0);
        AG_CHECK_INT(a->n, ns);

        /* Silence in, silence out, whatever the topology. */
        for (i = 0; i < 256; i++) {
            AG_CHECK(ag_amp_tick(a, 0.0f) == 0.0f);
        }
        ag_amp_reset(a);
        for (i = 0; i < PROBE_N; i++) {
            const float y = ag_amp_tick(a, probe[i]);
            acc += (double)y * (double)y;
        }
        loud[ns] = 10.0 * log10(acc / (double)PROBE_N + 1e-30);
        /* It made something, and it did not blow up. */
        AG_CHECK(loud[ns] > -200.0 && a->peak_out < 100.0f);
    }
    /* Each valve added is another sixty of gain before the master, so the
     * output grows monotonically with the stage count. */
    AG_CHECK(loud[2] > loud[1]);
    AG_CHECK(loud[3] > loud[2]);
    AG_CHECK(loud[4] > loud[3]);

    /*
     * A stage with no table is exactly a multiply.  Two chains, identical but
     * for the second stage being linear with unity gain: the linear one must
     * come out as the one-stage chain does, to the sample.
     */
    {
        float *one = (float *)malloc(sizeof(float) * 2048);
        float *two = (float *)malloc(sizeof(float) * 2048);
        if (one != 0 && two != 0) {
            ag_amp_defaults(&cfg, TFS);
            cfg.os = 1;
            cfg.adaa = 0;
            cfg.drive = 0.2f;
            /*
             * And the matching layer out, which this test used to get for free.
             * A second stage now arrives with its own fitted bank and a flat trim
             * - on the two-valve model that trim is +5.2 dB - so "the two differ
             * by a filter" stopped being true of the *matching* filters while
             * remaining true of the claim being tested, which is about the valve.
             * amp_no_voicing leaves the coupling capacitor and cathode shelf,
             * because those are the circuit and they are the filter the comparison
             * below is about.
             */
            amp_no_voicing(&cfg);
            cfg.n_stages = 1;
            if (ag_amp_build(a, k, &cfg, tab, 0, probe, PROBE_N) == 0) {
                for (i = 0; i < 2048; i++) {
                    one[i] = ag_amp_tick(a, probe[i]);
                }
            }
            cfg.n_stages = 2;
            cfg.linear[1] = 1;
            cfg.g12 = 1.0f;
            if (ag_amp_build(a, k, &cfg, tab, 0, probe, PROBE_N) == 0) {
                for (i = 0; i < 2048; i++) {
                    two[i] = ag_amp_tick(a, probe[i]);
                }
            }
            /*
             * Not bit-identical: the linear stage still carries its own coupling
             * capacitor and cathode shelf, which are real filters.  What has to
             * hold is that it did not distort - the two differ by a filter, not
             * by a nonlinearity, so the difference is small and smooth.
             */
            {
                double d = 0.0, r = 0.0;
                for (i = 512; i < 2048; i++) {
                    d += (double)(two[i] - one[i]) * (two[i] - one[i]);
                    r += (double)one[i] * one[i];
                }
                AG_CHECK(r > 1e-20);
                AG_CHECK(10.0 * log10(d / (r + 1e-30) + 1e-30) < 0.0);
            }
        }
        free(one);
        free(two);
    }

    free(k);
    free(tab);
    free(a);
    free(probe);
}

/* ------------------------------------------------------------------------ */
/* Presets                                                                   */
/* ------------------------------------------------------------------------ */

/*
 * A preset has to reproduce the chain that saved it, sample for sample, without
 * a solver anywhere in the loading.  That is the whole claim, and it is the kind
 * of claim that fails quietly: a field left out of the header shows up as a
 * slightly different filter, which sounds like a different amplifier rather than
 * like a bug.  So: bake one chain, save it, load it into a second, and require
 * the two to agree bit for bit on a real signal.
 */
static void test_preset(void)
{
    ag_ckt_t    *k = (ag_ckt_t *)malloc(sizeof(ag_ckt_t));
    float       *tabA = (float *)malloc(sizeof(float) * AG_AMP_TAB_FLOATS);
    float       *tabB = (float *)malloc(sizeof(float) * AG_AMP_TAB_FLOATS);
    ag_amp_t    *a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    ag_amp_t    *b = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    float       *probe = (float *)malloc(sizeof(float) * PROBE_N);
    uint8_t     *blob = NULL;
    ag_amp_cfg_t cfg;
    uint32_t     size, wrote;
    int          i, diff = 0;

    if (k == 0 || tabA == 0 || tabB == 0 || a == 0 || b == 0 || probe == 0) {
        AG_CHECK(0);
        goto done;
    }
    ag_amp_probe_pluck(probe, PROBE_N, TFS);
    ag_amp_defaults(&cfg, TFS);
    cfg.os = 2;
    AG_CHECK_INT(ag_amp_build(a, k, &cfg, tabA, 0, probe, PROBE_N), 0);

    size = ag_amp_preset_size(a->n, a->tab_n);
    AG_CHECK(size > 0);
    blob = (uint8_t *)malloc(size);
    if (blob == 0) {
        AG_CHECK(0);
        goto done;
    }
    /* One byte short must fail rather than write past the end. */
    AG_CHECK_INT(ag_amp_preset_save(a, blob, size - 1), 0);
    wrote = ag_amp_preset_save(a, blob, size);
    AG_CHECK_INT(wrote, size);

    AG_CHECK_INT(ag_amp_preset_load(b, blob, size, tabB, TFS), 0);
    AG_CHECK_INT(b->n, a->n);
    AG_CHECK(b->latency_samples == a->latency_samples);
    AG_CHECK(b->couple_hz[0] == a->couple_hz[0]);
    AG_CHECK(b->shelf_db[1] == a->shelf_db[1]);

    ag_amp_reset(a);
    ag_amp_reset(b);
    for (i = 0; i < PROBE_N; i++) {
        if (ag_amp_tick(a, probe[i]) != ag_amp_tick(b, probe[i])) {
            diff++;
        }
    }
    AG_CHECK_INT(diff, 0);

    /* Rubbish in the header is refused, not trusted. */
    ((uint32_t *)blob)[0] = 0xdeadbeefu;
    AG_CHECK(ag_amp_preset_load(b, blob, size, tabB, TFS) != 0);

done:
    free(blob);
    free(probe);
    free(b);
    free(a);
    free(tabB);
    free(tabA);
    free(k);
}

/* ------------------------------------------------------------------------ */
/* Blending a valve curve with a diode one                                   */
/* ------------------------------------------------------------------------ */

/*
 * A valve gives even harmonics and an asymmetry; a diode pair gives odd ones and
 * near-symmetry.  Blending the two curves has to walk between them, and it has
 * to stay exact while doing it: the antiderivative must remain the integral of
 * the blended curve or the antialiasing average quietly stops working.
 */
static void test_blend(void)
{
    ag_ckt_t      *k = (ag_ckt_t *)malloc(sizeof(ag_ckt_t));
    float         *ta = (float *)malloc(sizeof(float) * 2048 * 3);
    float         *tb2 = (float *)malloc(sizeof(float) * 2048 * 3);
    float         *tm = (float *)malloc(sizeof(float) * 2048 * 3);
    ag_tube_spec_t sp;
    ag_tube_t      va, di, mx;
    int            i;

    if (k == 0 || ta == 0 || tb2 == 0 || tm == 0) {
        AG_CHECK(0);
        goto done;
    }
    ag_tube_spec_jcm800(&sp, 0);
    ag_tube_init(&va);
    AG_CHECK_INT(ag_tube_bake(&va, k, &sp, ta, ta + 2048, ta + 4096, 2048, 1.0f,
                              0.0f, 0.01f),
                 0);
    AG_CHECK_INT(ag_tube_bake_clipper(&di, k, 10.0e3f, 0.35f, va.gain_bypassed,
                                      tb2, tb2 + 2048, tb2 + 4096, 2048, va.lo,
                                      va.hi),
                 0);

    /* Both are zero at zero and both invert with the same small-signal slope,
     * so the knob is a tone control and not a volume control. */
    AG_CHECK(ag_tube_curve(&di, 0.0f) == 0.0f);
    {
        const float gv = (ag_tube_curve(&va, 0.002f) - ag_tube_curve(&va, -0.002f));
        const float gd = (ag_tube_curve(&di, 0.002f) - ag_tube_curve(&di, -0.002f));
        AG_CHECK(gv < 0.0f && gd < 0.0f);
        AG_CHECK(fabsf(gd - gv) < 0.15f * fabsf(gv));
    }
    /* The diode curve is the symmetric one: that is the whole point of having
     * it, and the valve's own asymmetry is checked elsewhere. */
    AG_CHECK(fabsf(fabsf(ag_tube_curve(&di, 2.0f)) -
                   fabsf(ag_tube_curve(&di, -2.0f))) <
             0.10f * fabsf(ag_tube_curve(&di, 2.0f)));

    /* The ends are what was asked for, and the middle is between them. */
    mx = va;
    ag_tube_blend(&mx, &va, &di, 0.0f, tm, tm + 2048, tm + 4096);
    for (i = 0; i < 2048; i++) {
        AG_CHECK(tm[i] == va.t[i]);
    }
    ag_tube_blend(&mx, &va, &di, 1.0f, tm, tm + 2048, tm + 4096);
    for (i = 0; i < 2048; i++) {
        AG_CHECK(tm[i] == di.t[i]);
    }
    ag_tube_blend(&mx, &va, &di, 0.5f, tm, tm + 2048, tm + 4096);
    {
        int between = 0;
        for (i = 0; i < 2048; i++) {
            const float lo = va.t[i] < di.t[i] ? va.t[i] : di.t[i];
            const float hi = va.t[i] < di.t[i] ? di.t[i] : va.t[i];
            if (tm[i] >= lo - 1e-3f && tm[i] <= hi + 1e-3f) {
                between++;
            }
        }
        AG_CHECK_INT(between, 2048);
    }
    /*
     * And the blended antiderivative is still the integral of the blended
     * curve.  If it were not, the antialiasing average would be integrating one
     * function and dividing by another - which is inaudible on a tone and
     * dreadful on a decay.
     */
    {
        float worst = 0.0f;
        for (i = 1; i < 2048; i++) {
            const float step = (tm[i - 1] + tm[i]) * 0.5f;
            const float got = tm[2048 + i] - tm[2048 + i - 1];
            const float d = got > step ? got - step : step - got;
            if (d > worst) {
                worst = d;
            }
        }
        /*
         * A tenth of a volt, against steps of about a hundred.  Not tighter:
         * the antiderivative runs to 1e5 in index units, where a float ulp is
         * already 0.008, so a stricter threshold would be measuring the
         * subtraction rather than the identity.
         */
        AG_CHECK(worst < 0.1f);
    }
    /* A diode has no grid, so blocking fades out with the mix. */
    AG_CHECK(tm[2 * 2048 + 2047] < va.g[2047]);

done:
    free(tm);
    free(tb2);
    free(ta);
    free(k);
}

/* Blending inside a live chain: the ends are the curves, and re-blending
 * afterwards needs no solver. */
static void test_blend_in_chain(void)
{
    ag_ckt_t    *k = (ag_ckt_t *)malloc(sizeof(ag_ckt_t));
    float       *tab = (float *)malloc(sizeof(float) * AG_AMP_TAB_FLOATS);
    float       *work = (float *)malloc(sizeof(float) *
                                  AG_AMP_BLEND_FLOATS(AG_AMP_TAB_N));
    ag_amp_t    *a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    float       *probe = (float *)malloc(sizeof(float) * PROBE_N);
    ag_amp_cfg_t cfg;
    double       e[3];
    int          i, m;

    if (k == 0 || tab == 0 || work == 0 || a == 0 || probe == 0) {
        AG_CHECK(0);
        goto done;
    }
    ag_amp_probe_pluck(probe, PROBE_N, TFS);
    ag_amp_defaults(&cfg, TFS);
    cfg.os = 1;
    cfg.adaa = 0;
    AG_CHECK_INT(ag_amp_build(a, k, &cfg, tab, 0, probe, PROBE_N), 0);

    for (m = 0; m < 3; m++) {
        double acc = 0.0;
        /* The solver only on the first call; after that it is a mix. */
        AG_CHECK_INT(
            ag_amp_blend_stage(a, m == 0 ? k : NULL, 1, 0.5f * (float)m, work),
            0);
        ag_amp_reset(a);
        for (i = 0; i < 256; i++) {
            AG_CHECK(ag_amp_tick(a, 0.0f) == 0.0f);
        }
        ag_amp_reset(a);
        for (i = 0; i < PROBE_N; i++) {
            const float y = ag_amp_tick(a, probe[i]);
            acc += (double)y * (double)y;
        }
        e[m] = acc;
        AG_CHECK(e[m] > 0.0);
    }
    /* All three differ - the knob does something - and none of them blew up. */
    AG_CHECK(e[0] != e[1] && e[1] != e[2]);
    AG_CHECK(a->peak_out < 100.0f);

done:
    free(probe);
    free(a);
    free(work);
    free(tab);
    free(k);
}

/*
 * A knob turned under a playing signal must not clear the filters.
 *
 * This is what separates ag_amp_set_knobs from ag_amp_set_voicing, and it is not
 * cosmetic: eight biquads and three halfbands zeroed under a ringing note is a
 * click and a dropout on every keypress, which is the difference between a knob
 * somebody can sweep and one they can only step.  So the test is the literal
 * claim - the samples that come out after the call are the samples that would
 * have come out without it.
 */
static void test_knobs_keep_state(void)
{
    ag_ckt_t    *k = (ag_ckt_t *)malloc(sizeof(ag_ckt_t));
    float       *tab = (float *)malloc(sizeof(float) * AG_AMP_TAB_FLOATS);
    ag_amp_t    *a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    float       *probe = (float *)malloc(sizeof(float) * PROBE_N);
    ag_amp_cfg_t cfg;
    float        ref[16], got[16];
    int          i, pass;

    if (k == 0 || tab == 0 || a == 0 || probe == 0) {
        AG_CHECK(0);
        goto done;
    }
    ag_amp_probe_pluck(probe, PROBE_N, TFS);
    ag_amp_defaults(&cfg, TFS);
    AG_CHECK_INT(ag_amp_build(a, k, &cfg, tab, 0, probe, PROBE_N), 0);

    /* Three runs of the same thousand samples, differing only in what is called
     * at the thousandth: nothing, the knob path, the rebuild path. */
    for (pass = 0; pass < 3; pass++) {
        ag_amp_reset(a);
        for (i = 0; i < 1000; i++) {
            (void)ag_amp_tick(a, probe[i]);
        }
        if (pass == 1) {
            AG_CHECK_INT(ag_amp_set_knobs(a, &cfg), 0);
        } else if (pass == 2) {
            AG_CHECK_INT(ag_amp_set_voicing(a, &cfg), 0);
        }
        for (i = 0; i < 16; i++) {
            const float y = ag_amp_tick(a, probe[1000 + i]);
            if (pass == 0) {
                ref[i] = y;
            } else {
                got[i] = y;
            }
        }
        if (pass == 1) {
            /* Bit for bit: an unchanged config through the knob path is not
             * approximately a no-op, it is a no-op. */
            for (i = 0; i < 16; i++) {
                AG_CHECK(got[i] == ref[i]);
            }
        } else if (pass == 2) {
            /* And the reset really is one, or the check above proves nothing. */
            int same = 1;
            for (i = 0; i < 16; i++) {
                if (got[i] != ref[i]) {
                    same = 0;
                }
            }
            AG_CHECK(!same);
        }
    }

    /* The knob still moves what it is supposed to move. */
    cfg.drive = 0.77f;
    AG_CHECK_INT(ag_amp_set_knobs(a, &cfg), 0);
    AG_CHECK(a->gain[0] == 0.77f);

    /*
     * And it refuses the one setting whose state cannot survive.  The halfbands
     * hold history at the old rate and the blocking capacitor's constants are
     * per sample, so a change of os through this path would be silently wrong,
     * which is worse than a return code.
     */
    {
        ag_amp_cfg_t bad = cfg;
        const int    was = a->cfg.os;
        bad.os = a->cfg.os == 4 ? 2 : 4;
        AG_CHECK_INT(ag_amp_set_knobs(a, &bad), -1);
        AG_CHECK_INT(a->cfg.os, was);
    }

done:
    free(probe);
    free(a);
    free(tab);
    free(k);
}

/* ------------------------------------------------------------------------ */
/* The voicing banks, which is what a model mostly is                        */
/* ------------------------------------------------------------------------ */

/*
 * Small-signal gain of the whole chain at one frequency, in dB.
 *
 * Small enough that nothing clips and no grid conducts - two milliradians at the
 * first grid - so the chain is linear and a ratio of two runs is a filter
 * response.  A second of settling first, because the input coupling capacitor is
 * a hundred millisecond time constant and its startup transient swamps a small
 * signal for the first fifth of it (the note in test_amp_chain).
 */
static double amp_gain_db(ag_amp_t *a, float hz)
{
    const int settle = (int)TFS;
    const int meas = (int)(TFS / hz) * 8;
    double    acc = 0.0;
    int       i;

    ag_amp_reset(a);
    for (i = 0; i < settle + meas; i++) {
        const double s =
            sin(2.0 * 3.14159265358979 * (double)hz * (double)i / (double)TFS);
        const float y = ag_amp_tick(a, (float)(0.002 * s));
        if (i >= settle) {
            acc += (double)y * (double)y;
        }
    }
    return 10.0 * log10(acc / (double)meas + 1e-30);
}

/*
 * Both voicing banks do something.
 *
 * This is a regression test for a failure that made no sound at all.  Both
 * chains were initialised, reset and ticked, and nothing ever pushed a section
 * into either of them - so every band in cfg.voice and cfg.tone was ignored, and
 * an empty biquad chain is a unity gain, which means the amplifier still worked
 * and simply had no voicing.  It went unnoticed because the numbers in
 * ag_amp_model looked right where they were read: what was missing was the code
 * that turned them into filters.  Measured at the time: a render with the whole
 * post bank zeroed was bit-identical in every third-octave band to one with the
 * fitted bank, and `fit` scored the stored answer at 7.69 dB rms where the fit
 * that produced it had recorded 1.29.
 *
 * So the check is a ratio of two runs of the same chain, one band apart, on a
 * signal small enough that the chain is linear.  A peaking section at Q 1 is
 * exactly its own dB at its centre, so the ratio is the band.
 */
static void test_voicing_banks(void)
{
    ag_ckt_t    *k = (ag_ckt_t *)malloc(sizeof(ag_ckt_t));
    float       *tab = (float *)malloc(sizeof(float) * AG_AMP_TAB_FLOATS);
    ag_amp_t    *a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    float       *probe = (float *)malloc(sizeof(float) * PROBE_N);
    ag_amp_cfg_t cfg;
    double       flat_400, flat_3150;
    int          b;

    if (k == 0 || tab == 0 || a == 0 || probe == 0) {
        AG_CHECK(0);
        free(k);
        free(tab);
        free(a);
        free(probe);
        return;
    }
    ag_amp_probe_pluck(probe, PROBE_N, TFS);

    /* Both banks off, as the reference.  os 1 and no antialiasing: this is a
     * measurement of filters, and the oversampler is 34 samples of latency that
     * the ratio does not need. */
    ag_amp_defaults(&cfg, TFS);
    cfg.os = 1;
    cfg.adaa = 0;
    amp_no_voicing(&cfg);
    cfg.mid_db = 0.0f;
    if (ag_amp_build(a, k, &cfg, tab, 0, probe, PROBE_N) != 0) {
        AG_CHECK(0);
        goto done;
    }
    /* Nothing in either bank, so nothing in either chain. */
    AG_CHECK_INT(a->voice[0].n, 0);
    AG_CHECK_INT(a->tone.n, 0);
    flat_400 = amp_gain_db(a, 400.0f);
    flat_3150 = amp_gain_db(a, 3150.0f);

    /* One band in the pre bank: 400 Hz, +6 dB, and it has to arrive as +6 dB. */
    cfg.voice[0][2].hz = 400.0f;
    cfg.voice[0][2].db = 6.0f;
    cfg.voice[0][2].q = 1.0f;
    if (ag_amp_build(a, k, &cfg, tab, 0, probe, PROBE_N) != 0) {
        AG_CHECK(0);
        goto done;
    }
    AG_CHECK_INT(a->voice[0].n, 1);
    AG_CHECK(fabs(amp_gain_db(a, 400.0f) - flat_400 - 6.0) < 0.5);
    /* And it is a band and not a level: 3150 Hz must not have moved. */
    AG_CHECK(fabs(amp_gain_db(a, 3150.0f) - flat_3150) < 0.5);
    cfg.voice[0][2].db = 0.0f;

    /* The same for the tone stack, which is the bank the fitted presence lives
     * in and the one whose absence was measured as no difference at all. */
    cfg.tone[5].hz = 3150.0f;
    cfg.tone[5].db = 9.0f;
    cfg.tone[5].q = 1.0f;
    cfg.tone_shelf = 0;
    if (ag_amp_build(a, k, &cfg, tab, 0, probe, PROBE_N) != 0) {
        AG_CHECK(0);
        goto done;
    }
    AG_CHECK_INT(a->tone.n, 1);
    AG_CHECK(fabs(amp_gain_db(a, 3150.0f) - flat_3150 - 9.0) < 0.5);
    AG_CHECK(fabs(amp_gain_db(a, 400.0f) - flat_400) < 0.5);

    /*
     * And tone_shelf turns that band into a shelf rather than a peak.
     *
     * The shape is checked rather than the type, because the point of the shelf
     * is what its poles do and there is no way to ask a section what it is: a
     * first-order high shelf puts its zero and pole symmetrically about f0, so at
     * f0 it is half of its own dB, while a peaking section is all of it.  Half of
     * nine decibels is four and a half, which is four and a half away from nine -
     * far outside anything a tolerance has to argue about.
     */
    cfg.tone_shelf = 1;
    if (ag_amp_build(a, k, &cfg, tab, 0, probe, PROBE_N) != 0) {
        AG_CHECK(0);
        goto done;
    }
    AG_CHECK_INT(a->tone.n, 1);
    AG_CHECK(fabs(amp_gain_db(a, 3150.0f) - flat_3150 - 4.5) < 0.6);

done:
    free(probe);
    free(a);
    free(tab);
    free(k);
}

/* ------------------------------------------------------------------------ */
/* The models                                                               */
/* ------------------------------------------------------------------------ */

/*
 * Every model builds, and each one's filters landed where its own component
 * values put them.
 *
 * The corners are the check because they are the one thing that cannot be right
 * by accident: each is a capacitor against a resistance, both of which live in
 * ag_tube_spec_*, and a stage wired up with the wrong spec would come out with
 * the wrong corner rather than with no sound.  That is exactly how a chain would
 * fail if ag_amp_spec ever handed stage 2 the netlist for stage 1.
 */
static void test_models(void)
{
    ag_ckt_t    *k = (ag_ckt_t *)malloc(sizeof(ag_ckt_t));
    float       *tab = (float *)malloc(sizeof(float) * AG_AMP_TAB_FLOATS);
    ag_amp_t    *a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    float       *probe = (float *)malloc(sizeof(float) * PROBE_N);
    ag_amp_cfg_t cfg;
    int          m, i;

    if (k == 0 || tab == 0 || a == 0 || probe == 0) {
        AG_CHECK(0);
        free(k);
        free(tab);
        free(a);
        free(probe);
        return;
    }
    ag_amp_probe_pluck(probe, PROBE_N, TFS);

    /* The names round-trip, and nothing else answers to them. */
    for (m = 0; m < AG_AMP_MODEL_N; m++) {
        AG_CHECK_INT(ag_amp_model_by_name(ag_amp_model_name(m)), m);
    }
    AG_CHECK_INT(ag_amp_model_by_name("jcm"), -1);
    AG_CHECK_INT(ag_amp_model_by_name("jcm8000"), -1);
    AG_CHECK_INT(ag_amp_model_by_name(""), -1);
    AG_CHECK_INT(ag_amp_model_by_name(0), -1);
    AG_CHECK_STR(ag_amp_model_name(AG_AMP_MODEL_N), "?");
    /*
     * Every model has been fitted against a capture of the amplifier it is named
     * for, and the tools print that beside the name.  Checked one id at a time
     * rather than in a loop, because the point of the flag is that it is a fact
     * per model: the next one added starts unfitted and has to say so.
     */
    AG_CHECK_INT(ag_amp_model_fitted(AG_AMP_MODEL_JCM800), 1);
    AG_CHECK_INT(ag_amp_model_fitted(AG_AMP_MODEL_BOGNER), 1);
    AG_CHECK_INT(ag_amp_model_fitted(AG_AMP_MODEL_SLO), 1);
    AG_CHECK_INT(ag_amp_model_fitted(AG_AMP_MODEL_N), 0);
    AG_CHECK_INT(ag_amp_model_fitted(-1), 0);
    /* An id out of range is the JCM800 rather than a broken config. */
    ag_amp_model(&cfg, 99, TFS);
    AG_CHECK_INT(cfg.model, AG_AMP_MODEL_JCM800);

    for (m = 0; m < AG_AMP_MODEL_N; m++) {
        ag_amp_model(&cfg, m, TFS);
        cfg.os = 1;
        cfg.adaa = 0;
        AG_CHECK_INT(cfg.model, m);
        AG_CHECK_INT(ag_amp_build(a, k, &cfg, tab, 0, probe, PROBE_N), 0);
        AG_CHECK_INT(a->n, cfg.n_stages);
        /* Nowhere did Newton give up. */
        for (i = 0; i < a->n; i++) {
            AG_CHECK_INT(a->tube[i].bake_noncvg, 0);
        }
        /* Silence in, silence out - the operating point is subtracted exactly. */
        ag_amp_reset(a);
        for (i = 0; i < 256; i++) {
            AG_CHECK(ag_amp_tick(a, 0.0f) == 0.0f);
        }
        /* And it distorts: a big input has to come back with the peak
         * compressed relative to a small one. */
        {
            double small_peak = 0.0, big_peak = 0.0;
            int    j;
            for (j = 0; j < 2; j++) {
                const float g = j == 0 ? 0.02f : 1.0f;
                double     *peak = j == 0 ? &small_peak : &big_peak;
                ag_amp_reset(a);
                for (i = 0; i < PROBE_N; i++) {
                    const float y = ag_amp_tick(a, probe[i] * g);
                    const double a2 = y < 0.0f ? -(double)y : (double)y;
                    if (a2 > *peak) {
                        *peak = a2;
                    }
                }
            }
            AG_CHECK(small_peak > 0.0);
            AG_CHECK(big_peak < small_peak * 50.0); /* 50x in, less than 50x out */
        }
    }

    /*
     * The crunch model, which is a Shiva's crunch channel: a 2203 with three gain
     * stages, the second one hot, and everything else at the 2203's own values.
     *
     * The three plate loads are the check that it is that and not something else -
     * 100 k, 220 k, 100 k, with the hot valve in the middle - and the corners are
     * the 2203's: 1.6 Hz at the input, 142 Hz between the stages from the bright
     * cap, 285 Hz of cathode shelf from 0.68 uF across 820 R.
     */
    ag_amp_model(&cfg, AG_AMP_MODEL_BOGNER, TFS);
    cfg.os = 1;
    cfg.adaa = 0;
    AG_CHECK_INT(ag_amp_build(a, k, &cfg, tab, 0, probe, PROBE_N), 0);
    AG_CHECK_INT(a->n, 3);
    AG_CHECK(a->spec[0].rplate == 100.0e3f);
    AG_CHECK(a->spec[1].rplate == 220.0e3f);
    AG_CHECK(a->spec[2].rplate == 100.0e3f);
    AG_CHECK(a->spec[1].rcath == 820.0f);
    AG_CHECK(fabsf(a->couple_hz[0] - 1.58f) < 0.1f);  /* 100 nF into 1.01 M */
    AG_CHECK(fabsf(a->couple_hz[1] - 142.0f) < 3.0f); /* 2.2 nF into 508 k */
    AG_CHECK(fabsf(a->couple_hz[2] - 139.6f) < 3.0f); /* 2.2 nF into 518 k */
    AG_CHECK(fabsf(a->shelf_hz[0] - 86.7f) < 1.0f);   /* 0.68 uF over 2k7 */
    AG_CHECK(fabsf(a->shelf_hz[1] - 285.0f) < 3.0f);  /* and over 820 R */
    AG_CHECK(a->shelf_db[0] < -6.0f); /* 2k7 unbypassed is a lot of gain lost */
    AG_CHECK(fabsf(a->load_hz - 7.23f) < 0.2f);
    /* The middle valve is the hot one: 220 k gives it more gain than either of
     * the stock stages around it. */
    AG_CHECK(fabsf(a->tube[1].gain_bypassed) > fabsf(a->tube[0].gain_bypassed));
    AG_CHECK(fabsf(a->tube[1].gain_bypassed) > fabsf(a->tube[2].gain_bypassed));
    /* And the gain pot at noon, like every other pot in this tree. */
    AG_CHECK(a->gain[1] == 0.5f);

    /*
     * The SLO: four stages, the third one cold, and every attenuator between them
     * a resistor rather than a constant.
     *
     * The plate loads are the check that the right netlist was used at all -
     * 220 k, 100 k, 100 k, 220 k is the published pattern, and a chain that had
     * been given the same stage four times would come out with four of the same
     * number.  The couplings are all 22 nF and the corner is set by what each
     * stage is driven from, which is how the dividers show up in a filter.
     */
    ag_amp_model(&cfg, AG_AMP_MODEL_SLO, TFS);
    cfg.os = 1;
    cfg.adaa = 0;
    AG_CHECK_INT(ag_amp_build(a, k, &cfg, tab, 0, probe, PROBE_N), 0);
    AG_CHECK_INT(a->n, 4);
    AG_CHECK(a->spec[0].rplate == 220.0e3f);
    AG_CHECK(a->spec[1].rplate == 100.0e3f);
    AG_CHECK(a->spec[2].rplate == 100.0e3f);
    AG_CHECK(a->spec[3].rplate == 220.0e3f);
    AG_CHECK(fabsf(a->couple_hz[0] - 7.2f) < 0.2f); /* 22 nF into 1.01 M */
    /*
     * And the three hot stages, **four, two and four times the schematic's
     * corner**, because `cfg.couple_mul` puts a matching bass cut in front of each
     * of them: 7.1 Hz becomes 28, 19.6 becomes 39, 7.0 becomes 28.
     *
     * The schematic's own values are kept in the arithmetic below so that the
     * change stays visible: 22 nF into 518 k of series resistance plus the 500 k
     * pot, and 22 nF into 38 k + 330 k in front of the cold clipper.  Why they are
     * multiplied at all is the 49 Hz intermodulation product measured against the
     * 5150 capture - the comment in ag_amp.c has the numbers - and this check
     * failing was the right thing to happen when that went in.
     *
     * **78 Hz is the number to defend here, and 157 was not.**  The multiplier was
     * 8, which put this corner above the 82 Hz open E: the stage then cut the
     * fundamental of a low note and passed its second harmonic, and `harm` measured
     * thirty decibels of asymmetry that no valve had made.  The rule the value is
     * now chosen by is that no coupling corner sits above the lowest note on the
     * instrument, so a bass cut stays a bass cut.
     */
    AG_CHECK(fabsf(a->couple_hz[1] - 4.0f * 7.1f) < 4.0f * 0.3f);
    AG_CHECK(fabsf(a->couple_hz[2] - 2.0f * 19.6f) < 2.0f * 0.5f);
    /*
     * And the rule itself, on every stage, because it is the rule and not the
     * factors that matters: **an octave below the lowest note on the instrument**.
     * Merely under 82 Hz is not enough - a first-order corner at 57 Hz still takes
     * 2 dB off an 82 Hz fundamental and a quarter of that off its second harmonic,
     * and three of those stack into asymmetry no valve made.  41 Hz is the line.
     */
    {
        int st;
        for (st = 0; st < a->n; st++) {
            AG_CHECK(a->couple_hz[st] < 41.0f);
        }
    }
    /* And the multiplier is where the model says it is, first stage excluded. */
    AG_CHECK(cfg.couple_mul[0] == 1.0f);
    AG_CHECK(cfg.couple_mul[1] == 4.0f);
    AG_CHECK(cfg.couple_mul[2] == 2.0f);
    AG_CHECK(cfg.couple_mul[3] == 4.0f);
    /*
     * No cathode shelf on the third stage, and that is the whole point of it:
     * with no bypass capacitor there is no shelf to design, because the cathode
     * follows the signal at every frequency instead of at some of them.  39 k of
     * it, which is what both sources for this amplifier say.
     */
    AG_CHECK(a->spec[2].rcath == 39.0e3f);
    AG_CHECK(a->shelf_hz[2] == 0.0f);
    AG_CHECK(a->shelf_db[2] == 0.0f);
    /* Every other stage has the Soldano cathode: 1k8 with a microfarad across it,
     * which corners at 88 Hz. */
    AG_CHECK(fabsf(a->shelf_hz[0] - 88.4f) < 1.0f);
    AG_CHECK(fabsf(a->shelf_hz[3] - 88.4f) < 1.0f);
    /*
     * And the one constant that is left is the gain pot at noon.  Nothing else
     * between the stages is a multiplier any more - if this ever reads as
     * something other than a half, a divider has gone back into a constant.
     */
    AG_CHECK(a->gain[1] == 0.5f);
    AG_CHECK(a->gain[2] == 1.0f);
    AG_CHECK(a->gain[3] == 1.0f);

    free(probe);
    free(a);
    free(tab);
    free(k);
}

/* ------------------------------------------------------------------------ */
/* The cold clipper                                                          */
/* ------------------------------------------------------------------------ */

/*
 * An unbypassed stage is baked with its cathode resistor in circuit, and its
 * axis reaches where its curve does.
 *
 * Two failures, one of which was silent and the other nearly so.
 *
 * The bake sweeps the curve with the cathode held at its bias, because the
 * difference between held and free is a first-order shelf that design() puts in
 * front of the table.  With no bypass capacitor there is no shelf - so the same
 * sweep would have handed back a stage with several times its real gain and a
 * knee in the wrong place, looking exactly like a working valve.  What this
 * checks is the slope of the table that came out: it has to be the open-cathode
 * gain, not the bypassed one, and for a 22 k cathode those differ by seven
 * times.
 *
 * The axis was worse.  fit_range locates saturation by sweeping the grid to
 * +4000 V, and on an unbypassed stage that is far outside anything the valve
 * model was fitted over: Newton gives up, the value that comes back is the
 * cut-off plate voltage, the apparent swing is negative, and the fit falls
 * through to the +-5 V it keeps for a curve with no slope at all.  A cold
 * clipper baked over +-5 V stops a tenth of the way up its own linear region and
 * clamps both halves of the wave instead of one, which measured as a fifth of
 * all lookups off the end of the table and nothing else.
 */
static void test_cold_clipper(void)
{
    ag_ckt_t      *k = (ag_ckt_t *)malloc(sizeof(ag_ckt_t));
    float         *tab = (float *)malloc(sizeof(float) * 3 * 2048);
    ag_tube_spec_t sp;
    ag_tube_t      tb;
    float          slope;

    if (k == 0 || tab == 0) {
        AG_CHECK(0);
        free(k);
        free(tab);
        return;
    }
    ag_tube_spec_slo(&sp, 2);
    AG_CHECK(sp.ccath == 0.0f);
    AG_CHECK(sp.rcath == 39.0e3f);

    ag_tube_init(&tb);
    AG_CHECK_INT(ag_tube_bake(&tb, k, &sp, tab, tab + 2048, tab + 2 * 2048, 2048,
                              1.0f, 0.0f, 0.01f),
                 0);
    AG_CHECK_INT(tb.bake_noncvg, 0);

    /* The table is the open-cathode curve, so its slope at the bias is the
     * open-cathode gain and nothing like the bypassed one. */
    slope = (ag_tube_curve(&tb, 0.005f) - ag_tube_curve(&tb, -0.005f)) / 0.01f;
    AG_CHECK(fabsf(slope - tb.gain_unbypassed) < 0.15f * fabsf(tb.gain_unbypassed));
    AG_CHECK(fabsf(tb.gain_bypassed) > 3.0f * fabsf(tb.gain_unbypassed));

    /*
     * The axis covers the linear side.  Where the curve goes is measured in
     * apps/common/tube/README.md: cut-off arrives about five volts under the bias
     * and the conducting side stays linear for tens of volts, so an axis that
     * stopped at +5 V would be the bug above and one that reaches past +30 is the
     * curve.
     */
    AG_CHECK(tb.hi > 30.0f);
    AG_CHECK(tb.lo < -4.0f && tb.lo > -12.0f);

    /* And it is a cold clipper: flat on the cut-off side well before the axis
     * ends, still moving on the other.  That asymmetry is the stage's entire
     * reason for existing. */
    AG_CHECK(fabsf(ag_tube_curve(&tb, -12.0f) - ag_tube_curve(&tb, -20.0f)) < 0.1f);
    AG_CHECK(fabsf(ag_tube_curve(&tb, 20.0f) - ag_tube_curve(&tb, 12.0f)) > 10.0f);

    free(tab);
    free(k);
}

/* ------------------------------------------------------------------------ */
/* Which model has the most gain                                             */
/* ------------------------------------------------------------------------ */

/*
 * Three valves have to mean more gain than two, and this is the test that says
 * so.
 *
 * It exists because the first version of `slo` did not.  Its front end was
 * copied from the crunch model - stock 100 k plates and that model's 0.55 volume
 * between the stages - and the divider in front of its cold clipper had been
 * swept against the hump depth of a NAM *Marshall* capture, a metric that rewards
 * distorting less.  The result measured **19 dB less gain than the two-valve
 * JCM800 and 1.4 dB less compression**: a three-stage lead channel that was the
 * cleanest of the three models.  Nothing in the tree noticed, because every check
 * asked whether a chain worked and none asked which chain was hotter.
 *
 * So the three properties, all on the same probe at the same drive with the
 * voicing switched off, because this is about valves and not about equalisers:
 *
 *   1. small-signal gain through the stages: slo > jcm800 > bogner
 *   2. compression over a 20 dB input range, same order
 *   3. energy above 2 kHz, where the probe has none of its own: slo highest
 *
 * Ratios rather than absolute values, with room around them - the point is the
 * order, and a model whose voicing is refitted may move a decibel.
 */
static void model_gain_probe(ag_amp_t *a, ag_ckt_t *k, float *tab,
                             const float *probe, int model, double *chain_gain,
                             double *compression, double *hash_db)
{
    ag_amp_cfg_t cfg;
    double       loud = 0.0, quiet = 0.0, all = 0.0, high = 0.0, lp = 0.0;
    /* One pole at 2 kHz, which is all it takes to separate what the amplifier
     * made from what the probe brought: the probe's own harmonics stop at
     * 1.3 kHz. */
    const double kf = exp(-2.0 * 3.14159265358979 * 2000.0 / (double)TFS);
    int          i;

    *chain_gain = 0.0;
    *compression = 0.0;
    *hash_db = 0.0;

    ag_amp_model(&cfg, model, TFS);
    amp_no_voicing(&cfg);
    cfg.mid_db = 0.0f;
    cfg.master = 1.0f; /* volts at the last plate: comparable between models */
    if (ag_amp_build(a, k, &cfg, tab, 0, probe, PROBE_N) != 0) {
        AG_CHECK(0);
        return;
    }
    *chain_gain = 1.0;
    for (i = 0; i < a->n; i++) {
        /* The slope of the curve this stage actually carries: an unbypassed
         * stage was swept with its cathode resistor in circuit. */
        const float g = a->spec[i].ccath > 0.0f ? a->tube[i].gain_bypassed
                                                : a->tube[i].gain_unbypassed;
        *chain_gain *= (double)a->gain[i] * fabs((double)g);
    }

    ag_amp_reset(a);
    for (i = 0; i < PROBE_N; i++) {
        const double y = (double)ag_amp_tick(a, probe[i]);
        double       h;
        lp = (1.0 - kf) * y + kf * lp;
        h = y - lp;
        loud += y * y;
        all += y * y;
        high += h * h;
    }
    ag_amp_reset(a);
    for (i = 0; i < PROBE_N; i++) {
        const double y = (double)ag_amp_tick(a, probe[i] * 0.1f);
        quiet += y * y;
    }
    /* 20 dB less in; how much less came out.  The shortfall is the compression. */
    *compression = 20.0 - 10.0 * log10(loud / (quiet + 1e-300) + 1e-300);
    *hash_db = 10.0 * log10(high / (all + 1e-300) + 1e-300);
}

static void test_model_gain_order(void)
{
    ag_ckt_t *k = (ag_ckt_t *)malloc(sizeof(ag_ckt_t));
    float    *tab = (float *)malloc(sizeof(float) * AG_AMP_TAB_FLOATS);
    ag_amp_t *a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    float    *probe = (float *)malloc(sizeof(float) * PROBE_N);
    double    gain[AG_AMP_MODEL_N], comp[AG_AMP_MODEL_N], hash[AG_AMP_MODEL_N];
    int       m;

    if (k == 0 || tab == 0 || a == 0 || probe == 0) {
        AG_CHECK(0);
        free(k);
        free(tab);
        free(a);
        free(probe);
        return;
    }
    ag_amp_probe_pluck(probe, PROBE_N, TFS);

    for (m = 0; m < AG_AMP_MODEL_N; m++) {
        model_gain_probe(a, k, tab, probe, m, &gain[m], &comp[m], &hash[m]);
    }

    /*
     * The order, and it is not the one this test was written with.
     *
     * It used to say jcm800 > bogner, on the reasoning that a hot-rodded Marshall
     * beats a crunch channel.  Then the crunch model was rebuilt from what a Shiva
     * is documented to be - three gain stages and a cathode follower - and came out
     * hotter than the two-valve Marshall by 26 dB.  The captures say the same
     * thing: 20 dB less into each of them comes back 16.1 dB, 18.2 dB and 18.9 dB
     * short respectively, so **both of the three-and-four-stage amplifiers are more
     * compressed than the Marshall and by about the same amount as each other**.
     * The ordering here follows the amplifiers rather than the intuition.
     */
    AG_CHECK(gain[AG_AMP_MODEL_SLO] > gain[AG_AMP_MODEL_BOGNER]);
    AG_CHECK(gain[AG_AMP_MODEL_BOGNER] > gain[AG_AMP_MODEL_JCM800]);
    AG_CHECK(comp[AG_AMP_MODEL_SLO] > comp[AG_AMP_MODEL_JCM800] + 1.0);
    AG_CHECK(comp[AG_AMP_MODEL_BOGNER] > comp[AG_AMP_MODEL_JCM800] + 1.0);
    /*
     * And each one against **its own capture**, which is the check that means
     * something: the compression each amplifier does, measured by rendering the
     * same take through the capture twice, twenty decibels apart.
     *
     *      capture              measured    this chain
     *      Mars Gain 8            16.1 dB      5.6 dB   <- two valves, short
     *      Bogner Ecstasy         18.2 dB     17.2 dB
     *      Peavey 5150            18.9 dB     19.5 dB
     *
     * The JCM800 model is ten decibels short and is *not* asserted against its
     * capture, because that gap is honest: those captures contain a power amplifier
     * and a supply that sags, and this chain has neither.  The other two reach the
     * figure with preamp gain alone, which matches the number without matching the
     * mechanism - worth knowing before anybody reads 17.2 as a triumph.
     */
    AG_CHECK(comp[AG_AMP_MODEL_BOGNER] > 14.0 && comp[AG_AMP_MODEL_BOGNER] < 21.0);
    /*
     * THE BRIGHTNESS ORDERING IS NO LONGER ASSERTED ON THE BARE CIRCUIT, AND THE
     * REASON IS A COMPONENT VALUE RATHER THAN A TOLERANCE
     *
     * This used to say the four-valve chain puts more of its energy above 2 kHz
     * than the two-valve one - -15.1 dB against -18.8 - and it did, because its
     * three hot stages had eight times the schematic's coupling corners and very
     * little bass reached them.  Those corners came down to an octave below the
     * lowest note (see cfg.couple_mul in ag_amp.c: a 57 Hz corner was
     * manufacturing thirty decibels of second harmonic on a low E), so more low
     * frequency now reaches the clippers and the bare circuit measures -18.3 -
     * half a decibel from the Marshall's rather than three above it.
     *
     * That is a real consequence and worth stating rather than papering over: on
     * the bare circuit the two are now the same brightness.  The comparison a
     * listener makes is with the voicing in, and there the four-valve model's
     * output bank carries +14.3 dB at 3.15 kHz against the Marshall's +8.5, so the
     * ordering is intact where it is audible.  What is *not* intact is the claim
     * that the circuits differ, and pretending otherwise with a looser tolerance
     * would hide the thing to fix - which is still ag_tube_spec_slo, as the note
     * below has said all along.
     */
    /*
     * The SLO against the crunch model on that last measure is **not** asserted,
     * and the reason is a measurement rather than a tolerance.
     *
     * Our four-valve chain puts less of its energy above 2 kHz than the two-valve
     * crunch one does - -17.0 dB against -15.2 - because at 95 dB of gain
     * everything after the first valve is a squared wave, and a square keeps four
     * fifths of its energy in the fundamental, while a barely-clipped wave carries
     * its harmonics on the edges.  More gain is not more relative top end.
     *
     * The captures do it the other way round: the real 5150 measures -6.8 dB and
     * the real Bogner -10.8, so the ordering *should* invert and ours does not.
     * That is a known gap with two named causes, both in ag_tube_spec_slo - the
     * 2.2 nF bright capacitor across the 470 k divider, worth up to 5.8 dB of
     * treble, and the 1000 pF across the cold clipper's plate load - and until
     * they are built the matching filters are covering for them.  Asserting an
     * order this chain cannot yet produce would be asserting a wish.
     */
    /* And by a margin worth having, not by a hair: the four-valve chain was
     * 19 dB the wrong way round when this test was written, and is now 28 dB the
     * right way. */
    AG_CHECK(20.0 * log10(gain[AG_AMP_MODEL_SLO] / gain[AG_AMP_MODEL_JCM800]) >
             2.0);
    /*
     * And the compression is checked against the *amplifier* rather than against
     * the other models: 20 dB less into the real 5150 capture comes back 1.1 dB
     * quieter, which is 18.9 dB of compression, and this chain measures 19.5.
     * Within a decibel of the thing it is copying is the claim; a chain that
     * compressed 10 dB would pass every ordering above and still be a different
     * amplifier.
     */
    AG_CHECK(comp[AG_AMP_MODEL_SLO] > 15.0 && comp[AG_AMP_MODEL_SLO] < 23.0);

    if (ag_test_failures != 0) {
        for (m = 0; m < AG_AMP_MODEL_N; m++) {
            printf("  %-7s chain gain %.1f dB, compression %.1f dB, above 2 kHz"
                   " %.1f dB\n",
                   ag_amp_model_name(m), 20.0 * log10(gain[m] + 1e-300), comp[m],
                   hash[m]);
        }
    }

    free(probe);
    free(a);
    free(tab);
    free(k);
}

/*
 * A preset round-trips for every model, three stages included.
 *
 * test_preset already does this for the default chain; what this adds is the
 * topology.  A three-stage chain exercises the parts of the header that the
 * two-stage one leaves at their defaults - the third spec, the third axis pair,
 * the third table - and a field left out of the blob shows up here as a chain
 * that plays slightly differently rather than as a refusal.
 */
static void test_preset_models(void)
{
    ag_ckt_t *k = (ag_ckt_t *)malloc(sizeof(ag_ckt_t));
    float    *tabA = (float *)malloc(sizeof(float) * AG_AMP_TAB_FLOATS);
    float    *tabB = (float *)malloc(sizeof(float) * AG_AMP_TAB_FLOATS);
    ag_amp_t *a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    ag_amp_t *b = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    float    *probe = (float *)malloc(sizeof(float) * PROBE_N);
    uint8_t  *blob = 0;
    int       m;

    if (k == 0 || tabA == 0 || tabB == 0 || a == 0 || b == 0 || probe == 0) {
        AG_CHECK(0);
        goto done;
    }
    ag_amp_probe_pluck(probe, PROBE_N, TFS);

    for (m = 0; m < AG_AMP_MODEL_N; m++) {
        ag_amp_cfg_t cfg;
        uint32_t     size, wrote;
        int          i;

        ag_amp_model(&cfg, m, TFS);
        cfg.os = 2;
        if (ag_amp_build(a, k, &cfg, tabA, 0, probe, PROBE_N) != 0) {
            AG_CHECK(0);
            continue;
        }
        size = ag_amp_preset_size(a->n, a->tab_n);
        AG_CHECK(size > 0);
        free(blob);
        blob = (uint8_t *)malloc(size);
        if (blob == 0) {
            AG_CHECK(0);
            continue;
        }
        wrote = ag_amp_preset_save(a, blob, size);
        AG_CHECK_INT((int)wrote, (int)size);
        /* Loading needs no solver: that is the whole claim of the format. */
        AG_CHECK_INT(ag_amp_preset_load(b, blob, wrote, tabB, TFS), 0);
        AG_CHECK_INT(b->n, a->n);
        AG_CHECK_INT(b->cfg.model, m);
        AG_CHECK_INT(b->cfg.n_stages, a->cfg.n_stages);
        for (i = 0; i < b->n; i++) {
            AG_CHECK(b->spec[i].rcath == a->spec[i].rcath);
            AG_CHECK(b->spec[i].ccath == a->spec[i].ccath);
            AG_CHECK(b->spec[i].ccouple == a->spec[i].ccouple);
            AG_CHECK(b->gain[i] == a->gain[i]);
        }
        /* Sample for sample on a real signal, which is what "the same chain"
         * has to mean.  One report a model rather than one a sample: the whole
         * point of a round-trip failure is which field is missing, and twenty
         * thousand identical lines say less about that than the first index. */
        {
            int bad = -1;
            ag_amp_reset(a);
            ag_amp_reset(b);
            for (i = 0; i < PROBE_N && bad < 0; i++) {
                const float ya = ag_amp_tick(a, probe[i]);
                const float yb = ag_amp_tick(b, probe[i]);
                if (ya != yb) {
                    bad = i;
                    printf("  preset mismatch: model %s, sample %d, %.9g vs"
                           " %.9g\n",
                           ag_amp_model_name(m), i, (double)ya, (double)yb);
                }
            }
            AG_CHECK_INT(bad, -1);
        }
    }

done:
    free(blob);
    free(probe);
    free(b);
    free(a);
    free(tabB);
    free(tabA);
    free(k);
}

/* ------------------------------------------------------------------------ */
/* The tone stack, against the same network in the solver                    */
/* ------------------------------------------------------------------------ */

/*
 * ag_tone is a specialisation: the same netlist, the same integration rule and
 * the same theta as ag_ckt, with the constant matrix solved once per knob
 * position so that the run is one 4x4 multiply instead of two triangular solves.
 * "The same" is a claim, so it is checked the way this tree checks that kind of
 * claim - by building the network twice and running both.
 *
 * The bar is a fraction of a percent rather than bit-exactness: ag_ckt carries
 * its matrix in float and factors it at build time, while ag_tone solves in
 * double and rounds once at the end, so the two agree to about where float
 * rounding puts them and not closer.
 */
static void ckt_tonestack(ag_ckt_t *k, const ag_tone_spec_t *sp, float fs,
                          float treble, float mid, float bass)
{
#define RES(x) ((x) > 1.0f ? (x) : 1.0f)
    ag_ckt_init(k, fs);
    (void)ag_ckt_add_vin(k, 7, 0);
    (void)ag_ckt_add_r(k, 7, 1, RES(sp->rsrc));
    (void)ag_ckt_add_r(k, 1, 2, RES(sp->rslope));
    (void)ag_ckt_add_c(k, 1, 3, sp->ctreb);
    (void)ag_ckt_add_r(k, 3, 4, RES((1.0f - treble) * sp->rtreb));
    (void)ag_ckt_add_r(k, 4, 5, RES(treble * sp->rtreb));
    (void)ag_ckt_add_c(k, 2, 5, sp->cbass);
    (void)ag_ckt_add_r(k, 5, 6, RES(bass * sp->rbass));
    (void)ag_ckt_add_c(k, 2, 6, sp->cmid);
    (void)ag_ckt_add_r(k, 6, 0, RES(mid * sp->rmid));
    (void)ag_ckt_add_r(k, 4, 0, RES(sp->rload));
    (void)ag_ckt_build(k);
#undef RES
}

static void test_tone_stack(void)
{
    ag_ckt_t       *k = (ag_ckt_t *)malloc(sizeof(ag_ckt_t));
    ag_tone_t      *t = (ag_tone_t *)malloc(sizeof(ag_tone_t));
    ag_tone_spec_t  sp;
    static const float pots[4][3] = { { 0.5f, 0.5f, 0.5f },
                                      { 1.0f, 0.5f, 0.0f },
                                      { 0.0f, 1.0f, 1.0f },
                                      { 0.8f, 0.2f, 0.6f } };
    int p;

    if (k == 0 || t == 0) {
        AG_CHECK(0);
        free(k);
        free(t);
        return;
    }
    ag_tone_marshall(&sp);

    for (p = 0; p < 4; p++) {
        const float tr = pots[p][0], md = pots[p][1], bs = pots[p][2];
        double      num = 0.0, den = 0.0;
        int         i;

        AG_CHECK_INT(ag_tone_design(t, &sp, TFS, tr, md, bs), 0);
        ag_tone_reset(t);
        ckt_tonestack(k, &sp, TFS, tr, md, bs);
        ag_ckt_reset(k);

        /* A pluck rather than a sine: a tone stack is three time constants, and
         * two of them are slow enough that a steady tone would only ever show
         * the third. */
        for (i = 0; i < 8000; i++) {
            const double s = sin(2.0 * 3.14159265358979 * 220.0 * (double)i /
                                 (double)TFS) +
                             0.5 * sin(2.0 * 3.14159265358979 * 3300.0 *
                                       (double)i / (double)TFS);
            const float  x = (float)(s * (i < 400 ? (double)i / 400.0 : 1.0));
            const float  a = ag_tone_tick(t, x);
            const float  b = ag_ckt_tick(k, x, 4);
            num += (double)(a - b) * (a - b);
            den += (double)b * b;
        }
        AG_CHECK(den > 1e-9);
        /*
         * How close the two have to be, and why it is not one number.
         *
         * Away from the stops they agree below -60 dB, which is where two float
         * realisations of one matrix belong.  With a pot *at* a stop the network
         * holds an ohm of pot track against a megohm of grid leak - six orders
         * across one matrix - and ag_ckt carries its matrix in float where this
         * solves in double, so the two part company at about -50 dB.  That is
         * conditioning and not topology: a wrong network is wrong by decibels,
         * not by a third of a percent, and the two settings that show it are
         * exactly the two with a pot at an end.
         */
        {
            const double db = 10.0 * log10(num / (den + 1e-30) + 1e-30);
            const int    at_stop = tr < 0.02f || tr > 0.98f || md < 0.02f ||
                                md > 0.98f || bs < 0.02f || bs > 0.98f;
            const double bound = at_stop ? -45.0 : -60.0;
            if (db >= bound) {
                printf("  tone stack differs from the solver by %.1f dB at"
                       " treble %.2f mid %.2f bass %.2f\n",
                       db, (double)tr, (double)md, (double)bs);
            }
            AG_CHECK(db < bound);
        }
    }

    /*
     * And the controls do what their names say, measured rather than assumed.
     * Small signal, one frequency each, everything else at noon.
     */
    {
        static const float hz[3] = { 100.0f, 800.0f, 4000.0f };
        int                b;
        for (b = 0; b < 3; b++) {
            double lo = 0.0, hi = 0.0;
            int    j, i;
            for (j = 0; j < 2; j++) {
                float tr = 0.5f, md = 0.5f, bs = 0.5f;
                double acc = 0.0;
                const float v = j == 0 ? 0.05f : 0.95f;
                if (b == 0) {
                    bs = v;
                } else if (b == 1) {
                    md = v;
                } else {
                    tr = v;
                }
                AG_CHECK_INT(ag_tone_design(t, &sp, TFS, tr, md, bs), 0);
                ag_tone_reset(t);
                for (i = 0; i < 6000; i++) {
                    const double s = sin(2.0 * 3.14159265358979 * (double)hz[b] *
                                         (double)i / (double)TFS);
                    const float  y = ag_tone_tick(t, (float)s);
                    if (i > 2000) {
                        acc += (double)y * y;
                    }
                }
                if (j == 0) {
                    lo = acc;
                } else {
                    hi = acc;
                }
            }
            /* Every one of them raises its own band, and by something worth
             * having: the smallest of the three is the mid control, which is
             * why a passive stack is famous for its dip. */
            AG_CHECK(10.0 * log10(hi / (lo + 1e-30)) > 3.0);
        }
    }

    free(t);
    free(k);
}
/*
 * The per-stage coupling multiplier does what it says, and only that.
 *
 * `cfg.couple_mul` is where a bass cut in front of one clipping stage lives, and
 * the reason it needs a test is that it is easy for it to do *nothing*: it reaches
 * the audio path through one line in `design`, and a zero or a missing default
 * would silently leave the schematic's corner in place - which is exactly what the
 * field is for changing.  So: a low tone, three settings, and the low end has to
 * move in the right direction and by a plausible amount.
 *
 * A late stage rather than the first, because the first coupling corner in these
 * netlists is 1.6 Hz and doubling it is still 3.2 Hz: `tube_render sens` measures
 * that as no change at all, and a test that used stage 1 would pass whatever the
 * code did.
 */
static void test_couple_mul(void)
{
    static const float muls[3] = { 1.0f, 2.0f, 8.0f };
    ag_ckt_t    *k = (ag_ckt_t *)malloc(sizeof(ag_ckt_t));
    float       *tab = (float *)malloc(sizeof(float) * AG_AMP_TAB_FLOATS);
    ag_amp_t    *a = (ag_amp_t *)malloc(sizeof(ag_amp_t));
    float       *probe = (float *)malloc(sizeof(float) * PROBE_N);
    double       rms[3];
    int          m, i;

    if (k == 0 || tab == 0 || a == 0 || probe == 0) {
        AG_CHECK(0);
        free(k); free(tab); free(a); free(probe);
        return;
    }
    /* 80 Hz, which every one of these models passes and every coupling corner in
     * them is below - so what changes is the corner and not the note. */
    for (i = 0; i < PROBE_N; i++) {
        probe[i] = 0.25f * (float)sin(2.0 * 3.14159265358979 * 80.0 *
                                      (double)i / (double)TFS);
    }
    for (m = 0; m < 3; m++) {
        ag_amp_cfg_t cfg;
        double       e = 0.0;
        int          b;
        ag_amp_model(&cfg, AG_AMP_MODEL_BOGNER, TFS);
        /* The voicing out of the way: this is about one high-pass corner. */
        amp_no_voicing(&cfg);
        cfg.mid_db = 0.0f;
        cfg.couple_mul[1] = muls[m]; /* the second stage, see the note above */
        if (ag_amp_build(a, k, &cfg, tab, 0, probe, PROBE_N) != 0) {
            AG_CHECK(0);
            break;
        }
        ag_amp_reset(a);
        {
            /*
             * The **fundamental**, by one bin of a transform, and not the total
             * energy of the output.
             *
             * The first version of this test measured total rms and failed: an
             * 80 Hz tone through three clipping valves is mostly harmonics, so the
             * total barely moves when the fundamental is cut.  What the coupling
             * corner does is to the fundamental, and that is what has to be looked
             * at.
             */
            double re = 0.0, im = 0.0;
            int    nn = 0;
            for (i = 0; i < PROBE_N; i++) {
                const double y = (double)ag_amp_tick(a, probe[i]);
                if (i > PROBE_N / 4) { /* past the settling of the filters */
                    const double w = 2.0 * 3.14159265358979 * 80.0 *
                                     (double)i / (double)TFS;
                    re += y * cos(w);
                    im += y * sin(w);
                    nn++;
                }
            }
            e = (re * re + im * im) / ((double)(nn ? nn : 1) *
                                       (double)(nn ? nn : 1));
        }
        rms[m] = 10.0 * log10(e + 1e-300);
    }
    /*
     * More corner, less 80 Hz - monotone, and the amounts are worth writing down
     * because they are much smaller than the filter alone would suggest.
     *
     * Measured: x2 on the second stage's 142 Hz corner costs the fundamental
     * **0.4 dB** and x8 costs 7.3 dB, where a first-order high-pass on its own
     * would take 5.5 dB for the first of those.  The clipper puts the fundamental
     * back: past the knee the output waveform's shape is set by the envelope, not
     * by how much of the fundamental arrived, which is the same reason
     * pre-emphasis cannot deliver a top end through a limiter.  The first version
     * of this test asked for 0.5 dB at x2 and failed - correctly.
     */
    AG_CHECK(rms[1] < rms[0] - 0.2);
    AG_CHECK(rms[2] < rms[1] - 3.0);
    /* And 1.0 has to be the schematic itself - a zero must mean the same, since a
     * config that was memset to zero has to play the netlist and not silence. */
    {
        ag_amp_cfg_t cfg;
        double       e = 0.0;
        int          b;
        ag_amp_model(&cfg, AG_AMP_MODEL_BOGNER, TFS);
        amp_no_voicing(&cfg);
        cfg.mid_db = 0.0f;
        cfg.couple_mul[1] = 0.0f;
        if (ag_amp_build(a, k, &cfg, tab, 0, probe, PROBE_N) == 0) {
            double re = 0.0, im = 0.0;
            int    nn = 0;
            ag_amp_reset(a);
            for (i = 0; i < PROBE_N; i++) {
                const double y = (double)ag_amp_tick(a, probe[i]);
                if (i > PROBE_N / 4) {
                    const double w = 2.0 * 3.14159265358979 * 80.0 *
                                     (double)i / (double)TFS;
                    re += y * cos(w);
                    im += y * sin(w);
                    nn++;
                }
            }
            e = (re * re + im * im) / ((double)(nn ? nn : 1) *
                                       (double)(nn ? nn : 1));
            AG_CHECK(fabs(10.0 * log10(e + 1e-300) - rms[0]) < 0.01);
        } else {
            AG_CHECK(0);
        }
    }
    free(k);
    free(tab);
    free(a);
    free(probe);
}

void run_tube_tests(void)
{
    test_spec_matches_bench();
    test_curve_is_a_valve();
    test_adaa_arithmetic();
    test_blocking();
    test_oversampler();
    test_sections();
    test_amp_chain();
    test_stage_counts();
    test_preset();
    test_blend();
    test_blend_in_chain();
    test_knobs_keep_state();
    test_voicing_banks();
    test_models();
    test_cold_clipper();
    test_model_gain_order();
    test_preset_models();
    test_tone_stack();
    test_couple_mul();
}
