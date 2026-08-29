/*
 * ag_tube - one valve stage as a static transfer curve, baked from the solver.
 *
 * This is the cheap half of the answer in docs/08-circuit-simulation.md.  The
 * baked stage in apps/common/ckt/ag_stage.c keeps the whole linear network as
 * state and tabulates a function of two variables; this keeps a function of
 * one, and the linear network moves out of the stage into filters around it.
 *
 * What is given up and what is not, stated plainly, because the difference is
 * the whole design:
 *
 *   - Intermodulation is NOT given up.  Any memoryless nonlinearity expands
 *     a*cos(w1 t) + b*cos(w2 t) into terms at every m*w1 +- n*w2; that is
 *     algebra, not a property of a valve.  A waveshaper has intermodulation
 *     too, which is why "does this give intermodulation" is the wrong question
 *     to separate the two.
 *
 *   - The plate load line IS given up.  ag_stage carries the plate's driving
 *     point as a second table axis because in the amplifier that got built it
 *     moves over 142..319 V.  Here it is frozen at the operating point, and
 *     the price of that is a number measured by tools/tube_render.c against
 *     the two-axis model, not a matter of opinion.
 *
 *   - Memory is not given up entirely, only moved.  The coupling capacitors,
 *     the cathode bypass and the Miller roll-off are linear, so they live in
 *     the biquads in ag_biq.h - which means the chain still has frequency
 *     dependent drive and still has the signal-dependent DC shift that an
 *     asymmetrically clipping stage pushes onto its coupling capacitor.  What
 *     is actually missing is memory *inside* the nonlinearity: grid current
 *     charging the coupling capacitor (blocking distortion) and the cathode
 *     bypass being pumped by large-signal cathode current.  Both are an
 *     additive offset on the grid to first order, which is what `vbias` in
 *     ag_tube_tick_bias exists for.
 *
 * The curve is baked by sweeping the same ag_ckt solver over DC, so there is
 * exactly one Koren model in the tree and no chance of two copies drifting
 * apart.  That reuse costs something, and the number is worth stating rather
 * than guessing at: measured on the guest, one DC point through the full matrix
 * solver is about 18 600 instructions, so a two-stage build with the axes fitted
 * comes to 212 M instructions - 882 ms at 240 MHz - and 364 ms if the axes are
 * handed back from a previous fit.  That is a preset load, not a knob, and about
 * what the two-axis bake costs.
 *
 * It does not have to stay that way.  With the cathode held and the load line
 * linear the sweep has two unknowns, so it could call ag_triode_eval directly
 * instead of assembling a seven-node matrix - two hundred instructions a point
 * rather than eighteen thousand, which would put the whole build near ten
 * milliseconds.  Not done: it is a second piece of netlist algebra to keep in
 * step with the first, and it wants the same host test that ag_stage has, the
 * fast path against the solver to a millivolt.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_TUBE_H
#define AG_TUBE_H

#include <stdint.h>

#include "ag_ckt.h"

/*
 * Bounds on how many cells the antialiasing average will walk before it gives up
 * and uses the cumulative antiderivative instead.
 *
 * Both paths compute the same integral; they differ in where they lose precision.
 * Walking cells sums positive lengths times values taken from the same straight
 * lines, so nothing is ever subtracted and the answer is good to an ulp however
 * small the step is.  The cumulative table subtracts two nearly equal running
 * integrals, which is safe when the step is large and is exactly the arithmetic
 * that made the two-axis model crackle when it was small (rake 12 in docs/08).
 *
 * The crossover cannot be a fixed number of cells, and that took a measurement
 * to see.  The cumulative table's magnitude grows with the number of points -
 * it is a running sum over the whole axis - so its cancellation error is
 *
 *      |H| * 2^-24 / span   ~   5.5e-6 * n / span_in_cells   volts
 *
 * which at a fixed sixteen-cell crossover gets worse in proportion to n.  A
 * 262144 point table used as a reference measured 29 dB of difference against a
 * 1024 point one; with the antialiasing switched off the same pair differed by
 * 84 dB.  The table was never the problem - the reference was.
 *
 * So the crossover is n/16, which holds that error near 1e-4 V whatever the
 * table size, and the walk is bounded by the same number.  The floor and the
 * ceiling below are there so that a very small table still subtracts sometimes
 * and a very large one does not walk for a thousand iterations.
 */
