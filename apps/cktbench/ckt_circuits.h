/*
 * The circuits the benchmark costs, so that the same netlists can be built by
 * the host tests and by the guest bench without either copying the other.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CKT_CIRCUITS_H
#define CKT_CIRCUITS_H

#include "ag_ckt.h"
#include "ag_stage.h"

/* Node numbers shared by every circuit below. */
enum {
    CKT_N_IN = 1,     /* the source                                       */
    CKT_N_SUPPLY = 2, /* B+ rail                                          */
    CKT_N_STAGE0 = 3  /* first valve stage: grid, plate, cathode          */
};

/*
 * Four nodes per valve stage: the coupling node the previous plate drives, the
 * grid behind its stopper, the plate, and the cathode.
 */
#define CKT_COUPLE(s)  (CKT_N_STAGE0 + 4 * (s) + 0)
#define CKT_GRID(s)    (CKT_N_STAGE0 + 4 * (s) + 1)
#define CKT_PLATE(s)   (CKT_N_STAGE0 + 4 * (s) + 2)
#define CKT_CATHODE(s) (CKT_N_STAGE0 + 4 * (s) + 3)

/*
 * A cascade of `stages` 12AX7 gain stages, each one:
 *
 *   previous plate -| |- 22 nF -> 470 k -> grid, 100 k grid to ground
 *   100 k plate load from the 250 V rail, 1k5 cathode with a bypass
 *
 * The 470 k / 100 k pair is both the grid stopper and the volume divider that
 * sits between stages in every real amplifier, and leaving it out is not a
 * simplification - a bare cascade of gain-60 stages puts sixty volts on the
 * next grid, which is not a circuit anybody built and not one Newton enjoys.
 *
 * Returns the output node, or 0 if it did not fit.
 */
int ckt_build_preamp(ag_ckt_t *k, float fs, int stages, float bypass_uf);

/*
 * The same preamp, but solved one stage at a time.
 *
 * Cascading valve stages into a single matrix is what a circuit simulator
 * does and it is the wrong thing to do here, for a reason that is about
 * arithmetic rather than about circuits: the stages multiply, so whatever
 * error the solver has at the first grid comes out of the last plate
 * multiplied by the total gain of the chain.  At about 25x per stage, single
 * precision has spent its seven digits by the third valve, and Newton stops
 * converging - measured, 2883 samples out of 4800 at three stages.
 *
 * Solving stage by stage is not a shortcut around that; it is what the
 * topology already says.  Each stage is coupled to the next through a
 * capacitor into a high-impedance grid, so the only thing that crosses the
 * boundary is a voltage.  Each stage carries a lumped copy of the next one's
 * input network as its load, so the loading is not lost, and each solve is
 * over eight unknowns instead of twenty-eight - which is also, incidentally,
 * an order of magnitude cheaper, because the solve is cubic in that number.
 */
/*
 * Eight, because the tables are what cost memory and eight of them assume a
 * machine with PSRAM.  Overridable for one that has none: the ESP32 board this
 * project measures on has 320 KB of internal RAM in total, and the baked
 * tables alone are 70 KB per stage at the default resolution.  Three stages at
 * CKT_BAKE_N1=128 is the amplifier this benchmark exists to time, and it fits.
 */
#ifndef CKT_MAX_CHAIN
#define CKT_MAX_CHAIN 8
#endif

typedef struct ckt_chain {
    ag_ckt_t stage[CKT_MAX_CHAIN];
    int      out[CKT_MAX_CHAIN];
    int      n;
} ckt_chain_t;

/* One stage of the chain on its own, for baking and for comparison. */
int ckt_build_stage_alone(ag_ckt_t *k, float fs);

/*
 * The same stage with a Marshall tone stack inside the same matrix, to answer
 * whether tone controls between stages cost extra.  Six capacitors instead of
 * three; no second solve.
 */
int ckt_build_stage_with_tonestack(ag_ckt_t *k, float fs);

/* ------------------------------------------------------------------------ */
/* A named valve stage, so that a real amplifier can be written down          */
/* ------------------------------------------------------------------------ */

/*
 * One triode stage with everything around it, as a list of component values.
 *
 * Two of these describe the front end of a JCM800, which is what a two-stage
 * crunch is: the values are the whole difference between the first valve and
 * the second, and writing them as a struct rather than as two nearly identical
 * functions keeps that difference visible.
 *
 * `rsrc` and `rload` are how the stages are joined.  Each stage is driven by
 * an ideal source through `rsrc` - the output impedance of the stage in front
 * of it, which for a 12AX7 with a 100k plate load is about 38k - and each
 * carries `rload` through its own output capacitor as a stand-in for the input
 * impedance of the stage behind it.  So the coupling capacitor between two
 * stages appears twice, once in each, doing two different jobs: loading the
 * one in front, passing signal to the one behind.  Without `rsrc` the grid of
 * the second valve would be driven by something with no impedance at all, and
 * grid current - which is what blocking distortion is made of - would cost it
 * nothing.
 */
