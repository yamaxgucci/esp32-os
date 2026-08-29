/*
 * ag_amp - two valve stages and the three filters around them.
 *
 *   in -> [drive] -> F1 -> || 2x/4x || T1 -> F2 -> T2 || -> F3 -> [master] -> out
 *
 * and after that, outside this file, the cabinet convolution in ag_ir.
 *
 * What each part costs is measured on the guest rather than argued about here;
 * the table is beside `cfg->os` in ag_amp.c.  Two valves at four times with
 * antialiasing come to 4086 instructions a sample, 37.5% of a core at 22.05 kHz,
 * and the cabinet after them to 981 more.
 *
 * The oversampled region holds *both* valves, with F2 running inside it.  The
 * alternative - drop back to the base rate between the stages so that F2 is
 * cheap - needs a second pair of interpolation and decimation filters, and two
 * of those cost more than running two biquads at four times the rate.
 *
 * WHAT IS A COMPONENT VALUE AND WHAT IS TASTE
 *
 * This matters more than it sounds, because it decides which numbers may be
 * turned and which may not.  Everything in F1, F2 and F3 except two entries is
 * derived from the netlist in ag_tube_spec_jcm800:
 *
 *   F1  1.6 Hz high pass      the input coupling capacitor, 100 nF into 1 M
 *       low shelf at 285 Hz   V1's cathode bypass, 0.68 uF across 820 R, and
 *                             its depth is the ratio of two small-signal gains
 *                             the bake measured - not a dialled number
 *       low pass              TASTE: the requested top cut.  The real grid
 *                             stopper against the stray at the grid corners
 *                             near 19 kHz, so anything lower is voicing.
 *
 *   F2  142 Hz high pass      V1b's coupling capacitor, 2.2 nF into 470 k.
 *                             This is the same 150 Hz that was asked for, and
 *                             it is not an equaliser - it is the bright cap
 *                             that makes a high gain Marshall high gain.
 *       low shelf at 285 Hz   V1b's cathode bypass, same as above
 *       peaking               TASTE: the mid lift
 *
 *   F3  7.2 Hz high pass      the output coupling capacitor, 22 nF into 1 M
 *
 * So the two knobs that are taste are named as taste, and the rest cannot be
 * turned without changing a component.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_AMP_H
#define AG_AMP_H

#include <stdint.h>

#include "ag_biq.h"
#include "ag_os.h"
#include "ag_tone.h"
#include "ag_tube.h"

/*
 * How many stages the chain can hold.  Live count is cfg.n_stages, 1 to this;
 * the arrays are always this long so that nothing has to be reallocated when a
 * preset with a different topology is loaded.
 *
 * A stage is a stage.  The runtime has no idea whether the table in front of it
 * came from a triode, a diode pair or a transistor - it is a curve, and where
 * the curve came from is the host baker's business.  A stage with no table at
 * all is the degenerate case: gain and its filter block, nothing else.
 */
#define AG_AMP_STAGES 4


/*
 * Points across the curve, measured rather than chosen.  tube_render "res"
 * renders the same two seconds of a plucked decay through every size, on the
 * same axes and with the antialiasing off so that interpolation is the only
 * thing being measured, against a reference 64 times finer.  The second column
 * is the 1.5-5 kHz band, where a speaker passes everything and the ear forgives
 * least:
 *
 *      points     all      1.5-5 kHz    memory for the pair
 *       256     -60.1 dB    -52.9 dB           4 KB
 *       512     -70.2 dB    -64.1 dB           8 KB
 *      1024     -79.0 dB    -74.4 dB          16 KB
 *      2048     -88.2 dB    -83.8 dB          32 KB
 *      4096     -97.0 dB    -90.3 dB          64 KB
 *
 * Every doubling is worth 6 to 11 dB, which is what a piecewise-linear
 * approximation of a smooth curve should give, and 2048 is chosen with room
 * either way rather than at the edge of anything.  (8192 comes back worse than
 * 4096, because there the number has reached the reference's floor and not its
 * own - which is the whole reason the reference is 64 times finer than 2048 and
 * not four times.)
 *
 * For scale: the two-axis model reaches -57.7 dB in the same band and occupies
 * 139 KB for two stages.  This is 28 dB better on a quarter of the memory, and
 * the reason is not cleverness - it is that a triode's plate swings 300 V, so
 * ten millivolts of interpolation error is nothing, while the second axis the
 * other model needs is genuinely expensive.
 *
 * A lookup costs the same whatever it is looking into, so this is a memory
 * decision and nothing else.
 */
#ifndef AG_AMP_TAB_N
#define AG_AMP_TAB_N 2048
#endif

/* Floats the caller has to provide to ag_amp_build: curve, antiderivative and
 * grid current for each stage. */
