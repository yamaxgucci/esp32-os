/*
 * ArgonOS - host tests for the analogue circuit solver (apps/common/ckt).
 *
 * A performance figure for a solver that solves the wrong circuit is worse
 * than no figure at all, so the cost measurements in apps/cktbench are only
 * worth reading if these pass first.  Three things are checked, in order of
 * how quietly they fail:
 *
 * 1. ag_mathf against the host's libm.  The libc shim shipped with a powf that
 *    returns its first argument and an expf that returns zero; a device
 *    equation built on those produces a valve that never conducts and a signal
 *    that stays clean, which looks like a working clean channel.
 * 2. A linear RC network against the closed-form answer.  If the companion
 *    model or the stamping is wrong, every nonlinear result is wrong too, and
 *    it will look like a modelling problem rather than an arithmetic one.
 * 3. The nonlinear devices against what physics requires of them: a diode pair
 *    that stops at a diode drop, a valve stage that sits at a sane operating
 *    point, inverts, has gain of tens, and clips asymmetrically.  The last one
 *    is the whole reason for preferring a circuit to a waveshaper - symmetric
 *    clipping is a fuzz box, not an amplifier.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"

#include <math.h>
#include <stdlib.h>

#include "ag_ckt.h"
#include "ag_mathf.h"
#include "ag_stage.h"
#include "ckt_circuits.h"

#define FS 48000.0f

static float relerr(float got, float want)
{
    const float d = fabsf(got - want);
    const float m = fabsf(want);
    return m > 1e-30f ? d / m : d;
}

/* ------------------------------------------------------------------------ */

static void test_mathf(void)
{
    int   i;
    float worst_exp = 0.0f, worst_log = 0.0f, worst_pow = 0.0f;
    float worst_sqrt = 0.0f, worst_tanh = 0.0f;

    for (i = -800; i <= 800; i++) {
        const float x = (float)i * 0.1f; /* -80 .. 80 */
        const float e = relerr(ag_expf(x), expf(x));
        if (e > worst_exp) {
            worst_exp = e;
        }
    }

    for (i = 1; i <= 20000; i++) {
        const float x = (float)i * 0.01f; /* 0.01 .. 200 */
        float       e = relerr(ag_logf(x), logf(x));
        if (fabsf(logf(x)) < 0.05f) {
            e = 0.0f; /* near the root the relative error is meaningless */
        }
        if (e > worst_log) {
            worst_log = e;
        }
        e = relerr(ag_sqrtf(x), sqrtf(x));
        if (e > worst_sqrt) {
            worst_sqrt = e;
        }
    }

    /* The exponents the triode actually asks for: E1^0.4 and E1^1.4. */
    for (i = 1; i <= 5000; i++) {
        const float x = (float)i * 0.02f; /* 0.02 .. 100 */
        float       e = relerr(ag_powf(x, 0.4f), powf(x, 0.4f));
        if (e > worst_pow) {
            worst_pow = e;
        }
        e = relerr(ag_powf(x, 1.4f), powf(x, 1.4f));
        if (e > worst_pow) {
            worst_pow = e;
        }
    }

    for (i = -1000; i <= 1000; i++) {
        const float x = (float)i * 0.01f;
        const float e = fabsf(ag_tanhf(x) - tanhf(x));
        if (e > worst_tanh) {
            worst_tanh = e;
        }
    }

    /* 1e-5 relative is 100 dB down - far below anything audio resolves, and
     * loose enough that these do not become a test of the host's libm. */
    AG_CHECK(worst_exp < 1e-5f);
    AG_CHECK(worst_log < 1e-5f);
    AG_CHECK(worst_pow < 1e-4f);
    AG_CHECK(worst_sqrt < 1e-5f);
    AG_CHECK(worst_tanh < 1e-5f);

    /* The guarded edges, which is where a Newton iteration wanders. */
    AG_CHECK(ag_expf(-200.0f) == 0.0f);
    AG_CHECK(ag_expf(200.0f) > 1e30f);
    AG_CHECK(ag_logf(0.0f) < -80.0f);
    AG_CHECK(ag_logf(-1.0f) < -80.0f);
    AG_CHECK(ag_sqrtf(-1.0f) == 0.0f);
    AG_CHECK(ag_powf(0.0f, 1.4f) == 0.0f);
}

/* ------------------------------------------------------------------------ */

/*
 * Vin -> R -> node 2 -> C -> ground.  A first-order low pass with a cutoff
 * this test picks so that the answer is a round number: at f = fc the
 * magnitude is exactly 1/sqrt(2) and the phase is -45 degrees.
 */
static void build_rc(ag_ckt_t *k, float r, float c)
{
    ag_ckt_init(k, FS);
    AG_CHECK(ag_ckt_add_vin(k, 1, 0) == 0);
    AG_CHECK(ag_ckt_add_r(k, 1, 2, r) == 0);
    AG_CHECK(ag_ckt_add_c(k, 2, 0, c) == 0);
    AG_CHECK(ag_ckt_build(k) == 0);
}

