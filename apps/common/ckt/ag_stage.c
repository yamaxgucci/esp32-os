/*
 * ag_stage - baking a fixed-topology circuit stage into constants and a table.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_stage.h"

#include "ag_mathf.h"

/*
 * Baking runs once, so it can afford to be strict - but not stricter than the
 * arithmetic.  A flat 1e-9 V was the first attempt and it is unreachable: one
 * ulp of a float at 200 V is 1.5e-5 V, so the iteration never stopped and
 * every grid point was reported unconverged while quietly holding an answer
 * good to eight digits.  Relative, at a part in a million, is 2e-4 V on a
 * plate - about 140 dB below the signal there.
 */
#define BAKE_ATOL  1.0e-6f
#define BAKE_RTOL  1.0e-6f
#define BAKE_ITERS 200

/*
 * How many table cells an antialiasing interval may span before the run stops
 * adding the pieces up and uses the cumulative table instead.  At audio rates
 * the interval is a fraction of a cell nearly always; four is generous enough
 * that the cumulative path is reached only on transients, where its division
 * is by a large number and therefore safe.
 */
#define AG_STAGE_WALK_CELLS 4

/* ------------------------------------------------------------------------ */
/* bake                                                                      */
/* ------------------------------------------------------------------------ */

/* Difference of two node voltages out of a solution vector, ground = 0. */
static float node_diff(const float *x, int a, int b)
{
    const float va = a > 0 ? x[a - 1] : 0.0f;
    const float vb = b > 0 ? x[b - 1] : 0.0f;
    return va - vb;
}

/*
 * Solve the two-port problem for one driving point:
 *
 *   v = p + K*i,   i = F(v)
 *
 * by Newton on the residual r(v) = v - p - K*F(v), whose Jacobian is
 * I - K*dF/dv.  Warm-started from `v`, which the caller carries along the grid
 * row so that neighbouring points cost two or three passes.
 */
static int solve_port(const ag_ckt_nl_t *dev, const float K[4], float p1,
                      float p2, float *v1, float *v2, float *i1, float *i2)
{
    int it;

    for (it = 0; it < BAKE_ITERS; it++) {
        float j[4], r1, r2, a11, a12, a21, a22, det, d1, d2;

        ag_ckt_dev_eval(dev, *v1, *v2, i1, i2, j);

        r1 = *v1 - p1 - (K[0] * *i1 + K[1] * *i2);
        r2 = *v2 - p2 - (K[2] * *i1 + K[3] * *i2);

        a11 = 1.0f - (K[0] * j[0] + K[1] * j[2]);
        a12 = -(K[0] * j[1] + K[1] * j[3]);
        a21 = -(K[2] * j[0] + K[3] * j[2]);
        a22 = 1.0f - (K[2] * j[1] + K[3] * j[3]);

        det = a11 * a22 - a12 * a21;
        if (det > -1.0e-30f && det < 1.0e-30f) {
            return it;
        }
        d1 = (r1 * a22 - r2 * a12) / det;
        d2 = (a11 * r2 - a21 * r1) / det;

        /*
         * Bound the step.  Off the edge of the useful range the map is flat
         * to a part in a million and Newton, having nothing to bite on, will
         * happily jump a kilovolt and come back.  This runs once at build
         * time, so a slow approach costs nothing and a wild one costs
         * convergence.
         */
        if (d1 > 20.0f) {
            d1 = 20.0f;
        }
        if (d1 < -20.0f) {
            d1 = -20.0f;
        }
        if (d2 > 50.0f) {
            d2 = 50.0f;
        }
        if (d2 < -50.0f) {
            d2 = -50.0f;
        }

        *v1 -= d1;
        *v2 -= d2;

        {
            const float a1 = d1 < 0.0f ? -d1 : d1;
            const float a2 = d2 < 0.0f ? -d2 : d2;
            const float s1 = *v1 < 0.0f ? -*v1 : *v1;
            const float s2 = *v2 < 0.0f ? -*v2 : *v2;
            if (a1 < BAKE_ATOL + BAKE_RTOL * s1 &&
                a2 < BAKE_ATOL + BAKE_RTOL * s2) {
                ag_ckt_dev_eval(dev, *v1, *v2, i1, i2, 0);
                return it + 1;
            }
        }
    }
    ag_ckt_dev_eval(dev, *v1, *v2, i1, i2, 0);
    return BAKE_ITERS;
}

