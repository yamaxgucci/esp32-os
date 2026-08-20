/*
 * ag_tube - see ag_tube.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_tube.h"

#include "ag_mathf.h"

/* Node numbers of the DC netlist below.  Dense, so that no unknown is left
 * floating - GMIN would carry it, but a node with nothing on it is a mistake
 * waiting rather than a design. */
enum {
    TN_SRC = 1,   /* the sweep source                                      */
    TN_B = 2,     /* B+                                                    */
    TN_COUP = 3,  /* grid side of the coupling capacitor                   */
    TN_GRID = 4,  /* grid, behind the stopper                              */
    TN_PLATE = 5,
    TN_CATH = 6,
    TN_LOADREF = 7 /* the far end of the AC load, held at the quiescent plate */
};

#define TUBE_SETTLE 12 /* Newton warm-up ticks before a number is believed  */

static float absf(float x) { return x < 0.0f ? -x : x; }

/*
 * The stage at DC, with the two capacitors that matter replaced by what they
 * are at audio frequencies rather than at zero.
 *
 * The coupling capacitor is a short: that is what it is above 1.6 Hz, and it
 * is why the grid divider rgrid/(rgrid+rsrc) and the drop across the stopper
 * when grid current flows both end up inside the curve, where they belong.
 * What the capacitor does at DC - refuse to pass the grid current, so that the
 * charge stays on it and drags the grid negative - is the one thing a static
 * curve cannot express, and it is handled as `vbias` in ag_tube_tick_bias.
 *
 * `bypass` holds the cathode at `vk`, which is what the bypass capacitor does
 * above its own corner.  Below it the cathode resistor is local feedback; that
 * is a first-order shelf and it goes in front of the curve, see ag_tube_shelf.
 *
 * `load` hangs rload on the plate referenced to `vpq` rather than to ground -
 * because that is where the output coupling capacitor holds the far end.  It
 * is the difference between the DC load line and the AC one, worth about 15%
 * of gain per stage and 30% over two, so it is not optional.
 */
static int build_dc(ag_ckt_t *k, const ag_tube_spec_t *sp, int bypass, float vk,
                    int load, float vpq)
{
    ag_ckt_init(k, 48000.0f); /* no capacitors here, so fs never gets used */

    if (ag_ckt_add_vin(k, TN_SRC, 0) != 0 ||
        ag_ckt_add_vdc(k, TN_B, 0, sp->vsupply) != 0 ||
        ag_ckt_add_r(k, TN_SRC, TN_COUP, sp->rsrc) != 0 ||
        ag_ckt_add_r(k, TN_COUP, 0, sp->rgrid) != 0 ||
        ag_ckt_add_r(k, TN_COUP, TN_GRID, sp->rstop) != 0 ||
        ag_ckt_add_r(k, TN_B, TN_PLATE, sp->rplate) != 0) {
        return -1;
    }
    if (bypass) {
        if (ag_ckt_add_vdc(k, TN_CATH, 0, vk) != 0) {
            return -1;
        }
    } else {
        if (ag_ckt_add_r(k, TN_CATH, 0, sp->rcath) != 0) {
            return -1;
        }
    }
    if (ag_ckt_add_triode(k, TN_GRID, TN_PLATE, TN_CATH, &sp->valve) != 0) {
        return -1;
    }
    if (load) {
        if (ag_ckt_add_r(k, TN_PLATE, TN_LOADREF, sp->rload) != 0 ||
            ag_ckt_add_vdc(k, TN_LOADREF, 0, vpq) != 0) {
            return -1;
        }
    }
    return ag_ckt_build(k);
}

/* One DC point.  The solver keeps its last solution as the warm start, so a
 * monotone sweep costs one or two Newton passes a point. */
static float dc_at(ag_ckt_t *k, float vin, int node)
{
    return ag_ckt_tick(k, vin, node);
}

static void settle(ag_ckt_t *k, float vin)
{
    int i;
    for (i = 0; i < TUBE_SETTLE; i++) {
        (void)ag_ckt_tick(k, vin, TN_PLATE);
    }
}

/* Small-signal gain at the operating point, by central difference.  One
 * millivolt: large enough that the difference is not float noise on a 130 V
 * node, small enough that the curve has not bent. */
static float slope_at_bias(ag_ckt_t *k)
{
    const float e = 1.0e-3f;
    float      up, dn;
    settle(k, e);
    up = dc_at(k, e, TN_PLATE);
    settle(k, -e);
    dn = dc_at(k, -e, TN_PLATE);
    return (up - dn) / (2.0f * e);
}

void ag_tube_init(ag_tube_t *tb)
{
    int i;
    unsigned char *p = (unsigned char *)tb;
    if (tb == 0) {
        return;
    }
    for (i = 0; i < (int)sizeof(*tb); i++) {
        p[i] = 0;
    }
    tb->seen_lo = 1.0e30f;
    tb->seen_hi = -1.0e30f;
}

void ag_tube_reset(ag_tube_t *tb)
{
    if (tb == 0) {
        return;
    }
    tb->have_prev = 0;
    tb->prev_p = 0.0f;
    tb->clamped = 0;
    tb->samples = 0;
    tb->seen_lo = 1.0e30f;
    tb->seen_hi = -1.0e30f;
    tb->vc = 0.0f;
    tb->vc_peak = 0.0f;
}

int ag_tube_set_adaa(ag_tube_t *tb, int on)
{
    if (tb == 0 || (on && tb->h == 0)) {
        return 0;
    }
    tb->adaa = on ? 1u : 0u;
    tb->have_prev = 0;
    return 1;
}

int ag_tube_set_blocking(ag_tube_t *tb, const ag_tube_spec_t *sp, float fs,
                         int on)
{
    if (tb == 0 || sp == 0 || fs <= 0.0f || (on && tb->g == 0)) {
        return 0;
    }
    tb->vc = 0.0f;
    tb->vc_peak = 0.0f;
    tb->block = on ? 1u : 0u;
    if (!on) {
        return 1;
    }
    if (sp->ccouple <= 0.0f || sp->rgrid <= 0.0f) {
        tb->block = 0;
        return 0;
    }
    tb->blk_k = 1.0f / (fs * sp->ccouple);
    tb->blk_g = 1.0f / (fs * sp->ccouple * sp->rgrid);
    tb->blk_decay = 1.0f / (1.0f + tb->blk_g);
    return 1;
}