/* Peak of the output over the last whole cycles, after the transient. */
static float sweep_gain(ag_ckt_t *k, float hz, float amp, int out_node)
{
    const int settle = (int)(FS / hz) * 20;
    const int meas = (int)(FS / hz) * 8;
    float     peak = 0.0f;
    int       i;
    double    ph = 0.0;
    const double dp = 2.0 * 3.14159265358979 * (double)hz / (double)FS;

    for (i = 0; i < settle; i++) {
        ag_ckt_tick(k, amp * (float)sin(ph), out_node);
        ph += dp;
    }
    for (i = 0; i < meas; i++) {
        const float y = ag_ckt_tick(k, amp * (float)sin(ph), out_node);
        const float a = y < 0.0f ? -y : y;
        if (a > peak) {
            peak = a;
        }
        ph += dp;
    }
    return peak / amp;
}

static void test_linear_rc(void)
{
    ag_ckt_t *k = (ag_ckt_t *)calloc(1, sizeof(ag_ckt_t));
    const float r = 1000.0f;
    const float c = 1.0f / (2.0f * 3.14159265f * 1000.0f * 1000.0f); /* fc 1k */
    float       g;

    build_rc(k, r, c);

    /* Well below cutoff: unity. */
    g = sweep_gain(k, 50.0f, 1.0f, 2);
    AG_CHECK(fabsf(g - 1.0f) < 0.02f);

    /* At cutoff: -3 dB.  Trapezoidal integration warps frequency slightly,
     * and 1 kHz against a 48 kHz rate is warped by well under a percent. */
    ag_ckt_reset(k);
    g = sweep_gain(k, 1000.0f, 1.0f, 2);
    AG_CHECK(fabsf(g - 0.7071f) < 0.02f);

    /*
     * A decade above cutoff the analogue answer is -20 dB, or 0.0995 - and
     * the solver does not give that, on purpose.  A trapezoidal companion
     * model is the bilinear transform, and the bilinear transform warps
     * frequency: the analogue rate w maps to (2/T)*tan(w*T/2), which at
     * 10 kHz against a 48 kHz sample rate is 17% higher, so the filter is
     * 1.4 dB further down than the textbook says.
     *
     * That is not an error to be tuned out, it is the discretisation, and it
     * is worth having a test say so: it is the same warping that makes a
     * treble control modelled at 48 kHz sit wrong, and one of the reasons an
     * amplifier model wants to run oversampled even where nothing clips.
     */
    ag_ckt_reset(k);
    g = sweep_gain(k, 10000.0f, 1.0f, 2);
    {
        const double t = 1.0 / (double)FS;
        const double w_warped = (2.0 / t) * tan(3.14159265358979 * 10000.0 * t);
        const double wc = 2.0 * 3.14159265358979 * 1000.0;
        const double want = 1.0 / sqrt(1.0 + (w_warped / wc) * (w_warped / wc));
        AG_CHECK(fabsf(g - (float)want) < 0.005f);
        AG_CHECK(g < 0.0995f); /* warped down, never up */
    }

    /* A linear network is linear: ten times in, ten times out.  This is the
     * check that caught the IR convolution normalising by peak instead of by
     * energy, and it costs nothing to repeat here. */
    ag_ckt_reset(k);
    {
        const float g1 = sweep_gain(k, 1000.0f, 0.1f, 2);
        ag_ckt_reset(k);
        {
            const float g10 = sweep_gain(k, 1000.0f, 10.0f, 2);
            AG_CHECK(fabsf(g1 - g10) < 0.001f);
        }
    }

    /* Every sample of a linear network converges on the first Newton pass:
     * there is nothing to iterate on. */
    AG_CHECK(k->iters_last <= 2u);
    AG_CHECK_INT(k->nonconverged, 0);

    free(k);
}

/* ------------------------------------------------------------------------ */

/*
 * Vin -> 4k7 -> node 2, antiparallel silicon pair from node 2 to ground.
 * The Tube Screamer's clipping section, minus the op-amp.
 */
static void test_diode_clipper(void)
{
    ag_ckt_t *k = (ag_ckt_t *)calloc(1, sizeof(ag_ckt_t));
    float     small, large;

    ag_ckt_init(k, FS);
    AG_CHECK(ag_ckt_add_vin(k, 1, 0) == 0);
    AG_CHECK(ag_ckt_add_r(k, 1, 2, 4700.0f) == 0);
    /* 1N914: Is 2.52 nA, n 1.752, Vt 25.85 mV -> n*Vt = 45.3 mV. */
    AG_CHECK(ag_ckt_add_diode_pair(k, 2, 0, 2.52e-9f, 0.0453f) == 0);
    AG_CHECK(ag_ckt_build(k) == 0);

    /* 10 mV in: far below conduction, so the pair is invisible and the
     * output follows the input. */
    small = sweep_gain(k, 1000.0f, 0.01f, 2);
    AG_CHECK(small > 0.97f);

    /* 5 V in: the pair holds the node at about a diode drop.  Silicon at
     * these currents sits near 0.6 V; anything above 0.9 V means the diode is
     * not conducting, and anything below 0.3 V means it is a short. */
    ag_ckt_reset(k);
    large = sweep_gain(k, 1000.0f, 5.0f, 2) * 5.0f; /* back to volts */
    AG_CHECK(large > 0.35f);
    AG_CHECK(large < 0.9f);

    /* Symmetric pair, symmetric clipping: the odd-harmonic case, and the
     * contrast against the valve below. */
    ag_ckt_reset(k);
    {
        float  vmax = -1e9f, vmin = 1e9f;
        int    i;
        double ph = 0.0;
        const double dp = 2.0 * 3.14159265358979 * 1000.0 / (double)FS;
        for (i = 0; i < 4800; i++) {
            const float y = ag_ckt_tick(k, 5.0f * (float)sin(ph), 2);
            if (i > 2400) {
                if (y > vmax) {
                    vmax = y;
                }
                if (y < vmin) {
                    vmin = y;
                }
            }
            ph += dp;
        }
        AG_CHECK(fabsf(vmax + vmin) < 0.01f * vmax);
    }

    AG_CHECK_INT(k->nonconverged, 0);
    free(k);
}