typedef struct ckt_stage_spec {
    float rsrc;    /* output impedance of whatever drives this stage    */
    float ccouple; /* input coupling capacitor                          */
    float rgrid;   /* grid leak                                         */
    float rstop;   /* grid stopper; use a small value, not zero         */
    float vsupply; /* B+                                                */
    float rplate;
    float rcath;
    float ccath; /* cathode bypass; 0 for unbypassed                    */
    float cload; /* output coupling capacitor                           */
    float rload; /* input impedance of whatever this stage drives       */
    /*
     * Capacitance from the grid side of the coupling capacitor to ground: the
     * Miller capacitance of this valve plus the stray of the wiring that
     * feeds it.  It was missing entirely, which gave every stage a bandwidth
     * no valve has, and the second one in a chain is then asked to clip a
     * signal carrying pick noise to 15 kHz.
     *
     * Against `rsrc` it is a first order low pass at 1/(2*pi*rsrc*cmiller).
     * Physically a hot 12AX7 stage is 100-200 pF, which is a corner up around
     * 30 kHz; larger values are a treble bleed rather than a parasitic, and
     * are a tone control, so they belong to whoever is voicing the amplifier.
     * Zero disables it.
     */
    float cmiller;
    /*
     * The valve's own inter-electrode capacitances, which were not modelled
     * at all.  Published for a 12AX7: grid-plate 1.7 pF, grid-cathode 1.6 pF,
     * plate-cathode 0.46 pF.
     *
     * Grid-plate is the one that matters and the one a lumped capacitor to
     * ground cannot stand in for.  Sitting across the valve it is local
     * negative feedback: at high frequencies it feeds the inverted plate back
     * to the grid, which rounds the corner of a clipping edge *while the edge
     * is being made*, not afterwards.  Its Miller multiplication - the effect
     * `cmiller` was faking - falls out of that for free, and correctly, since
     * it follows the stage's gain instead of a guess at it.
     *
     * `cplate` is the socket and wiring to ground, which nothing else covers.
     */
    float cgp;
    float cgk;
    float cpk;
    float cplate;
    /*
     * Put a tone stack on this stage's plate instead of a plain load.  It goes
     * inside the same matrix, which costs three more capacitors and no second
     * solve - see the note above ckt_build_stage_with_tonestack.  It also
     * throws away fifteen to twenty decibels, which is what makes it possible
     * to cascade a third valve after it without the result being a square
     * wave generator.
     */
    int tonestack;
    /*
     * Where the tone stack's controls are set, 1 being where they were nailed
     * before anyone looked.  They were nailed at full treble: measured on
     * noise, the stage with the stack on it runs +3 dB at 4-6 kHz and is
     * still +2.5 dB at 10 kHz relative to 1 kHz - which is the amplifier
     * emphasising exactly the band where clipping fizz lives, after the
     * clipping has made it.
     *
     * Lower is less.  The treble control is the resistance between the treble
     * capacitor and the output, so the value divides into it.
     */
    float ts_treble;
    float ts_bass;
} ckt_stage_spec_t;

/* Returns the plate node, or 0. */
int ckt_build_spec_stage(ag_ckt_t *k, float fs, const ckt_stage_spec_t *sp);

/*
 * The two gain stages of a Marshall 2203/2204 front end, in order.  These are
 * the values that make it a crunch rather than a clean channel: a cold-ish
 * 2k7 cathode on the first valve, an 820R on the second so it runs hotter and
 * clips earlier, and 100k plate loads on both.
 *
 * Written from the widely published topology rather than from a schematic in
 * hand - treat the values as a recognisable JCM800-style front end and correct
 * them if you have the real thing.
 */
void ckt_jcm800_stage(ckt_stage_spec_t *out, int index);

int   ckt_build_chain(ckt_chain_t *c, float fs, int stages);
float ckt_chain_tick(ckt_chain_t *c, float vin);
/* Newton passes and failures summed over the chain, for the bench. */
unsigned ckt_chain_iters(const ckt_chain_t *c);
unsigned ckt_chain_noncvg(const ckt_chain_t *c);
void     ckt_chain_reset_stats(ckt_chain_t *c);