/*
 * Fit the axis to the curve.
 *
 * In one dimension this is a property of the curve alone: outside the range
 * where the plate moves, the stage is either cut off or holding the grid with
 * its stopper, and both ends are flat.  So the fit is "where has the curve
 * come within a thousandth of its own extremes", and there is no fixed point
 * to iterate - which is the whole difference from the two-axis bake, where the
 * range depends on the signal, which depends on the tables, which depend on
 * the range (rake 9 in docs/08).
 */
#define TUBE_PROBE 256 /* stack, and this only has to locate a plateau */

/*
 * Fit the axis to the curve, to within `tol` of saturation.
 *
 * The probe is logarithmic on the grid-current side, because that is the side
 * with the long tail: a linear probe fine enough to place a +20 V edge would
 * need thousands of points to reach the +2 kV where the curve finally stops
 * moving, and where it stops moving is what "saturated" has to be measured
 * against.  Cut-off, at the other end, arrives within a few volts.
 */
static void fit_range(ag_ckt_t *k, float tol, float *lo, float *hi)
{
    const int probe = TUBE_PROBE;
    float     v[TUBE_PROBE], u[TUBE_PROBE];
    float     sat, cut, swing, band, a, b;
    int       i;

    for (i = 0; i < probe; i++) {
        /* -40 V to +4000 V, linear below +1 V and geometric above it. */
        const float t = (float)i / (float)(probe - 1);
        u[i] = t < 0.5f ? -40.0f + 82.0f * t
                        : ag_expf(ag_logf(1.0f) + (t - 0.5f) * 2.0f *
                                                      ag_logf(4000.0f));
    }
    settle(k, u[0]);
    for (i = 0; i < probe; i++) {
        v[i] = dc_at(k, u[i], TN_PLATE);
    }
    /*
     * The extremes as measured, not as assumed to be the two end points.
     *
     * The end points are the two extremes for every stage whose cathode is held
     * at its bias, and for those this is the same arithmetic it always was.  It
     * is not true of an unbypassed stage: with 22 k in the cathode and four
     * thousand volts on the grid the operating point is nowhere near anything
     * the valve model was fitted over, Newton gives up, and what comes back is
     * the *cut-off* plate voltage - so `sat` read from the last point equalled
     * `cut`, the swing came out negative, and the whole fit fell through to the
     * +-5 V it uses when a curve has no slope at all.
     *
     * That failure was silent and it was expensive: a cold clipper baked over
     * +-5 V has a table that stops a tenth of the way up its own linear region,
     * so a stage that should have passed one half of the wave nearly untouched
     * clamped both of them.  The run reported 18% of its lookups off the end of
     * the table, which is the only place it showed.
     */
    cut = v[0];
    sat = v[0];
    for (i = 1; i < probe; i++) {
        if (v[i] > cut) {
            cut = v[i];
        }
        if (v[i] < sat) {
            sat = v[i];
        }
    }
    swing = cut - sat;
    if (swing < 1.0f) { /* a curve with no slope at all: keep something usable */
        *lo = -5.0f;
        *hi = 5.0f;
        return;
    }
    band = swing * (tol > 1.0e-5f ? tol : 1.0e-5f);

    a = u[0];
    for (i = 0; i < probe; i++) {
        if (v[i] >= cut - band) {
            a = u[i];
        } else {
            break;
        }
    }
    /*
     * The saturated end, found walking *up* rather than down from the top.
     *
     * On a monotone curve the two give the same answer - once inside the band it
     * stays inside - and this one does not depend on the last probe point being
     * trustworthy.  Where the solver has given up somewhere past saturation, a
     * downward walk stops at the first nonsense and reports the top of the probe
     * as the knee; an upward walk has already found the knee by then.
     */
    b = u[probe - 1];
    for (i = 0; i < probe; i++) {
        if (v[i] <= sat + band) {
            b = u[i];
            break;
        }
    }
    if (b <= a) {
        a = -5.0f;
        b = 5.0f;
    }
    /*
     * A little either side, scaled to that side rather than to the span: the two
     * ends are orders of magnitude apart here, so one margin taken from the
     * whole width would push the cut-off end out to twice where the curve
     * flattens and spend a third of the table on nothing.
     */
    {
        const float ma = a < 0.0f ? -a * 0.06f : 0.2f;
        const float mb = b > 0.0f ? b * 0.06f : 0.2f;
        *lo = a - (ma > 0.2f ? ma : 0.2f);
        *hi = b + (mb > 0.2f ? mb : 0.2f);
    }
}