#define AG_AMP_TAB_FLOATS (AG_AMP_STAGES * 3 * AG_AMP_TAB_N)

/*
 * One band of either voicing bank.  db of 0 disables it.
 *
 * EIGHT, AND WHY THE EIGHTH IS AT 630 Hz
 *
 * The seven were octave-spaced - 100, 200, 400, 800, 1600, 3150, 5000 - and the
 * objective they are fitted against measures third-octaves.  A bank that can
 * only put a bump on octave centres cannot answer a measurement taken in thirds,
 * and the widest place where it could not is between 400 and 800: with Q 1 the
 * only way to lift 630 is to lift both neighbours and flatten the shape that was
 * wanted.
 *
 * It was found by ear before it was found by measurement.  Turning the live
 * tool's mid knob, +4 dB at 635 Hz took the even-to-odd balance on the slo from
 * +9.6 dB to +3.4 - the difference between "another amplifier" and "this one" -
 * while the magnitude fit called the same setting two tenths of a decibel worse.
 * Both of the frequencies that turned out to matter, 630 and 1600, are
 * third-octave centres.
 */
#define AG_AMP_VOICE_N 8
typedef struct ag_amp_band {
    float hz, db, q;
} ag_amp_band_t;

/*
 * Which chain this is: the component values of every stage, the topology, and
 * the voicing that goes with them.
 *
 * A model is not a bank of numbers somebody liked.  It picks the netlist the
 * bake sweeps - ag_tube_spec_jcm800, _bogner or _slo - and each of those says
 * in its own comment what is a component value and what is a departure made for
 * a stated reason.  What a model cannot do is make a chain sound like a named
 * amplifier: that is the voicing fit against a capture, step 4 of "Authoring a
 * preset for another amplifier" in README.md, and ag_amp_model_fitted says
 * whether it has been run.  Only JCM800 answers yes.
 */
enum ag_amp_model_id {
    AG_AMP_MODEL_JCM800 = 0, /* two hot-rodded valves, no volume between them */
    AG_AMP_MODEL_BOGNER = 1, /* two stock valves with a volume between them */
    AG_AMP_MODEL_SLO = 2,    /* four valves, the third one cold */
    /*
     * Not an amplifier at all: one op-amp stage that clips in its own feedback
     * loop, which is what a Tube Screamer is.  It is here because everything this
     * module does to a valve - bake a curve, put the stage's own filters around
     * it, fit matching banks against a capture - is exactly what a pedal needs,
     * and the only thing that had to be added was the curve.  It has one stage,
     * no tone stack and no loudspeaker.
     */
    AG_AMP_MODEL_TS9 = 3,
    AG_AMP_MODEL_N = 4
};