/* ------------------------------------------------------------------------ */

/*
 * One 12AX7 gain stage, the circuit that is in the first position of almost
 * every guitar amplifier ever built:
 *
 *   node 1  input source
 *   node 2  grid, through a 22 nF coupling cap, with a 1 M grid leak
 *   node 3  plate, 100 k to the 250 V supply
 *   node 4  cathode, 1k5 to ground with a 22 uF bypass
 *   node 5  the supply
 */
static void build_triode_stage(ag_ckt_t *k)
{
    ag_triode_model_t m;
    ag_triode_model_12ax7(&m);

    ag_ckt_init(k, FS);
    AG_CHECK(ag_ckt_add_vin(k, 1, 0) == 0);
    AG_CHECK(ag_ckt_add_vdc(k, 5, 0, 250.0f) == 0);
    AG_CHECK(ag_ckt_add_c(k, 1, 2, 22.0e-9f) == 0);
    AG_CHECK(ag_ckt_add_r(k, 2, 0, 1.0e6f) == 0);
    AG_CHECK(ag_ckt_add_r(k, 5, 3, 100.0e3f) == 0);
    AG_CHECK(ag_ckt_add_r(k, 4, 0, 1500.0f) == 0);
    AG_CHECK(ag_ckt_add_c(k, 4, 0, 22.0e-6f) == 0);
    AG_CHECK(ag_ckt_add_triode(k, 2, 3, 4, &m) == 0);
    AG_CHECK(ag_ckt_build(k) == 0);
}

/* Run silence until the bias settles, and report where it settled. */
static void settle(ag_ckt_t *k, float *vplate, float *vcathode)
{
    int i;
    for (i = 0; i < (int)(FS * 0.5f); i++) {
        ag_ckt_tick(k, 0.0f, 3);
    }
    *vplate = k->x[3 - 1];
    *vcathode = k->x[4 - 1];
}

static void test_triode_bias(void)
{
    ag_ckt_t *k = (ag_ckt_t *)calloc(1, sizeof(ag_ckt_t));
    float     vp = 0.0f, vk = 0.0f;

    build_triode_stage(k);
    settle(k, &vp, &vk);

    /*
     * The textbook operating point for this circuit is a plate around
     * 170-210 V and a cathode around 1.0-1.8 V, giving roughly 0.5-0.8 mA.
     * A plate stuck at 250 V means the valve is not conducting at all; a
     * plate near 0 V means it is a short.  Both are the failures that a
     * plausible-looking waveform hides.
     */
    AG_CHECK(vp > 140.0f);
    AG_CHECK(vp < 230.0f);
    AG_CHECK(vk > 0.6f);
    AG_CHECK(vk < 2.5f);
    AG_CHECK_INT(k->nonconverged, 0);

    free(k);
}

static void test_triode_gain_and_asymmetry(void)
{
    ag_ckt_t *k = (ag_ckt_t *)calloc(1, sizeof(ag_ckt_t));
    float     vp = 0.0f, vk = 0.0f;
    int       i;
    double    ph;
    const double dp = 2.0 * 3.14159265358979 * 1000.0 / (double)FS;

    build_triode_stage(k);
    settle(k, &vp, &vk);

    /* Small signal: gain of a 12AX7 stage with a 100k plate load into an open
     * circuit is around 50-65, and it inverts. */
    {
        float pk_pos = 0.0f, pk_neg = 0.0f;
        ph = 0.0;
        for (i = 0; i < 4800; i++) {
            const float y = ag_ckt_tick(k, 0.01f * (float)sin(ph), 3) - vp;
            if (i > 2400) {
                if (y > pk_pos) {
                    pk_pos = y;
                }
                if (-y > pk_neg) {
                    pk_neg = -y;
                }
            }
            ph += dp;
        }
        {
            const float gain = 0.5f * (pk_pos + pk_neg) / 0.01f;
            AG_CHECK(gain > 30.0f);
            AG_CHECK(gain < 90.0f);
            /* Small signal must be nearly symmetric, or the "asymmetry" test
             * below would be measuring a bias error instead of clipping. */
            AG_CHECK(fabsf(pk_pos - pk_neg) < 0.25f * pk_pos);
        }
    }

    /*
     * Hard drive: 3 V of input into a stage with gain 60 asks for 180 V of
     * swing out of a 250 V supply, so it must clip - and it must clip
     * differently on the two halves.  Cutoff runs into the supply rail
     * softly, saturation runs into grid conduction hard, and the difference
     * between the two is the second harmonic that a symmetric waveshaper
     * cannot produce.
     */
    ag_ckt_reset(k);
    settle(k, &vp, &vk);
    {
        float pk_pos = 0.0f, pk_neg = 0.0f;
        ph = 0.0;
        for (i = 0; i < 9600; i++) {
            const float y = ag_ckt_tick(k, 3.0f * (float)sin(ph), 3) - vp;
            if (i > 4800) {
                if (y > pk_pos) {
                    pk_pos = y;
                }
                if (-y > pk_neg) {
                    pk_neg = -y;
                }
            }
            ph += dp;
        }
        AG_CHECK(pk_pos > 10.0f);
        AG_CHECK(pk_neg > 10.0f);
        /* At least 10% apart: that is the even-harmonic content. */
        AG_CHECK(fabsf(pk_pos - pk_neg) > 0.10f * (pk_pos + pk_neg) * 0.5f);
    }

    AG_CHECK_INT(k->nonconverged, 0);
    free(k);
}