int ag_tube_bake(ag_tube_t *tb, ag_ckt_t *scratch, const ag_tube_spec_t *sp,
                 float *tab, float *tabh, float *tabg, int n, float lo, float hi,
                 float tol)
{
    float vk, vpq, hmid;
    int   i;

    if (tb == 0 || scratch == 0 || sp == 0 || tab == 0 || n < 8) {
        return -1;
    }

    /*
     * Pass one: where the stage sits with no signal.  Unbypassed and unloaded,
     * because at DC that is what it is - the bypass capacitor carries no
     * current and the output capacitor blocks.
     */
    if (build_dc(scratch, sp, 0, 0.0f, 0, 0.0f) != 0) {
        return -1;
    }
    settle(scratch, 0.0f);
    (void)dc_at(scratch, 0.0f, TN_PLATE);
    vpq = scratch->x[TN_PLATE - 1];
    vk = scratch->x[TN_CATH - 1];
    tb->vq_plate = vpq;
    tb->vq_cath = vk;
    tb->vq_grid = scratch->x[TN_GRID - 1];

    /* The two small-signal gains whose ratio is the cathode shelf.  Taken from
     * the same solver as the curve, so the shelf is measured rather than
     * estimated from mu and rp. */
    if (build_dc(scratch, sp, 0, 0.0f, 1, vpq) != 0) {
        return -1;
    }
    tb->gain_unbypassed = slope_at_bias(scratch);

    if (build_dc(scratch, sp, 1, vk, 1, vpq) != 0) {
        return -1;
    }
    tb->gain_bypassed = slope_at_bias(scratch);

    /*
     * Pass two: the curve itself, on the loaded stage - bypassed if it has a
     * bypass capacitor, and with the cathode resistor in circuit if it has not.
     *
     * The second case is the cold clipper in ag_tube_spec_slo, and it is not a
     * detail.  The whole reason the curve may be swept with the cathode held is
     * that the difference between held and free is a first-order shelf which
     * design() puts in front of it - and with ccath at zero there is no shelf,
     * because there is no capacitor to make one: the cathode follows the signal
     * at *every* frequency.  Sweeping such a stage bypassed anyway would hand
     * back several times its real gain and a knee in the wrong place, and it
     * would look exactly like a working valve, which is the kind of wrong that
     * survives a listening test and gets blamed on something else.
     */
    if (sp->ccath <= 0.0f && build_dc(scratch, sp, 0, 0.0f, 1, vpq) != 0) {
        return -1;
    }
    if (hi <= lo) {
        fit_range(scratch, tol, &lo, &hi);
    }
    tb->n = n;
    tb->step = (hi - lo) / (float)(n - 1);
    /*
     * Move the axis so that zero lands exactly on a grid point.
     *
     * Otherwise the curve at no signal is an interpolation between two
     * neighbours and comes out a fraction of a millivolt off zero - which is a
     * DC offset at the first plate, amplified by sixty into the second grid, and
     * it does not go away because it is not noise.  Silence has to be silence
     * bit for bit, and that is cheap to arrange: shift lo by at most half a
     * step.
     */
    {
        const int i0 = (int)(-lo / tb->step + 0.5f);
        lo = -tb->step * (float)i0;
        hi = lo + tb->step * (float)(n - 1);
        tb->zero_p = (float)i0;
    }
    tb->lo = lo;
    tb->hi = hi;
    /*
     * And the step from the shifted axis, by exactly the formula anything else
     * would use to recover it.
     *
     * The axis travels as three numbers - lo, hi and n - and everything the run
     * does depends on `step`, which is not determined by those three to the last
     * bit: (hi - lo) / (n - 1) in float need not return the step that built hi
     * from lo.  Recomputing it here, rather than keeping the one the shift was
     * measured with, makes the axis a canonical description of itself, so
     * ag_tube_attach reproduces this bake exactly instead of nearly.
     *
     * Found as a preset that would not round-trip: three stages saved and loaded
     * back came out identical for the first two and differed in the last place of
     * the mantissa for the third, whose axis happened not to survive the
     * subtraction.  Nothing was missing from the blob - the two chains simply had
     * step_inv values one ulp apart, which moves every lookup by a millionth of a
     * cell.  Zero still lands on a grid point, because that is carried as an
     * index and not as an offset, so silence stays exact.
     */
    tb->step = (hi - lo) / (float)(n - 1);
    tb->step_inv = 1.0f / tb->step;
    /*
     * Where the walk gives up and the cumulative table takes over.
     *
     * n/128 rather than n/16, and the difference is entirely about cost.  Both
     * paths are safe here - the crossover only has to be wide enough that
     * subtracting two running integrals does not cancel - and at 2048 points the
     * cheap one measures -101.9 dB against walking every cell, which is 45 dB
     * below anything else in this model.  What n/16 bought instead was a walk of
     * up to 128 iterations, and a guitar at a kilohertz slews about ninety cells
     * a sample, so that loop was running nearly every time.
     */
    tb->walk_max = n / 128;

    if (tb->walk_max < AG_TUBE_WALK_MIN) {
        tb->walk_max = AG_TUBE_WALK_MIN;
    }
    if (tb->walk_max > AG_TUBE_WALK_MAX) {
        tb->walk_max = AG_TUBE_WALK_MAX;
    }

    scratch->nonconverged = 0;
    settle(scratch, lo);
    for (i = 0; i < n; i++) {
        tab[i] = dc_at(scratch, lo + tb->step * (float)i, TN_PLATE) - vpq;
        /*
         * The grid current at the same solved point, from the same valve.
         * Taken from the device rather than from the drop across the stopper:
         * the stopper current is a difference of two node voltages that agree to
         * six figures at the operating point, and the valve knows the answer
         * directly.
         */
        if (tabg != 0) {
            ag_triode_op_t op;
            const float    vk_now = scratch->x[TN_CATH - 1];
            ag_triode_eval(&sp->valve, scratch->x[TN_GRID - 1] - vk_now,
                           scratch->x[TN_PLATE - 1] - vk_now, &op);
            tabg[i] = op.ig;
        }
    }
    tb->bake_noncvg = scratch->nonconverged;
    tb->g = tabg;
    /*
     * And take the zero from this sweep rather than from the operating point the
     * other netlist found.  The two agree to Newton's tolerance, which on a
     * 130 V node is a tenth of a millivolt - small, and not zero, and the whole
     * point of the previous paragraph was to make it exactly zero.
     */
    {
        const int i0 = (int)tb->zero_p;
        if (i0 >= 0 && i0 < n) {
            const float off = tab[i0];
            for (i = 0; i < n; i++) {
                tab[i] -= off;
            }
            /*
             * And the same for the grid current, for the same reason and with one
             * addition.  Koren's soft knee does not return exactly zero below
             * conduction - it returns something very small - and "very small" is
             * not zero when it is integrated onto a capacitor for a hundred
             * milliseconds.  What blocking is about is the *change* from the
             * operating point, which already has whatever standing current there
             * is baked into the bias, so the operating point is subtracted; and
             * then it is clamped at zero, because a grid is a diode and cannot
             * pull the capacitor the other way.
             */
            if (tabg != 0) {
                const float goff = tabg[i0];
                tb->g_first = n - 1;
                for (i = 0; i < n; i++) {
                    tabg[i] -= goff;
                    if (tabg[i] < 0.0f) {
                        tabg[i] = 0.0f;
                    }
                }
                /* Where it first becomes nonzero, so the run can skip the
                 * lookup below it.  Interpolation reaches back one cell. */
                for (i = 0; i < n; i++) {
                    if (tabg[i] > 0.0f) {
                        tb->g_first = i > 0 ? i - 1 : 0;
                        break;
                    }
                }
            }
        }
    }
    tb->t = tab;

    /*
     * The antiderivative, in index units so that the step cancels against the
     * span it will be divided by.  Exact for a piecewise-linear curve: the
     * integral over a cell is the average of its two ends.
     *
     * Referenced to the middle of the table rather than to its left edge, which
     * halves the magnitude and so halves the cancellation when two of these are
     * subtracted.  Six decibels for one subtraction at bake time.
     */
    if (tabh != 0) {
        tabh[0] = 0.0f;
        for (i = 1; i < n; i++) {
            tabh[i] = tabh[i - 1] + (tab[i - 1] + tab[i]) * 0.5f;
        }
        hmid = tabh[n / 2];
        for (i = 0; i < n; i++) {
            tabh[i] -= hmid;
        }
        tb->h = tabh;
    } else {
        tb->h = 0;
    }

    ag_tube_reset(tb);
    return 0;
}