typedef struct ag_amp_cfg {
    float fs;
    /* One of ag_amp_model_id.  Decides which netlist every stage is baked from,
     * and travels inside a preset so that a blob can say what it is. */
    int   model;
    /* 1 to AG_AMP_STAGES. */
    int   n_stages;
    /*
     * Gain into each stage, applied after that stage's filter block.  gain[0] is
     * the drive knob and gain[1] the interstage attenuator a Plexi has and a
     * 2203 does not; the rest default to unity.
     */
    float gain[AG_AMP_STAGES];
    /* No table: the stage is its filter block and its gain, and nothing else. */
    int   linear[AG_AMP_STAGES];


    int   os;   /* 1, 2, 4 or 8 */
    int   adaa; /* antialiasing inside the curve; nearly free, so default on */
    /*
     * Blocking distortion - grid current charging each stage's input coupling
     * capacitor.  See the note in ag_tube.h; it is the memory a static curve
     * cannot have, and the reason the two stages behave differently under it is
     * that their coupling capacitors differ by a factor of forty-five.
     */
    int blocking;
    /*
     * How much of the blocking to keep, from 0 to 1.  One is the capacitor as
     * it is; a half puts half the grid's charge on it and so half the bias
     * shift, with the same recovery time and the same linear response.
     *
     * IT IS NOT A COMPONENT.  Nothing on a schematic reads "half the grid
     * current", and this is here so the effect can be turned down and listened
     * to rather than only switched off - the two models that ship with blocking
     * off lost something real along with the fault.  Zero is the same as
     * `blocking = 0` and one is the same as the model without this field, so a
     * config memset to zero still means what it always did once `blocking` is
     * set.
     */
    float block_depth;

    /*
     * Volts at the first valve's source per unit of input, and the gain knob.
     *
     * In volts because the curve is: a 12AX7 biased at -0.74 V clips when its
     * grid swings about six tenths of a volt.  **1.0 is the top of the range** -
     * there the first valve is just clipping on a normalised take and the second
     * has long since stopped caring - and 0.5 is the useful working setting.
     * Above 1.0 nothing new happens except more compression and more of the
     * recording's own noise floor; 2.5 was tried and is too much.
     */
    /* Mirrors of gain[0] and gain[1], kept because every knob, tool argument and
     * note in this tree calls them that.  ag_amp_build copies them in. */
    float drive;
    /*
     * V1b's input coupling capacitor, in farads; 0 keeps the 2.2 nF the netlist
     * has.
     *
     * The one component value here that a listener is expected to move, and
     * apps/cktbench/ckt_circuits.c already marks it as the one chosen by ear
     * rather than copied.  Against the 470 k it sees it corners at 142 Hz, which
     * keeps the open low E's fundamental out of the valve that does the clipping
     * - that is what makes a 2203 tight instead of muddy.  It is also why this
     * chain measures ten decibels short of a Marshall capture below 100 Hz.  A
     * Plexi uses 22 nF there and corners at 14.
     */
    float ccouple2;
    /*
     * The first valve's cathode resistor and plate load, in ohms; 0 keeps what
     * the netlist has.
     *
     * These two are why the chain has no clean setting, and the netlist says so
     * out loud: V1a is hot-rodded to 820 R and 220 k against a stock 2k7 and
     * 100 k, which runs it at a warmer bias with far more gain, so it clips
     * itself instead of handing a big clean signal to V1b.  Measured on a take
     * with nothing above 1 kHz, so that every decibel up there is something the
     * chain made: at the default the second valve is driven hard enough to put
     * hash 49 dB under the signal at 3.5-6 kHz, and the same chain with the
     * interstage attenuator at 0.3 puts *nothing* there - 67 dB down, which is
     * the 16-bit file's own floor.  A low drive setting on the hot chain is not
     * a clean setting; it is a quiet badly-clipped one, which is exactly what it
     * sounds like.
     *
     * So these are exposed, because "what a stock 2203 front end does" has to be
     * a setting rather than a rebuild.
     */
    float rcath1, rplate1;
    /* Units of output per volt at the second plate.  The second plate swings
     * about a hundred volts, so 1/150 lands near full scale; ag_amp_peak
     * reports what it actually did, which is how this should be set. */
    float master;

    /*
     * How deep the top cut is, in dB, and zero means the old behaviour: a
     * second-order low-pass at `top_hz`.  Non-zero makes it a first-order shelf
     * of that depth instead, which is what a pedal's tone network actually is -
     * six decibels an octave, not twelve.
     */
    float top_db;
    /*
     * A TOP CUT AT THE OUTPUT, WHICH IS A DIFFERENT COMPONENT FROM top_hz
     *
     * Same shape - a resistor into a capacitor, first order, and split into two
     * shelves when it is deeper than 20 dB - and a different place in the chain:
     * this one is built into the output block, after the last clipping stage,
     * where a stompbox's tone control is soldered.  top_hz above is a valve
     * amplifier's *input* rolloff and is built in front of the first valve.
     *
     * Two pairs of numbers rather than one pair plus a flag saying which end,
     * because there is nothing for the matching layer to decide here: where a
     * component sits is the schematic's business.  Each is built where it is, and
     * a device with both sets both.
     *
     * That it matters at all is measured, on the TS9, with nothing else changed:
     * described with top_hz - so in front of the diodes - it read 5.64 dB loud
     * and 13.78 of swing error; in its own place, 2.03 and 1.89.  In front, the
     * diodes are fed a signal that has already lost its top and the fizz they then
     * make is never filtered by anything; behind, the pedal removes what it made.
     */
    float out_top_hz;
    float out_top_db;
    /* The two voicing numbers.  Zero disables either. */
    float top_hz;
    float mid_hz, mid_db, mid_q;

    /*
     * The voicing bank, in front of both valves.
     *
     * This is the tone stack in the sense that matters here: it decides what
     * reaches the grids, and therefore what gets distorted, which is a different
     * thing from equalising the output afterwards.  The measurement that made it
     * necessary: against a NAM capture of a Marshall on the same take, this chain
     * was flat where the reference had a presence hump - its amplifier adds about
     * 9 dB at 4 kHz over the dry signal and this one added nothing.  Raising the
     * gain does not fix that (two decibels over two octaves of drive) and neither
     * does equalising the output, because the harmonics have to be *made* before
     * they can be shaped.
     *
     * Fitted rather than dialled - tube_render "fit" runs the take, compares
     * third-octave bands against the reference and moves each band by a fraction
     * of its error until it stops improving.
     */
    /*
     * ONE BANK PER STAGE, AND WHY IT IS NOT ONE BANK IN FRONT OF EVERYTHING
     *
     * `voice[i]` is the matching block in front of stage `i`.  It used to be a
     * single bank in front of the whole chain, which cannot express the thing a
     * real high-gain amplifier does: a little bass taken out **before each**
     * clipping valve rather than a lot before the first one.  With one bank the
     * fit has to choose, and what it chooses is audible - the crunch model's
     * single bank came out at +5.68 dB at 200 Hz in front of three clipping
     * stages, which is the opposite of what the circuit wants.
     *
     * The order these are fitted in is a level ladder, and the ladder is the
     * reason per-stage banks are worth their cost.  In a cascade the last valve
     * sees the biggest signal, so it distorts first: at a level low enough that
     * only it is working, any difference in harmonic structure from the capture
     * belongs to it, and `voice[n-1]` is what corrects it.  Raise the level until
     * the stage before it joins in and the new difference is that stage's, and so
     * on back to the first.  Absolute tone is not the target at this stage - the
     * output bank and the impulse close that at the end - the target is what each
     * valve is fed and therefore what kind of distortion it makes.
     *
     * What this is not allowed to be is a redistribution of the amplifier's own
     * gain.  `gain[]`, `drive` and every component value are the circuit
     * and stay where the schematic put them; a measured attempt to fix the gain
     * distribution by attenuating in front of the last valve with `gain[3]`
     * improved the knee and broke the model ordering, because a cut with no
     * matching lift earlier is just a quieter amplifier.  Trims live here, in the
     * matching layer, where they are per stage and can compensate each other.
     */
    ag_amp_band_t voice[AG_AMP_STAGES][AG_AMP_VOICE_N];

    /*
     * A flat trim in front of each stage, in dB, part of the same matching block.
     *
     * Seven peaking sections cannot make a clean broadband change, and the step
     * that needs one is the last one in the ladder: when every stage's harmonic
     * structure matches but the chain as a whole compresses by the wrong amount,
     * the fix is the same trim on every stage rather than a new shape on one.
     * Zero is unity and costs one multiply.
     */
    float vtrim[AG_AMP_STAGES];

    /*
     * The tone stack, after both valves and before the cabinet.
     *
     * Not a second thought and not an equaliser bolted on: **a 2203's tone stack
     * is after its clipping stages**, hanging off V1b's plate, and this model did
     * not have one.  Putting the whole correction in front of the valves was
     * tried first and it saturates - the fit ran both top bands into their
     * eighteen decibel limit and was still five decibels short at 5 kHz.  The
     * reason is that a hard limiter blanks a small signal in the presence of a
     * large one, so pre-emphasising a top end the guitar barely has cannot get it
     * through: the presence in a real Marshall comes from after the distortion,
     * which is exactly where the real one puts it.
     *
     * So the split is physical rather than convenient.  In front of the valves
     * goes what decides *what gets distorted*; behind them goes what shapes what
     * came out.
     */
    ag_amp_band_t tone[AG_AMP_VOICE_N];

    /*
     * Build the tone stack's top bands as high shelves rather than peaking
     * sections.
     *
     * This is the fix for a rattle, and the rattle was measured rather than
     * guessed.  A listener's recording of it, amplified 30 dB: bursts of a tone
     * at 3.9 kHz, one every half period, and **the same 3.9 kHz on two
     * different notes** - which a harmonic cannot do and a filter resonance
     * does by definition.  The two fitted top bands are peaking sections at
     * +9.95 dB and +14.94 dB with Q 1, whose poles sit at radius 0.80 and ring
     * for 1.4 ms; behind a clipper, every clipping edge pings them twice a
     * cycle.  The note moves, the ring does not, and a fixed pitch under a
     * moving note is exactly what does not sound like an amplifier.
     *
     * A shelf has one real pole and cannot ring, and a real presence control is
     * a shelf.  Bands at or above 2 kHz become shelves; the low ones stay
     * peaking, because a 100 Hz shelf would lift everything under it.
     */
    /*
     * The tone stack, and the three knobs that belong to it.
     *
     * A **knob block** in the sense of README.md's first section: a real passive
     * network in the position the schematic puts it, live at run time, and not a
     * matching filter.  `tone_stack` is 0 when a model has no netlist for one -
     * which is the case for any model whose schematic has not been read yet, and
     * saying so is better than lending it somebody else's.
     *
     * Each pot is 0 to 1 and **0.5 is noon**, which is where every model is
     * fitted and what a capture with "EQ at 5" in its name was made at.
     */
    int   tone_stack;
    float tone_treble, tone_mid, tone_bass;

    /*
     * WHERE THE BASS IS CUT, AND WHY IT HAS TO BE PER STAGE
     *
     * A multiplier on each stage's coupling-cap corner: 1.0 is the schematic, 2.0
     * is half the capacitor.  0 means 1.0, so a zeroed config is the netlist.
     *
     * This is a **matching** control in front of a clipping stage, and it exists
     * because a lumped one cannot do the job.  Every high gain design cuts bass
     * before each hot stage - that is what the small interstage capacitors are for,
     * 2.2 nF in a 2203 - and it is cut *a little before each* rather than a lot
     * before the first.  The two are not the same amplifier even when the overall
     * response matches: with one big cut in front, the first stage clips a signal
     * with no bottom in it and everything after amplifies what that made; with the
     * cut distributed, every stage clips a signal that still has some bass and adds
     * its own low-order products, and the later cuts trim what the earlier stages
     * made.
     *
     * What the difference measures as, on the crunch model against its own capture
     * (`tube_render harm`, two tones at 147 and 196 Hz): the amplifier puts the
     * 49 Hz difference product 42 to 58 dB under the tones and gets *quieter* as it
     * is driven harder, while this chain put it 33 to 39 dB down and got *louder* -
     * 5 to 25 dB more subsonic intermodulation, growing with level.  That is what a
     * listener calls farting on a low chord, and it is what a distributed cut is
     * for.
     *
     * Costs nothing at run time: the coupling high-pass is already a biquad per
     * stage, and this only moves its corner.  It is also readable back as a
     * component value, which a bank of matching filters is not - a fitted 1.5 here
     * says "this amplifier behaves as if that capacitor were 1.5 nF, not 2.2".
     */
    float couple_mul[AG_AMP_STAGES];
    /*
     * How far the block between one plate and the next grid may exceed unity,
     * in decibels.  Zero is the physical answer - a coupling capacitor, a grid
     * leak and a pot are all passive, and the valve's own gain is in its curve -
     * and ag_amp_no_interstage_gain takes the block down to it.
     *
     * It is a knob because being honest here costs the overdrive: with the block
     * at unity this chain compresses 18.7 dB less than the capture does, since
     * only one of its two valves clips and it can only be made to clip harder by
     * being handed more than the plate in front of it can swing.  What that
     * buys, and what it costs in the bias wander that is heard as a wrong note,
     * is a measurement rather than an opinion - so the number is here to be
     * moved and listened to.
     */
    float interstage_db;

    int tone_shelf;    /*
     * How much of the grid-current tail each axis keeps, as a fraction of the
     * curve's whole swing - see ag_tube_bake.  It bounds the fit from outside;
     * inside that bound the fit follows the signal.
     */
    float axis_tol;

    /*
     * Explicit axes, one pair per stage, used instead of fitting when hi > lo.
     *
     * A preset can carry the ranges the fit found rather than finding them
     * again, which saves three passes of baking at load time.  It also makes a
     * measurement possible that otherwise is not: the fit converges to a
     * slightly different axis for every table size, and on a hard-clipped
     * waveform that moves the clipping edges by a fraction of a sample - which
     * dominates any difference the table size makes.  Comparing table sizes
     * means holding the axis still.
     */
    float axis_lo[AG_AMP_STAGES], axis_hi[AG_AMP_STAGES];
} ag_amp_cfg_t;