#ifndef AG_TUBE_WALK_MIN
#define AG_TUBE_WALK_MIN 16
#endif
#ifndef AG_TUBE_WALK_MAX
#define AG_TUBE_WALK_MAX 256
#endif

/*
 * The component values of one stage, DC-relevant part only.
 *
 * ccath and ccouple do not appear in the curve - a static curve has no
 * frequency - but they do decide the two filters that belong to this stage, so
 * they are carried here and handed out by ag_tube_shelf / ag_tube_couple_hz.
 * Keeping them beside the resistors is what stops the filters from becoming
 * numbers somebody dialled in.
 */
/*
 * What kind of thing this stage is.
 *
 * Zero is a triode, so a spec that predates this field is still a valve.  The
 * pedal kind is an antiparallel diode pair across the feedback resistor of a
 * non-inverting op-amp stage - a Tube Screamer - and it uses `ri`, `ci`, `rf` and
 * the two diode numbers instead of the plate and cathode.
 */
typedef enum ag_tube_kind {
    AG_TUBE_TRIODE = 0,
    AG_TUBE_TS_FEEDBACK = 1,
    /*
     * No curve at all: the stage is its filters and its gain and nothing else.
     * Nothing switches on this - bake_all skips a stage marked linear in
     * ag_amp_cfg_t before it ever looks at the kind - and it is here so that a
     * spec for such a stage does not have to claim to be a triode.
     */
    AG_TUBE_LINEAR = 2,
    /*
     * A cathode follower.  The same valve and the same solver, wired the other
     * way round: the plate goes straight to B+, the cathode resistor is never
     * bypassed because it *is* the output, and what the next stage sees is taken
     * from the cathode instead of the plate.
     *
     * Two things about it matter to a chain rather than to a stage.  It does not
     * invert - a common-cathode stage does - and its gain is a hair under one, so
     * it is a buffer and not an amplifier.  And it clips in a way nothing else
     * here does: the top is limited by grid current, which starts to flow as the
     * grid catches up with its own cathode, and the bottom by the valve going
     * into cutoff and leaving the cathode resistor to pull the node down on its
     * own.  Those two limits are nothing like each other, which is why a follower
     * is heard as a compression rather than as a fuzz.
     */
    AG_TUBE_CF = 3
} ag_tube_kind_t;

typedef struct ag_tube_spec {
    float rsrc;    /* output impedance of whatever drives this stage       */
    float rgrid;   /* grid leak                                            */
    float rstop;   /* grid stopper                                         */
    float vsupply; /* B+                                                   */
    float rplate;
    float rcath;
    float ccath;   /* cathode bypass; 0 means unbypassed                   */
    float ccouple; /* input coupling capacitor                             */
    float cload;   /* output coupling capacitor                            */
    float rload;   /* input impedance of whatever this stage drives        */
    /*
     * What the grid leak returns to, in volts.  Zero - and so every stage
     * written before this field existed - means ground, which is what a leak
     * behind a coupling capacitor does.
     *
     * A cathode follower in a Marshall or a Soldano is not behind a coupling
     * capacitor: its grid is tied straight to the previous plate, so it sits at
     * that plate's quiescent voltage and its cathode follows a couple of volts
     * under it.  That is the whole reason a follower has headroom - a grounded
     * leak would self-bias it to about four volts, and four volts is less than
     * the stage in front of it swings in the first millisecond.
     */
    float vgrid_ref;
    ag_triode_model_t valve;
    /* Zero for a valve; see ag_tube_kind_t. */
    int   kind;
    /* The pedal, and only the pedal: the input leg (resistor and the capacitor
     * that makes the mid hump), the feedback resistor including the drive pot,
     * and the diode's saturation current and n*Vt. */
    float ri, ci, rf, dio_is, dio_nvt;
} ag_tube_spec_t;