/* Value inside cell i at fraction f. */
static float lin(const ag_tube_t *tb, int i, float f)
{
    return tb->t[i] + (tb->t[i + 1] - tb->t[i]) * f;
}

/* The antiderivative at index position p, exact within the cell. */
static float hat(const ag_tube_t *tb, float p)
{
    int   i = (int)p;
    float f;
    if (i > tb->n - 2) {
        i = tb->n - 2;
    }
    f = p - (float)i;
    return tb->h[i] + f * (tb->t[i] + f * (tb->t[i + 1] - tb->t[i]) * 0.5f);
}

/*
 * Integral of the curve from index a to index b, both inside the table, a < b.
 *
 * Three paths, and which one runs is decided by the size of the step rather
 * than by taste.  Inside one cell the curve is a straight line and its average
 * is its midpoint, so both the subtraction and the division cancel
 * algebraically - the only cancellation that costs nothing.  Across a few cells
 * the integral is a sum of positive lengths times values off those same
 * straight lines, so again nothing is subtracted.  Only across many cells is
 * the cumulative table used, and there the denominator is at least
 * AG_TUBE_ADAA_CELLS wide, which is what makes subtracting two running
 * integrals safe.
 *
 * Getting this order wrong is audible and does not show up on silence: it is a
 * noise of constant size, so it grows relative to the music as the music
 * decays, and it was found by ear on a fading note (rake 12 in docs/08).
 */
static float integ_inside(const ag_tube_t *tb, float a, float b)
{
    int   i0 = (int)a, i1 = (int)b, i;
    float acc, fa, fb;

    if (i0 > tb->n - 2) {
        i0 = tb->n - 2;
    }
    if (i1 > tb->n - 2) {
        i1 = tb->n - 2;
    }
    if (i0 == i1) {
        return (b - a) * lin(tb, i0, (a + b) * 0.5f - (float)i0);
    }
    if (i1 - i0 > tb->walk_max && tb->h != 0) {
        return hat(tb, b) - hat(tb, a);
    }
    fa = a - (float)i0;
    fb = b - (float)i1;
    acc = (1.0f - fa) * lin(tb, i0, (fa + 1.0f) * 0.5f);
    for (i = i0 + 1; i < i1; i++) {
        acc += (tb->t[i] + tb->t[i + 1]) * 0.5f;
    }
    acc += fb * lin(tb, i1, fb * 0.5f);
    return acc;
}

/* The curve at index position p, extended flat outside the table. */
static float read_at(const ag_tube_t *tb, float p)
{
    const float last = (float)(tb->n - 1);
    int         i;
    if (p <= 0.0f) {
        return tb->t[0];
    }
    if (p >= last) {
        return tb->t[tb->n - 1];
    }
    i = (int)p;
    return lin(tb, i, p - (float)i);
}

/*
 * The average of the curve over the span the input crossed since last sample -
 * which is what antialiasing is, in one line.  An instant of a clipped
 * waveform carries the corner and a corner is infinite bandwidth; its average
 * over the sample interval is not.
 *
 * Outside the table the curve is flat, and this accounts for that exactly
 * rather than clamping the endpoints and hoping: the parts of the span that lie
 * beyond either end contribute their length times the edge value.  Clamping
 * instead would make the average jump about as the signal crossed the edge,
 * which is a new nonlinearity in the place least able to afford one.
 */
static float span_avg(const ag_tube_t *tb, float pa, float pb, uint32_t *clamped)
{
    const float last = (float)(tb->n - 1);
    float       a = pa < pb ? pa : pb;
    float       b = pa < pb ? pb : pa;
    float       tot = b - a, acc, ia, ib, lenl = 0.0f, lenr = 0.0f;

    if (tot <= 1.0e-7f) {
        if (a < 0.0f || b > last) {
            (*clamped)++;
        }
        return read_at(tb, (pa + pb) * 0.5f);
    }

    /*
     * The span is entirely inside the table, which is the case nearly every
     * sample.  Two things fall away with it, both exactly rather than nearly:
     * every test for the flat extension, and - when the span also stays inside
     * one cell - the division, because there the integral is the length times a
     * value and the length is what it is about to be divided by.  This FPU has
     * no divider, so that cancellation is worth 37 instructions.
     */
    if (a >= 0.0f && b <= last) {
        int i0 = (int)a, i1 = (int)b;
        if (i0 > tb->n - 2) {
            i0 = tb->n - 2;
        }
        if (i1 > tb->n - 2) {
            i1 = tb->n - 2;
        }
        if (i0 == i1) {
            return lin(tb, i0, (a + b) * 0.5f - (float)i0);
        }
        return integ_inside(tb, a, b) / tot;
    }

    if (a < 0.0f) {
        lenl = (b < 0.0f ? b : 0.0f) - a;
    }
    if (b > last) {
        lenr = b - (a > last ? a : last);
    }
    if (lenl > 0.0f || lenr > 0.0f) {
        (*clamped)++;
    }
    ia = a < 0.0f ? 0.0f : (a > last ? last : a);
    ib = b < 0.0f ? 0.0f : (b > last ? last : b);
    acc = lenl * tb->t[0] + lenr * tb->t[tb->n - 1];
    if (ib > ia) {
        acc += integ_inside(tb, ia, ib);
    }
    return acc / tot;
}

/*
 * The lookup, given the input in volts and the same input already converted to
 * an index.  Split out so that blocking, which needs the index for its own
 * table, does not compute it twice.
 */