/* ------------------------------------------------------------------------ */

/*
 * A JFET common-source stage.  Included because it is the nonlinearity that
 * costs nothing transcendental - the contrast with the valve is the point.
 */
static void test_jfet_stage(void)
{
    ag_ckt_t       *k = (ag_ckt_t *)calloc(1, sizeof(ag_ckt_t));
    ag_jfet_model_t m;
    float           vd0;
    int             i;

    ag_jfet_model_j201(&m);

    ag_ckt_init(k, FS);
    AG_CHECK(ag_ckt_add_vin(k, 1, 0) == 0);
    AG_CHECK(ag_ckt_add_vdc(k, 5, 0, 9.0f) == 0);
    AG_CHECK(ag_ckt_add_c(k, 1, 2, 100.0e-9f) == 0);
    AG_CHECK(ag_ckt_add_r(k, 2, 0, 1.0e6f) == 0);
    AG_CHECK(ag_ckt_add_r(k, 5, 3, 47.0e3f) == 0); /* drain load */
    AG_CHECK(ag_ckt_add_r(k, 4, 0, 2.2e3f) == 0);  /* source degeneration */
    AG_CHECK(ag_ckt_add_c(k, 4, 0, 22.0e-6f) == 0);
    AG_CHECK(ag_ckt_add_jfet(k, 2, 3, 4, &m) == 0);
    AG_CHECK(ag_ckt_build(k) == 0);

    for (i = 0; i < (int)(FS * 0.5f); i++) {
        ag_ckt_tick(k, 0.0f, 3);
    }
    vd0 = k->x[3 - 1];
    {
        /* Separate the startup transient from steady running: a solver that
         * struggles while the supply rail is charging is tolerable, one that
         * struggles on signal is not. */
        const uint32_t noncvg_startup = k->nonconverged;
        AG_CHECK_INT(noncvg_startup, 0);
    }

    /* The drain must sit between the rails, not at either one. */
    AG_CHECK(vd0 > 1.0f);
    AG_CHECK(vd0 < 8.5f);

    /* And the stage must invert with gain above one. */
    {
        float        pk = 0.0f;
        double       ph = 0.0;
        const double dp = 2.0 * 3.14159265358979 * 1000.0 / (double)FS;
        for (i = 0; i < 4800; i++) {
            const float y = ag_ckt_tick(k, 0.05f * (float)sin(ph), 3) - vd0;
            const float a = y < 0.0f ? -y : y;
            if (i > 2400 && a > pk) {
                pk = a;
            }
            ph += dp;
        }
        AG_CHECK(pk / 0.05f > 2.0f);
    }

    AG_CHECK_INT(k->nonconverged, 0);
    free(k);
}

/* ------------------------------------------------------------------------ */

/*
 * The valve's Jacobian against finite differences.
 *
 * This is the test that would have saved a whole afternoon.  A Jacobian that
 * disagrees with its own function does not make Newton fail loudly; it makes
 * it circle the answer with an amplitude set by the size of the disagreement,
 * which reads as "the solver does not quite converge" and sends you looking at
 * the solver, the tolerance, the pivoting and the precision - in that order,
 * and all of them wrong.  Two minutes of central differences says whether the
 * derivatives are the problem before any of that.
 */