/*
 * An Ibanez TS9 Tube Screamer with all three knobs at noon, which is what the
 * capture in assets/audio/guitar-di/Ibanez.nam was taken at - its own metadata says
 * "Drive 5, Tone 5, Level 5".  Values from the published schematic; the drive pot
 * is a 500k at half.
 */
/* index 0 the input buffer, 1 the clipping stage. */
void ag_tube_spec_ts9(ag_tube_spec_t *out, int index);

/* The published 12AX7 fit and the two JCM800 preamp stages, from the same
 * numbers as apps/cktbench/ckt_circuits.c - test_tube.c checks they agree. */
void ag_tube_spec_jcm800(ag_tube_spec_t *out, int index);

/*
 * Two more chains, selected by ag_amp_model: a two-valve crunch with a volume
 * between the stages, and the four cascaded valves of a Soldano SLO-100 overdrive
 * channel, the third of them cold.
 *
 * Each function's comment says where its numbers come from, and the three are not
 * the same kind of thing.  The JCM800's are the tree's own, duplicated in
 * apps/cktbench and checked field by field.  The SLO's are two independent public
 * analyses of that amplifier which agree value for value.  The crunch model's are
 * a family resemblance - a Plexi front end with three departures argued for one at
 * a time - and it says so.  What makes any of them a particular amplifier is the
 * fit against a capture, which is README.md's business and not the resistors'.
 */
void ag_tube_spec_bogner(ag_tube_spec_t *out, int index); /* index 0..1 */
void ag_tube_spec_slo(ag_tube_spec_t *out, int index);   /* index 0..3 */