typedef struct ag_amp {
    ag_amp_cfg_t   cfg;
    int            n; /* live stages */
    ag_tube_spec_t spec[AG_AMP_STAGES];
    ag_tube_t      tube[AG_AMP_STAGES];
    /*
     * One filter block in front of each stage, carrying that stage's own coupling
     * capacitor and cathode shelf.  The first one runs at the base rate, outside
     * the oversampled region; every later one runs inside it, because it sits
     * between two nonlinearities.
     */
    ag_biq_chain_t pre[AG_AMP_STAGES];
    ag_biq_chain_t f_out; /* the output coupling high pass */
    float          gain[AG_AMP_STAGES];
    int            linear[AG_AMP_STAGES];
    /* The matching bank in front of each stage.  voice[0] is at the base rate,
     * because it is in front of the oversampled region; every later one runs
     * inside it, at fs*os, because it sits between two nonlinearities.  Then the
     * output bank, after the valves and before the cabinet. */
    ag_biq_chain_t voice[AG_AMP_STAGES], tone;
    /* cfg.vtrim in linear form, one per stage. */
    float          vtrim[AG_AMP_STAGES];
    /* The passive stack, between the output coupling capacitor and the master -
     * where a 2203 has it.  Built only when cfg.tone_stack says so. */
    ag_tone_t      stack;
    int            stack_on;
    /*
     * Which valve the tone stack sits in front of, or `n` for the output.
     *
     * A 2203 has it between V1b and V2a, and a passive Marshall network loses
     * about twenty decibels - so on a three-valve chain this is not decoration,
     * it is what keeps the last grid off the end of its axis.  On two valves the
     * stack has nothing after it but linear blocks, so it commutes and this is
     * `n`.
     */
    int            stack_at;
    /* Three cascaded halfbands; 2x uses the first, 4x the first two. */
    ag_os8_t       os;
    int            tab_n;
    /* What the build decided, so that it can be printed rather than assumed. */
    float top_hz_used, mid_hz_used, out_top_hz_used;
    float couple_hz[AG_AMP_STAGES];
    float shelf_hz[AG_AMP_STAGES], shelf_db[AG_AMP_STAGES];
    float load_hz;
    float latency_samples;

    /* What the run saw. */
    float    peak_out;
    uint32_t samples;
    /*
     * Whatever left each stage, most recently.  A diagnostic tap and nothing
     * else: it costs one store per stage per oversampled sample and it exists
     * so that a line can be traced to the block that first has it, instead of
     * being argued about from the far end of the chain.
     */
    float tap[AG_AMP_STAGES];
} ag_amp_t;