int ag_stage_bake(ag_stage_t *s, ag_ckt_t *k, int out_node, float *tab_mem,
                  float *tabH_mem, int n1, int n2, float p1_lo, float p1_hi,
                  float p2_lo, float p2_hi)
{
    float rhs[AG_CKT_MAX_UNK];
    float z_const[AG_CKT_MAX_UNK], z_u[AG_CKT_MAX_UNK];
    float z_c[AG_STAGE_MAX_C][AG_CKT_MAX_UNK];
    float c1[AG_CKT_MAX_UNK], c2[AG_CKT_MAX_UNK];
    float K[4];
    int   i, m, iv;
    int   port1a, port1b, port2a, port2b;
    const ag_ckt_nl_t *dev;

    if (s == 0 || k == 0 || !k->built || k->nnl != 1 || tab_mem == 0 ||
        n1 < 2 || n2 < 2 || k->nc > AG_STAGE_MAX_C) {
        return -1;
    }
    dev = &k->nl[0];

    /* Port terminals: (a,c) and (b,c) of the device. */
    port1a = dev->a;
    port1b = dev->c;
    port2a = dev->b;
    port2b = dev->c;

    /* ieq_dc is deliberately left alone: see ag_stage_init. */
    for (i = 0; i < AG_STAGE_MAX_C; i++) {
        s->geq[i] = 0.0f;
        s->ieq[i] = 0.0f;
    }
    s->nc = k->nc;
    for (i = 0; i < k->nc; i++) {
        s->geq[i] = k->c[i].geq;
    }

    if (ag_ckt_factor_g0(k) != 0) {
        return -1;
    }

    /*
     * Six solves of the constant matrix, and that is the whole O(n^3) cost of
     * the model: the DC sources, the input, one per capacitor, and one per
     * device terminal.
     */
    for (i = 0; i < k->n; i++) {
        rhs[i] = k->rhs0[i];
    }
    ag_ckt_apply_g0(k, rhs, z_const);

    for (i = 0; i < k->n; i++) {
        rhs[i] = 0.0f;
    }
    iv = -1;
    for (i = 0; i < k->nv; i++) {
        if (k->v[i].driven) {
            iv = k->nnodes + i;
        }
    }
    if (iv < 0) {
        return -1;
    }
    rhs[iv] = 1.0f;
    ag_ckt_apply_g0(k, rhs, z_u);
    rhs[iv] = 0.0f;

    for (m = 0; m < k->nc; m++) {
        /* A unit companion current, stamped the way ag_ckt_tick stamps it. */
        for (i = 0; i < k->n; i++) {
            rhs[i] = 0.0f;
        }
        if (k->c[m].b > 0) {
            rhs[k->c[m].b - 1] -= 1.0f;
        }
        if (k->c[m].a > 0) {
            rhs[k->c[m].a - 1] += 1.0f;
        }
        ag_ckt_apply_g0(k, rhs, z_c[m]);
    }

    /* A unit current out of port 1's first terminal into its second. */
    for (i = 0; i < k->n; i++) {
        rhs[i] = 0.0f;
    }
    if (port1a > 0) {
        rhs[port1a - 1] -= 1.0f;
    }
    if (port1b > 0) {
        rhs[port1b - 1] += 1.0f;
    }
    ag_ckt_apply_g0(k, rhs, c1);

    for (i = 0; i < k->n; i++) {
        rhs[i] = 0.0f;
    }
    if (port2a > 0) {
        rhs[port2a - 1] -= 1.0f;
    }
    if (port2b > 0) {
        rhs[port2b - 1] += 1.0f;
    }
    ag_ckt_apply_g0(k, rhs, c2);

    K[0] = node_diff(c1, port1a, port1b);
    K[1] = node_diff(c2, port1a, port1b);
    K[2] = node_diff(c1, port2a, port2b);
    K[3] = node_diff(c2, port2a, port2b);
    s->k11 = K[0];
    s->k12 = K[1];
    s->k21 = K[2];
    s->k22 = K[3];

    /* Driving point coefficients. */
    s->p1_const = node_diff(z_const, port1a, port1b);
    s->p1_u = node_diff(z_u, port1a, port1b);
    s->p2_const = node_diff(z_const, port2a, port2b);
    s->p2_u = node_diff(z_u, port2a, port2b);
    for (m = 0; m < k->nc; m++) {
        s->p1_c[m] = node_diff(z_c[m], port1a, port1b);
        s->p2_c[m] = node_diff(z_c[m], port2a, port2b);
    }

    /* Capacitor voltages and the output, as the same kind of sum. */
    for (m = 0; m < k->nc; m++) {
        const int ca = k->c[m].a, cb = k->c[m].b;
        int       q;
        s->cv_const[m] = node_diff(z_const, ca, cb);
        s->cv_u[m] = node_diff(z_u, ca, cb);
        for (q = 0; q < k->nc; q++) {
            s->cv_c[m][q] = node_diff(z_c[q], ca, cb);
        }
        s->cv_i1[m] = node_diff(c1, ca, cb);
        s->cv_i2[m] = node_diff(c2, ca, cb);
    }
    s->out_const = node_diff(z_const, out_node, 0);
    s->out_u = node_diff(z_u, out_node, 0);
    for (m = 0; m < k->nc; m++) {
        s->out_c[m] = node_diff(z_c[m], out_node, 0);
    }
    s->out_i1 = node_diff(c1, out_node, 0);
    s->out_i2 = node_diff(c2, out_node, 0);

    /* The table. */
    s->tab = tab_mem;
    s->tabH = tabH_mem;
    s->adaa = 0;
    s->n1 = n1;
    s->n2 = n2;
    s->p1_lo = p1_lo;
    s->p2_lo = p2_lo;
    s->p1_step = (p1_hi - p1_lo) / (float)(n1 - 1);
    s->p1_step_inv = (float)(n1 - 1) / (p1_hi - p1_lo);
    s->p2_step_inv = (float)(n2 - 1) / (p2_hi - p2_lo);
    s->bake_worst_iters = 0;
    s->bake_nonconverged = 0;

    {
        const float dp1 = (p1_hi - p1_lo) / (float)(n1 - 1);
        const float dp2 = (p2_hi - p2_lo) / (float)(n2 - 1);
        int         a, b;
        for (b = 0; b < n2; b++) {
            const float p2 = p2_lo + dp2 * (float)b;
            /* Warm start each row from its first point rather than from the
             * previous row: the map moves much faster along p1. */
            float v1 = 0.0f, v2 = p2;
            for (a = 0; a < n1; a++) {
                const float p1 = p1_lo + dp1 * (float)a;
                float       i1 = 0.0f, i2 = 0.0f;
                const int   used =
                    solve_port(dev, K, p1, p2, &v1, &v2, &i1, &i2);
                if ((uint32_t)used > s->bake_worst_iters) {
                    s->bake_worst_iters = (uint32_t)used;
                }
                if (used >= BAKE_ITERS) {
                    s->bake_nonconverged++;
                }
                tab_mem[(b * n1 + a) * 2 + 0] = i1;
                tab_mem[(b * n1 + a) * 2 + 1] = i2;
            }
        }

        /*
         * The antiderivative along p1, node by node.  The trapezoid rule is
         * not an approximation here: between two nodes the table is read by
         * linear interpolation, so the thing being integrated *is* a straight
         * line, and the trapezoid is its exact integral.  Getting this wrong -
         * integrating some other curve than the one the lookup will actually
         * follow - would leave the antiderivative and the function slightly
         * inconsistent, which is precisely the error ADAA amplifies by
         * dividing by a small number.
         */
        if (tabH_mem != 0) {
            int b;
            for (b = 0; b < n2; b++) {
                int a;
                tabH_mem[(b * n1) * 2 + 0] = 0.0f;
                tabH_mem[(b * n1) * 2 + 1] = 0.0f;
                for (a = 1; a < n1; a++) {
                    const int cur = (b * n1 + a) * 2;
                    const int prv = (b * n1 + a - 1) * 2;
                    tabH_mem[cur + 0] =
                        tabH_mem[prv + 0] +
                        0.5f * s->p1_step * (tab_mem[prv + 0] + tab_mem[cur + 0]);
                    tabH_mem[cur + 1] =
                        tabH_mem[prv + 1] +
                        0.5f * s->p1_step * (tab_mem[prv + 1] + tab_mem[cur + 1]);
                }
            }
        }
    }

    ag_stage_reset(s);
    return 0;
}