typedef struct ag_tube {
    /*
     * The curve, and its integral in index units.  Index units rather than
     * volts because the average of the curve over a span is the integral
     * divided by the span, and if both are in the same units the step cancels
     * and cannot be got wrong.
     */
    const float *t;
    const float *h;
    /*
     * Grid current at each point of the same axis, in amps.  Zero over most of
     * it - the grid only conducts once it comes up to the cathode - and the
     * reason it is a table rather than a formula is that it has to be the same
     * valve as the curve beside it, solved at the same operating point.
     *
     * NULL unless the bake was given a buffer for it, and without it there is
     * no blocking distortion.
     */
    const float *g;
    int          n;

    float lo, hi, step, step_inv;
    /*
     * The index that zero volts sits on, exactly, as a float.
     *
     * The lookup is p = zero_p + v * step_inv rather than (v - lo) * step_inv,
     * and the difference is not cosmetic.  The axis is shifted at bake time so
     * that zero lands on a grid point and the curve there is exactly zero, but
     * (lo * step_inv) does not come back as an integer in floating point, so the
     * subtraction form lands an ulp off the node and interpolates.  That leaves a
     * tenth of a microvolt at the first plate, sixty times that at the second,
     * and an amplifier that is not silent when nothing is playing.
     */
    float zero_p;

    /* Where the stage sits with no signal, from the bake. */
    float vq_plate, vq_cath, vq_grid;

    /*
     * Small-signal gain at the operating point with the cathode bypassed and
     * with it open.  Their ratio is the depth of the cathode shelf, and it is
     * measured from the same solver rather than estimated from mu and rp - see
     * ag_tube_shelf.
     */
    float gain_bypassed, gain_unbypassed;

    /* Antialiasing: off unless a second table was handed to the bake. */
    uint8_t adaa;
    uint8_t have_prev;
    float   prev_p; /* previous input, in index units, unclamped */
    /*
     * Where the average stops walking cells and starts subtracting - see the
     * note on AG_TUBE_WALK_MIN.  Set by the bake from the table size; writable,
     * because forcing it to the whole table makes the walk exact, and an exact
     * answer is what the cheap one has to be measured against.
     */
    int walk_max;

    /*
     * How often a lookup ran off the end, and how far the input actually went.
     * Unlike the two-axis model, running off the end here is not the model
     * guessing: the curve is flat outside - cut off on one side, grid current
     * through the stopper on the other - and the lookup extends it flat, which
     * is the physical answer.  The counters are still kept, because "flat
     * enough" is a claim and a claim wants a number.
     */
    uint32_t clamped;
    /*
     * Of those, the ones off the *bottom* - where the valve is cut off and the
     * curve is flat because it really is flat.  Kept apart from `clamped`
     * because the two ends mean opposite things: the top is a stage saturating,
     * which is what a driven valve does, and the bottom is a stage that has been
     * switched off, which under blocking is a gate opening and closing.
     */
    uint32_t cut;
    uint32_t samples;
    float    seen_lo, seen_hi;

    /* From the bake: grid points where Newton hit its iteration limit. */
    uint32_t bake_noncvg;

    /*
     * Blocking distortion: the charge the grid leaves on the input coupling
     * capacitor.
     *
     * This is the memory a static curve cannot have, and it is half of how a
     * cranked amplifier answers the picking hand.  The grid is a diode; when the
     * signal drives it above the cathode it conducts, and that current has to
     * come through the coupling capacitor, which charges.  The only way back is
     * the grid leak, so the charge sits there and holds the grid negative -
     * the stage biases itself toward cut-off, goes quieter and more asymmetric,
     * and recovers over milliseconds.  On a hard pick it is audible as the note
     * choking and then blooming.
     *
     * The curve was baked with the capacitor treated as a short, which is what it
     * is within a cycle; this is the slow part that treatment leaves out, so the
     * two do not overlap.  What is modelled:
     *
     *     C dvc/dt = ig(v - vc) + (v - vc)/rgrid
     *
     * with vc the volts across the capacitor and v the source.  The two stages
     * behave completely differently and that is the point: V1a's 100 nF into 1 M
     * recovers over 100 ms, while V1b's 2.2 nF into 470 k recovers in 1 ms -
     * which is exactly why a high gain amplifier uses a small capacitor there.
     */
    uint8_t block;
    /*
     * The first index at which the grid draws anything at all.  Below it the
     * lookup is known to return zero, so the whole thing collapses to a compare
     * and a decay - and the grid only conducts on the peaks, so that is the
     * common case.  Measured on the guest: blocking cost 717 instructions a
     * sample at 4x before this and far less after.
     */
    int   g_first;
    float vc;       /* volts across the coupling capacitor                 */
    float   blk_k;  /* T/C:        volts of vc per amp-second of grid current */
    float   blk_g;  /* T/(C*rgrid): the leak back, per sample              */
    /*
     * 1/(1 + blk_g), because the implicit step is a division and this FPU has no
     * divider: 37 instructions against 6 for a multiply, once a sample per stage
     * per oversampled step.  Written out because the first version divided and
     * the skip that was supposed to make blocking cheap did nothing measurable.
     */
    float blk_decay;
    float   vc_peak; /* how far it went, for reporting                     */
} ag_tube_t;

void ag_tube_init(ag_tube_t *tb);