/* fs, two stages of a hot JCM800 front end, 4x with antialiasing, and the two
 * voicing numbers where the design asked for them.  The same as ag_amp_model
 * with AG_AMP_MODEL_JCM800, and it stays the name every tool and note uses. */
void ag_amp_defaults(ag_amp_cfg_t *cfg, float fs);

/*
 * The whole configuration for one model: topology, per-stage gains, the two
 * voicing knobs and both voicing banks.
 *
 * Everything the runtime needs is decided here, so a tool switching models
 * replaces the config rather than patching it - which matters because the models
 * differ in stage count, and a stage count changed under a config whose gains
 * were meant for another topology is silent nonsense rather than an error.
 *
 * An unknown id falls back on the JCM800 rather than failing, because this is
 * called from argument parsing on both the host and the guest and a preset that
 * plays the wrong amplifier is easier to notice than one that does not load.
 */
void ag_amp_model(ag_amp_cfg_t *cfg, int model, float fs);

/* "jcm800", "bogner", "slo"; "?" for an id out of range. */
const char *ag_amp_model_name(int model);

/*
 * The tone stack netlist a model carries, if it has one.  Returns 0 when the
 * model has no schematic for it yet - which is not the same as a model with a
 * flat tone stack, and the difference is the point: an amplifier whose values
 * nobody has read gets no controls rather than borrowed ones.
 */