static void test_triode_jacobian(void)
{
    ag_triode_model_t m;
    ag_triode_op_t    o, hi, lo;
    int               i, j;
    float             worst_g = 0.0f, worst_p = 0.0f, worst_ig = 0.0f;

    ag_triode_model_12ax7(&m);

    for (i = -40; i <= 20; i++) {
        const float vgk = (float)i * 0.1f;
        for (j = 1; j <= 25; j++) {
            const float vpk = (float)j * 10.0f;
            const float hg = 1.0e-3f, hp = 1.0e-2f;
            float       num;

            ag_triode_eval(&m, vgk, vpk, &o);
            /* Only where there is a current worth differentiating: below a
             * microamp the finite difference is its own noise. */
            if (o.ip < 1.0e-6f) {
                continue;
            }

            ag_triode_eval(&m, vgk + hg, vpk, &hi);
            ag_triode_eval(&m, vgk - hg, vpk, &lo);
            num = (hi.ip - lo.ip) / (2.0f * hg);
            if (relerr(o.dip_dvgk, num) > worst_g) {
                worst_g = relerr(o.dip_dvgk, num);
            }

            ag_triode_eval(&m, vgk, vpk + hp, &hi);
            ag_triode_eval(&m, vgk, vpk - hp, &lo);
            num = (hi.ip - lo.ip) / (2.0f * hp);
            if (relerr(o.dip_dvpk, num) > worst_p) {
                worst_p = relerr(o.dip_dvpk, num);
            }
        }
    }

    for (i = -60; i <= 60; i++) {
        const float vgk = (float)i * 0.02f;
        const float h = 1.0e-4f;
        float       num;
        ag_triode_eval(&m, vgk, 100.0f, &o);
        /*
         * A hundred nanoamps is where grid current starts to mean anything:
         * through a 1 M grid leak that is 0.1 V of bias shift.  Below it the
         * current is exponentially small and a central difference is measuring
         * its own rounding - 39% "error" at two nanoamps, 0.3% at a hundred.
         */
        if (o.ig < 1.0e-7f) {
            continue;
        }
        ag_triode_eval(&m, vgk + h, 100.0f, &hi);
        ag_triode_eval(&m, vgk - h, 100.0f, &lo);
        num = (hi.ig - lo.ig) / (2.0f * h);
        if (relerr(o.dig_dvgk, num) > worst_ig) {
            worst_ig = relerr(o.dig_dvgk, num);
        }
    }

    /* 1% is loose for a derivative and tight for a central difference taken in
     * single precision: the step has to be big enough to survive rounding and
     * small enough to be a derivative, and 1% is what that costs. */
    AG_CHECK(worst_g < 0.01f);
    AG_CHECK(worst_p < 0.01f);
    AG_CHECK(worst_ig < 0.01f);
}

/*
 * A linear network has nothing to iterate on, so ag_ckt_build factors it once
 * and every sample is a pair of triangular solves.  If that flag ever stops
 * being set, nothing breaks and nothing says so - the tone stack just costs
 * four times what it should, for ever.
 */
static void test_linear_fast_path(void)
{
    ag_ckt_t *k = (ag_ckt_t *)calloc(1, sizeof(ag_ckt_t));

    AG_CHECK(ckt_build_rc(k, FS) == 2);
    AG_CHECK_INT(k->linear, 1);
    ag_ckt_tick(k, 1.0f, 2);
    AG_CHECK_INT(k->iters_last, 1);

    AG_CHECK(ckt_build_tonestack(k, FS) == 4);
    AG_CHECK_INT(k->linear, 1);

    /* And a circuit with a device in it must not take the fast path. */
    AG_CHECK(ckt_build_diode_clipper(k, FS) == 2);
    AG_CHECK_INT(k->linear, 0);

    free(k);
}

/*
 * The cascade, solved stage by stage.  What is checked is that it is still an
 * amplifier: it biases, it passes signal, it clips on both halves and it clips
 * them differently, and the solver is not spending its iteration limit.
 */
static void test_chain(void)
{
    ckt_chain_t *c = (ckt_chain_t *)calloc(1, sizeof(ckt_chain_t));
    int          i;
    float        pk_pos = -1e9f, pk_neg = 1e9f, dc;
    double       ph = 0.0;
    const double dp = 2.0 * 3.14159265358979 * 1000.0 / (double)FS;

    AG_CHECK(ckt_build_chain(c, FS, 4) == 1);

    for (i = 0; i < (int)(FS * 0.5f); i++) {
        ckt_chain_tick(c, 0.0f);
    }
    dc = ckt_chain_tick(c, 0.0f);
    /* Idle, the last plate sits at its operating point like any other. */
    AG_CHECK(dc > 100.0f);
    AG_CHECK(dc < 240.0f);

    ckt_chain_reset_stats(c);
    for (i = 0; i < 4800; i++) {
        const float y = ckt_chain_tick(c, 0.2f * (float)sin(ph)) - dc;
        if (i > 2400) {
            if (y > pk_pos) {
                pk_pos = y;
            }
            if (y < pk_neg) {
                pk_neg = y;
            }
        }
        ph += dp;
    }

    /* Four stages of gain on 200 mV: the last plate must be swinging tens of
     * volts, and asymmetrically. */
    AG_CHECK(pk_pos > 10.0f);
    AG_CHECK(-pk_neg > 10.0f);
    AG_CHECK(fabsf(pk_pos + pk_neg) > 0.05f * (pk_pos - pk_neg));

    /* Under 5% of samples may hit the iteration limit.  Some do: the plate is
     * loaded by a coupling capacitor whose companion conductance is eighty
     * times the valve's own, and the last digit of the plate voltage goes with
     * it.  What matters is that it is rare and bounded, not that it is zero. */
    AG_CHECK(ckt_chain_noncvg(c) < 4800u / 20u);
    /* And that the common case is a handful of passes, not the limit. */
    AG_CHECK(ckt_chain_iters(c) < 4800u * 4u * 8u);

    free(c);
}

/* ------------------------------------------------------------------------ */

