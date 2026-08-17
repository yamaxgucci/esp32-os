/*
 * ag_stage - a circuit stage with a fixed topology, baked.
 *
 * This is how a commercial amplifier model is built, and it is not a
 * simplification of the circuit: the answer in the table is the answer the
 * solver in ag_ckt.c gives, computed once instead of eighty thousand times a
 * second.
 *
 * The reasoning, in four steps.
 *
 * 1. Take the nonlinear device out.  What is left - resistors, capacitor
 *    companion conductances, the supply, the input - is a linear network, and
 *    a linear network's answer is a weighted sum of what drives it.
 *
 * 2. So the whole solution is
 *
 *        x = z  +  i1 * c1  +  i2 * c2
 *
 *    where z is the solution with the device removed and c1, c2 are fixed
 *    vectors: the network's response to a unit current injected at each of the
 *    device's terminals.  Both are computed once, at build time, by solving
 *    the constant matrix - which is the only place an O(n^3) factorisation
 *    happens in the whole scheme.
 *
 * 3. The device only ever sees two voltages, and those are two rows of that
 *    same expression:
 *
 *        v1 = p1 + K11*i1 + K12*i2
 *        v2 = p2 + K21*i1 + K22*i2
 *
 *    with K a fixed 2x2.  Together with the device's own i = F(v), that is a
 *    two-dimensional problem, and it depends on nothing but (p1, p2).
 *
 * 4. Therefore the entire nonlinear solve is a function of two numbers, and a
 *    function of two numbers can be tabulated.  At run time the sample costs a
 *    few dozen multiply-adds and one bilinear lookup - no exp, no pow, no
 *    matrix, no Newton.
 *
 * What is given up: the component values are frozen into the tables, so a
 * control that changes the nonlinear part means re-baking.  Controls that live
 * in the linear part - tone, volume, the load - do not, and that is most of
 * them.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_STAGE_H
#define AG_STAGE_H

#include "ag_ckt.h"

/*
 * Capacitors in one stage.  Seven is what a stage with a tone stack and a
 * grid capacitance actually has - coupling, cathode bypass, load, treble,
 * bass, mid, Miller - and the eighth is room to add one without this failing
 * again.  It failed silently the first time: `wide bake failed`, with nothing
 * to say that the netlist had simply outgrown the array.
 *
 * The cost is per capacitor actually present, not per slot: the tick loops
 * run to `nc`.  The struct grows, since cv_c is square in this.
 */
#define AG_STAGE_MAX_C 12