static float tick_at(ag_tube_t *tb, float x, float p)
{
    float y;

    tb->samples++;
    if (x < tb->seen_lo) {
        tb->seen_lo = x;
    }
    if (x > tb->seen_hi) {
        tb->seen_hi = x;
    }

    if (!tb->adaa) {
        if (p <= 0.0f || p >= (float)(tb->n - 1)) {
            tb->clamped++;
        }
        return read_at(tb, p);
    }
    if (!tb->have_prev) {
        tb->prev_p = p;
        tb->have_prev = 1;
    }
    y = span_avg(tb, tb->prev_p, p, &tb->clamped);
    tb->prev_p = p;
    return y;
}

/* The grid current at index position p, extended flat outside like the curve. */
static float grid_at(const ag_tube_t *tb, float p)
{
    const float last = (float)(tb->n - 1);
    int         i;
    if (p <= 0.0f) {
        return tb->g[0];
    }
    if (p >= last) {
        return tb->g[tb->n - 1];
    }
    i = (int)p;
    return tb->g[i] + (tb->g[i + 1] - tb->g[i]) * (p - (float)i);
}

float ag_tube_tick(ag_tube_t *tb, float v)
{
    float y, u, p, ig;

    if (tb == 0 || tb->t == 0) {
        return 0.0f;
    }
    if (!tb->block) {
        return ag_tube_tick_bias(tb, v, 0.0f);
    }

    /*
     * The capacitor's charge holds the grid down, so what the valve sees is the
     * source minus it - and the index of that is wanted twice, once for the
     * curve and once for the grid current, so it is computed once.
     */
    u = v - tb->vc;
    p = tb->zero_p + u * tb->step_inv;
    y = tick_at(tb, u, p);

    /*
     * Then move the charge:
     *
     *     C dvc/dt = ig(v - vc) - vc/rgrid
     *
     * and note what is *not* in that equation.  The full node equation is
     * C dvc/dt = (v - vc)/rgrid + ig, whose linear part is the coupling
     * capacitor's high pass - and that high pass is already a biquad in F1 and
     * F2, where it belongs.  The equation is linear in vc for a given ig, so the
     * two parts superpose exactly, and taking the whole thing here would model
     * the same capacitor twice: written that way, a DC step ended with the entire
     * step across the capacitor, which is the high pass doing its job and has
     * nothing to do with blocking.  What is left is the extra charge the grid
     * puts there, which is the part a static curve cannot have.
     *
     * The decay is taken implicitly, so it is stable at any rate; the grid
     * current is explicit, and whether that is safe is a question about time
     * constants rather than taste.  The loop the grid current closes is
     * C*(rsrc + rstop + the grid's own resistance): 90 us for V1b's 2.2 nF and
     * 8 ms for V1a's 100 nF, which at four times oversampling is eight samples
     * and seven hundred.  At 1x V1b's is two, which is the edge of where forward
     * Euler rings, so test_tube.c drives a burst into it and checks that the
     * charge climbs monotonically.
     */
    if (p < (float)tb->g_first) {
        /* Below conduction, which is nearly all of the time: nothing is being
         * added, so all that is left is the leak. */
        if (tb->vc != 0.0f) {
            tb->vc *= tb->blk_decay;
        }
        return y;
    }
    ig = grid_at(tb, p);
    tb->vc = (tb->vc + tb->blk_k * ig) * tb->blk_decay;
    if (tb->vc > tb->vc_peak) {
        tb->vc_peak = tb->vc;
    }
    return y;
}

float ag_tube_tick_bias(ag_tube_t *tb, float v, float vbias)
{
    const float x = v + vbias;
    if (tb == 0 || tb->t == 0) {
        return 0.0f;
    }
    return tick_at(tb, x, tb->zero_p + x * tb->step_inv);
}

float ag_tube_curve(const ag_tube_t *tb, float v)
{
    if (tb == 0 || tb->t == 0) {
        return 0.0f;
    }
    return read_at(tb, tb->zero_p + v * tb->step_inv);
}

void ag_tube_shelf(const ag_tube_t *tb, const ag_tube_spec_t *sp, float *f_zero,
                   float *db)
{
    float fz = 0.0f, d = 0.0f;

    if (tb != 0 && sp != 0 && sp->ccath > 0.0f && sp->rcath > 0.0f) {
        const float gb = absf(tb->gain_bypassed);
        const float gu = absf(tb->gain_unbypassed);
        fz = 1.0f / (6.28318531f * sp->rcath * sp->ccath);
        if (gb > 1.0e-6f && gu > 1.0e-6f) {
            /* 20*log10(gu/gb) = 8.68589 * ln(gu/gb) */
            d = 8.6858896f * ag_logf(gu / gb);
        }
    }
    if (f_zero != 0) {
        *f_zero = fz;
    }
    if (db != 0) {
        *db = d;
    }
}

float ag_tube_couple_hz(const ag_tube_spec_t *sp)
{
    float r;
    if (sp == 0 || sp->ccouple <= 0.0f) {
        return 0.0f;
    }
    /*
     * What the capacitor charges through: the impedance in front of it plus the
     * grid leak behind it.  The stopper is not in this - no current flows in it
     * until the grid conducts - and the valve is not either, for the same
     * reason.
     */
    r = sp->rsrc + sp->rgrid;
    return 1.0f / (6.28318531f * sp->ccouple * r);
}

float ag_tube_load_hz(const ag_tube_spec_t *sp)
{
    if (sp == 0 || sp->cload <= 0.0f || sp->rload <= 0.0f) {
        return 0.0f;
    }
    return 1.0f / (6.28318531f * sp->cload * sp->rload);
}

