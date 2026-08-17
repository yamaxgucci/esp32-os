/*
 * ag_ckt - modified nodal analysis with Newton-Raphson, one sample per solve.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_ckt.h"

#include "ag_mathf.h"

/*
 * Built out entirely unless asked for.  A Newton solve that will not settle
 * cannot be reasoned about from the outside - the only useful thing is the
 * sequence of iterates, and guessing at it instead cost two wrong fixes here
 * before the trace was written.  Compile the host diagnostic with
 * -DAG_CKT_TRACE and set ag_ckt_trace around the sample of interest.
 */
#ifdef AG_CKT_TRACE
#include <stdio.h>
int ag_ckt_trace = 0;
#endif

/*
 * A conductance to ground on every node.  SPICE calls it gmin and it exists
 * for one reason: a node whose only connections are a reverse-biased junction
 * and an open capacitor has no path to ground at all, the matrix is singular,
 * and the solve produces infinities rather than an error.  1e-9 S is 1 GOhm -
 * inaudible, and enough to keep the matrix invertible.
 */
#define GMIN 1.0e-9f

/*
 * Newton stops when no unknown has moved by more than VNTOL + RELTOL*|v|.
 *
 * The relative term is not a nicety.  A flat 100 uV tolerance looks generous
 * until the node in question is a valve plate sitting at 120 V: the plate
 * current is computed through exp and pow, whose single-precision result
 * carries about a part in a million, and a part in a million of 1.5 mA through
 * a 100 k load is several millivolts at the plate.  Newton then wanders around
 * the answer at the millivolt level and never reaches 100 uV, however many
 * iterations it is given - which is what 905 unconverged samples out of 9600
 * looked like, all of them in the loud part.  Asking for more precision than
 * the arithmetic has is not a stricter test, it is a broken one.
 *
 * 1e-4 relative is 80 dB below whatever the node is carrying - on a plate
 * swinging a hundred volts, twenty millivolts - and the absolute floor keeps
 * small-signal nodes honest.  Both are the same idea as SPICE's RELTOL and
 * VNTOL, ten times tighter, because audio is judged by its noise floor and a
 * DC bias point is not.  Tighter than this is not available: single precision
 * carries seven digits, the elimination spends two or three of them on a
 * matrix that spans nine orders of magnitude, and a valve stage then
 * multiplies what is left by its gain.  Asking for more does not get more
 * accuracy - it gets thirty-two iterations instead of three, and the same
 * answer.
 */
#define VNTOL  1.0e-5f
#define RELTOL 1.0e-4f

#define MAX_ITER 32

/*
 * Accumulator type inside the elimination.  Storage stays float either way -
 * this only changes what the multiply-subtract inner loop rounds to.  Build
 * with -DAG_CKT_LU_DOUBLE to answer the question "is single precision enough
 * for this matrix", which for a valve circuit is a real question: a 22 uF
 * cathode bypass stamps 2.1 S next to a 470 k grid stopper's 2.1 uA/V, six
 * orders apart, and the ESP32-S3 has no double-precision FPU - so if the
 * answer were no, the cost of saying yes would be software floating point.
 */
#ifdef AG_CKT_LU_DOUBLE
typedef double ag_acc_t;
#else
typedef float ag_acc_t;
#endif

/*
 * Junction limiting.  exp(v/nVt) with v unbounded overflows to infinity on the
 * first Newton step that overshoots, and from there the solve never returns.
 * Clamping the exponent argument and continuing linearly above it is what SPICE
 * does (pnjlim); the limit is above any voltage a real junction survives, so it
 * changes nothing that is physically reachable.
 */
#define EXP_ARG_MAX 40.0f

/*
 * Width of the grid-conduction knee, in volts.  A real grid-cathode junction
 * is a diode with a thermal voltage around 26 mV; 50 mV is that, rounded up
 * far enough to keep the Newton iteration comfortable and still narrow enough
 * that nothing audible rounds off.
 */
#define KGRID_VT 0.05f

/* Defined with the rest of the linear algebra, below; ag_ckt_build needs them
 * to factor a linear circuit once instead of once per sample. */
static void equilibrate(ag_ckt_t *k, float m[AG_CKT_MAX_UNK][AG_CKT_MAX_UNK]);
static int  lu_factor(float m[AG_CKT_MAX_UNK][AG_CKT_MAX_UNK], int n,
                      int *perm);
static void lu_apply(const ag_ckt_t *k,
                     const float m[AG_CKT_MAX_UNK][AG_CKT_MAX_UNK],
                     const int *perm, const float *rhs, float *x);

void ag_triode_model_12ax7(ag_triode_model_t *m)
{
    if (m == 0) {
        return;
    }
    m->mu = 100.0f;
    m->ex = 1.4f;
    m->kg1 = 1060.0f;
    m->kp = 600.0f;
    m->kvb = 300.0f;
    m->rgk = 2000.0f;
    m->vg0 = 0.33f;
}

void ag_jfet_model_j201(ag_jfet_model_t *m)
{
    if (m == 0) {
        return;
    }
    m->beta = 1.0e-3f;
    m->vto = -0.7f;
    m->lambda = 0.02f;
}