typedef struct ag_stage {
    int nc;

    /* Capacitor companion state, exactly as in the full solver. */
    float geq[AG_STAGE_MAX_C];
    float ieq[AG_STAGE_MAX_C];
    /*
     * The same state at the DC operating point, so that reset puts the stage
     * where a warmed-up amplifier already is.  The full solver has to charge
     * its way there over about half a second of silence, and that transient is
     * both a waste and a trap: it visits driving points the table was never
     * baked for, so the model would spend its first moments guessing.
     */
    float ieq_dc[AG_STAGE_MAX_C];

    /*
     * Driving point, before the device does anything:
     *   p = p_const + p_u * vin + sum_k p_c[k] * ieq[k]
     */
    float p1_const, p1_u, p1_c[AG_STAGE_MAX_C];
    float p2_const, p2_u, p2_c[AG_STAGE_MAX_C];

    /*
     * Everything wanted afterwards - each capacitor's voltage and the output
     * node - as the same kind of sum, plus the two device currents:
     *   o = o_const + o_u * vin + sum_k o_c[k] * ieq[k] + o_i1 * i1 + o_i2 * i2
     *
     * Named coefficients in separate arrays, and that is the fast form, which
     * is not what anyone expects.  Packing all of this into two dense matrices
     * against one vector [1, vin, ieq..., i1, i2] is the tidier design, uses
     * the same number of multiplies, walks memory contiguously - and measured
     * 627 instructions a sample against 412 on the guest, half as fast again.
     * The generic dot product has a run-time trip count, so at -Os it is a
     * real call with a real loop; the named form is straight-line code the
     * compiler keeps in registers.  Tried, measured, reverted - do not tidy
     * this back up without measuring it.
     */
    float cv_const[AG_STAGE_MAX_C], cv_u[AG_STAGE_MAX_C];
    float cv_c[AG_STAGE_MAX_C][AG_STAGE_MAX_C];
    float cv_i1[AG_STAGE_MAX_C], cv_i2[AG_STAGE_MAX_C];

    float out_const, out_u, out_c[AG_STAGE_MAX_C], out_i1, out_i2;

    /* The baked solution: i1 and i2 interleaved, over a p1 x p2 grid. */
    const float *tab;
    int          n1, n2;
    float        p1_lo, p1_step_inv, p1_step;
    float        p2_lo, p2_step_inv;

    /*
     * Antiderivative of the same thing along p1, for antialiasing.  NULL
     * unless a buffer was handed to the bake, in which case ag_stage_set_adaa
     * turns it on.
     *
     * The idea, in one line: instead of asking what the current is at this
     * instant, ask what its average was since the last sample - which is a
     * difference of antiderivatives divided by the step.  A sampled instant of
     * a clipped waveform carries the corner, and the corner is infinite
     * bandwidth; its average over the sample does not.  It buys roughly what
     * doubling or quadrupling the sample rate buys, at a fraction of the cost,
     * because the expensive part - the whole rest of the circuit - still runs
     * once per output sample.
     */
    const float *tabH;
    uint8_t      adaa;
    uint8_t      have_prev;
    float        prev_p1, prev_p2;
    float        prev_p1_dc, prev_p2_dc;

    /* Diagnostics from the bake, and how often the run hits the table edge -
     * a lookup that clamps is a lookup that is guessing.  last_p1/last_p2 are
     * how the table gets sized from measurement rather than from a guess. */
    uint32_t bake_worst_iters;
    uint32_t bake_nonconverged; /* grid points that hit the iteration limit */
    uint32_t clamped;
    uint32_t samples;
    float    last_p1, last_p2;

    /* The 2x2 that couples the device's own currents back into its own
     * terminal voltages.  Kept because it is the thing that explains the
     * shape of the table: K[3] is the plate load line. */
    float k11, k12, k21, k22;
} ag_stage_t;

/*
 * Bake `k` (which must already be built, and have exactly one nonlinear
 * device) into `s`.  `tab_mem` must hold n1 * n2 * 2 floats and must outlive
 * `s`.  The p ranges are in volts at the device's two ports.
 *
 * Returns 0 on success.  Baking factors the constant matrix and leaves the
 * circuit usable, so the same ag_ckt_t can still be ticked afterwards - which
 * is how the host test compares the two against each other.
 */
/*
 * tabH_mem may be NULL; pass a second buffer of the same size to get the
 * antiderivative table as well, which is what ag_stage_set_adaa needs.
 */
int ag_stage_bake(ag_stage_t *s, ag_ckt_t *k, int out_node, float *tab_mem,
                  float *tabH_mem, int n1, int n2, float p1_lo, float p1_hi,
                  float p2_lo, float p2_hi);

/* Returns 0 if there is no antiderivative table to switch on. */
int ag_stage_set_adaa(ag_stage_t *s, int on);

/*
 * Zero everything, including the recorded operating point.  Call once before
 * the first bake.  Re-baking on purpose does *not* clear the operating point,
 * so that a stage can be baked wide, settled to find where it lives, and then
 * re-baked tightly around that without losing the bias it just found.
 */
void ag_stage_init(ag_stage_t *s);

/* Back to the operating point recorded by ag_stage_mark_dc (zeros until it is
 * called), and clear the counters. */
void ag_stage_reset(ag_stage_t *s);

/* Take the state the stage is in now as its operating point.  Call it after
 * running silence through a freshly baked stage. */
void ag_stage_mark_dc(ag_stage_t *s);

float ag_stage_tick(ag_stage_t *s, float vin);

/*
 * Sizing the tables from measurement rather than from a guess is done by
 * running a baked stage and watching last_p1 / last_p2 - see ckt_bake_chain in
 * apps/cktbench/ckt_circuits.c, which also shows why it has to be iterated.
 */

#endif /* AG_STAGE_H */