int ag_amp_tone_spec(int model, ag_tone_spec_t *out);

/* 1 if this model is a pedal: no loudspeaker anywhere, and its tone control is
 * its own circuit.  See the note on the implementation - the usual 6.3 kHz test
 * calls a Tube Screamer a loudspeaker. */

/* The reverse, for a command line or an environment variable.  -1 if unknown. */
int ag_amp_model_by_name(const char *s);

/*
 * The capture this model's voicing was fitted against, as a path under the
 * tree, or NULL for a model that was never fitted to one.
 *
 * The pairing was written down only in the prose of each model's block - "a NAM
 * capture of a Peavey 5150 on the red channel", with the filename in a command
 * line further down - which is fine for a reader and no use to a tool that wants
 * to run the reference.  Anything that needs to put the real amplifier next to
 * this one asks here instead of carrying its own table, because two tables
 * disagree eventually and the disagreement is silent: the wrong capture still
 * plays, and it still sounds like an amplifier.
 */
const char *ag_amp_model_capture(int model);

/*
 * Has this model's voicing been fitted against a capture of the amplifier it is
 * named for, or is it a starting point?
 *
 * Worth a function rather than a comment, because the difference is not audible
 * as wrongness - an unfitted chain sounds like a working amplifier that is not
 * the one on the label - and every tool that prints a model name should be able
 * to print this beside it.  Fitting is step 4 of README.md, "Authoring a preset
 * for another amplifier", and it needs a reference render of the same DI take
 * through a capture of the real thing.
 */
int ag_amp_model_fitted(int model);

/*
 * The netlist stage `i` of `model` is baked from - what ag_amp_build hands the
 * bake, exposed because the tools bake single stages of their own (tube_render
 * "curve", "spec") and a second copy of the mapping is a second thing to keep in
 * step.  Past a model's own stages it repeats the last one; see the comment on
 * the implementation.
 */
void ag_amp_spec(int model, int i, ag_tube_spec_t *out);

/*
 * A plucked two-note decay, which is what the axes have to be fitted to.
 *
 * Not a nicety and not a sine.  Fitting an axis to a steady tone is a rake
 * docs/08 already stepped on: a stage driven into grid current moves its own
 * operating point over tens of milliseconds, and it is the attack and the decay
 * that visit the driving points a steady tone never reaches.  Two notes rather
 * than one because table error is a deterministic function of the input and so
 * lands exactly on the harmonics of a single tone, where nothing can see it.
 */
void ag_amp_probe_pluck(float *buf, int n, float fs);

