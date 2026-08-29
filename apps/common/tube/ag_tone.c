/*
 * ag_tone - see ag_tone.h.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include "ag_tone.h"

#include "ag_ckt.h" /* for AG_CKT_THETA: one integration rule in this tree */

/*
 * Nodes, ground being 0 and the array index one less:
 *
 *   1  input, fed from the source through rsrc
 *   2  the slope node - where the bass and mid capacitors hang
 *   3  the top of the treble pot, behind the treble capacitor
 *   4  the treble pot's wiper, which is the output
 *   5  the top of the bass pot, which is also the bottom of the treble pot
 *   6  the top of the mid pot
 *
 * The wiper being its own node is the whole of the topology.  The netlist in
 * apps/cktbench had it collapsed onto node 5 - two halves of the treble pot
 * stamped between the same pair of nodes, so they read as one 62.5 k resistor
 * and the output came off the bass pot instead - which is a network with no
 * treble control in it at all.  It cost nothing there, because that netlist
 * exists to be *timed*, and a measurement of its response is what found it.
 */
#define TN 6
#define IDX(n) ((n) - 1)

static void stamp_g(double g[TN][TN], int a, int b, double gv)
{
    if (a > 0) {
        g[IDX(a)][IDX(a)] += gv;
    }
    if (b > 0) {
        g[IDX(b)][IDX(b)] += gv;
    }
    if (a > 0 && b > 0) {
        g[IDX(a)][IDX(b)] -= gv;
        g[IDX(b)][IDX(a)] -= gv;
    }
}

/* Gauss with partial pivoting, on a copy: four right-hand sides, so the matrix
 * is factored once and the four solves ride along in the same elimination. */
static int solve4(double a[TN][TN], double b[TN][4])
{
    int i, j, k, p;
    for (i = 0; i < TN; i++) {
        double m = a[i][i] < 0.0 ? -a[i][i] : a[i][i];
        p = i;
        for (j = i + 1; j < TN; j++) {
            const double v = a[j][i] < 0.0 ? -a[j][i] : a[j][i];
            if (v > m) {
                m = v;
                p = j;
            }
        }
        if (m < 1e-18) {
            return -1;
        }
        if (p != i) {
            for (j = 0; j < TN; j++) {
                const double t = a[i][j];
                a[i][j] = a[p][j];
                a[p][j] = t;
            }
            for (j = 0; j < 4; j++) {
                const double t = b[i][j];
                b[i][j] = b[p][j];
                b[p][j] = t;
            }
        }
        for (j = i + 1; j < TN; j++) {
            const double f = a[j][i] / a[i][i];
            if (f == 0.0) {
                continue;
            }
            for (k = i; k < TN; k++) {
                a[j][k] -= f * a[i][k];
            }
            for (k = 0; k < 4; k++) {
                b[j][k] -= f * b[i][k];
            }
        }
    }
    for (i = TN - 1; i >= 0; i--) {
        for (k = 0; k < 4; k++) {
            double s = b[i][k];
            for (j = i + 1; j < TN; j++) {
                s -= a[i][j] * b[j][k];
            }
            b[i][k] = s / a[i][i];
        }
    }
    return 0;
}

void ag_tone_marshall(ag_tone_spec_t *out)
{
    if (out == 0) {
        return;
    }
    out->rsrc = 38.0e3f; /* a 100k plate load against the valve's rp */
    out->rslope = 33.0e3f;
    out->ctreb = 500.0e-12f;
    out->rtreb = 250.0e3f;
    out->cbass = 22.0e-9f;
    out->rbass = 1.0e6f;
    out->cmid = 22.0e-9f;
    out->rmid = 25.0e3f;
    out->rload = 1.0e6f; /* the master volume that follows it */
}