void ag_stage_reset(ag_stage_t *s)
{
    int i;
    if (s == 0) {
        return;
    }
    for (i = 0; i < AG_STAGE_MAX_C; i++) {
        s->ieq[i] = s->ieq_dc[i];
    }
    s->prev_p1 = s->prev_p1_dc;
    s->prev_p2 = s->prev_p2_dc;
    /* The first sample after a reset uses the plain lookup: with no previous
     * driving point there is no interval to average over.  One sample. */
    s->have_prev = 0;
    s->clamped = 0;
    s->samples = 0;
}

void ag_stage_init(ag_stage_t *s)
{
    int i;
    if (s == 0) {
        return;
    }
    for (i = 0; i < AG_STAGE_MAX_C; i++) {
        s->ieq_dc[i] = 0.0f;
        s->ieq[i] = 0.0f;
        s->geq[i] = 0.0f;
    }
    s->nc = 0;
    s->tab = 0;
    s->tabH = 0;
    s->adaa = 0;
    s->have_prev = 0;
    s->prev_p1 = 0.0f;
    s->prev_p2 = 0.0f;
    s->prev_p1_dc = 0.0f;
    s->prev_p2_dc = 0.0f;
    s->clamped = 0;
    s->samples = 0;
    s->bake_worst_iters = 0;
    s->bake_nonconverged = 0;
}