/*
 * Bake both stages and design the filters.  `scratch` is one ag_ckt_t borrowed
 * for the bake; `tab` must hold AG_AMP_STAGES * 2 * n floats and outlive `a`.
 * Pass n = 0 for AG_AMP_TAB_N, which is what the measurement settled on;
 * anything else is there so that the measurement can be repeated.
 *
 * `probe` is the signal the axes are fitted to - ag_amp_probe_pluck, repeated as
 * needed - and passing NULL falls back to fitting them to the curve alone, which
 * is measurably worse and is mostly there so that the difference can be shown.
 *
 * Why the signal and not the curve: the grid conducts through 78k, so the curve
 * never plateaus - at +8 V the plate still has an eighth of its swing left to
 * give and at +128 V it still has 1.6% - and an axis fitted to where it finally
 * stops moving runs to two kilovolts.  The signal at the second grid stops
 * around forty.  So five sixths of a curve-fitted table lies where the music
 * never goes, and tube_render "res" measures what that costs: against the
 * signal-fitted axis, the best curve-fitted one is 48 dB away in the 1.5-5 kHz
 * band, and no table size recovers it.
 *
 * The fit is iterated three times and the ranges are only ever widened, because
 * each stage's axis changes the signal the next one sees; unioning instead of
 * replacing is what makes it converge rather than chase its own tail.
 *
 * Returns 0 on success.  Measured on the guest: 212 M instructions, 882 ms at
 * 240 MHz, of which the DC sweeps are about two thirds and the four probe runs
 * the rest.  Handing the axes back in `cfg.axis_lo` / `axis_hi` skips the fitting
 * and brings it to 364 ms.  Both are preset loads; see ag_tube.h for the change
 * that would make it a knob and why it has not been made.
 */
int ag_amp_build(ag_amp_t *a, ag_ckt_t *scratch, const ag_amp_cfg_t *cfg,
                 float *tab, int n, const float *probe, int probe_n);

/*
 * Re-do only what a knob needs.  `drive` and `master` are plain
 * multiplies and need nothing; the voicing numbers and `os` need the filters
 * designed again, which is microseconds.  Neither touches the curve.  Returns 0
 * on success.
 */
int ag_amp_set_voicing(ag_amp_t *a, const ag_amp_cfg_t *cfg);

/*
 * The same, but without clearing the filters - which is what a knob under a
 * playing signal needs.
 *
 * ag_amp_set_voicing resets, and that is right for a tool that measures one
 * setting after another: the tail of the previous one is not part of the answer.
 * It is wrong for somebody turning a knob, because zeroing eight biquads and
 * three halfbands under a ringing note is a click and a dropout on every
 * keypress.  Changing a coefficient under a running filter is a step, but a step
 * proportional to how far the knob moved.
 *
 * Refuses a change of `os`, which is the one setting whose state cannot survive:
 * the halfbands hold history at the old rate and the blocking capacitor's
 * constants are per sample.  Use ag_amp_set_voicing for that one.
 */
int ag_amp_set_knobs(ag_amp_t *a, const ag_amp_cfg_t *cfg);

void ag_amp_reset(ag_amp_t *a);

float ag_amp_tick(ag_amp_t *a, float x);

/* Lookups that ran off the end of a curve, summed over the stages.  Not an
 * error here - the curve is flat outside and the lookup extends it flat, which
 * is the physical answer - but a number worth watching, since "flat enough" is
 * a claim. */
uint32_t ag_amp_clamped(const ag_amp_t *a);

/*
 * How much gain there is between valve i-1's plate and valve i's grid, at the
 * loudest frequency in the band a valve is handed: the coupling network, the
 * fitted bank, the mid lift and the trim, all together, as a ratio.
 *
 * It should never be above one.  Everything in that path is passive - a coupling
 * capacitor, a grid leak, a pot where there is one - and the valve's own gain
 * and its grid divider are inside the baked curve, so nothing is left there that
 * can amplify.  `ag_amp_no_interstage_gain` takes any excess back out of the
 * trim, leaving every band's frequency, Q and relative decibels alone, and
 * returns how many stages it had to correct.
 *
 * The reason this needs saying at all: every other block in the chain reads a
 * ratio, so a boost in front of a valve and a cut after it score the same and
 * the fit is free to invent one.  Blocking answers to volts instead, and that is
 * where it shows - as a coupling capacitor charged past the supply rail.
 */
float ag_amp_interstage_gain(const ag_amp_t *a, int i);
int   ag_amp_no_interstage_gain(ag_amp_t *a);

/* ------------------------------------------------------------------------ */
/* Presets: a chain as bytes                                                 */
/* ------------------------------------------------------------------------ */