/*
 * The baked stage against the solver it was baked from.
 *
 * This is the check the whole fixed-topology idea stands on.  If the table is
 * the same model computed in advance, the two must produce the same waveform;
 * if they do not, then what has happened is that a circuit was quietly
 * replaced by an approximation of one, which is exactly what this whole
 * subsystem exists to avoid.
 */
static void test_baked_matches_solver(void)
{
    static float       tab[256 * 9 * 2];
    static ckt_chain_t solver;
    static ckt_baked_chain_t baked;
    ag_ckt_t          *k = (ag_ckt_t *)calloc(1, sizeof(ag_ckt_t));
    ag_stage_t        *s = (ag_stage_t *)calloc(1, sizeof(ag_stage_t));
    float              sine[48];
    int                i, out_node;

    for (i = 0; i < 48; i++) {
        sine[i] = (float)sin(2.0 * 3.14159265358979 * i / 48.0);
    }

    /*
     * The reduced linear model, checked against something known.  K22 is the
     * plate load line seen by the valve: a 100 k plate resistor working into
     * the next stage's 570 k is about 85 k, and it has to come out negative -
     * pulling current out of the plate pulls the plate down.
     */
    ag_stage_init(s);
    out_node = ckt_build_stage_alone(k, FS);
    AG_CHECK(out_node != 0);
    AG_CHECK_INT(ag_stage_bake(s, k, out_node, tab, 0, 256, 9, -20.0f, 20.0f,
                               190.0f, 260.0f),
                 0);
    AG_CHECK(s->k22 < -70000.0f);
    AG_CHECK(s->k22 > -100000.0f);
    /* The grid sees its stopper the same way, and the two ports barely see
     * each other - which is why this is a nearly separable problem. */
    AG_CHECK(s->k11 < -50000.0f);
    AG_CHECK(fabsf(s->k12) < 10.0f);
    AG_CHECK(fabsf(s->k21) < 10.0f);

    /* Baking must leave the circuit itself usable. */
    for (i = 0; i < (int)(FS * 0.5f); i++) {
        ag_ckt_tick(k, 0.0f, out_node);
    }
    AG_CHECK(k->x[5 - 1] > 140.0f);
    AG_CHECK(k->x[5 - 1] < 230.0f);

    /*
     * Now the path that would actually ship: two passes, ranges fitted to the
     * signal, operating point found at build time.  One stage first, because
     * a chain hides which stage is wrong.
     */
    for (i = 1; i <= 4; i++) {
        double        err = 0.0, sig = 0.0, ma = 0.0, mb = 0.0;
        static double A[4800], B[4800];
        int           j;

        AG_CHECK_INT(ckt_bake_chain(&baked, k, FS, i, sine, 48, 6000,
                                    CKT_VARIANT_PLAIN, 0),
                     0);
        AG_CHECK(ckt_build_chain(&solver, FS, i) == 1);
        /* Every grid point of a fitted table must have converged, or part of
         * the table is a guess wearing a number. */
        for (j = 0; j < i; j++) {
            AG_CHECK_INT(baked.stage[j].bake_nonconverged, 0);
        }

        for (j = 0; j < (int)(FS * 0.5f); j++) {
            ckt_chain_tick(&solver, sine[j % 48]);
            ckt_baked_chain_tick(&baked, sine[j % 48]);
        }
        ckt_baked_chain_reset(&baked); /* clears the clamp counter too */
        for (j = 0; j < (int)(FS * 0.2f); j++) {
            ckt_baked_chain_tick(&baked, sine[j % 48]);
        }
        for (j = 0; j < 4800; j++) {
            A[j] = ckt_chain_tick(&solver, sine[j % 48]);
            B[j] = ckt_baked_chain_tick(&baked, sine[j % 48]);
            ma += A[j];
            mb += B[j];
        }
        ma /= 4800.0;
        mb /= 4800.0;
        /* Same operating point, to a tenth of a volt out of 170. */
        AG_CHECK(fabs(ma - mb) < 0.15);
        for (j = 0; j < 4800; j++) {
            const double d = (A[j] - ma) - (B[j] - mb);
            err += d * d;
            sig += (A[j] - ma) * (A[j] - ma);
        }
        /*
         * 50 dB down is the bar.  Measured: -80 dB on one stage, -62 through
         * three, and that -62 is the running solver's own noise amplified by
         * the stages after it, not the table's - it does not improve when the
         * table is made sixteen times bigger.
         */
        AG_CHECK(10.0 * log10(err / (sig + 1e-30)) < -50.0);
        /* Nothing may run off the edge of a table: that is the model
         * guessing rather than answering. */
        AG_CHECK_INT(ckt_baked_chain_clamped(&baked), 0);
    }

    free(k);
    free(s);
}

/* ------------------------------------------------------------------------ */

/*
 * Antialiasing must not change the sound, only the alias floor.
 *
 * ADAA reports the average of the current over the sample interval instead of
 * its value at the instant.  On a signal that moves slowly compared with the
 * sample rate those two are the same thing, so a low note must come out
 * unchanged; if it does not, what has been built is not an antialiasing filter
 * but a tone control.  Whether it actually suppresses aliasing is a spectral
 * measurement and lives in tools/ckt_smoke.c, where there is an FFT to do it
 * with: -13.4 dB plain against -40.8 dB with ADAA at twice the rate.
 */