void ag_stage_mark_dc(ag_stage_t *s)
{
    int i;
    if (s == 0) {
        return;
    }
    for (i = 0; i < AG_STAGE_MAX_C; i++) {
        s->ieq_dc[i] = s->ieq[i];
    }
    s->prev_p1_dc = s->last_p1;
    s->prev_p2_dc = s->last_p2;
}

/* ------------------------------------------------------------------------ */
/* run                                                                       */
/* ------------------------------------------------------------------------ */

/*
 * Grid coordinates, clamped to the table, returning whether the clamp did
 * anything.  The caller counts, not this - with antialiasing on, one sample
 * looks the table up at two driving points, and a counter incremented here
 * would report twice as much guessing as there was.
 */
static int cell_of(ag_stage_t *s, float p1, float p2, int *a, float *f1,
                   int *b, float *f2)
{
    float g1 = (p1 - s->p1_lo) * s->p1_step_inv;
    float g2 = (p2 - s->p2_lo) * s->p2_step_inv;
    int   hit_edge = 0;

    if (g1 < 0.0f) {
        g1 = 0.0f;
        hit_edge = 1;
    }
    if (g1 > (float)(s->n1 - 1)) {
        g1 = (float)(s->n1 - 1);
        hit_edge = 1;
    }
    if (g2 < 0.0f) {
        g2 = 0.0f;
        hit_edge = 1;
    }
    if (g2 > (float)(s->n2 - 1)) {
        g2 = (float)(s->n2 - 1);
        hit_edge = 1;
    }
    *a = (int)g1;
    *b = (int)g2;
    if (*a > s->n1 - 2) {
        *a = s->n1 - 2;
    }
    if (*b > s->n2 - 2) {
        *b = s->n2 - 2;
    }
    *f1 = g1 - (float)*a;
    *f2 = g2 - (float)*b;
    return hit_edge;
}

static int lookup(ag_stage_t *s, float p1, float p2, float *i1, float *i2)
{
    int   a, b;
    float f1, f2;
    const int edge = cell_of(s, p1, p2, &a, &f1, &b, &f2);
    {
        const float *r0 = s->tab + (b * s->n1 + a) * 2;
        const float *r1 = r0 + s->n1 * 2;
        const float  i1a = r0[0] + (r0[2] - r0[0]) * f1;
        const float  i1b = r1[0] + (r1[2] - r1[0]) * f1;
        const float  i2a = r0[1] + (r0[3] - r0[1]) * f1;
        const float  i2b = r1[1] + (r1[3] - r1[1]) * f1;
        *i1 = i1a + (i1b - i1a) * f2;
        *i2 = i2a + (i2b - i2a) * f2;
    }
    return edge;
}

/*
 * The average of the table over the interval [pa, pb] of the grid drive, on
 * the p2 slice `pm` - which is what antialiasing wants, and which is not the
 * same thing as a difference of antiderivatives divided by the step.
 *
 * Writing it as (H(pb) - H(pa)) / (pb - pa) is the textbook form and it
 * crackles.  Twice a cycle, at the peaks of the waveform, pb - pa passes
 * through zero; the numerator is then a difference of two nearly equal
 * cumulative integrals, which in single precision leaves two or three
 * significant digits, and the quotient is noise.  It is loudest, relative to
 * the music, exactly when the music is quietest - a listener hears it as
 * crackle that grows as a note decays.
 *
 * The fix is not a smaller epsilon, it is noticing that within one cell the
 * table is a straight line, and the average of a straight line over an
 * interval is its value at the midpoint.  The subtraction and the division
 * both cancel algebraically, which is the only kind of cancelling that costs
 * nothing.  Only when the interval spans a cell boundary is the cumulative
 * form needed at all, and by then the denominator is at least a fraction of a
 * cell, so there is nothing catastrophic left in it.
 */