void ag_tube_spec_jcm800(ag_tube_spec_t *out, int index)
{
    if (out == 0) {
        return;
    }
    ag_triode_model_12ax7(&out->valve);
    out->vsupply = 330.0f;
    out->rcath = 820.0f;
    out->ccath = 0.68e-6f;
    out->cload = 22.0e-9f;

    if (index == 0) {
        /*
         * V1a, hot-rodded: 820R cathode instead of the stock 2k7 and a 220k
         * plate load, so this valve clips too instead of handing a big clean
         * signal to the next one.  Two valves clipping do not sound like one
         * clipping twice as hard, because the first one's compression is
         * already in the signal the second one sees.
         *
         * These are the same numbers as ckt_jcm800_stage(0) in
         * apps/cktbench/ckt_circuits.c, and test_tube.c checks that field by
         * field - two copies of a component value is exactly the kind of thing
         * that drifts silently and then gets blamed on the model.
         */
        out->rsrc = 10.0e3f; /* a guitar pickup, roughly */
        out->ccouple = 100.0e-9f;
        out->rgrid = 1.0e6f;
        out->rstop = 68.0e3f;
        out->rplate = 220.0e3f;
        out->rload = 470.0e3f;
    } else {
        /*
         * V1b, driven straight off V1a with no volume control between them,
         * which is what makes a 2203 high gain rather than a Plexi.  The 2.2 nF
         * coupling capacitor is the one value chosen by ear: against the half
         * megohm it sees it corners at 142 Hz, above the open low E and below
         * its second harmonic, so the fundamental does not arrive at the valve
         * that does the clipping and intermodulate with everything above it.
         */
        out->rsrc = 38.0e3f; /* the 100k plate load against the valve's rp */
        out->ccouple = 2.2e-9f;
        out->rgrid = 470.0e3f;
        out->rstop = 1.0e3f;
        out->rplate = 100.0e3f;
        out->rload = 1.0e6f;
    }
}

/*
 * A Bogner crunch channel, built as what it is documented to be: a JCM800 2203
 * with three gain stages instead of two, a cathode follower, and a second valve
 * run much hotter than Marshall ran it.
 *
 * WHERE THIS COMES FROM, and it is a different kind of source again.  The Bogner
 * Ecstasy has no published schematic and the traced ones are photographs; the
 * **Shiva** does, and the amplifier's crunch channel is described in the places
 * that have both schematics as "a standard Marshall 2203 specification with some
 * modifications for higher gain: three gain stages plus a cathode follower, the
 * second gain stage much hotter than a standard 2203".  So the topology and the
 * base values are documented, and this file says which of the three departures
 * from a 2203 is an interpretation of that sentence rather than a number anybody
 * printed:
 *
 *   V1a  the stock 2203 front end: 100 k and 2k7 bypassed.  Documented.
 *   V1b  **220 k and 820 R** - "much hotter" turned into the two values that
 *        make a 12AX7 stage hotter, and the ones the 2203's own hot-rod uses
 *        (ag_tube_spec_jcm800 does exactly this to *its* first valve).  This is
 *        the interpretation.
 *   V2a  a third gain stage, back to 100 k and 2k7.  That the Shiva has one is
 *        documented; its values are not, so they are the amplifier's own standard.
 *
 * Everything else is a 2203: the 68 k stopper and 1 M leak at the input, 2.2 nF
 * interstage couplings against 470 k grid leaks, 0.68 uF across every cathode.
 * The 4.7 nF this model used to carry between the first two valves was invented -
 * argued for, measured, and still invented - and 2.2 nF is what the amplifier it
 * is derived from actually has.
 *
 * The **cathode follower is not modelled**, and what that costs is stated rather
 * than hidden: a follower is a buffer with a gain near one, so leaving it out
 * loses almost nothing in level, but it drives the tone stack from about a
 * kilohm instead of from a plate's forty-eight, and a passive network's response
 * depends on what drives it.  ag_amp_tone_spec sets the stack's source impedance
 * to 1 k for this model for that reason - the buffer is represented by its output
 * impedance, which is the only thing about it the rest of the chain can tell.
 *
 * WHY THREE STAGES.  Measured, on the capture this model is fitted to: 20 dB less
 * into the real amplifier comes back only 1.8 dB quieter, which is **18.2 dB of
 * compression** - within a decibel of the 5150's 18.9.  The two-valve version of
 * this model managed 4.0.  A crunch channel that a listener would call crunch is
 * still a very saturated amplifier, and two valves cannot get there.
 */
void ag_tube_spec_bogner(ag_tube_spec_t *out, int index)
{
    if (out == 0) {
        return;
    }
    ag_triode_model_12ax7(&out->valve);
    out->vsupply = 330.0f;
    out->ccath = 0.68e-6f;
    out->cload = 22.0e-9f;

    if (index <= 0) {
        /* The stock 2203 front end, which is where the Shiva's crunch channel
         * starts: 100 k against 2k7, so this valve has about half the gain of the
         * hot-rodded one in ag_tube_spec_jcm800 and stays mostly linear. */
        out->rplate = 100.0e3f;
        out->rcath = 2.7e3f;
        out->rsrc = 10.0e3f;      /* a guitar pickup, roughly */
        out->ccouple = 100.0e-9f; /* 1.6 Hz into the 1 M leak */
        out->rgrid = 1.0e6f;
        out->rstop = 68.0e3f;
        out->rload = 470.0e3f;
    } else if (index == 1) {
        /*
         * The hot one.  220 k and 820 R is what "much hotter than a standard
         * 2203" is taken to mean, and it is the same pair the tree's own JCM800
         * hot-rod uses: a warmer bias with far more gain, so the valve clips
         * itself instead of handing a big clean signal on.
         */
        out->rplate = 220.0e3f;
        out->rcath = 820.0f;
        out->rsrc = 38.0e3f;    /* the 100k plate load against the valve's rp */
        out->ccouple = 2.2e-9f; /* 142 Hz into 508 k, the 2203's bright cap */
        out->rgrid = 470.0e3f;
        out->rstop = 1.0e3f;
        out->rload = 470.0e3f;
    } else {
        /* The third gain stage, at the amplifier's standard values, driving the
         * cathode follower that this chain does not have. */
        out->rplate = 100.0e3f;
        out->rcath = 2.7e3f;
        out->rsrc = 48.0e3f; /* the 220k plate load against rp */
        out->ccouple = 2.2e-9f;
        out->rgrid = 470.0e3f;
        out->rstop = 1.0e3f;
        out->rload = 1.0e6f;
    }
}