int ag_tone_design(ag_tone_t *t, const ag_tone_spec_t *sp, float fs, float treble,
                   float mid, float bass)
{
    double g[TN][TN], b[TN][4];
    double gsrc;
    int    i, j;

    if (t == 0 || sp == 0 || fs <= 0.0f) {
        return -1;
    }
    treble = treble < 0.0f ? 0.0f : (treble > 1.0f ? 1.0f : treble);
    mid = mid < 0.0f ? 0.0f : (mid > 1.0f ? 1.0f : mid);
    bass = bass < 0.0f ? 0.0f : (bass > 1.0f ? 1.0f : bass);
    t->treble = treble;
    t->mid = mid;
    t->bass = bass;

    for (i = 0; i < TN; i++) {
        for (j = 0; j < TN; j++) {
            g[i][j] = 0.0;
        }
        for (j = 0; j < 4; j++) {
            b[i][j] = 0.0;
        }
    }

    /* The same companion conductance the solver uses, so the two discretisations
     * are the same one rather than two that agree. */
    t->geq[0] = (float)((double)sp->ctreb * (double)fs / (double)AG_CKT_THETA);
    t->geq[1] = (float)((double)sp->cbass * (double)fs / (double)AG_CKT_THETA);
    t->geq[2] = (float)((double)sp->cmid * (double)fs / (double)AG_CKT_THETA);

    /*
     * A pot at either end still has an ohm of track in it, and that floor is
     * physical rather than defensive.
     *
     * Stamping a genuine short instead - 1e-9 ohms, a conductance of 1e9 next to
     * the megohm's 1e-6 - is fifteen orders of magnitude across one matrix.  In
     * the double solve here that is survivable; in ag_ckt, which carries its
     * matrix in float, it is not, and the two implementations disagreed at
     * exactly the two settings where a pot was at an end.  One ohm keeps the
     * conditioning six orders wide, changes no response anybody can measure, and
     * is what the track between the wiper and the end lug actually is.
     */
#define RES(x) ((x) > 1.0 ? (x) : 1.0)
    gsrc = 1.0 / RES((double)sp->rsrc);
    stamp_g(g, 1, 0, gsrc);
    stamp_g(g, 1, 2, 1.0 / RES((double)sp->rslope));
    stamp_g(g, 3, 4, 1.0 / RES((1.0 - (double)treble) * (double)sp->rtreb));
    stamp_g(g, 4, 5, 1.0 / RES((double)treble * (double)sp->rtreb));
    stamp_g(g, 5, 6, 1.0 / RES((double)bass * (double)sp->rbass));
    stamp_g(g, 6, 0, 1.0 / RES((double)mid * (double)sp->rmid));
    stamp_g(g, 4, 0, 1.0 / RES((double)sp->rload));
    /* The capacitors' conductances stamp exactly like resistors. */
    stamp_g(g, 1, 3, (double)t->geq[0]);
    stamp_g(g, 2, 5, (double)t->geq[1]);
    stamp_g(g, 2, 6, (double)t->geq[2]);
#undef RES

    /*
     * Four right-hand sides, one per thing that drives the network: the input
     * through its source conductance, and the three capacitor current sources.
     * A companion source between a and b injects into a and out of b.
     */
    b[IDX(1)][0] = gsrc;
    b[IDX(1)][1] = 1.0;
    b[IDX(3)][1] = -1.0;
    b[IDX(2)][2] = 1.0;
    b[IDX(5)][2] = -1.0;
    b[IDX(2)][3] = 1.0;
    b[IDX(6)][3] = -1.0;

    if (solve4(g, b) != 0) {
        return -1;
    }
    /* What the run actually needs: the three capacitor voltages and the output,
     * as combinations of the four drivers. */
    for (j = 0; j < 4; j++) {
        t->k[0][j] = (float)(b[IDX(1)][j] - b[IDX(3)][j]);
        t->k[1][j] = (float)(b[IDX(2)][j] - b[IDX(5)][j]);
        t->k[2][j] = (float)(b[IDX(2)][j] - b[IDX(6)][j]);
        t->k[3][j] = (float)b[IDX(4)][j];
    }
    return 0;
}

void ag_tone_reset(ag_tone_t *t)
{
    int i;
    if (t == 0) {
        return;
    }
    for (i = 0; i < 3; i++) {
        t->vpre[i] = 0.0f;
        t->ipre[i] = 0.0f;
    }
}

/*
 * Written out rather than looped, and the difference is not decoration.
 *
 * The nested form - four by four with two-dimensional indexing, then two more
 * loops over the capacitors - measured **249 instructions a sample** on the
 * guest under -icount, which is fifteen per multiply-add.  Every one of them
 * recomputes an address, and the loop counters are on the stack.  Flat, with
 * every coefficient loaded once by name, the same arithmetic is what the FPU
 * actually has to do and nothing else.
 *
 * The order is the same: the three companion sources from the previous state,
 * one matrix multiply, then carry the capacitor voltages and currents forward
 * exactly as ag_ckt does at the end of its own tick.
 */
float ag_tone_tick(ag_tone_t *t, float x)
{
    float d1, d2, d3;
    float v0, v1, v2, v3;
    const float *k;

    if (t == 0) {
        return x;
    }
    d1 = t->geq[0] * t->vpre[0] + AG_CKT_THETA_C * t->ipre[0];
    d2 = t->geq[1] * t->vpre[1] + AG_CKT_THETA_C * t->ipre[1];
    d3 = t->geq[2] * t->vpre[2] + AG_CKT_THETA_C * t->ipre[2];

    k = &t->k[0][0];
    v0 = k[0] * x + k[1] * d1 + k[2] * d2 + k[3] * d3;
    v1 = k[4] * x + k[5] * d1 + k[6] * d2 + k[7] * d3;
    v2 = k[8] * x + k[9] * d1 + k[10] * d2 + k[11] * d3;
    v3 = k[12] * x + k[13] * d1 + k[14] * d2 + k[15] * d3;

    t->ipre[0] = t->geq[0] * v0 - d1;
    t->ipre[1] = t->geq[1] * v1 - d2;
    t->ipre[2] = t->geq[2] * v2 - d3;
    t->vpre[0] = v0;
    t->vpre[1] = v1;
    t->vpre[2] = v2;
    return v3;
}