static int average_over(ag_stage_t *s, float pa, float pb, float pm, float *i1,
                        float *i2)
{
    int   ca, cb, r2a, r2b;
    float fa, fb, f2, f2b;
    int   edge;

    edge = cell_of(s, pa, pm, &ca, &fa, &r2a, &f2);
    edge |= cell_of(s, pb, pm, &cb, &fb, &r2b, &f2b);

    {
        const float *ra0 = s->tab + (r2a * s->n1 + ca) * 2;
        const float *ra1 = ra0 + s->n1 * 2;
        int          k;
        float       *out[2];
        out[0] = i1;
        out[1] = i2;

        if (ca == cb) {
            /* One straight segment: the mean is the midpoint value. */
            const float fm = 0.5f * (fa + fb);
            for (k = 0; k < 2; k++) {
                const float g_i = ra0[k] + (ra1[k] - ra0[k]) * f2;
                const float g_n =
                    ra0[2 + k] + (ra1[2 + k] - ra0[2 + k]) * f2;
                *out[k] = g_i + (g_n - g_i) * fm;
            }
        } else if (cb - ca <= AG_STAGE_WALK_CELLS &&
                   ca - cb <= AG_STAGE_WALK_CELLS) {
            /*
             * A few cells: add the pieces up, cell by cell, and never touch
             * the cumulative table.
             *
             * The cumulative form has the same disease as the naive ADAA and
             * for the same reason, just rarer: when the interval straddles a
             * boundary by a hair, H(pb) - H(pa) is again a difference of
             * nearly equal large numbers over a nearly zero step.  At low
             * level the driving point creeps across boundaries one at a time,
             * so it happens once per crossing - one bad sample each time,
             * which is a click, and a stream of clicks is the crackle that was
             * still there after the same-cell case was fixed.
             *
             * Summing partial cells has no cancellation in it at all: each
             * piece is a positive length times a value from the same segment,
             * and as the interval shrinks the sum shrinks with it, so the
             * quotient stays well behaved.
             */
            const int   lo = ca < cb ? ca : cb;
            const int   hi = ca < cb ? cb : ca;
            const float flo = ca < cb ? fa : fb;
            const float fhi = ca < cb ? fb : fa;
            int         c;
            float       acc[2];
            float       len = 0.0f;

            acc[0] = 0.0f;
            acc[1] = 0.0f;
            for (c = lo; c <= hi; c++) {
                const float  a0 = (c == lo) ? flo : 0.0f;
                const float  a1 = (c == hi) ? fhi : 1.0f;
                const float  w = a1 - a0;
                const float  fm = 0.5f * (a0 + a1);
                const float *r0 = s->tab + (r2a * s->n1 + c) * 2;
                const float *r1 = r0 + s->n1 * 2;
                if (w <= 0.0f) {
                    continue;
                }
                len += w;
                for (k = 0; k < 2; k++) {
                    const float g_i = r0[k] + (r1[k] - r0[k]) * f2;
                    const float g_n =
                        r0[2 + k] + (r1[2 + k] - r0[2 + k]) * f2;
                    acc[k] += w * (g_i + (g_n - g_i) * fm);
                }
            }
            if (len > 0.0f) {
                const float inv = 1.0f / len;
                *out[0] = acc[0] * inv;
                *out[1] = acc[1] * inv;
            } else {
                *out[0] = 0.0f;
                *out[1] = 0.0f;
            }
        } else {
            /*
             * A jump of many cells - a pick attack, or the first sample after
             * a preset change.  Here the cumulative table earns its keep: the
             * step is large, so dividing by it is harmless, and walking fifty
             * cells would not be.
             */
            const float *rb0 = s->tab + (r2b * s->n1 + cb) * 2;
            const float *rb1 = rb0 + s->n1 * 2;
            const float *qa0 = s->tabH + (r2a * s->n1 + ca) * 2;
            const float *qa1 = qa0 + s->n1 * 2;
            const float *qb0 = s->tabH + (r2b * s->n1 + cb) * 2;
            const float *qb1 = qb0 + s->n1 * 2;
            const float  inv = 1.0f / (pb - pa);
            for (k = 0; k < 2; k++) {
                const float ga_i = ra0[k] + (ra1[k] - ra0[k]) * f2;
                const float ga_n =
                    ra0[2 + k] + (ra1[2 + k] - ra0[2 + k]) * f2;
                const float gb_i = rb0[k] + (rb1[k] - rb0[k]) * f2;
                const float gb_n =
                    rb0[2 + k] + (rb1[2 + k] - rb0[2 + k]) * f2;
                const float Ga = qa0[k] + (qa1[k] - qa0[k]) * f2;
                const float Gb = qb0[k] + (qb1[k] - qb0[k]) * f2;
                const float Ha =
                    Ga + s->p1_step * (fa * ga_i + 0.5f * fa * fa * (ga_n - ga_i));
                const float Hb =
                    Gb + s->p1_step * (fb * gb_i + 0.5f * fb * fb * (gb_n - gb_i));
                *out[k] = (Hb - Ha) * inv;
            }
        }
    }
    return edge;
}