/*
 * Bake `sp` into `tb`.  `scratch` is one ag_ckt_t the baker borrows and leaves
 * in an undefined state; `tab` must hold `n` floats and outlive `tb`; `tabh`
 * may be NULL, and a second buffer of the same size is what ag_tube_set_adaa
 * needs.
 *
 * The axis is volts at the stage's source - the node in front of rsrc - not
 * volts at the grid, because that is the quantity the code upstream actually
 * has.  The grid divider and the drop across the stopper when grid current
 * flows are inside the curve where they belong.
 *
 * Pass lo >= hi to let the baker fit the range, and `tol` then decides how much
 * of it to keep: the axis runs to where the curve has come within `tol` of the
 * value it saturates at, as a fraction of its whole swing.
 *
 * This is the one thing about a one-dimensional valve that is genuinely awkward,
 * and it is worth writing the numbers down.  Measured on V1a: at +1 V of source
 * the plate still has 63 V of its 266 V swing left to give, at +8 V it has 39, at
 * +32 V it has 16, and at +128 V it still has 4.5.  The grid conducts through
 * 78k, so the approach to saturation goes roughly as 1/v and there is no plateau
 * anywhere useful:
 *
 *   tol 0.08   ->  runs to about +27 V
 *   tol 0.01   ->  runs to about +175 V
 *   tol 0.002  ->  runs to about +780 V
 *   tol 0.0005 ->  runs to about +2 kV
 *
 * while the knee that decides how the valve sounds is one and a half volts wide.
 * So a curve-fitted axis is the wrong answer whichever tol is picked, and the
 * right one is to fit it to the signal instead - which ag_amp_build does, using
 * this only as an outer bound.  Measured, 1.5-5 kHz band: the best curve-fitted
 * axis is 48 dB from a signal-fitted one and no table size recovers it.
 *
 * Outside the axis the curve is extended flat, so a narrow axis loses level on
 * the hardest-clipped peaks and keeps the shape of everything else.
 *
 * Returns 0 on success.
 */
int ag_tube_bake(ag_tube_t *tb, ag_ckt_t *scratch, const ag_tube_spec_t *sp,
                 float *tab, float *tabh, float *tabg, int n, float lo, float hi,
                 float tol);

/* Returns 0 if there is no antiderivative table to switch on. */
int ag_tube_set_adaa(ag_tube_t *tb, int on);

/*
 * Blocking distortion on or off.  Needs the grid-current table and the sample
 * rate the stage will actually run at - the oversampled one, if it is inside an
 * oversampled region, because the capacitor is charged by every sample that
 * passes through it.  Returns 0 if there is no grid table to switch on.
 */
/* `depth` scales the charge the grid puts on the capacitor: 1 is the circuit,
 * 0 is blocking off, and in between is neither - see ag_amp_cfg_t.block_depth. */
int ag_tube_set_blocking(ag_tube_t *tb, const ag_tube_spec_t *sp, float fs,
                         int on, float depth);

/* Forget the previous sample and clear the counters.  Not the curve. */
void ag_tube_reset(ag_tube_t *tb);

/* One sample.  `v` is volts at the source; the result is volts at the plate,
 * with the operating point removed, so silence in is silence out.  Applies
 * blocking if it is switched on. */
float ag_tube_tick(ag_tube_t *tb, float v);

/*
 * The same with an additional offset on the input, on top of whatever blocking
 * is doing.  Anything else that shifts the operating point slowly belongs here -
 * supply sag is the obvious next one, and so is the cathode bypass being pumped
 * by large-signal cathode current, since both reach the valve as a shift of the
 * same voltage.
 */
float ag_tube_tick_bias(ag_tube_t *tb, float v, float vbias);

/* Stateless read of the curve, for plots and tests. */
float ag_tube_curve(const ag_tube_t *tb, float v);

/*
 * The cathode bypass, as the first-order shelf it is.
 *
 * The curve is baked with the cathode held at its operating point, which is
 * what the bypass capacitor does at audio frequencies.  Below the corner the
 * capacitor stops holding it, the cathode resistor becomes local feedback, and
 * the stage loses gain *and* distorts less.  A shelf after the valve would
 * only take back the gain, so the shelf goes in front, where less drive means
 * less of both - which is the right first-order answer and free.
 *
 * `f_zero` is 1/(2*pi*rcath*ccath), the corner of the cathode network itself.
 * `db` is negative: how much less gain the stage has below it, measured as the
 * ratio of the two small-signal gains the bake took.  Zero db and zero f_zero
 * mean there is no capacitor and nothing to do.
 */
void ag_tube_shelf(const ag_tube_t *tb, const ag_tube_spec_t *sp, float *f_zero,
                   float *db);