void ag_ckt_init(ag_ckt_t *k, float fs)
{
    int i, j;
    if (k == 0) {
        return;
    }
    for (i = 0; i < AG_CKT_MAX_UNK; i++) {
        k->rhs0[i] = 0.0f;
        k->rhs[i] = 0.0f;
        k->x[i] = 0.0f;
        k->perm[i] = i;
        k->d[i] = 1.0f;
        for (j = 0; j < AG_CKT_MAX_UNK; j++) {
            k->g0[i][j] = 0.0f;
            k->a[i][j] = 0.0f;
        }
    }
    k->fs = fs > 0.0f ? fs : 48000.0f;
    k->n = 0;
    k->nnodes = 0;
    k->nr = 0;
    k->nc = 0;
    k->nv = 0;
    k->nnl = 0;
    k->iters_last = 0;
    k->iters_total = 0;
    k->samples = 0;
    k->nonconverged = 0;
    k->built = 0;
}

static int note_node(ag_ckt_t *k, int n)
{
    if (n < 0 || n > AG_CKT_MAX_NODES) {
        return -1;
    }
    if (n > k->nnodes) {
        k->nnodes = n;
    }
    return 0;
}

int ag_ckt_add_r(ag_ckt_t *k, int a, int b, float ohms)
{
    if (k == 0 || k->nr >= AG_CKT_MAX_R || ohms <= 0.0f) {
        return -1;
    }
    if (note_node(k, a) || note_node(k, b)) {
        return -1;
    }
    k->r[k->nr].a = (uint8_t)a;
    k->r[k->nr].b = (uint8_t)b;
    k->r[k->nr].g = 1.0f / ohms;
    k->nr++;
    return 0;
}

int ag_ckt_add_c(ag_ckt_t *k, int a, int b, float farads)
{
    if (k == 0 || k->nc >= AG_CKT_MAX_C || farads <= 0.0f) {
        return -1;
    }
    if (note_node(k, a) || note_node(k, b)) {
        return -1;
    }
    k->c[k->nc].a = (uint8_t)a;
    k->c[k->nc].b = (uint8_t)b;
    /*
     * Companion model: Geq = C/(theta*T), and the source carries the previous
     * voltage plus (1-theta)/theta of the previous current.  theta = 1/2 is
     * the trapezoidal rule, theta = 1 is backward Euler.
     *
     * Trapezoidal is the accurate one and it is what was here, but its
     * companion recursion has its pole at exactly z = -1: on the unit circle,
     * at Nyquist, marginally stable by construction.  Anything that excites
     * it rings there forever.  In a linear network that is harmless and
     * correct - nothing listens at Nyquist.  Through a valve that is clipping
     * it is neither: the nonlinearity mixes that component straight back down
     * into the audio band.
     *
     * Measured on a guitar take, content at half the oversampled rate: the
     * nearly-linear first stage -85 dB, the clipping second stage -50 dB, and
     * oversampling does not help because the pole simply moves to the new
     * Nyquist.  That 35 dB is the buzz.
     *
     * theta a little over a half puts the pole at -(1-theta)/theta, inside
     * the circle, so the ringing decays.  0.55 gives -0.818, which dies away
     * in about ten samples, and costs the same instructions - only the
     * constants change.  What it buys back is accuracy: the rule is first
     * order rather than second, so the very top of the band is slightly
     * damped.  At 96 kHz that is far above anything a guitar cabinet passes.
     */
    k->c[k->nc].geq = farads * k->fs / AG_CKT_THETA;
    k->c[k->nc].ieq = 0.0f;
    k->c[k->nc].vpre = 0.0f;
    k->c[k->nc].ipre = 0.0f;
    k->nc++;
    return 0;
}

static int add_v(ag_ckt_t *k, int a, int b, float volts, int driven)
{
    if (k == 0 || k->nv >= AG_CKT_MAX_V) {
        return -1;
    }
    if (note_node(k, a) || note_node(k, b)) {
        return -1;
    }
    k->v[k->nv].a = (uint8_t)a;
    k->v[k->nv].b = (uint8_t)b;
    k->v[k->nv].v = volts;
    k->v[k->nv].driven = (uint8_t)driven;
    k->nv++;
    return 0;
}

int ag_ckt_add_vdc(ag_ckt_t *k, int a, int b, float volts)
{
    return add_v(k, a, b, volts, 0);
}

int ag_ckt_add_vin(ag_ckt_t *k, int a, int b) { return add_v(k, a, b, 0.0f, 1); }

int ag_ckt_add_diode_pair(ag_ckt_t *k, int a, int b, float is, float nvt)
{
    if (k == 0 || k->nnl >= AG_CKT_MAX_NL) {
        return -1;
    }
    if (note_node(k, a) || note_node(k, b)) {
        return -1;
    }
    k->nl[k->nnl].kind = AG_NL_DIODE_PAIR;
    k->nl[k->nnl].a = (uint8_t)a;
    k->nl[k->nnl].b = (uint8_t)b;
    k->nl[k->nnl].c = 0;
    k->nl[k->nnl].p0 = is;
    k->nl[k->nnl].p1 = nvt;
    k->nnl++;
    return 0;
}