int ag_stage_set_adaa(ag_stage_t *s, int on)
{
    if (s == 0 || (on && s->tabH == 0)) {
        return 0;
    }
    s->adaa = (uint8_t)(on ? 1 : 0);
    return 1;
}

float ag_stage_tick(ag_stage_t *s, float vin)
{
    const int nc = s->nc;
    float     p1, p2, i1, i2, out;
    float     cv[AG_STAGE_MAX_C];
    int       m, q;

    p1 = s->p1_const + s->p1_u * vin;
    p2 = s->p2_const + s->p2_u * vin;
    for (m = 0; m < nc; m++) {
        p1 += s->p1_c[m] * s->ieq[m];
        p2 += s->p2_c[m] * s->ieq[m];
    }
#ifdef AG_STAGE_COARSE
    /*
     * A probe, compiled out of every normal build: throw away three more bits
     * of the driving point than float already does and see whether the noise
     * floor follows.  It does - on a note 30 dB down the inharmonic floor went
     * from -67.4 dB to -39.9 dB - and that is the measurement which says the
     * model's low-level noise is the resolution of these two numbers, not the
     * table, not the aliasing and not the convolution after it.
     *
     * Build it with:
     *   gcc -O2 -DAG_STAGE_COARSE=1 ... tools/ckt_play.c ... -o ckt_coarse
     *   ckt_coarse step:1237 0.25 build/tone 2
     */
    p1 = (float)((int32_t)(p1 * 4096.0f)) / 4096.0f;
    p2 = (float)((int32_t)(p2 * 4096.0f)) / 4096.0f;
#endif
    s->last_p1 = p1;
    s->last_p2 = p2;

    if (!s->adaa || !s->have_prev) {
        s->clamped += (uint32_t)lookup(s, p1, p2, &i1, &i2);
    } else {
        /*
         * Both ends of the interval on the same slice of p2, and that "same"
         * is not negotiable.  The obvious saving is to remember this end's
         * value and reuse it next sample as the far end; it does not work,
         * because H is the integral from the bottom of the table, so two p2
         * slices carry two different constants of integration, and their
         * difference then gets divided by the step.  Measured: the four-stage
         * alias figure went from -40.8 dB to -18.8 dB.
         */
        s->clamped += (uint32_t)average_over(s, s->prev_p1, p1,
                                             0.5f * (p2 + s->prev_p2), &i1,
                                             &i2);
    }
    s->prev_p1 = p1;
    s->prev_p2 = p2;
    s->have_prev = 1;

    out = s->out_const + s->out_u * vin + s->out_i1 * i1 + s->out_i2 * i2;
    for (m = 0; m < nc; m++) {
        out += s->out_c[m] * s->ieq[m];
        cv[m] = s->cv_const[m] + s->cv_u[m] * vin + s->cv_i1[m] * i1 +
                s->cv_i2[m] * i2;
    }
    for (m = 0; m < nc; m++) {
        for (q = 0; q < nc; q++) {
            cv[m] += s->cv_c[m][q] * s->ieq[q];
        }
    }

    /*
     * Carry the capacitors.  Trapezoidal, written so that the new companion
     * source comes straight out of the new voltage:
     *   i(t)     = Geq*v(t) - ieq
     *   ieq(t+T) = Geq*v(t) + i(t) = 2*Geq*v(t) - ieq
     */
    for (m = 0; m < nc; m++) {
        s->ieq[m] = (1.0f + AG_CKT_THETA_C) * s->geq[m] * cv[m] -
                    AG_CKT_THETA_C * s->ieq[m];
    }
    s->samples++;
    return out;
}