/*
 * The Soldano SLO-100's overdrive channel: four cascaded 12AX7 stages, the third
 * of them cold, with the signal thrown away twice on the way through.
 *
 * WHERE THESE NUMBERS COME FROM.  Two independent public analyses of the SLO-100
 * that agree with each other value for value - ampbooks.com's circuit analysis of
 * the overdrive channel and robrobinette.com's annotated walk through the same
 * amplifier.  That is not a schematic in the tree's own hand the way
 * ckt_jcm800_stage is, so each departure below says what it is; but it is two
 * sources agreeing rather than a family resemblance, which is what the first
 * version of this model had.
 *
 *   stage           plate   cathode   bypass   in       stopper / leak
 *   OD1  V1a        220 k   1k8       1 uF     22 nF    68 k / 1 M
 *   OD2  V1b        100 k   1k8       1 uF     22 nF    470 k series, 500 k pot
 *   OD3  V2a cold   100 k   39 k      none     22 nF    220 k / 330 k
 *   OD4  V2b        220 k   1k8       1 uF     22 nF    220 k / 1 M
 *
 * It is matched against a capture of a **Peavey 5150**, and that is deliberate
 * rather than a compromise: the two amplifiers are the same design to within the
 * cathode capacitors - the forums that have both schematics side by side report
 * identical plate and cathode resistors stage for stage - and the 5150's
 * schematic is a scan while the SLO's analysis is text.  A netlist that can be
 * read beats one that has to be guessed at from a photograph.
 *
 * WHAT MAKES IT A LEAD CHANNEL, and it is not what the first attempt thought.
 *
 * **The attenuation is in the netlist, not in a constant.**  Every stage here is
 * fed through a divider that this model already carries: the source impedance
 * against the grid leak.  Between OD1 and OD2 that is 470 k in series with a
 * 500 k gain pot, so about half the signal reaches the second grid before the pot
 * is even considered; in front of the cold clipper it is 220 k against a 330 k
 * leak, another 0.56.  The first version of this model had no such network and a
 * hand-picked 0.15 in cfg.gain[2] standing in for all of it.
 *
 * **The cold clipper is 39 k unbypassed**, which is where both sources agree and
 * is far colder than the 10 k the previous guess used.  Unbypassed and that cold,
 * the stage has a gain of about two: it is there to clip one side of the wave,
 * not to amplify.
 *
 * **And the gain is enormous anyway** - four stages of 55 to 68 with dividers
 * between them comes out near 95 dB through the chain, against the 2203's 67.
 * That is what a lead channel is, and the dividers are what keep it from being a
 * fuzz.
 *
 * TWO REACTIVE PARTS THAT ARE NOT MODELLED YET, both stated so that nobody has
 * to rediscover them from the fit:
 *
 *   - **1000 pF across the cold clipper's plate load.**  Against 100 k in
 *     parallel with the valve's own plate resistance, about 38 k, that is a
 *     low pass at 4.2 kHz - the SLO's own fizz control, and this chain has no
 *     field for a plate capacitor.
 *   - **2.2 nF across the 470 k series resistor** between OD1 and OD2.  Above
 *     150 Hz the capacitor shorts the resistor, which turns the divider from 0.5
 *     into nearly 1: a first-order treble shelf worth about 5.8 dB.
 *
 * Both currently end up inside the matching filters instead of in the circuit,
 * which is exactly the substitution README.md's first section warns about.  They
 * are the next two things to build, not the next two things to fit.
 */
void ag_tube_spec_slo(ag_tube_spec_t *out, int index)
{
    if (out == 0) {
        return;
    }
    ag_triode_model_12ax7(&out->valve);
    out->vsupply = 330.0f;
    /* The Soldano standard, on every stage but the cold one: 1k8 with a microfarad
     * across it, which corners at 88 Hz - so the bottom octave sees a little less
     * gain than the midrange, on all four valves at once. */
    out->rcath = 1.8e3f;
    out->ccath = 1.0e-6f;
    out->ccouple = 22.0e-9f;
    out->cload = 22.0e-9f;

    if (index <= 0) {
        out->rplate = 220.0e3f;
        out->rsrc = 10.0e3f; /* a guitar pickup, roughly */
        out->rgrid = 1.0e6f;
        out->rstop = 68.0e3f;
        /*
         * What OD1 drives: the 470 k series resistor and the gain pot behind it.
         * 470 k plus the pot's own 500 k against the next grid is what the plate
         * sees, and 820 k is that to one E24 value.
         */
        out->rload = 820.0e3f;
    } else if (index == 1) {
        out->rplate = 100.0e3f;
        /*
         * The 220 k plate load against the valve's rp, and then the 470 k series
         * resistor in front of the gain pot: the source impedance this stage is
         * driven from *is* most of the attenuation, and putting it here rather
         * than in a gain constant is the whole point.  518 k against the pot's
         * 500 k is a divider of 0.49 before the wiper is considered; the wiper at
         * noon is the other half, and it lives in cfg.g12 until the pot is a
         * live control.
         */
        out->rsrc = 518.0e3f;
        out->rgrid = 500.0e3f;
        out->rstop = 1.0e3f;
        out->rload = 330.0e3f; /* the cold clipper's leak */
    } else if (index == 2) {
        /* The cold clipper.  100 k plate, 39 k of cathode with nothing across
         * it, and a 220 k grid stopper into a 330 k leak - which is another
         * divider of 0.56 and also what makes its grid conduct softly. */
        out->rplate = 100.0e3f;
        out->rcath = 39.0e3f;
        out->ccath = 0.0f;
        out->rsrc = 38.0e3f; /* the 100k plate load against rp */
        out->rgrid = 330.0e3f;
        out->rstop = 220.0e3f;
        out->rload = 1.0e6f;
    } else {
        /* OD4, the recovery stage: back to 220 k and the Soldano cathode, behind
         * another 220 k stopper.  Its grid leak is the one value here that
         * neither source states; 1 M is what the position wants and it is marked
         * as unconfirmed for that reason. */
        out->rplate = 220.0e3f;
        out->rsrc = 38.0e3f;
        out->rgrid = 1.0e6f;
        out->rstop = 220.0e3f;
        out->rload = 1.0e6f; /* the tone stack that follows */
    }
}