int ag_ckt_add_triode(ag_ckt_t *k, int g, int p, int cath,
                      const ag_triode_model_t *m)
{
    if (k == 0 || k->nnl >= AG_CKT_MAX_NL || m == 0) {
        return -1;
    }
    if (note_node(k, g) || note_node(k, p) || note_node(k, cath)) {
        return -1;
    }
    k->nl[k->nnl].kind = AG_NL_TRIODE;
    k->nl[k->nnl].a = (uint8_t)g;
    k->nl[k->nnl].b = (uint8_t)p;
    k->nl[k->nnl].c = (uint8_t)cath;
    k->nl[k->nnl].m.triode = *m;
    k->nnl++;
    return 0;
}

int ag_ckt_add_jfet(ag_ckt_t *k, int g, int d, int s, const ag_jfet_model_t *m)
{
    if (k == 0 || k->nnl >= AG_CKT_MAX_NL || m == 0) {
        return -1;
    }
    if (note_node(k, g) || note_node(k, d) || note_node(k, s)) {
        return -1;
    }
    k->nl[k->nnl].kind = AG_NL_JFET;
    k->nl[k->nnl].a = (uint8_t)g;
    k->nl[k->nnl].b = (uint8_t)d;
    k->nl[k->nnl].c = (uint8_t)s;
    k->nl[k->nnl].m.jfet = *m;
    k->nnl++;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* stamping helpers.  Index 0 is ground and is never an unknown, so a node    */
/* number n lives at row n-1.                                                */
/* ------------------------------------------------------------------------ */

#define IDX(n) ((n) - 1)

static void stamp_g(float m[AG_CKT_MAX_UNK][AG_CKT_MAX_UNK], int a, int b,
                    float g)
{
    if (a > 0) {
        m[IDX(a)][IDX(a)] += g;
    }
    if (b > 0) {
        m[IDX(b)][IDX(b)] += g;
    }
    if (a > 0 && b > 0) {
        m[IDX(a)][IDX(b)] -= g;
        m[IDX(b)][IDX(a)] -= g;
    }
}

/* Transconductance: current from p to q controlled by (vc - vd). */
static void stamp_gm(float m[AG_CKT_MAX_UNK][AG_CKT_MAX_UNK], int p, int q,
                     int c, int d, float gm)
{
    if (p > 0 && c > 0) {
        m[IDX(p)][IDX(c)] += gm;
    }
    if (p > 0 && d > 0) {
        m[IDX(p)][IDX(d)] -= gm;
    }
    if (q > 0 && c > 0) {
        m[IDX(q)][IDX(c)] -= gm;
    }
    if (q > 0 && d > 0) {
        m[IDX(q)][IDX(d)] += gm;
    }
}

static void stamp_i(float *rhs, int a, int b, float i)
{
    /* Current i flows from node a to node b through the element. */
    if (a > 0) {
        rhs[IDX(a)] -= i;
    }
    if (b > 0) {
        rhs[IDX(b)] += i;
    }
}

int ag_ckt_build(ag_ckt_t *k)
{
    int i, j, row;

    if (k == 0) {
        return -1;
    }
    k->n = k->nnodes + k->nv;
    if (k->n <= 0 || k->n > AG_CKT_MAX_UNK) {
        return -1;
    }

    for (i = 0; i < AG_CKT_MAX_UNK; i++) {
        k->rhs0[i] = 0.0f;
        for (j = 0; j < AG_CKT_MAX_UNK; j++) {
            k->g0[i][j] = 0.0f;
        }
    }

    for (i = 0; i < k->nnodes; i++) {
        k->g0[i][i] += GMIN;
    }
    for (i = 0; i < k->nr; i++) {
        stamp_g(k->g0, k->r[i].a, k->r[i].b, k->r[i].g);
    }
    for (i = 0; i < k->nc; i++) {
        stamp_g(k->g0, k->c[i].a, k->c[i].b, k->c[i].geq);
    }

    /* Voltage sources: one extra unknown each, its own current. */
    row = k->nnodes;
    for (i = 0; i < k->nv; i++, row++) {
        const int a = k->v[i].a, b = k->v[i].b;
        if (a > 0) {
            k->g0[IDX(a)][row] += 1.0f;
            k->g0[row][IDX(a)] += 1.0f;
        }
        if (b > 0) {
            k->g0[IDX(b)][row] -= 1.0f;
            k->g0[row][IDX(b)] -= 1.0f;
        }
        k->rhs0[row] = k->v[i].v;
    }

    /*
     * A circuit with no nonlinear device has a matrix that never changes:
     * resistors are constants, capacitor companion conductances depend only on
     * the sample rate, and only the right-hand side moves.  Factor it once
     * here and every sample becomes two triangular solves - O(n^2) instead of
     * O(n^3), and no Newton loop at all.  A tone stack is exactly this case,
     * and so is every filter, which is most of what an amplifier is.
     */
    k->linear = (k->nnl == 0) ? 1u : 0u;
    if (k->linear) {
        for (i = 0; i < k->n; i++) {
            for (j = 0; j < k->n; j++) {
                k->a[i][j] = k->g0[i][j];
            }
        }
        equilibrate(k, k->a);
        if (lu_factor(k->a, k->n, k->perm) != 0) {
            return -1;
        }
    }

    ag_ckt_reset(k);
    k->built = 1;
    return 0;
}

void ag_ckt_reset(ag_ckt_t *k)
{
    int i;
    if (k == 0) {
        return;
    }
    for (i = 0; i < k->nc; i++) {
        k->c[i].vpre = 0.0f;
        k->c[i].ipre = 0.0f;
        k->c[i].ieq = 0.0f;
    }
    for (i = 0; i < AG_CKT_MAX_UNK; i++) {
        k->x[i] = 0.0f;
    }
    k->iters_last = 0;
    k->iters_total = 0;
    k->samples = 0;
    k->nonconverged = 0;
}

/* ------------------------------------------------------------------------ */
/* device models                                                             */
/* ------------------------------------------------------------------------ */

/*
 * log(1 + e^x) without the overflow.  Above +20 the one is lost in the float
 * anyway and the answer is x; below -20 the log is the exponential itself.  In
 * between it costs one exp and one log - the two dearest things in this file,
 * which is why the triode is the component that decides the budget.
 */
static float log1p_exp(float x)
{
    if (x > 20.0f) {
        return x;
    }
    if (x < -20.0f) {
        return ag_expf(x);
    }
    return ag_logf(1.0f + ag_expf(x));
}

/* Newton stamp for an antiparallel diode pair across (a, b). */
static void stamp_diode_pair(ag_ckt_t *k, const ag_ckt_nl_t *d, const float *v)
{
    const float va = d->a > 0 ? v[IDX(d->a)] : 0.0f;
    const float vb = d->b > 0 ? v[IDX(d->b)] : 0.0f;
    float       vd = va - vb;
    float       arg, e, ei, id, gd;

    arg = vd / d->p1;
    if (arg > EXP_ARG_MAX) {
        arg = EXP_ARG_MAX;
    }
    if (arg < -EXP_ARG_MAX) {
        arg = -EXP_ARG_MAX;
    }
    e = ag_expf(arg);
    ei = 1.0f / e; /* cheaper than a second exp: 37 instructions against 63 */

    id = d->p0 * (e - ei);
    gd = d->p0 * (e + ei) / d->p1;
    if (gd < GMIN) {
        gd = GMIN;
    }

    stamp_g(k->a, d->a, d->b, gd);
    /* Companion source: the linearisation is i = gd*v + (id - gd*vd). */
    stamp_i(k->rhs, d->a, d->b, id - gd * vd);
}

/*
 * Koren triode.
 *
 *   s  = sqrt(kvb + Vpk^2)
 *   E1 = (Vpk/kp) * log(1 + exp(kp*(1/mu + Vgk/s)))
 *   Ip = 2 * E1^ex / kg1     for E1 > 0
 *
 * The derivatives are analytic rather than finite-difference on purpose: a
 * numerical Jacobian needs a second and third evaluation of exactly this
 * function, and this function is the expensive one.  Note that E1^ex and
 * E1^(ex-1) come out of a single call, since E1^1.4 = E1 * E1^0.4.
 */
/*
 * Koren triode, current and Jacobian.
 *
 *   s  = sqrt(kvb + Vpk^2)
 *   a  = kp*(1/mu + Vgk/s)
 *   E1 = (Vpk/kp) * log(1 + exp(a))
 *   Ip = 2 * E1^ex / kg1     for E1 > 0
 *
 * The derivatives are analytic rather than finite-difference on purpose: a
 * numerical Jacobian needs a second and third evaluation of exactly this
 * function, and this function is the expensive one.  E1^ex and E1^(ex-1) come
 * out of a single ag_powf, since E1^1.4 = E1 * E1^0.4.
 *
 * dE1/dVpk is the term worth writing out.  Vpk appears twice - once as the
 * factor in front, and once inside s - so the chain rule has two pieces:
 *
 *   dE1/dVpk = log(1+exp(a))/kp  -  Vpk^2 * Vgk * sigmoid(a) / s^3
 *
 * The first piece is E1/Vpk written so that it survives Vpk = 0; the second is
 * what a valve's plate resistance is made of.
 */
void ag_triode_eval(const ag_triode_model_t *m, float vgk, float vpk,
                    ag_triode_op_t *out)
{
    float s, inv_s, arg, l, e1, p04, sig;

    out->ip = 0.0f;
    out->dip_dvgk = 0.0f;
    out->dip_dvpk = 0.0f;

    if (vpk < 0.0f) {
        vpk = 0.0f; /* no conduction backwards; a real valve does not either */
    }

    s = ag_sqrtf(m->kvb + vpk * vpk);
    inv_s = 1.0f / s;
    arg = m->kp * (1.0f / m->mu + vgk * inv_s);
    l = log1p_exp(arg);
    e1 = (vpk / m->kp) * l;

    if (e1 > 1.0e-9f) {
        float dip_de1;
        p04 = ag_powf(e1, m->ex - 1.0f); /* E1^0.4, and the only pow here */
        dip_de1 = 2.0f * m->ex * p04 / m->kg1;
        out->ip = 2.0f * (p04 * e1) / m->kg1;
        sig = arg > 20.0f
                  ? 1.0f
                  : (arg < -20.0f ? ag_expf(arg) : 1.0f / (1.0f + ag_expf(-arg)));
        out->dip_dvgk = dip_de1 * (vpk * sig * inv_s);
        {
            const float t1 = l * (1.0f / m->kp);
            const float t2 = vpk * vpk * vgk * sig * inv_s * inv_s * inv_s;
            out->dip_dvpk = dip_de1 * (t1 - t2);
        }
    }

    /*
     * Grid conduction, as a soft knee rather than an `if (vgk > vg0)`.  A hard
     * switch makes a 500 uS conductance appear and vanish between one Newton
     * iteration and the next, so the solve finds the conducting answer, sees
     * that it is below the knee, finds the non-conducting answer, sees that it
     * is above, and never settles.  KGRID_VT wide, the knee has a derivative
     * everywhere.
     */
    {
        const float u = (vgk - m->vg0) * (1.0f / KGRID_VT);
        const float gg = 1.0f / m->rgk;
        const float sg =
            u > 20.0f ? 1.0f
                      : (u < -20.0f ? ag_expf(u) : 1.0f / (1.0f + ag_expf(-u)));
        out->ig = gg * KGRID_VT * log1p_exp(u);
        out->dig_dvgk = gg * sg;
    }
}

static void stamp_triode(ag_ckt_t *k, const ag_ckt_nl_t *t, const float *v)
{
    const ag_triode_model_t *m = &t->m.triode;
    const float vg = t->a > 0 ? v[IDX(t->a)] : 0.0f;
    const float vp = t->b > 0 ? v[IDX(t->b)] : 0.0f;
    const float vk = t->c > 0 ? v[IDX(t->c)] : 0.0f;
    const float vgk = vg - vk;
    float       vpk = vp - vk;
    ag_triode_op_t op;
    float          dip_dvgk, dip_dvpk, ip;

    if (vpk < 0.0f) {
        vpk = 0.0f;
    }
    ag_triode_eval(m, vgk, vpk, &op);
    ip = op.ip;
    dip_dvgk = op.dip_dvgk;
    dip_dvpk = op.dip_dvpk;

    if (dip_dvpk < GMIN) {
        dip_dvpk = GMIN;
    }

    /*
     * Plate current: a conductance plate-to-cathode plus a transconductance
     * driven by the grid, and a companion source carrying whatever the
     * linearisation does not.
     */
    stamp_g(k->a, t->b, t->c, dip_dvpk);
    stamp_gm(k->a, t->b, t->c, t->a, t->c, dip_dvgk);
    stamp_i(k->rhs, t->b, t->c, ip - dip_dvpk * vpk - dip_dvgk * vgk);

    /*
     * Grid conduction.  Above about a third of a volt the grid is a diode to
     * the cathode, and it is what makes a valve stage compress and shift its
     * bias when hit hard - blocking distortion.  A model without it clips
     * symmetrically and sounds like a fuzz pedal.
     */
    {
        const float ggd = op.dig_dvgk < GMIN ? GMIN : op.dig_dvgk;
        stamp_g(k->a, t->a, t->c, ggd);
        stamp_i(k->rhs, t->a, t->c, op.ig - ggd * vgk);
    }
}

/* Shichman-Hodges N-JFET.  No transcendentals at all - this is the cheap one. */
static void stamp_jfet(ag_ckt_t *k, const ag_ckt_nl_t *t, const float *v)
{
    const ag_jfet_model_t *m = &t->m.jfet;
    const float vg = t->a > 0 ? v[IDX(t->a)] : 0.0f;
    const float vd = t->b > 0 ? v[IDX(t->b)] : 0.0f;
    const float vs = t->c > 0 ? v[IDX(t->c)] : 0.0f;
    float       vgs = vg - vs;
    float       vds = vd - vs;
    float       vov = vgs - m->vto;
    float       id = 0.0f, gm = 0.0f, gds = 0.0f;
    int         swapped = 0;

    if (vds < 0.0f) {
        /* Symmetric device: swap drain and source and negate at the end. */
        swapped = 1;
        vgs = vg - vd;
        vds = -vds;
        vov = vgs - m->vto;
    }

    if (vov > 0.0f) {
        const float lam = 1.0f + m->lambda * vds;
        if (vds < vov) { /* triode region */
            id = m->beta * (2.0f * vov * vds - vds * vds) * lam;
            gm = m->beta * 2.0f * vds * lam;
            gds = m->beta * ((2.0f * vov - 2.0f * vds) * lam +
                             (2.0f * vov * vds - vds * vds) * m->lambda);
        } else { /* saturation */
            id = m->beta * vov * vov * lam;
            gm = m->beta * 2.0f * vov * lam;
            gds = m->beta * vov * vov * m->lambda;
        }
    }

    if (gds < GMIN) {
        gds = GMIN;
    }

    {
        const int dn = swapped ? t->c : t->b;
        const int sn = swapped ? t->b : t->c;
        stamp_g(k->a, dn, sn, gds);
        stamp_gm(k->a, dn, sn, t->a, sn, gm);
        stamp_i(k->rhs, dn, sn, id - gds * vds - gm * vgs);
    }
}

/* ------------------------------------------------------------------------ */
/* dense LU with partial pivoting                                            */
/* ------------------------------------------------------------------------ */

/*
 * The power of two nearest to 1/sqrt(x), built by halving the exponent.
 *
 * Equilibration scales are only ever multiplied into the matrix, so a scale
 * that is an exact power of two changes no mantissa bit anywhere: the whole
 * conditioning fix costs nothing in accuracy of its own.  A scale of, say,
 * 289.3 would introduce a rounding error into every element it touched, which
 * is precisely the thing being fixed.
 */
static float pow2_rsqrt(float x)
{
    union {
        float    f;
        uint32_t u;
    } v;
    int e;

    if (!(x > 0.0f)) {
        return 1.0f;
    }
    v.f = x;
    e = (int)((v.u >> 23) & 0xffu) - 127; /* floor(log2 x) */
    e = -(e / 2);                         /* 2^-(e/2) ~ 1/sqrt(x) */
    if (e > 100) {
        e = 100;
    }
    if (e < -100) {
        e = -100;
    }
    v.u = (uint32_t)(e + 127) << 23;
    return v.f;
}

/*
 * Equilibrate k->a in place, filling k->d, and scale k->rhs to match.
 *
 * The comment belongs here rather than at the call site because this is the
 * fix for the defect that cost the most time in this file.
 *
 * A valve circuit's MNA matrix spans nine orders of magnitude - a 22 uF
 * cathode bypass at 48 kHz stamps 2.1 S into one row while a 470 k grid
 * stopper stamps 2.1 uS into another - and the columns are as lopsided as the
 * rows, because a transconductance of 1.4 mS sits in the plate row under the
 * grid column whose own diagonal is 1.2e-5.
 *
 * Row scaling alone (which is what scaled partial pivoting amounts to) is not
 * enough for that.  It leaves the grid unknown resolved to about 1e-4 V, and a
 * stage with a gain of 140 turns 1e-4 V at the grid into 14 mV at the plate -
 * which was exactly the amplitude of the limit cycle Newton sat in on the
 * second valve of a cascade: 190.4436, 190.4479, 190.4508, and round again,
 * for ever.  It looked like a convergence bug and was a conditioning bug.
 *
 * Scaling rows and columns by the same D makes every diagonal about one and
 * leaves the matrix symmetric in scale, so the pivot choice and the
 * elimination both work on numbers that mean the same thing.
 */
static void equilibrate(ag_ckt_t *k, float m[AG_CKT_MAX_UNK][AG_CKT_MAX_UNK])
{
    const int n = k->n;
    int       i, j;

    for (i = 0; i < n; i++) {
        float mx = 0.0f;
        for (j = 0; j < n; j++) {
            const float v = m[i][j] < 0.0f ? -m[i][j] : m[i][j];
            if (v > mx) {
                mx = v;
            }
        }
        k->d[i] = pow2_rsqrt(mx);
    }
    for (i = 0; i < n; i++) {
        const float di = k->d[i];
        for (j = 0; j < n; j++) {
            m[i][j] *= di * k->d[j];
        }
    }
}

/*
 * LU with partial pivoting, in place, keeping the multipliers below the
 * diagonal so the factorisation can be reused on another right-hand side.
 * perm[i] is the original row now sitting at row i.
 */
static int lu_factor(float m[AG_CKT_MAX_UNK][AG_CKT_MAX_UNK], int n, int *perm)
{
    int i, j, p, col;

    for (i = 0; i < n; i++) {
        perm[i] = i;
    }

    for (col = 0; col < n; col++) {
        float best = 0.0f;
        p = col;
        for (i = col; i < n; i++) {
            const float v = m[i][col] < 0.0f ? -m[i][col] : m[i][col];
            if (v > best) {
                best = v;
                p = i;
            }
        }
        if (best < 1.0e-20f) {
            return -1; /* singular: the netlist has an unreachable node */
        }
        if (p != col) {
            const int t = perm[col];
            perm[col] = perm[p];
            perm[p] = t;
            for (j = 0; j < n; j++) {
                const float f = m[col][j];
                m[col][j] = m[p][j];
                m[p][j] = f;
            }
        }
        {
            const ag_acc_t inv = (ag_acc_t)1 / (ag_acc_t)m[col][col];
            for (i = col + 1; i < n; i++) {
                const ag_acc_t f = (ag_acc_t)m[i][col] * inv;
                m[i][col] = (float)f; /* keep L */
                if (f != 0) {
                    for (j = col + 1; j < n; j++) {
                        m[i][j] = (float)((ag_acc_t)m[i][j] -
                                          f * (ag_acc_t)m[col][j]);
                    }
                }
            }
        }
    }
    return 0;
}

/* Solve with an existing factorisation.  rhs is in unscaled units; x comes
 * back in unscaled units too. */
static void lu_apply(const ag_ckt_t *k,
                     const float m[AG_CKT_MAX_UNK][AG_CKT_MAX_UNK],
                     const int *perm, const float *rhs, float *x)
{
    const int n = k->n;
    int       i, j;
    float     b[AG_CKT_MAX_UNK];

    for (i = 0; i < n; i++) {
        b[i] = rhs[perm[i]] * k->d[perm[i]];
    }
    for (i = 1; i < n; i++) {
        ag_acc_t sum = (ag_acc_t)b[i];
        for (j = 0; j < i; j++) {
            sum -= (ag_acc_t)m[i][j] * (ag_acc_t)b[j];
        }
        b[i] = (float)sum;
    }
    for (i = n - 1; i >= 0; i--) {
        ag_acc_t sum = (ag_acc_t)b[i];
        for (j = i + 1; j < n; j++) {
            sum -= (ag_acc_t)m[i][j] * (ag_acc_t)b[j];
        }
        b[i] = (float)(sum / (ag_acc_t)m[i][i]);
    }
    /* Column scale off, back to volts and amps. */
    for (i = 0; i < n; i++) {
        x[i] = b[i] * k->d[i];
    }
}

/* ------------------------------------------------------------------------ */
/* the constant matrix on its own, for building fixed-topology models        */
/* ------------------------------------------------------------------------ */

int ag_ckt_factor_g0(ag_ckt_t *k)
{
    int i, j;

    if (k == 0 || !k->built) {
        return -1;
    }
    for (i = 0; i < k->n; i++) {
        for (j = 0; j < k->n; j++) {
            k->a[i][j] = k->g0[i][j];
        }
    }
    equilibrate(k, k->a);
    return lu_factor(k->a, k->n, k->perm);
}

void ag_ckt_apply_g0(ag_ckt_t *k, const float *rhs, float *x)
{
    if (k == 0 || rhs == 0 || x == 0) {
        return;
    }
    lu_apply(k, k->a, k->perm, rhs, x);
}

int ag_ckt_dev_ports(const ag_ckt_nl_t *d)
{
    return (d != 0 && d->kind == AG_NL_DIODE_PAIR) ? 1 : 2;
}

void ag_ckt_dev_eval(const ag_ckt_nl_t *d, float v1, float v2, float *i1,
                     float *i2, float *j)
{
    if (d == 0) {
        return;
    }

    if (d->kind == AG_NL_TRIODE) {
        ag_triode_op_t op;
        float          vpk = v2 < 0.0f ? 0.0f : v2;
        ag_triode_eval(&d->m.triode, v1, vpk, &op);
        *i1 = op.ig;
        *i2 = op.ip;
        if (j != 0) {
            j[0] = op.dig_dvgk;
            j[1] = 0.0f; /* grid current does not see the plate */
            j[2] = op.dip_dvgk;
            j[3] = v2 < 0.0f ? GMIN : op.dip_dvpk;
        }
        return;
    }

    if (d->kind == AG_NL_JFET) {
        const ag_jfet_model_t *m = &d->m.jfet;
        const float            vov = v1 - m->vto;
        float                  id = 0.0f, gm = 0.0f, gds = 0.0f;
        const float            vds = v2;
        *i1 = 0.0f;
        if (vov > 0.0f && vds > 0.0f) {
            const float lam = 1.0f + m->lambda * vds;
            if (vds < vov) {
                id = m->beta * (2.0f * vov * vds - vds * vds) * lam;
                gm = m->beta * 2.0f * vds * lam;
                gds = m->beta * ((2.0f * vov - 2.0f * vds) * lam +
                                 (2.0f * vov * vds - vds * vds) * m->lambda);
            } else {
                id = m->beta * vov * vov * lam;
                gm = m->beta * 2.0f * vov * lam;
                gds = m->beta * vov * vov * m->lambda;
            }
        }
        *i2 = id;
        if (j != 0) {
            j[0] = GMIN;
            j[1] = 0.0f;
            j[2] = gm;
            j[3] = gds < GMIN ? GMIN : gds;
        }
        return;
    }

    /* Antiparallel diode pair: one port. */
    {
        float arg = v1 / d->p1;
        float e, ei;
        if (arg > EXP_ARG_MAX) {
            arg = EXP_ARG_MAX;
        }
        if (arg < -EXP_ARG_MAX) {
            arg = -EXP_ARG_MAX;
        }
        e = ag_expf(arg);
        ei = 1.0f / e;
        *i1 = d->p0 * (e - ei);
        *i2 = 0.0f;
        if (j != 0) {
            j[0] = d->p0 * (e + ei) / d->p1;
            j[1] = 0.0f;
            j[2] = 0.0f;
            j[3] = 1.0f; /* keeps the 2x2 solve nonsingular for a 1-port */
        }
    }
}

/* ------------------------------------------------------------------------ */

float ag_ckt_tick(ag_ckt_t *k, float vin, int out_node)
{
    int   i, j, it;
    float vprev[AG_CKT_MAX_UNK];

    if (k == 0 || !k->built) {
        return 0.0f;
    }

    /*
     * Capacitor companion sources for this timestep.  Trapezoidal:
     *   i(t) = Geq*v(t) - (Geq*v(t-T) + i(t-T))
     * so the constant part is the companion current source.
     */
    for (i = 0; i < k->nc; i++) {
        k->c[i].ieq = k->c[i].geq * k->c[i].vpre + AG_CKT_THETA_C * k->c[i].ipre;
    }

    /* Linear circuits: the matrix was factored at build time, so this is one
     * pair of triangular solves and no iteration at all. */
    if (k->linear) {
        for (i = 0; i < k->n; i++) {
            k->rhs[i] = k->rhs0[i];
        }
        for (i = 0; i < k->nv; i++) {
            if (k->v[i].driven) {
                k->rhs[k->nnodes + i] = vin;
            }
        }
        for (i = 0; i < k->nc; i++) {
            stamp_i(k->rhs, k->c[i].b, k->c[i].a, k->c[i].ieq);
        }
        lu_apply(k, k->a, k->perm, k->rhs, k->x);
        k->iters_last = 1;
        k->iters_total++;
        k->samples++;
        goto carry_state;
    }

    for (it = 0; it < MAX_ITER; it++) {
        int converged;

        /* Constant stamp, then this iteration's linearisation on top. */
        for (i = 0; i < k->n; i++) {
            k->rhs[i] = k->rhs0[i];
            for (j = 0; j < k->n; j++) {
                k->a[i][j] = k->g0[i][j];
            }
        }
        for (i = 0; i < k->nv; i++) {
            if (k->v[i].driven) {
                k->rhs[k->nnodes + i] = vin;
            }
        }
        for (i = 0; i < k->nc; i++) {
            stamp_i(k->rhs, k->c[i].b, k->c[i].a, k->c[i].ieq);
        }
        for (i = 0; i < k->nnl; i++) {
            switch (k->nl[i].kind) {
            case AG_NL_TRIODE:
                stamp_triode(k, &k->nl[i], k->x);
                break;
            case AG_NL_JFET:
                stamp_jfet(k, &k->nl[i], k->x);
                break;
            default:
                stamp_diode_pair(k, &k->nl[i], k->x);
                break;
            }
        }

        /*
         * Solve A*x = b directly and take x as the next iterate.
         *
         * The residual form - solve A*dx = b - A*x and add - was tried here
         * and is worse, which is worth recording because it is the textbook
         * move and the reasoning for it sounds right: b is dominated by
         * capacitor companion currents (a 22 uF cathode bypass at 48 kHz
         * contributes 2.5 A of them) while the correction being looked for is
         * a millivolt, so subtracting first ought to keep the digits.  It does
         * not, because the subtraction itself happens in the same single
         * precision, and iterative refinement only buys anything when the
         * residual is computed more accurately than the solve.  Measured:
         * 57 unconverged samples out of 4800 direct, 271 in residual form, and
         * 1184 with the residual accumulated in double - the last of which
         * says the remaining failures are a genuine limit cycle in the model,
         * not lost precision, since more precision made them sharper.
         */
        for (i = 0; i < k->n; i++) {
            vprev[i] = k->x[i];
        }
        equilibrate(k, k->a);
        if (lu_factor(k->a, k->n, k->perm) != 0) {
            break;
        }
        /* equilibrate scales the matrix only; lu_apply takes the right-hand
         * side in its original units and does the row scaling itself, so
         * k->rhs goes in as the stamping left it. */
        lu_apply(k, k->a, k->perm, k->rhs, k->x);
        converged = 1;
        {
#ifdef AG_CKT_TRACE
            int worst_i = -1;
#endif
            for (i = 0; i < k->n; i++) {
                float d = k->x[i] - vprev[i];
                float v = k->x[i];
                if (d < 0.0f) {
                    d = -d;
                }
                if (v < 0.0f) {
                    v = -v;
                }
                if (d > VNTOL + RELTOL * v) {
                    converged = 0;
#ifdef AG_CKT_TRACE
                    worst_i = i;
#else
                    break;
#endif
                }
            }
#ifdef AG_CKT_TRACE
            if (ag_ckt_trace) {
                printf("    it %2d conv %d blocker %d (d %.3e tol %.3e)  x:", it,
                       converged, worst_i,
                       worst_i >= 0
                           ? (double)(k->x[worst_i] - vprev[worst_i])
                           : 0.0,
                       worst_i >= 0 ? (double)(VNTOL +
                                               RELTOL * (k->x[worst_i] < 0
                                                             ? -k->x[worst_i]
                                                             : k->x[worst_i]))
                                    : 0.0);
                for (i = 0; i < k->n; i++) {
                    printf(" %10.4f", (double)k->x[i]);
                }
                printf("\n");
            }
#endif
        }

        k->iters_total++;
        if (converged) {
            it++;
            break;
        }
    }

    k->iters_last = (uint32_t)it;
    k->samples++;
    if (it >= MAX_ITER) {
        k->nonconverged++;
    }

carry_state:
    /* Carry the capacitor states forward. */
    for (i = 0; i < k->nc; i++) {
        const float va = k->c[i].a > 0 ? k->x[IDX(k->c[i].a)] : 0.0f;
        const float vb = k->c[i].b > 0 ? k->x[IDX(k->c[i].b)] : 0.0f;
        const float vnow = va - vb;
        const float inow = k->c[i].geq * vnow - k->c[i].ieq;
        k->c[i].vpre = vnow;
        k->c[i].ipre = inow;
    }

    if (out_node <= 0 || out_node > k->nnodes) {
        return 0.0f;
    }
    return k->x[IDX(out_node)];
}