static void test_adaa(void)
{
    static float tab[256 * 9 * 2], tabH[256 * 9 * 2];
    ag_ckt_t    *k = (ag_ckt_t *)calloc(1, sizeof(ag_ckt_t));
    ag_stage_t  *s = (ag_stage_t *)calloc(1, sizeof(ag_stage_t));
    const int    out_node = ckt_build_stage_alone(k, FS);
    int          i;

    ag_stage_init(s);
    AG_CHECK(out_node != 0);

    /* Without an antiderivative table there is nothing to switch on, and
     * saying so is better than pretending. */
    AG_CHECK_INT(ag_stage_bake(s, k, out_node, tab, 0, 256, 9, -20.0f, 20.0f,
                               150.0f, 300.0f),
                 0);
    AG_CHECK_INT(ag_stage_set_adaa(s, 1), 0);
    AG_CHECK_INT(s->adaa, 0);

    AG_CHECK_INT(ag_stage_bake(s, k, out_node, tab, tabH, 256, 9, -20.0f, 20.0f,
                               150.0f, 300.0f),
                 0);
    AG_CHECK_INT(ag_stage_set_adaa(s, 1), 1);

    /* The antiderivative must be the integral of the table the lookup reads,
     * node by node - the trapezoid of a straight line, exactly. */
    {
        float worst = 0.0f, scale = 0.0f;
        int   b, a;
        for (b = 0; b < 9; b++) {
            for (a = 1; a < 256; a++) {
                const int   cur = (b * 256 + a) * 2;
                const int   prv = cur - 2;
                const float want =
                    0.5f * s->p1_step * (tab[prv + 1] + tab[cur + 1]);
                const float got = tabH[cur + 1] - tabH[prv + 1];
                const float e = got - want < 0.0f ? want - got : got - want;
                const float h = tabH[cur + 1] < 0.0f ? -tabH[cur + 1]
                                                     : tabH[cur + 1];
                if (e > worst) {
                    worst = e;
                }
                if (h > scale) {
                    scale = h;
                }
            }
        }
        /* Relative to the size of the running total, not absolute: H is a
         * cumulative sum, so a difference of two of its entries carries the
         * rounding of the whole sum, and asking for 1e-9 of an absolute
         * ampere-volt is asking float for digits it does not have. */
        AG_CHECK(worst < 1.0e-6f * scale);
    }

    /* A 110 Hz note: 436 samples per cycle, so the driving point crawls, and
     * the two must agree. */
    {
        static double A[4800], B[4800];
        double        err = 0.0, sig = 0.0, ma = 0.0, mb = 0.0;
        const double  dp = 2.0 * 3.14159265358979 * 110.0 / (double)FS;
        double        ph;
        int           pass;

        for (pass = 0; pass < 2; pass++) {
            ag_stage_set_adaa(s, pass);
            ag_stage_reset(s);
            for (i = 0; i < (int)(FS * 0.5f); i++) {
                ag_stage_tick(s, 0.0f);
            }
            ph = 0.0;
            for (i = 0; i < 4800; i++) {
                const double v = ag_stage_tick(s, (float)(2.0 * sin(ph)));
                if (pass == 0) {
                    A[i] = v;
                    ma += v;
                } else {
                    B[i] = v;
                    mb += v;
                }
                ph += dp;
            }
        }
        ma /= 4800.0;
        mb /= 4800.0;
        /* Same operating point: antialiasing must not shift the bias. */
        AG_CHECK(fabs(ma - mb) < 0.2);
        /*
         * Compared against the plain output delayed by half a sample, because
         * that is what ADAA returns: the average of the current over the
         * interval [n-1, n] is centred at n-1/2.  The delay is inherent to the
         * method and it is the whole of the difference at low frequencies -
         * measured against the undelayed signal it looks like a -43 dB error,
         * which is not an error, it is 0.4 degrees of phase at 110 Hz.
         */
        for (i = 1; i < 4800; i++) {
            const double half = 0.5 * (A[i] + A[i - 1]) - ma;
            const double d = half - (B[i] - mb);
            err += d * d;
            sig += half * half;
        }
        /*
         * -40 dB, and the residue is not an error: what is left after the
         * delay is accounted for is the difference between the average of a
         * curve over an interval and its value in the middle, which is the
         * high-frequency content ADAA exists to remove.  Measured -47.6 dB
         * here, -41.6 without the delay compensation.  Requiring much less
         * than that would be requiring ADAA not to work.
         */
        AG_CHECK(10.0 * log10(err / (sig + 1e-30)) < -40.0);
    }
    AG_CHECK_INT(s->clamped, 0);

    /*
     * The quiet case, which is the one that was broken and which no
     * measurement here was watching.
     *
     * Written as the definition has it - a difference of antiderivatives over
     * the step - antialiasing crackles: twice a cycle the step passes through
     * zero, the numerator loses its digits to cancellation, and what comes out
     * is noise of constant size, which is loudest relative to the music
     * exactly as the music decays.  It was found by listening to a guitar note
     * die away, not by any number in this file, which is why there is now a
     * number in this file.
     *
     * Ten millivolts of drive, and then silence: what the stage emits into
     * that silence has to be far below what it emits into the note.
     */
    {
        double sig = 0.0, quiet = 0.0;
        double ph = 0.0;
        const double dp = 2.0 * 3.14159265358979 * 220.0 / (double)FS;

        ag_stage_set_adaa(s, 1);
        ag_stage_reset(s);
        for (i = 0; i < (int)(FS * 0.5f); i++) {
            ag_stage_tick(s, 0.0f);
        }
        ag_stage_mark_dc(s);
        ag_stage_reset(s);

        for (i = 0; i < 9600; i++) {
            const double v = ag_stage_tick(s, (float)(0.01 * sin(ph)));
            if (i > 4800) {
                sig += v * v;
            }
            ph += dp;
        }
        {
            /* Let the stage settle back to its bias, then listen to nothing. */
            double dc = 0.0;
            for (i = 0; i < 4800; i++) {
                dc = ag_stage_tick(s, 0.0f);
            }
            for (i = 0; i < 9600; i++) {
                const double v = ag_stage_tick(s, 0.0f) - dc;
                quiet += v * v;
            }
        }
        /*
         * 60 dB is a floor with room to spare: the working implementation
         * measures about -95, and the broken one measured about -45.
         */
        AG_CHECK(10.0 * log10((quiet / 9600.0) / (sig / 4800.0 + 1e-30)) <
                 -60.0);
    }

    free(k);
    free(s);
}