int ag_tube_attach(ag_tube_t *tb, const float *tab, const float *tabh,
                   const float *tabg, int n, float lo, float hi)
{
    int i;

    if (tb == 0 || tab == 0 || n < 8 || hi <= lo) {
        return -1;
    }
    ag_tube_init(tb);
    tb->t = tab;
    tb->h = tabh;
    tb->g = tabg;
    tb->n = n;
    tb->lo = lo;
    tb->hi = hi;
    tb->step = (hi - lo) / (float)(n - 1);
    tb->step_inv = 1.0f / tb->step;
    /* The bake shifted the axis so that zero lands on a grid point; recover the
     * index rather than the offset, for the reason in the note on zero_p. */
    tb->zero_p = (float)(int)(-lo / tb->step + 0.5f);
    tb->walk_max = n / 128;
    if (tb->walk_max < AG_TUBE_WALK_MIN) {
        tb->walk_max = AG_TUBE_WALK_MIN;
    }
    if (tb->walk_max > AG_TUBE_WALK_MAX) {
        tb->walk_max = AG_TUBE_WALK_MAX;
    }
    tb->g_first = n - 1;
    if (tabg != 0) {
        for (i = 0; i < n; i++) {
            if (tabg[i] > 0.0f) {
                tb->g_first = i > 0 ? i - 1 : 0;
                break;
            }
        }
    }
    return 0;
}

/* Source through a resistor into an antiparallel diode pair: node 1 is driven,
 * node 2 is the clipped output. */
static int build_clipper(ag_ckt_t *k, float rseries, float vf)
{
    /* Shockley: I = Is*(exp(v/nVt) - 1), so the knee sits near nVt*ln(If/Is).
     * Picking Is from the wanted knee keeps `vf` meaning what it says. */
    const float nvt = 0.05f;
    const float is = 1.0e-3f * ag_expf(-vf / nvt);
    ag_ckt_init(k, 48000.0f);
    if (ag_ckt_add_vin(k, 1, 0) != 0 ||
        ag_ckt_add_r(k, 1, 2, rseries > 1.0f ? rseries : 1.0f) != 0 ||
        ag_ckt_add_r(k, 2, 0, 1.0e6f) != 0 || /* a DC path, or node 2 floats */
        ag_ckt_add_diode_pair(k, 2, 0, is, nvt) != 0) {
        return -1;
    }
    return ag_ckt_build(k);
}

int ag_tube_bake_clipper(ag_tube_t *tb, ag_ckt_t *scratch, float rseries,
                         float vf, float gain_match, float *tab, float *tabh,
                         float *tabg, int n, float lo, float hi)
{
    float slope, scale, hmid;
    int   i, i0;

    if (tb == 0 || scratch == 0 || tab == 0 || n < 8 || hi <= lo) {
        return -1;
    }
    if (build_clipper(scratch, rseries, vf) != 0) {
        return -1;
    }
    if (ag_tube_attach(tb, tab, tabh, tabg, n, lo, hi) != 0) {
        return -1;
    }
    settle(scratch, lo);
    for (i = 0; i < n; i++) {
        tab[i] = dc_at(scratch, lo + tb->step * (float)i, 2);
    }
    /* Zero at zero, exactly, for the same reason as the valve curve. */
    i0 = (int)tb->zero_p;
    if (i0 >= 0 && i0 < n) {
        const float off = tab[i0];
        for (i = 0; i < n; i++) {
            tab[i] -= off;
        }
    }
    /*
     * Match the slope, and invert.  A valve stage is inverting and a passive
     * clipper is not, so without the sign the blend would subtract one curve
     * from the other instead of walking between them.
     */
    slope = (i0 > 0 && i0 < n - 1) ? (tab[i0 + 1] - tab[i0 - 1]) /
                                         (2.0f * tb->step)
                                   : 1.0f;
    scale = (slope > 1.0e-9f || slope < -1.0e-9f) ? gain_match / slope : 1.0f;
    for (i = 0; i < n; i++) {
        tab[i] *= scale;
    }
    if (tabh != 0) {
        tabh[0] = 0.0f;
        for (i = 1; i < n; i++) {
            tabh[i] = tabh[i - 1] + (tab[i - 1] + tab[i]) * 0.5f;
        }
        hmid = tabh[n / 2];
        for (i = 0; i < n; i++) {
            tabh[i] -= hmid;
        }
    }
    if (tabg != 0) { /* a diode has no grid to charge anything */
        for (i = 0; i < n; i++) {
            tabg[i] = 0.0f;
        }
    }
    tb->gain_bypassed = gain_match;
    tb->gain_unbypassed = gain_match;
    return 0;
}

void ag_tube_blend(ag_tube_t *dst, const ag_tube_t *a, const ag_tube_t *b,
                   float mix, float *tab, float *tabh, float *tabg)
{
    const float w = mix < 0.0f ? 0.0f : (mix > 1.0f ? 1.0f : mix);
    int         i, n;

    if (dst == 0 || a == 0 || b == 0 || tab == 0 || a->t == 0 || b->t == 0 ||
        a->n != b->n) {
        return;
    }
    n = a->n;
    /*
     * (1-w)*A + w*B rather than A + (B-A)*w.  The second form is the usual one
     * and it is wrong at the ends: with A and B both large and close, A + (B-A)
     * does not come back as B, so the knob never quite reaches either curve.
     * This form is exact at 0 and at 1 by construction.
     */
    {
        const float wa = 1.0f - w;
        for (i = 0; i < n; i++) {
            tab[i] = wa * a->t[i] + w * b->t[i];
            if (tabh != 0) {
                tabh[i] = wa * (a->h != 0 ? a->h[i] : 0.0f) +
                          w * (b->h != 0 ? b->h[i] : 0.0f);
            }
            if (tabg != 0) {
                tabg[i] = wa * (a->g != 0 ? a->g[i] : 0.0f) +
                          w * (b->g != 0 ? b->g[i] : 0.0f);
            }
        }
    }
    /* Everything the lookup derives from the axis is unchanged; only where the
     * grid starts conducting has moved. */
    dst->t = tab;
    dst->h = tabh;
    dst->g = tabg;
    dst->g_first = n - 1;
    if (tabg != 0) {
        for (i = 0; i < n; i++) {
            if (tabg[i] > 0.0f) {
                dst->g_first = i > 0 ? i - 1 : 0;
                break;
            }
        }
    }
}