/*
 * A preset is the whole chain - component values, voicing, and the baked tables
 * - as one block of bytes, and the point of it is what loading does *not* need:
 * no circuit solver, no Newton, no axis fitting, no probe signal.  The host bakes
 * once; the target reads bytes and plays.  An application that only loads presets
 * never links ag_ckt at all.
 *
 * Two things that fall out and are worth knowing.  **The tables do not depend on
 * the sample rate** - a curve is a DC sweep and has no time in it - so one preset
 * plays at 22.05 and at 48 kHz; only the filters and the blocking constants are
 * rebuilt at load, from the component values that travel with it.  And **the axis
 * has to be baked at the top of the drive range**, because it is fitted to the
 * signal and the signal follows the gain knob: measured, the second stage's axis
 * runs to +32 V at drive 0.5 and +147 V at drive 2.5.  Freezing it at the top
 * costs about 10 dB of table accuracy - -74 dB instead of -84 - which is still
 * well under everything else in the model, and the alternative is a knob that
 * runs off the end of its own table.
 *
 * The bytes are the native layout, little-endian, which both the host and the
 * chip are.  The header carries the sizes of the two structs it embeds and the
 * loader refuses a mismatch, so a field added to either is a readable error
 * rather than silent garbage.
 */
#define AG_AMP_PRESET_MAGIC 0x50425541u /* "AUBP" */
/*
 * 4: `cfg.interstage_db` was added - the config block is longer again, and the
 *    same `cfg_size` check refuses a version 3 blob rather than reading the new
 *    field out of the old one's tail.
 * 2: `cfg.couple_mul` was added, so the config block is longer.  The loader
 * refuses anything that does not match `sizeof(ag_amp_cfg_t)` anyway - a preset
 * that loaded with a shifted config would play something else entirely - but the
 * version says *why* rather than leaving it as a size mismatch.
 */
/*
 * 3: the cabinet impulse lives in the preset.  It is part of what a chain sounds
 * like, and keeping it in a separate wav meant a preset could be copied while its
 * loudspeaker stayed behind.
 */
#define AG_AMP_PRESET_VER   4u

/*
 * Bytes a preset with this shape occupies.  `ir_frames` may be zero: a device
 * with no loudspeaker - a pedal - carries no impulse and says so.
 */
uint32_t ag_amp_preset_size(int n_stages, int tab_n, int ir_frames);

/*
 * Write `a` into `buf`, with `ir` as its loudspeaker.  int16 because that is what
 * `ag_ir_load` takes on the chip and what the impulse is on disk, so nothing is
 * converted anywhere.  Pass NULL and 0 for a device that has none.  Returns the
 * bytes written, or 0 if it did not fit.
 */
uint32_t ag_amp_preset_save(const ag_amp_t *a, const int16_t *ir,
                            int ir_frames, uint32_t ir_rate, void *buf,
                            uint32_t cap);

/*
 * The impulse a preset carries: a pointer into `buf`, valid as long as `buf` is,
 * and NULL when the preset has none.  Separate from the load so that a caller
 * who only wants the amplifier does not have to know about loudspeakers.
 */
const int16_t *ag_amp_preset_ir(const void *buf, uint32_t n, uint32_t *frames,
                                uint32_t *rate);

/*
 * Read a preset into `a`, with its tables copied into `tab` - which must hold
 * AG_AMP_STAGES * 3 * tab_n floats and outlive `a`, exactly as for ag_amp_build.
 * `fs` is the rate this chain will run at; everything rate-dependent is rebuilt
 * from the stored component values.  Returns 0 on success.
 */
int ag_amp_preset_load(ag_amp_t *a, const void *buf, uint32_t n, float *tab,
                       float fs);

/* Floats one blended stage needs for its two source curves. */
#define AG_AMP_BLEND_FLOATS(tab_n) (6 * (tab_n))

/*
 * Mix a diode curve into one stage, and leave it able to be mixed again.
 *
 * `work` is AG_AMP_BLEND_FLOATS(a->tab_n) floats the caller owns and keeps: the
 * valve curve and the clipper curve live there, and the stage's live tables are
 * the blend of them.  The first call bakes both - a preset load's worth of work,
 * and it needs the solver - and every call after that only mixes, which is a few
 * thousand multiply-adds and no solver at all.  That is what makes this a knob:
 * measured, a 2048-point stage re-blends in about 50 000 instructions, a fifth
 * of a millisecond, so it can run at block rate while somebody turns it.
 *
 * `mix` is 0 for all valve, 1 for all diodes.  The clipper is baked over the same
 * axis, normalised to the same small-signal slope and then scaled to the same
 * RMS over it, so the knob changes the shape of the distortion and not the
 * level - slope alone matched the quiet parts and left the diode side audibly
 * quieter in the loud ones, because the two ceilings differ.
 *
 * Returns 0 on success.  Pass `scratch` NULL to re-blend a stage that has been
 * blended before; that path never touches a circuit.
 */
int ag_amp_blend_stage(ag_amp_t *a, ag_ckt_t *scratch, int stage, float mix,
                       float *work);

#endif /* AG_AMP_H */