/*
 * A tone stack folded into the same matrix as the valve.  The question it
 * answers: do tone controls between stages cost a second solve?  They do not -
 * the baked model does not care how large the linear network around the device
 * is, only how many capacitors it has, because those are the state.
 */
static void test_tonestack_in_stage(void)
{
    ag_ckt_t *k = (ag_ckt_t *)calloc(1, sizeof(ag_ckt_t));
    int       out_node;
    int       i;

    out_node = ckt_build_stage_with_tonestack(k, FS);
    AG_CHECK(out_node != 0);
    /* Three capacitors become six; the valve is still the only device, which
     * is what keeps the baked problem two-dimensional. */
    AG_CHECK_INT(k->nc, 6);
    AG_CHECK_INT(k->nnl, 1);
    AG_CHECK(k->nc <= AG_STAGE_MAX_C);

    /* The valve still biases where it did: the stack hangs off the plate
     * through a capacitor and must not move the operating point. */
    for (i = 0; i < (int)(FS * 0.5f); i++) {
        ag_ckt_tick(k, 0.0f, out_node);
    }
    AG_CHECK(k->x[5 - 1] > 140.0f); /* the plate */
    AG_CHECK(k->x[5 - 1] < 230.0f);

    /*
     * And it is a tone control: the network has to treat 100 Hz and 5 kHz
     * differently, or it is just an attenuator with three capacitors in it.
     */
    {
        const float dc = ag_ckt_tick(k, 0.0f, out_node);
        float       g[2];
        int         band;
        static const double hz[2] = { 100.0, 5000.0 };

        for (band = 0; band < 2; band++) {
            const double dp = 2.0 * 3.14159265358979 * hz[band] / (double)FS;
            double       ph = 0.0;
            float        pk = 0.0f;
            int          n;
            /*
             * A fixed 4800 samples for both, not a fixed number of cycles.
             * Twelve cycles of 5 kHz is 108 samples, and after 100 Hz the
             * coupling capacitors are still carrying the low note - the
             * measurement then reports the tail of the previous band rather
             * than the response to this one, which is how this test first
             * concluded the tone stack was flat.
             */
            for (n = 0; n < 4800; n++) {
                const float y =
                    ag_ckt_tick(k, 0.05f * (float)sin(ph), out_node) - dc;
                const float a = y < 0.0f ? -y : y;
                if (n > 2400 && a > pk) {
                    pk = a;
                }
                ph += dp;
            }
            g[band] = pk / 0.05f;
        }
        AG_CHECK(g[0] > 0.05f);
        AG_CHECK(g[1] > 0.05f);
        /* At least 2 dB apart - a passive stack at noon is gentle, but not
         * flat.  Measured here: 1.55 at 100 Hz against 0.97 at 5 kHz, the
         * scooped middle every such network has. */
        AG_CHECK(g[0] / g[1] > 1.26f || g[1] / g[0] > 1.26f);
    }

    /*
     * Convergence, and this is worth recording rather than hiding.  A tone
     * stack hung on a valve is harder for the running solver than the valve
     * alone: 250 pF next to 22 uF is eight orders of magnitude of companion
     * conductance in one matrix, and about a tenth of the samples hit the
     * iteration limit while the rails charge.  The baked model does not care -
     * it factors that matrix once and never iterates - which is one more
     * reason the baked path is the one that ships.
     */
    AG_CHECK(k->nonconverged < k->samples / 4u);
    free(k);
}

void run_ckt_tests(void)
{
    test_mathf();
    test_linear_rc();
    test_diode_clipper();
    test_triode_jacobian();
    test_triode_bias();
    test_triode_gain_and_asymmetry();
    test_jfet_stage();
    test_linear_fast_path();
    test_chain();
    test_baked_matches_solver();
    test_adaa();
    test_tonestack_in_stage();
}