/* The input coupling capacitor's corner, 1/(2*pi*ccouple*(rsrc+rgrid)). */
float ag_tube_couple_hz(const ag_tube_spec_t *sp);

/* The output coupling capacitor's corner.  The plate's own source impedance
 * (rplate parallel with the valve's rp, about 38k on a 100k load) is small
 * against the megohm it feeds, so it is left out and named rather than
 * estimated. */
float ag_tube_load_hz(const ag_tube_spec_t *sp);

/*
 * Point a stage at tables that already exist, instead of baking them.
 *
 * This is what loading a preset does, and the reason it is worth a function of
 * its own is that it needs no circuit solver at all: a curve is a curve, and
 * where it came from - a triode, a diode pair, a transistor, a host that baked
 * it last week - is not something the runtime has any use for.  An application
 * that only plays presets does not have to link ag_ckt.
 *
 * `lo` and `hi` are the axis the tables were baked over; everything else the
 * lookup needs follows from them and from `n`.  `tabg` may be NULL, and then the
 * stage simply has no blocking.  Returns 0 on success.
 */
int ag_tube_attach(ag_tube_t *tb, const float *tab, const float *tabh,
                   const float *tabg, int n, float lo, float hi);

/*
 * A second curve for the same stage: an antiparallel diode pair behind a
 * resistor, which is what every overdrive pedal clips with.
 *
 * The point of it is blending.  A valve gives even harmonics and a lower half
 * that looks nothing like its upper half; a diode pair gives odd harmonics and
 * near-symmetry.  Mixing the two curves walks between them, and because a curve
 * is a curve the runtime never learns that one of them is not a valve.
 *
 * Baked over the same axis as the curve it will be mixed with, and **scaled so
 * that its small-signal slope matches** - `gain_match` is the slope to hit,
 * normally the valve's `gain_bypassed`.  Without that the blend knob would be a
 * volume control with a tone control hidden inside it: a passive clipper has a
 * gain under one and a 12AX7 stage has seventy.
 *
 * `vf` is the diode's forward knee in volts - 0.35 for silicon, 0.25 for a
 * germanium or a LED-ish softness - and it is what decides where the curve bends
 * relative to the valve's own knee.
 *
 * Returns 0 on success.
 */
int ag_tube_bake_clipper(ag_tube_t *tb, ag_ckt_t *scratch, float rseries,
                         float vf, float gain_match, float *tab, float *tabh,
                         float *tabg, int n, float lo, float hi);

/*
 * The other pedal curve: an antiparallel pair across the **feedback** resistor of a
 * non-inverting stage, which is what a Tube Screamer clips with.
 *
 * `ri` is the resistive part of the input leg, `rf` the feedback resistor plus the
 * drive pot, `is` and `nvt` the diode's saturation current and n*Vt.  All four are
 * real component values.  The frequency dependence of the input leg - the capacitor
 * that makes the mid hump - is deliberately *not* in here: see the note on the
 * implementation.  No ag_ckt needed; one unknown, solved directly.
 *
 * Returns 0 on success.
 */
int ag_tube_bake_ts(ag_tube_t *tb, float ri, float rf, float is, float nvt,
                    float *tab, float *tabh, float *tabg, int n, float lo,
                    float hi);

/*
 * Mix two baked table sets into a third, at `b` from 0 (all of A) to 1 (all
 * of B).
 *
 * Exact rather than approximate, and it matters that it is: the curve, its
 * antiderivative and the grid current are all linear in the mix, so the blended
 * antiderivative stays the exact integral of the blended curve and the
 * antialiasing average keeps working.  Both must be baked over the same axis and
 * both are zero at zero, so the blend is too - silence stays silence and there is
 * no step at the seam.
 *
 * The grid current of a clipper is zero, so blocking fades out with the mix,
 * which is what should happen: a diode has no grid to charge anything.
 */
void ag_tube_blend(ag_tube_t *dst, const ag_tube_t *a, const ag_tube_t *b,
                   float mix, float *tab, float *tabh, float *tabg);

#endif /* AG_TUBE_H */