/*
 * The same chain, baked: each stage reduced to a handful of constants and a
 * table of the solved nonlinearity, the way a fixed-topology amplifier model
 * is built.  See apps/common/ckt/ag_stage.h for why that is the same model
 * rather than a simpler one.
 *
 * 256 across the grid drive is measured: halving it to 128 costs 7 dB, and
 * doubling it to 512 buys 1.
 *
 * The second axis was 9 for a bad reason, and it is worth writing down how
 * the reasoning failed.  It was measured on a chain of identical stages fed a
 * sine, where the plate driving point moved 213..238 V - 25 volts, which 9
 * points cover easily.  In the amplifier that got built, the second stage is
 * driven by a hot first one and its plate driving point spans 142..319 V.
 * Nine points across that is sixty volts a cell with a straight line between
 * them, and a triode is nothing like straight over sixty volts.  The
 * measurement was real; the signal it was taken on was not the signal that
 * matters.
 *
 * Against a 512x65 render of a real guitar take, difference in the 1.5-5 kHz
 * band - where a speaker passes everything and the ear is least forgiving:
 *
 *      256x9   -46 dB      256x33  -55 dB
 *      256x17  -53 dB      256x65  -55 dB
 *
 * and with the range margin below cut from 35% to 15%, 256x17 reaches -58 dB.
 * That is a ten decibel improvement over what shipped, for twice the table
 * and not one extra instruction: a bilinear lookup costs the same whatever it
 * is looking into.
 *
 * 35 KB a stage, and 70 with the antiderivative table beside it.  Two stages
 * fit internal SRAM comfortably; a four stage chain does not, and would want
 * the tables in int16 - worth about 90 dB, which is far more than the 60 that
 * is being chased here.
 */
#ifndef CKT_BAKE_N1
#define CKT_BAKE_N1 256
#endif
#ifndef CKT_BAKE_N2
#define CKT_BAKE_N2 17
#endif

/*
 * How much wider than the observed driving range each table is baked.
 *
 * Every percent of it is resolution spent where the signal rarely goes.  It
 * was 35%, which put more than half of each table outside the music; cutting
 * it to 15% is worth 5 dB on its own and costs nothing.  It cannot go to zero
 * - the render is not the probe - and ag_stage counts the lookups that clamp
 * at the edge, so this is set by that count rather than by nerves.
 */
#ifndef CKT_BAKE_MARGIN
#define CKT_BAKE_MARGIN 0.15f
#endif

/*
 * How many times to re-fit the ranges.  Three is measured, not chosen: with
 * two, a chain whose stages barely move at first ends up with tables too
 * narrow for the signal those very tables then make bigger.
 */
#ifndef CKT_BAKE_FIT_PASSES
#define CKT_BAKE_FIT_PASSES 3
#endif

typedef struct ckt_baked_chain {
    ag_stage_t stage[CKT_MAX_CHAIN];
    float      tab[CKT_MAX_CHAIN][CKT_BAKE_N1 * CKT_BAKE_N2 * 2];
    /* The antiderivative tables, for antialiasing.  Same size again. */
    float tabH[CKT_MAX_CHAIN][CKT_BAKE_N1 * CKT_BAKE_N2 * 2];
    int   n;
} ckt_baked_chain_t;

/* What each stage of the chain is. */
enum {
    CKT_VARIANT_PLAIN = 0,     /* valve, coupling, cathode, load          */
    CKT_VARIANT_TONESTACK      /* the same with a tone stack folded in    */
};

/*
 * Bake `stages` of them.  `scratch` is one ag_ckt_t the baker borrows; `probe`
 * and `probe_n` are the signal the ranges are fitted to, repeated as needed -
 * a table is only valid over the driving points it was baked for, and the
 * honest way to choose those is to look.  `adaa` turns on antialiasing.
 * Returns 0 on success.
 */
int ckt_bake_chain(ckt_baked_chain_t *c, ag_ckt_t *scratch, float fs,
                   int stages, const float *probe, int probe_n, int settle,
                   int variant, int adaa);

float    ckt_baked_chain_tick(ckt_baked_chain_t *c, float vin);
unsigned ckt_baked_chain_clamped(const ckt_baked_chain_t *c);
void     ckt_baked_chain_reset(ckt_baked_chain_t *c);

/* Tube Screamer style clipper: 4k7 into an antiparallel 1N914 pair. */
int ckt_build_diode_clipper(ag_ckt_t *k, float fs);

/* A JFET common-source stage on a 9 V rail. */
int ckt_build_jfet_stage(ag_ckt_t *k, float fs);

/* First-order RC low pass - the cheapest thing the solver can be asked to do. */
int ckt_build_rc(ag_ckt_t *k, float fs);

/*
 * Passive Marshall/Fender tone stack, driven from a source: three capacitors
 * and five resistors, no nonlinearity at all.  It exists in this benchmark to
 * show what reactive components alone cost, since that is the part of the
 * answer people guess wrong.
 */
int ckt_build_tonestack(ag_ckt_t *k, float fs);

#endif /* CKT_CIRCUITS_H */
