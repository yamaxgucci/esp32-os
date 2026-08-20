/*
 * ag_amp - see ag_amp.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_amp.h"

#include "ag_mathf.h"

/*
 * The JCM800 is the whole of what follows, up to the per-model overrides at the
 * end - it is the chain everything in this tree was measured on, so it is the
 * base every other model is written as a departure from rather than a peer.
 * That is not deference: a departure can be read and argued with, while three
 * independent lists of forty numbers cannot.
 */
void ag_amp_model(ag_amp_cfg_t *cfg, int model, float fs)
{
    if (cfg == 0) {
        return;
    }
    if (model < 0 || model >= AG_AMP_MODEL_N) {
        model = AG_AMP_MODEL_JCM800;
    }
    cfg->model = model;
    cfg->fs = fs > 0.0f ? fs : 22050.0f;
    /*
     * Four times with antialiasing, because of what it buys against what it
     * costs: the difference between -13 dB of non-harmonic products and -56 dB.
     * Set os to 1 to hear what that difference is; the number belongs beside the
     * listening, which is what tools/tube_render.c prints.
     *
     * What it costs, measured on the guest under -icount rather than estimated
     * (`tubebench bench <out> <model>`, instructions a sample and percent of one
     * core at 22.05 kHz):
     *
     *                    jcm800, 2 valves   bogner, 3   slo, 4 valves
     *      1x                658   6.0%             -      -
     *      1x + adaa         929   8.5%     1206      1448  13.3%
     *      4x               3277  30.1%             -      -
     *      4x + adaa        4155  38.2%     5426      6094  56.0%
     *      8x + adaa        8081  74.2%    10838     11902 109.4%
     *
     * Both of these carry a tone stack; the two unstarred JCM800 rows predate it
     * and are 69 instructions light.  69 is what the stack costs, at the base rate,
     * once a sample - the same figure at 1x and at 4x, which is how a number proves
     * it is outside the oversampled region.
     *
     * The bogner column is a three-valve chain with a tone stack, measured after
     * the crunch model was rebuilt from a Shiva's topology; it sits where a third
     * valve puts it, between the other two.
     *
     * **Four valves at eight times does not fit**, and that is the first row in
     * this table that has ever said so: 109% of a core before the cabinet - with
     * three valves it is 99.6%, which is the same answer with no margin.  At the
     * default four times it is 56%, and the cabinet's 9% takes the four-valve chain
     * to about two thirds of one core - which leaves the rest of the machine a
     * third, and is the reason the oversampling is a setting rather than a
     * constant.
     *
     * It was 249 before the loop was written out.  Sixteen multiply-adds as a
     * four by four with two-dimensional indexing came to fifteen instructions
     * each, nearly all of it address arithmetic; flat, with every coefficient
     * named, it is 69.  The estimate before either measurement was "about 45",
     * which is the third time in this file that counting arithmetic has been
     * wrong about what a processor does with it.
     *
     * The cabinet convolution after it is 981 instructions, 9% of a core, so the
     * default is a little under half a core for two valves and a little under
     * three fifths for three.  The third valve cost 1041 instructions a sample -
     * a quarter more than the two-valve chain, and not a third, because the
     * up/down filter pair and the first filter block are paid once whatever the
     * topology; only what lives inside the oversampled region is multiplied.
     *
     * These numbers are larger than the ones this comment used to carry, and the
     * difference is not drift: they now include both voicing banks, which were
     * ticked and never built until the fix recorded in design().  Seven biquads
     * at the base rate is what the fitted voicing actually costs, and it had been
     * free by being absent.
     */
    cfg->os = 4;
    cfg->adaa = 1;
    cfg->blocking = 1;

    cfg->n_stages = 2; /* the JCM800 front end this is voiced for */
    /* Half, not one: one is the top of the range, not the middle of it. */
    cfg->drive = 0.5f;
    cfg->g12 = 1.0f;
    {
        int b;
        for (b = 0; b < AG_AMP_STAGES; b++) {
            cfg->gain[b] = 1.0f;
            cfg->linear[b] = 0;
        }
    }
    /*
     * The tone stack, on for any model that has a netlist for one, with every
     * pot at noon.  Noon is not a taste setting - it is the position the whole
     * matching procedure is defined at, and the position the captures whose
     * names say "EQ at 5" were made at.
     */
    {
        ag_tone_spec_t ts;
        cfg->tone_stack = ag_amp_tone_spec(model, &ts) == 0 ? 1 : 0;
    }
    cfg->tone_treble = 0.5f;
    cfg->tone_mid = 0.5f;
    cfg->tone_bass = 0.5f;

    /* The schematic's own corners, until something measured says otherwise. */
    {
        int cm;
        for (cm = 0; cm < AG_AMP_STAGES; cm++) {
            cfg->couple_mul[cm] = 1.0f;
        }
    }
    cfg->ccouple2 = 0.0f; /* the netlist's 2.2 nF */
    cfg->rcath1 = 0.0f;   /* the netlist's hot-rodded 820 R */
    cfg->rplate1 = 0.0f;  /* and its 220 k */
    cfg->tone_shelf = 0;  /* the fitted peaks, until the refit says otherwise */
    /*
     * At drive 0.5 on a guitar take this lands within a decibel of full scale;
     * tube_render prints the master that would have, since the right value moves
     * with the drive and the recording.
     *
     * Twice it has moved a long way, and both times it was following the chain
     * rather than setting anything.  1/220 to 1/1200 when the voicing was first
     * fitted: fifteen decibels of presence on a pick attack is fifteen decibels
     * of peak.  1/1200 to here when the **real tone stack** went in: a passive
     * Marshall network at noon throws away 13.4 dB at 1 kHz, and that is what a
     * master volume is for.
     *
     * And a third time, to 1/359, when the ladder took the front apart and the
     * output bank was refitted on top of it.  Worth writing down because of what
     * went wrong on the way: with the old 1/255 still in place the chain came out
     * at **+3.1 dBFS** on the take, and a clipped render still fits its bands, so
     * nothing in the spectral numbers said a word about it.  The master follows
     * whatever the chain peaks at and `tube_render render` prints the value; it has
     * to be re-read after every refit, not once.
     */
    cfg->master = 1.0f / 359.0f;

    cfg->top_hz = 12000.0f; /* clamped to 0.45*fs, and the clamp is printed */
    cfg->mid_hz = 700.0f;
    cfg->mid_db = 5.0f;
    cfg->mid_q = 0.8f;

    /*
     * The two voicing banks, fitted rather than dialled.
     *
     * tube_render "fit" ran the same take through this chain and through a NAM
     * capture of a Marshall, compared third-octave bands, and moved each band by
     * a fraction of its own error until it stopped improving: from 8.11 dB rms
     * flat to **0.97 dB rms**, over 63 Hz to 6.3 kHz.
     *
     * **These numbers got smaller when the real tone stack went in, and that is
     * the argument for building circuits rather than fitting them.**  Without the
     * stack the fit needed +6.12 dB of bass and +9.95/+14.94 of presence and
     * reached 1.27 dB rms; with the passive network in its own place after V1b -
     * which supplies +9.6 dB at 40 Hz, a decibel of dip at 630 and a rising top -
     * the same fit wants -1.09 dB of bass and +7.45/+12.55 of presence and gets
     * to 0.97.  A matching filter standing in for a missing network is doing two
     * jobs and doing neither of them the way the amplifier does.
     *
     * Which bank each band landed in is the physics rather than a convenience.
     * From 200 Hz to 1.6 kHz the guitar has the energy to drive the valves, so
     * what sits in front decides what gets distorted - and that is where nearly
     * six decibels at 1.6 kHz belongs, because the reference amplifier makes
     * upper-midrange harmonics that this one was not making at all.  Above and
     * below that, in front is the wrong place: a limiter blanks a small top end
     * rather than passing it (fitted in front, both top bands hit their 18 dB
     * limit and were still 5 dB short), and more bass in front of a clipping
     * valve is not more bass out of it but less midrange (10 nF in V1b's coupling
     * capacitor matched the low end and cost 4.7 dB at 315 Hz).  So the top and
     * the bottom go behind the valves - which is where a 2203 keeps its tone
     * stack anyway.
     *
     * These numbers belong with the cabinet they were fitted through.  Against
     * the Vox impulse in the tree the same fit only reaches 2.53 dB rms, because
     * seven octave-wide bands cannot undo a loudspeaker: re-run "fit" after
     * changing the cabinet.
     */
    {
        static const ag_amp_band_t pre[2][AG_AMP_VOICE_N] = {
            { /* stage 1 */
                { 100.0f, 0.96f, 1.0f }, { 200.0f, -4.29f, 1.0f },
                { 400.0f, -3.54f, 1.0f }, { 800.0f, 6.21f, 1.0f },
                { 1600.0f, 3.96f, 1.0f }, { 3150.0f, -2.04f, 1.0f },
                { 5000.0f, -1.29f, 1.0f }
            },
            { /* stage 2 */
                { 100.0f, 3.64f, 1.0f }, { 200.0f, 5.14f, 1.0f },
                { 400.0f, 0.64f, 1.0f }, { 800.0f, -6.86f, 1.0f },
                { 1600.0f, -1.61f, 1.0f }, { 3150.0f, -0.86f, 1.0f },
                { 5000.0f, -0.11f, 1.0f }
            }
        };
        static const float vtrim[2] = { 3.09f, 7.91f };
        static const ag_amp_band_t post[AG_AMP_VOICE_N] = {
            { 100.0f, -1.85f, 1.0f }, { 200.0f, 3.36f, 1.0f },
            { 400.0f, 7.15f, 1.0f }, { 800.0f, 2.88f, 1.0f },
            { 1600.0f, 3.47f, 1.0f }, { 3150.0f, 8.53f, 1.0f },
            { 5000.0f, 11.91f, 1.0f }
        };
        int b, st;
        for (b = 0; b < AG_AMP_VOICE_N; b++) {
            cfg->tone[b] = post[b];
        }
        for (st = 0; st < 2; st++) {
            cfg->vtrim[st] = vtrim[st];
            for (b = 0; b < AG_AMP_VOICE_N; b++) {
                cfg->voice[st][b] = pre[st][b];
            }
        }
    }

    /*
     * EVERY BLOCK PAST THE LAST LIVE STAGE, EMPTIED - AND NOT THE LIVE ONES
     *
     * The array is AG_AMP_STAGES long and a model uses as many of it as it has
     * valves, so the tail has to be defined: an empty bank is a unity gain and a
     * zero trim is a multiply by one, which is what an unused stage should cost.
     *
     * `st` starts at n_stages rather than at 1, and that is not a detail.  The
     * first version of this ran over every stage from the second on, from *after*
     * the model's own tables were assigned a few lines above - so it quietly wiped
     * the very banks the ladder had just put there, and the two-valve model played
     * with its second block empty and its trims at zero while the source said
     * otherwise.
     */
    {
        int st, b;
        for (st = cfg->n_stages > 0 ? cfg->n_stages : 1; st < AG_AMP_STAGES;
             st++) {
            cfg->vtrim[st] = 0.0f;
            for (b = 0; b < AG_AMP_VOICE_N; b++) {
                cfg->voice[st][b].hz = cfg->voice[0][b].hz;
                cfg->voice[st][b].db = 0.0f;
                cfg->voice[st][b].q = 1.0f;
            }
        }
    }

    cfg->axis_tol = 0.01f; /* settled by tube_render "res" - see ag_tube.h */
    /*
     * All four, not the two this used to zero.
     *
     * The fast path in ag_amp_build takes the axes from here when they are set,
     * and it takes them for every live stage - so a third stage left with
     * whatever was on the caller's stack is a table baked over a garbage range,
     * which is silence or noise rather than an error.  It never fired while
     * every model had two stages.
     */
    {
        int s;
        for (s = 0; s < AG_AMP_STAGES; s++) {
            cfg->axis_lo[s] = 0.0f;
            cfg->axis_hi[s] = 0.0f;
        }
    }

    if (model == AG_AMP_MODEL_JCM800) {
        return;
    }

    /*
     * The departures, per model.
     *
     * The component values are not here - they are in ag_tube_spec_bogner and
     * ag_tube_spec_slo, which is where a resistor belongs.  What is here is
     * everything those functions cannot carry: how hard each stage is driven,
     * and the voicing.
     *
     * Both voicings below are **fitted**, each against a NAM capture of the
     * amplifier it is named for, and each model's own block gives the reference,
     * the error it reached and the command that reproduces it.  Until those
     * captures arrived they were starting points, and what that was worth is
     * worth keeping: the crunch model scored 5.52 dB rms with a worst band of
     * 10.0 dB against the real thing, which is a chain whose *components* were
     * argued for and whose *spectrum* had never been compared with anything.
     *
     * Two things the fit is not.  It is not a substitute for the topology - both
     * models were re-cut before it was run, because a voicing bank cannot supply
     * gain a chain does not have, and the first `slo` was 19 dB short (see
     * cfg->gain[2] below).  And it is not portable: every band belongs with the
     * cabinet it was fitted through, so `AG_CAB_IR` is part of the answer.  For
     * these two that is a fair trade rather than a caveat, because both captures
     * are amplifier-only - `gear_type` "amp", no loudspeaker in them - so the
     * same impulse sits on both sides of the comparison and cancels.
     */
    if (model == AG_AMP_MODEL_BOGNER) {
        /*
         * Fitted, against a NAM capture of a **Bogner Ecstasy 101B** on the crunch
         * channel ("Bogner Ecstasy - Bright Crunchy Rock", modelled by
         * maestrodimusica): **2.01 dB rms** over 63 Hz to 6.3 kHz.  That capture's
         * `gear_type` is "amp" rather than "amp_cab", so it has no loudspeaker in it
         * and the same impulse sits on both sides of the comparison - a choice of
         * speaker that cancels exactly beats an impulse extracted from the
         * reference.
         *
         * **This is worse than the number it replaces, and the trade is the point.**
         * The two-valve version of this model fitted to 0.86 dB - and compressed
         * 4.0 dB where its own capture compresses 18.2.  The three-stage Shiva
         * chain compresses 17.2 and fits to 2.01.  One of those two is an amplifier
         * that behaves like the thing it is copying and is a couple of decibels off
         * in the spectrum; the other matched the spectrum with a chain whose dynamic
         * behaviour was nothing like it.  Under the rule at the top of README.md the
         * circuit comes first and the fit closes what is left, so 2.01 is the honest
         * starting error and not a regression.
         *
         * Worth recording about the fit itself: on this chain it reaches its best at
         * **iteration 1** and is at 4.27 by iteration 40.  At 92 dB of gain a
         * decibel added in front of the valves does not arrive as a decibel of
         * anything - it arrives as a different clipping pattern - so the update
         * stops being monotone and the loop walks away from its own minimum.  The
         * tool keeps the best rather than the last, which is the only reason there
         * is a usable answer here at all.
         *
         * Reproduce with:
         *   AG_MODEL=bogner tube_render fit build/listen/ref_bogner_22k_cab.wav \
         *                                   build/listen/di_gc2_22k.wav 40 0.5
         * and pass 0 iterations to score these numbers without moving them.
         */
        static const ag_amp_band_t pre[3][AG_AMP_VOICE_N] = {
            { /* stage 1 */
                { 100.0f, -0.43f, 1.0f }, { 200.0f, -1.18f, 1.0f },
                { 400.0f, 7.82f, 1.0f }, { 800.0f, -6.43f, 1.0f },
                { 1600.0f, 1.82f, 1.0f }, { 3150.0f, -2.68f, 1.0f },
                { 5000.0f, 1.07f, 1.0f }
            },
            { /* stage 2 */
                { 100.0f, 2.25f, 1.0f }, { 200.0f, -3.75f, 1.0f },
                { 400.0f, -6.00f, 1.0f }, { 800.0f, 1.50f, 1.0f },
                { 1600.0f, 4.50f, 1.0f }, { 3150.0f, 1.50f, 1.0f },
                { 5000.0f, -0.00f, 1.0f }
            },
            { /* stage 3 */
                { 100.0f, 5.46f, 1.0f }, { 200.0f, 3.21f, 1.0f },
                { 400.0f, 0.96f, 1.0f }, { 800.0f, -3.54f, 1.0f },
                { 1600.0f, -2.04f, 1.0f }, { 3150.0f, -3.54f, 1.0f },
                { 5000.0f, -0.54f, 1.0f }
            }
        };
        static const float vtrim[3] = { -3.89f, 3.43f, 0.46f };
        static const ag_amp_band_t post[AG_AMP_VOICE_N] = {
            { 100.0f, 5.15f, 1.0f }, { 200.0f, 6.49f, 1.0f },
            { 400.0f, 0.71f, 1.0f }, { 800.0f, 1.03f, 1.0f },
            { 1600.0f, -0.15f, 1.0f }, { 3150.0f, 3.70f, 1.0f },
            { 5000.0f, 3.82f, 1.0f }
        };
        int b, st;
        for (b = 0; b < AG_AMP_VOICE_N; b++) {
            cfg->tone[b] = post[b];
        }
        for (st = 0; st < 3; st++) {
            cfg->vtrim[st] = vtrim[st];
            for (b = 0; b < AG_AMP_VOICE_N; b++) {
                cfg->voice[st][b] = pre[st][b];
            }
        }
        /*
         * Three gain stages, which is what the Shiva's crunch channel has, and the
         * gain pot between the first two **at noon**.
         *
         * 0.55 stood here before, and it was a dialled number wearing a knob's
         * name: it was chosen because it made a crunch out of a two-valve chain
         * that did not have enough stages to be one.  What the amplifier actually
         * is measures 18.2 dB of compression on its own capture against that
         * chain's 4.0, so the missing thing was a valve, not a setting.
         */
        cfg->n_stages = 3;
        cfg->g12 = 0.5f;
        cfg->gain[2] = 1.0f;
        /* Mid-forward and a little lower than the Marshall's, in front of the
         * clipping valve where it changes what gets distorted.  TASTE. */
        cfg->mid_hz = 650.0f;
        cfg->mid_db = 4.0f;
        cfg->mid_q = 0.9f;
        /*
         * A third of the JCM800's presence, and as shelves rather than peaks.
         *
         * The amount is taste and is named as taste; the *shape* is not.  The
         * fitted peaking sections at +10 and +15 dB have poles at radius 0.80 and
         * ring for 1.4 ms, which behind a clipper is the 3.9 kHz rattle README.md
         * spends eight experiments on - a fixed pitch under a moving note.  A
         * shelf has one real pole and cannot ring, and a real presence control is
         * a shelf.  A new model has no reason to inherit the rattle.
         */
        cfg->tone_shelf = 1;
        /*
         * The rule above, applied: what peaked at -1 dBFS on the DI take at drive
         * 0.5, which `render` prints on every run.  The JCM800 comes out at 1/1156
         * by the same measurement against its stored 1/1200, so all three models
         * peak within half a decibel of each other - which is the property that
         * matters when two of them are compared by ear (rake 19 in docs/08: two
         * renders at different levels cannot be told apart from two different
         * models).
         *
         * It went 1/274, then 1/1054 with the fitted presence, and to here when the
         * third valve and the tone stack arrived.  That is the sentence
         * the JCM800's master carries too: sixteen decibels of fitted presence on
         * a pick attack is sixteen decibels of peak, and the master is what takes
         * it back.
         */
        cfg->master = 1.0f / 336.0f; /* re-read after the ladder refit */
        return;
    }

    /* AG_AMP_MODEL_SLO */
    {
        /*
         * Fitted, against a NAM capture of a **Peavey 5150** on the red channel
         * ("5150 RED GRINDR", modelled by cwolfbrandt, `gear_type` "amp" and so
         * with no loudspeaker of its own): **0.72 dB rms**, best at iteration 3 of
         * 40, which is the closest of the three models.
         *
         * A Soldano netlist against a Peavey capture on purpose.  The two
         * amplifiers are the same design to within their cathode capacitors - the
         * forums that hold both schematics report identical plate and cathode
         * resistors stage for stage - and the SLO's circuit exists as text where
         * the 5150's exists as a scan.  A netlist that can be read beats one that
         * has to be guessed at from a photograph, and the capture is the arbiter
         * either way.
         *
         * The shape of the answer is the 5150 in two numbers: **the midrange is
         * cut hard in front of the valves** - 10.5 dB at 200 Hz and 6.7 at 400 -
         * with 1.6 kHz lifted 6.6 dB there, while behind them the low end comes
         * *down* 9.5 dB and 3.15 kHz goes up 9.4.  A scooped amplifier, and the fit
         * put the scoop where it changes what gets distorted rather than where it
         * shapes what came out.
         *
         * It went from 1.71 dB to 0.72 when the guessed topology was replaced by
         * the published one, and the bass cut behind the valves is the sign of what
         * is still missing: this chain has 22 nF everywhere and no bright caps, so
         * it arrives with more low end than the amplifier has and the fit takes it
         * away again.
         *
         * Reproduce with:
         *   AG_MODEL=slo tube_render fit build/listen/ref_5150red_22k_cab.wav \
         *                                build/listen/di_gc2_22k.wav 40 0.5
         */
        /*
         * ITERATION 1 OF THE STEP-BY-STEP WALK.  NOT A FINISHED VOICING.
         *
         * Every earlier fitted answer for this model was thrown away: three of them
         * scored better and better on spectral rms and sounded worse and worse.  So
         * the match is being rebuilt one measured step at a time, each step looked
         * at before the next runs, and the output bank stays at zero until the walk
         * reaches it.  `tube_render step1` and `tube_render iter` are the two
         * commands; `duo` and `onset` are the diagnostics they are read against.
         *
         * WHAT ONE ITERATION IS
         *
         * The metric throughout is **products over notes**: two notes a just fifth
         * apart, and the energy in every line that is not one of them over the
         * energy in the two.  Harmonics and intermodulation together, on one grid of
         * the 41.205 Hz difference tone so nothing is counted twice, phase-averaged.
         * Percent is amplitude: 10% is -20 dB of that energy ratio.
         *
         *   step 1  find the input voltage where a *light* overdrive appears - 10% -
         *           and put ours on the amplifier's with the same flat trim on every
         *           pre-stage block.  Measured: the amplifier is there at 3.14 mV at
         *           the first grid and this chain was there at 4.91 mV, 3.9 dB late,
         *           closed by +0.96 dB on each of the four blocks.
         *
         *   then    one rung at a time, last block to first.  The ceiling is
         *           measured first - the amplifier saturates at -9.1 dB, which is
         *           33% half a decibel under - and the rung is min(10%, ceiling / n),
         *           so 8.2% here.  Each rung's *voltage* is read off the amplifier;
         *           then the block in front of that stage is shaped there, held at
         *           zero mean so its average cannot undo step one.
         *
         * WHAT ITERATION 1 GOT, AND WHAT IT DID NOT
         *
         *      block  rung    volts      rms before -> after
         *        4    8.2%   2.77 mV      4.08 -> 1.83 dB
         *        3   16.5%   7.38 mV      2.59 -> 1.50 dB
         *        2   24.7%  16.53 mV      4.54 -> 1.92 dB
         *        1   33.0%  99.05 mV      3.08 -> 2.68 dB
         *
         * Every rung's spectrum improved.  What got worse is where the rungs *are*:
         * after the pass our 8.2% point sits 3.1 dB under the amplifier's, 16.5% is
         * 7.6 dB under and 24.7% is 9.5 dB under, where step one had left the first
         * of them exact.  That is not a fault in the procedure, it is the coupling
         * the procedure has - a block with zero mean still changes what reaches the
         * valves, so shaping it moves the level at which a given amount of overdrive
         * appears.  Closing it is iteration 2: step one again, then the blocks again,
         * with smaller corrections each time.
         */
        static const ag_amp_band_t pre[4][AG_AMP_VOICE_N] = {
            { /* stage 1 */
                { 100.0f, 0.00f, 1.0f }, { 200.0f, 3.75f, 1.0f },
                { 400.0f, 0.75f, 1.0f }, { 800.0f, -3.75f, 1.0f },
                { 1600.0f, -5.25f, 1.0f }, { 3150.0f, 4.50f, 1.0f },
                { 5000.0f, 0.00f, 1.0f }
            },
            { /* stage 2 */
                { 100.0f, 5.25f, 1.0f }, { 200.0f, 2.25f, 1.0f },
                { 400.0f, 2.25f, 1.0f }, { 800.0f, 5.25f, 1.0f },
                { 1600.0f, 0.75f, 1.0f }, { 3150.0f, 3.00f, 1.0f },
                { 5000.0f, 3.75f, 1.0f }
            },
            { /* stage 3, the cold clipper */
                { 100.0f, -3.75f, 1.0f }, { 200.0f, 0.75f, 1.0f },
                { 400.0f, 3.75f, 1.0f }, { 800.0f, -3.75f, 1.0f },
                { 1600.0f, -3.75f, 1.0f }, { 3150.0f, -4.50f, 1.0f },
                { 5000.0f, -4.50f, 1.0f }
            },
            { /* stage 4, the recovery stage */
                { 100.0f, 0.43f, 1.0f }, { 200.0f, -4.07f, 1.0f },
                { 400.0f, 4.93f, 1.0f }, { 800.0f, -5.57f, 1.0f },
                { 1600.0f, -5.57f, 1.0f }, { 3150.0f, -4.82f, 1.0f },
                { 5000.0f, -0.32f, 1.0f }
            }
        };
        /* Step 1: the same flat trim on every block, which is what a flat
         * coefficient of the tone blocks means. */
        static const float vtrim[4] = { 0.96f, 0.96f, 0.96f, 0.96f };
        /*
         * REFITTED WITH THE BASS CUT IN, AND THE SHAPE OF IT IS THE EVIDENCE
         *
         * The previous answer had **-9.50 dB at 100 Hz behind the valves** and
         * -6.69 in front at 400: the fit was taking away low end that the chain
         * should never have made, because every coupling was 22 nF.  With the cut
         * in front of the hot stages (`couple_mul` below), the post bank's 100 Hz
         * band comes back to +1.14 and the front's 400 and 800 turn round from
         * -6.69 and -0.73 to -0.69 and +5.27.
         *
         * That is what a correction looks like when the thing it was correcting has
         * been fixed: the filters stop fighting the circuit.  Both banks now sit
         * inside +-11 dB with only one band past 6, where before the post bank
         * needed 9.4 dB of presence and -9.5 of bass at once.
         *
         * Reproduce with:
         *   AG_EVAL_DI=build/listen/gc1_di_22050.wav \
         *   argon match assets/audio/guitar-di/5150red.nam -Model slo 8
         */
        /* At zero until the walk reaches the output. */
        static const ag_amp_band_t post[AG_AMP_VOICE_N] = {
            { 100.0f, 0.00f, 1.0f },  { 200.0f, 0.00f, 1.0f },
            { 400.0f, 0.00f, 1.0f },  { 800.0f, 0.00f, 1.0f },
            { 1600.0f, 0.00f, 1.0f }, { 3150.0f, 0.00f, 1.0f },
            { 5000.0f, 0.00f, 1.0f }
        };
        int b, st;
        for (b = 0; b < AG_AMP_VOICE_N; b++) {
            cfg->tone[b] = post[b];
        }
        for (st = 0; st < 4; st++) {
            cfg->vtrim[st] = vtrim[st];
            for (b = 0; b < AG_AMP_VOICE_N; b++) {
                cfg->voice[st][b] = pre[st][b];
            }
        }
        /*
         * Four stages, which is what the schematic has and what this chain's
         * maximum happens to be: OD1, OD2, the cold clipper, and the recovery
         * stage that drives the tone stack.  The previous version had three,
         * because three was what the topology had been guessed at as.
         */
        cfg->n_stages = 4;
        /*
         * The gain pot, **at noon**, and that is now the only thing here that is
         * not a component.
         *
         * Everything else that attenuates in this amplifier is in the netlist:
         * ag_tube_spec_slo carries the 470 k series resistor as the second stage's
         * source impedance and the 500 k pot as its grid leak, which is a divider
         * of 0.49 before the wiper is considered, and the 220 k / 330 k pair in
         * front of the cold clipper is another 0.56.  What is left for a constant
         * is where the wiper sits, and the rule is that it sits in the middle.
         *
         * This replaces a hand-picked 0.15 that stood in for the whole network,
         * and the history is worth keeping because the mistake was not arithmetic.
         * That number had been swept against the hump depth of a NAM *Marshall
         * crunch* capture - a metric that rewards distorting less - which walked it
         * down until the third valve was a 12 dB insertion loss with a kink in it,
         * and the chain came out 19 dB quieter than a two-valve Marshall.  A metric
         * borrowed from another amplifier optimises you into being that amplifier,
         * badly; `test_model_gain_order` is the check that would have caught it.
         */
        cfg->g12 = 0.5f;
        /* No pot between the later stages: the dividers there are resistors, and
         * they are in the netlist where resistors belong. */
        cfg->gain[2] = 1.0f;
        /*
         * 1.0 INTO THE RECOVERY STAGE, AND AN ATTENUATOR HERE WAS TRIED AND
         * REJECTED BY A TEST
         *
         * `tube_render knee` sweeps a note from 36 dB down to full scale through
         * one stage, then two, three, four, with the capture's own column beside
         * them - the last valve distorts first, so a level sweep separates the
         * stages.  What it found: the fourth valve is **already saturated at the
         * quietest probe** (third harmonic 22 dB above what three stages make), so
         * this model has no regime at all in which the last valve distorts gently,
         * and its harmonic content moves 12 dB across the range while the
         * amplifier's moves 0.7.
         *
         * The cheap fix is to turn the last stage down, and it works on that
         * metric: 0.35 here puts the fourth valve's knee between 0.05 and 0.12 -
         * a quiet regime again - and cuts the spread across playing levels from
         * 12 dB to 6.  It is also wrong, and `test_model_gain_order` says why: at
         * 0.5 and at 0.35 the four-stage lead channel's small-signal gain falls
         * **below the three-stage crunch model's**, which is the exact failure that
         * test was written for after a hand-picked divider once made this the
         * cleanest of the three amplifiers.  A lead channel that is quieter than a
         * crunch channel is not a lead channel, whatever its harmonics look like.
         *
         * So the distribution is wrong somewhere that cannot be patched at the end:
         * the earlier stages have to do more of the work, and within "every knob at
         * noon" that means the netlist - the plate loads and the cold clipper's
         * dividers - not a gain constant.  Reading that schematic again is the
         * open work, and the knee table is the measurement to judge it by.
         */
        cfg->gain[3] = 1.0f;
        /* TASTE, and less of it than either two-valve model has: four stages make
         * their own upper midrange, so lifting what reaches the grids is asking a
         * filter for what a valve is already supplying. */
        cfg->mid_hz = 800.0f;
        cfg->mid_db = 2.0f;
        cfg->mid_q = 0.9f;
        cfg->tone_shelf = 1;
        /*
         * THE BASS CUT IN FRONT OF THE HOT STAGES: 22 nF -> 2.8 nF
         *
         * A matching value rather than a schematic one, and the largest single
         * improvement any measurement in this module has produced.  The netlist
         * gives every coupling 22 nF, so nothing takes bass out before the three
         * stages that clip, and C * rgrid comes to 22 ms - the period of the 49 Hz
         * beat of a low fourth, so the grid charge tracks that beat rather than
         * smoothing it.
         *
         * Measured against the 5150 capture with `harm` and `sens`: the 49 Hz
         * difference product sat **0.8 dB** under the notes where the amplifier puts
         * it **17.8 dB** under - the whole chain riding a subsonic wobble, which is
         * what a listener calls farting on a low chord.  Switching blocking off
         * accounted for 9.1 dB of it, so the grid charge was the mechanism.  x8 on
         * the stages after the first - 2.8 nF, an ordinary value in front of a hot
         * stage - takes it to -7.9 dB, and the mid lift pass one then chooses takes
         * it to **-13.4**, with compression landing on 18.9 dB against the
         * capture's 18.9.
         *
         * The first stage stays at the schematic: its corner is 1.6 Hz and the
         * multiplier of it is still under 13 Hz, which measures as doing nothing.
         *
         * THE RULE THESE ARE CHOSEN BY: AN OCTAVE BELOW THE LOWEST NOTE
         *
         * x8 on all three was wrong, and the measurement that says so is the second
         * harmonic of a low E.  The three do not share a grid leak, so the
         * schematic's corners are 7.1, 19.7 and 7.0 Hz, and x8 makes them 57, 158
         * and 56.  The middle one is above the 82 Hz open E outright; and a
         * first-order high-pass at 57 Hz still costs an 82 Hz fundamental 2 dB while
         * costing its 164 Hz second harmonic a quarter of that, so three of them
         * stack into several decibels of asymmetry that no valve made.
         *
         * Measured on the low E, H2 in dB under the fundamental, capture -37.7,
         * -17.4, -28.8 at the three levels:
         *
         *      x8, x4, x8   (57, 79, 56 Hz)    -6.8   -7.4   -7.1
         *      x4, x2, x4   (28, 39, 28 Hz)    -9.5  -20.0  -19.7
         *      x1 x1 x1     (7, 20, 7 Hz)      -7.3  -10.5   -9.6
         *
         * So a moderate cut beats both no cut and a big one, which is the domain
         * rule this started from, and the corner has to be an **octave** below the
         * lowest note rather than merely under it.  x4, x2, x4 - 28, 39 and 28 Hz,
         * all under 41 - and C * rgrid stays 5.5 to 11 ms, still clear of the 22 ms
         * that made the grid charge track the 49 Hz beat instead of smoothing it.
         *
         * The cost is the fart: at x8 the 49 Hz product sat 2.7 to 7.5 dB above the
         * capture's and at x4/x2/x4 it is 8.7 dB above at full scale.  That is the
         * ladder's to cut now, per stage, which is what a real design does - and
         * `ladder` measures it, one-sided, so a fit cannot buy anything by adding
         * subsonic intermodulation.  Trading thirty decibels of false asymmetry on
         * every low note for six of a product on a double stop is not a close call.
         */
        cfg->couple_mul[1] = 4.0f;
        cfg->couple_mul[2] = 2.0f;
        cfg->couple_mul[3] = 4.0f;
        /*
         * -1 dBFS on the same take at drive 0.5, as above.  It has followed the
         * chain four times now - 1/48, 1/308, 1/903, and here - and every move was
         * something else being corrected: the crunch front end, then the hot one,
         * then the fitted presence, and now the published netlist with its own
         * dividers and a tone stack.  A master volume is a consequence.
         */
        /*
         * 1/130, and it has followed the chain a fifth time.
         *
         * 1/48, 1/308, 1/903, 1/84 and now here, and every move was something else
         * being corrected rather than a level being chosen: the crunch front end,
         * the hot one, the fitted presence, the published netlist with its own
         * dividers and a tone stack, and now the bass cut in front of the hot
         * stages with the banks refitted around it.  At 1/84 this chain peaked at
         * **+2.8 dBFS** on the fitting take, which a listening render shows up as a
         * file clipped flat - `tube_render render` prints the master that would have
         * peaked at -1 dBFS, and that is where this number comes from every time.
         */
        cfg->master = 1.0f / 417.0f; /* re-read after the ladder refit */
    }
}

void ag_amp_defaults(ag_amp_cfg_t *cfg, float fs)
{
    ag_amp_model(cfg, AG_AMP_MODEL_JCM800, fs);
}

const char *ag_amp_model_name(int model)
{
    if (model == AG_AMP_MODEL_JCM800) {
        return "jcm800";
    }
    if (model == AG_AMP_MODEL_BOGNER) {
        return "bogner";
    }
    if (model == AG_AMP_MODEL_SLO) {
        return "slo";
    }
    return "?";
}

int ag_amp_model_by_name(const char *s)
{
    int m;
    if (s == 0) {
        return -1;
    }
    for (m = 0; m < AG_AMP_MODEL_N; m++) {
        const char *n = ag_amp_model_name(m);
        int         i = 0;
        while (n[i] != '\0' && s[i] == n[i]) {
            i++;
        }
        if (n[i] == '\0' && s[i] == '\0') {
            return m;
        }
    }
    return -1;
}

int ag_amp_model_fitted(int model)
{
    /*
     * All three, and each against a capture of the amplifier it is named for:
     * the JCM800 against "Mars Gain 8", the crunch model against a Bogner
     * Ecstasy 101B, the three-valve one against a Peavey 5150 red channel.  The
     * numbers and the commands are at each model's bank in ag_amp_model.
     *
     * The function stays, and it should stay even at three out of three, because
     * the next model will start at zero and an unfitted chain does not sound
     * broken - it sounds like a working amplifier that is not the one on the
     * label.
     */
    return model >= 0 && model < AG_AMP_MODEL_N ? 1 : 0;
}

/*
 * The linear network of both stages, as sections.
 *
 * F1 and F3 run at the base rate; F2 runs inside the oversampled region because
 * it sits between two nonlinearities, so it is designed at fs*os.  That is the
 * only reason `os` is a rebuild rather than a flag.
 */
/*
 * A matching-layer trim in dB as a multiplier.
 *
 * Bounded rather than trusted: the fitter that writes these is a search, and a
 * search that has found a way to make the error small by making the chain silent
 * has not found anything.  +-24 dB is wider than any honest answer and narrow
 * enough that a runaway cannot hide.
 */
static float trim_lin(float db)
{
    if (db > 24.0f) {
        db = 24.0f;
    }
    if (db < -24.0f) {
        db = -24.0f;
    }
    return db == 0.0f ? 1.0f : ag_powf(10.0f, db / 20.0f);
}

static void design(ag_amp_t *a)
{
    const float fs = a->cfg.fs;
    const float fos = fs * (float)(a->cfg.os < 1 ? 1 : a->cfg.os);
    ag_biq_t   *s;
    int         i;

    for (i = 0; i < AG_AMP_STAGES; i++) {
        ag_biq_chain_init(&a->pre[i]);
    }
    ag_biq_chain_init(&a->f_out);
    for (i = 0; i < AG_AMP_STAGES; i++) {
        ag_biq_chain_init(&a->voice[i]);
    }
    ag_biq_chain_init(&a->tone);
    for (i = 0; i < a->n; i++) {
        a->couple_hz[i] = ag_tube_couple_hz(&a->spec[i]);
        ag_tube_shelf(&a->tube[i], &a->spec[i], &a->shelf_hz[i], &a->shelf_db[i]);
    }
    a->load_hz = ag_tube_load_hz(&a->spec[a->n - 1]);

    /*
     * One block in front of each stage, carrying that stage's own coupling
     * capacitor and cathode shelf.  The first is at the base rate because it is
     * outside the oversampled region; the rest are inside it and are designed at
     * fos.  The two voicing numbers keep the places they were fitted in: the top
     * cut in front of everything, the mid lift in front of the last stage.
     */
    a->top_hz_used = 0.0f;
    a->mid_hz_used = 0.0f;
    for (i = 0; i < a->n; i++) {
        ag_biq_chain_t *ch = &a->pre[i];
        const float     rate = (i == 0) ? fs : fos;
        if (a->couple_hz[i] > 0.0f && (s = ag_biq_chain_push(ch)) != 0) {
            /* The corner the spec carries, which is already the schematic's value
             * divided by cfg.couple_mul - see ag_amp_build. */
            (void)ag_biq_hp1(s, rate, a->couple_hz[i]);
        }
        if (a->shelf_db[i] < -0.01f && (s = ag_biq_chain_push(ch)) != 0) {
            (void)ag_biq_shelf1(s, rate, a->shelf_hz[i], a->shelf_db[i]);
        }
        if (i == 0 && a->cfg.top_hz > 0.0f && (s = ag_biq_chain_push(ch)) != 0) {
            a->top_hz_used = ag_biq_lp2(s, rate, a->cfg.top_hz, 0.70710678f);
        }
        if (i == a->n - 1 && i > 0 && a->cfg.mid_hz > 0.0f &&
            a->cfg.mid_db != 0.0f && (s = ag_biq_chain_push(ch)) != 0) {
            a->mid_hz_used =
                ag_biq_peak(s, rate, a->cfg.mid_hz, a->cfg.mid_db, a->cfg.mid_q);
        }
    }

    /*
     * The two voicing banks, which until now were ticked and never built.
     *
     * Both chains are initialised here, reset by ag_amp_reset and run by
     * ag_amp_tick - and nothing ever pushed a section into either of them, so
     * every band in cfg.voice and cfg.tone did exactly nothing.  The
     * measurement that found it, on the JCM800 whose banks are the fitted ones:
     * a render with AG_NO_TONE, which zeroes the whole post bank, came out
     * **bit-identical in every third-octave band** to one without it; and `fit`
     * asked to score the stored answer without moving it reported 7.69 dB rms
     * against the 1.29 dB the fit had recorded when it found it.  An empty
     * filter chain is a unity gain, so this failed by sounding like a working
     * amplifier with its presence control at zero.
     *
     * Where each bank goes is in the header, and it is physics rather than
     * convenience: the pre bank at the base rate in front of the valves, because
     * it decides *what gets distorted*, and the tone stack after them, because a
     * limiter cannot be pre-emphasised into supplying a top end the guitar does
     * not have.  ag_amp_tick already ran them in those two places.
     */
    {
        int b;
        /*
         * One bank per stage, each at the rate its stage runs at: the first
         * outside the oversampled region and the rest inside it, because a filter
         * between two nonlinearities has to see the same samples the valves do.
         * Peaking sections throughout - what these correct is what a coupling
         * network and a bright cap do, and neither is a brick wall.
         */
        for (i = 0; i < a->n; i++) {
            const float rate = (i == 0) ? fs : fos;
            for (b = 0; b < AG_AMP_VOICE_N; b++) {
                const ag_amp_band_t *v = &a->cfg.voice[i][b];
                if (v->hz > 0.0f && v->db != 0.0f &&
                    (s = ag_biq_chain_push(&a->voice[i])) != 0) {
                    (void)ag_biq_peak(s, rate, v->hz, v->db,
                                      v->q > 0.0f ? v->q : 1.0f);
                }
            }
        }
        for (b = 0; b < AG_AMP_VOICE_N; b++) {
            const ag_amp_band_t *t = &a->cfg.tone[b];
            if (t->hz <= 0.0f || t->db == 0.0f ||
                (s = ag_biq_chain_push(&a->tone)) == 0) {
                continue;
            }
            /*
             * At and above 2 kHz a shelf rather than a peak when the model asks
             * for it - see cfg.tone_shelf, and ag_biq_hshelf1 for the 1.4 ms of
             * ring that a +15 dB peaking section has and a shelf cannot.  Below
             * that they stay peaks, because a 100 Hz shelf lifts everything under
             * it including the DC an asymmetric clipper leaves behind.
             */
            if (a->cfg.tone_shelf && t->hz >= 2000.0f) {
                (void)ag_biq_hshelf1(s, fs, t->hz, t->db);
            } else {
                (void)ag_biq_peak(s, fs, t->hz, t->db, t->q > 0.0f ? t->q : 1.0f);
            }
        }
    }

    /*
     * The tone stack, designed but not reset: a pot may be turned under a
     * sounding note, and the capacitor voltages in a real stack carry across
     * when a wiper moves.  ag_tone_design leaves the state alone for exactly
     * that reason, so this is safe on the knob path as well as at build time.
     */
    a->stack_on = 0;
    if (a->cfg.tone_stack) {
        ag_tone_spec_t ts;
        if (ag_amp_tone_spec(a->cfg.model, &ts) == 0 &&
            ag_tone_design(&a->stack, &ts, fs, a->cfg.tone_treble,
                           a->cfg.tone_mid, a->cfg.tone_bass) == 0) {
            a->stack_on = 1;
        }
    }

    /*
     * F3: the output coupling capacitor.
     *
     * Not decoration.  The second stage clips asymmetrically, so its output
     * carries a DC that moves with how hard it is being hit, and a cabinet
     * convolution fed a moving DC puts it straight into its own low end.
     */
    if (a->load_hz > 0.0f && (s = ag_biq_chain_push(&a->f_out)) != 0) {
        (void)ag_biq_hp1(s, fs, a->load_hz);
    }

    a->latency_samples = 0.0f;
    if (a->cfg.os == 2) {
        a->latency_samples = 2.0f * ag_os2_latency();
    } else if (a->cfg.os == 4) {
        a->latency_samples = 2.0f * ag_os4_latency();
    } else if (a->cfg.os == 8) {
        a->latency_samples = 2.0f * ag_os8_latency();
    }
    /*
     * Antialiasing centres each average half an oversampled sample late, once
     * per stage.  At 4x that is a quarter of a base sample for the pair - a
     * fifth of a degree of phase at 110 Hz, and the reason it is written down
     * rather than ignored is that the test has to compare against a delayed
     * reference or it measures the delay instead of the filtering.
     */
    if (a->cfg.adaa) {
        a->latency_samples +=
            (float)AG_AMP_STAGES * 0.5f / (float)(a->cfg.os < 1 ? 1 : a->cfg.os);
    }
}

void ag_amp_probe_pluck(float *buf, int n, float fs)
{
    int i;
    if (buf == 0 || n <= 0 || fs <= 0.0f) {
        return;
    }
    for (i = 0; i < n; i++) {
        const float t = (float)i / fs;
        /* A pick transient of a couple of milliseconds and a second of decay. */
        const float env = ag_expf(-t * 1.6f) * (1.0f - ag_expf(-t * 400.0f));
        float       s = 0.0f;
        int         h;
        /* A and E, eight harmonics each, so the products land between the
         * harmonics where a table error cannot hide on them. */
        for (h = 1; h <= 8; h++) {
            float sn, cs;
            ag_sincosf(6.28318531f * 110.0f * (float)h * t, &sn, &cs);
            s += sn / (float)h;
            ag_sincosf(6.28318531f * 164.81f * (float)h * t, &sn, &cs);
            s += sn / (float)h;
        }
        buf[i] = 0.22f * env * s;
    }
}

/*
 * How much wider than the observed range each axis is baked.
 *
 * Fifteen percent, which docs/08 measured against 35: every percent of it is
 * resolution spent where the signal rarely goes, and it cannot be zero because
 * the probe is not the performance.
 */
#define AMP_MARGIN 0.15f
/*
 * Each axis changes the signal the next stage sees, so the fit has to be
 * iterated: two passes leave a chain whose stages barely moved at first with
 * axes too narrow for the signal they then make.  Four rather than the three
 * docs/08 measured for the two-axis model, because this fit starts from a
 * deliberately narrow window and so has further to travel; the extra pass is a
 * coarse bake and costs about five hundred DC solves.
 */
#define AMP_FIT_PASSES 4

/*
 * How many points the tables have while the axes are being fitted.
 *
 * The fitting passes only have to find out *where* the signal goes, and that is
 * not sensitive to how finely the curve is resolved - the union and the margin
 * absorb the difference.  Baking at full size for every pass cost 438 M
 * instructions on the guest, of which the DC sweeps were nearly all; a coarse
 * table for the passes and a full one at the end brought it to 212 M, and the
 * measured difference in the render was under half a decibel.
 */
#define AMP_FIT_N 256

/*
 * Which netlist stage `i` of this model is baked from.
 *
 * Past the end of a model's own stages it repeats the last one, which is what
 * the two-stage chain always did when asked for four - a fourth valve is another
 * of the third, and a preset carrying a real topology per stage would replace
 * this whole function.  The rule matters because tube_live's stage-count key and
 * test_stage_counts both walk 1 to AG_AMP_STAGES on every model.
 */
/*
 * Which passive tone stack a model has, and whether it has one at all.
 *
 * The JCM800 does: a 2203 puts a Marshall TMB after V1b, and ag_tone_marshall
 * carries those values with its own note about where they came from.  The other
 * two do not, and that is deliberate rather than unfinished - their schematics
 * have not been read, so giving them the Marshall's network would be exactly the
 * substitution the rule in README.md exists to prevent: a component value that
 * was really a guess, wearing a knob.
 *
 * A model with no stack is a model with no tone controls, which is honest and
 * audible; the matching blocks still do their job around it.
 */
int ag_amp_tone_spec(int model, ag_tone_spec_t *out)
{
    if (out == 0) {
        return -1;
    }
    if (model == AG_AMP_MODEL_JCM800) {
        ag_tone_marshall(out);
        return 0;
    }
    if (model == AG_AMP_MODEL_BOGNER) {
        /*
         * A 2203 tone stack, because the channel is a 2203 derivative, driven from
         * **1 k** rather than from a plate: the cathode follower this chain does
         * not model is a buffer, and its output impedance is the only thing about
         * it the network downstream can tell.  The values are the Marshall set and
         * are a substitution - nobody publishes the Shiva's own.
         */
        ag_tone_marshall(out);
        out->rsrc = 1.0e3f;
        return 0;
    }
    if (model == AG_AMP_MODEL_SLO) {
        /*
         * The SLO's stack is a Marshall TMB - the amplifier is Marshall-derived
         * and every published analysis of it calls the tone network standard - so
         * it gets the same values, driven from its own recovery stage: 220 k plate
         * load against the valve's rp, about 48 k.
         *
         * **The values are a substitution and the driving impedance is a
         * simplification**, both stated because the rule says so.  Neither source
         * for this amplifier lists the stack's own capacitors, and the real one is
         * fed by a cathode follower, which is a buffer this chain does not model -
         * a follower's output impedance is nearer 1 k than 48 k, and a passive
         * network's response depends on what drives it.  What that costs is a
         * measurable amount of insertion loss and a little of the shape, and it is
         * the next thing to fix here rather than something the fit should hide.
         */
        ag_tone_marshall(out);
        out->rsrc = 48.0e3f;
        return 0;
    }
    return -1;
}

void ag_amp_spec(int model, int i, ag_tube_spec_t *out)
{
    if (model == AG_AMP_MODEL_BOGNER) {
        /* Three of its own: the stock front end, the hot valve, the third stage. */
        ag_tube_spec_bogner(out, i > 2 ? 2 : i);
    } else if (model == AG_AMP_MODEL_SLO) {
        /* Four of its own, so nothing repeats: OD1, OD2, the cold clipper and the
         * recovery stage each have their own entry. */
        ag_tube_spec_slo(out, i > 3 ? 3 : i);
    } else {
        ag_tube_spec_jcm800(out, i > 0 ? 1 : 0);
    }
}

static int bake_all(ag_amp_t *a, ag_ckt_t *scratch, float *tab, int n,
                    const float *lo, const float *hi)
{
    const float fos = a->cfg.fs * (float)(a->cfg.os < 1 ? 1 : a->cfg.os);
    int         i;
    for (i = 0; i < a->n; i++) {
        /* Laid out for a->tab_n whatever n is, so a coarse pass and the final
         * one write the same regions and the last one wins. */
        float *t = tab + (3 * i) * a->tab_n;
        float *h = t + n;
        float *g = t + 2 * a->tab_n;
        ag_tube_init(&a->tube[i]);
        if (a->linear[i]) {
            continue; /* no table: gain and filters only */
        }
        if (ag_tube_bake(&a->tube[i], scratch, &a->spec[i], t, h, g, n,
                         lo != 0 ? lo[i] : 1.0f, hi != 0 ? hi[i] : 0.0f,
                         a->cfg.axis_tol) != 0) {
            return -1;
        }
        (void)ag_tube_set_adaa(&a->tube[i], a->cfg.adaa);
        /* The oversampled rate, because both valves live inside that region and
         * the capacitor is charged by every sample that passes through it. */
        (void)ag_tube_set_blocking(&a->tube[i], &a->spec[i], fos,
                                   a->cfg.blocking);
    }
    return 0;
}

int ag_amp_build(ag_amp_t *a, ag_ckt_t *scratch, const ag_amp_cfg_t *cfg,
                 float *tab, int n, const float *probe, int probe_n)
{
    float cap_lo[AG_AMP_STAGES], cap_hi[AG_AMP_STAGES];
    float lo[AG_AMP_STAGES], hi[AG_AMP_STAGES];
    int   i, pass, fit_n;

    if (a == 0 || scratch == 0 || cfg == 0 || tab == 0) {
        return -1;
    }
    a->cfg = *cfg;
    if (a->cfg.os != 1 && a->cfg.os != 2 && a->cfg.os != 4 &&
        a->cfg.os != 8) {
        a->cfg.os = 1;
    }
    a->tab_n = n > 0 ? n : AG_AMP_TAB_N;
    fit_n = a->tab_n < AMP_FIT_N ? a->tab_n : AMP_FIT_N;
    a->n = a->cfg.n_stages < 1 ? 1 : a->cfg.n_stages;
    if (a->n > AG_AMP_STAGES) {
        a->n = AG_AMP_STAGES;
    }
    for (i = 0; i < AG_AMP_STAGES; i++) {
        ag_amp_spec(a->cfg.model, i, &a->spec[i]);
        a->linear[i] = a->cfg.linear[i];
        a->gain[i] = a->cfg.gain[i] > 0.0f ? a->cfg.gain[i] : 1.0f;
        a->vtrim[i] = trim_lin(a->cfg.vtrim[i]);
    }
    /* The two knobs every note and tool argument calls by name are the first two
     * gains, so they win over the array. */
    a->gain[0] = a->cfg.drive;
    if (a->n > 1) {
        a->gain[1] = a->cfg.g12;
    }
    if (a->cfg.ccouple2 > 0.0f && a->n > 1) {
        a->spec[1].ccouple = a->cfg.ccouple2;
    }
    /*
     * `couple_mul` is a **capacitor**, so it belongs here with the other component
     * overrides and not in the filter design.
     *
     * The first version of it multiplied the coupling high-pass corner and nothing
     * else, which is only half of what a smaller capacitor does: the same value
     * also sets the grid-charge time constant, C * rgrid, through
     * ag_tube_set_blocking - and blocking is where most of the subsonic
     * intermodulation in this model comes from (measured: switching it off takes
     * 9.1 dB off the 49 Hz product on the four-stage model).  A knob that moved the
     * audible corner but left the nonlinear time constant alone would have been a
     * filter pretending to be a component, and it would have made the one
     * measurement it exists for read wrong.
     */
    {
        int cm;
        for (cm = 0; cm < a->n && cm < AG_AMP_STAGES; cm++) {
            if (a->cfg.couple_mul[cm] > 0.0f &&
                a->cfg.couple_mul[cm] != 1.0f &&
                a->spec[cm].ccouple > 0.0f) {
                a->spec[cm].ccouple /= a->cfg.couple_mul[cm];
            }
        }
    }
    /* The first valve's two hot-rodded values, back to whatever was asked for.
     * They change the bake, not a filter, so they belong here and not in
     * ag_amp_set_knobs - a stock front end is a different curve. */
    if (a->cfg.rcath1 > 0.0f) {
        a->spec[0].rcath = a->cfg.rcath1;
    }
    if (a->cfg.rplate1 > 0.0f) {
        a->spec[0].rplate = a->cfg.rplate1;
    }
    ag_os8_init(&a->os);

    /*
     * Axes given outright: bake once and stop.  This is what a preset that
     * remembers its own ranges does, and what the resolution measurement needs.
     *
     * Every live stage has to have one, not the first two.  bake_all reads the
     * pair for each stage it bakes, so a chain of three whose third pair was
     * never set would take this path and bake that stage over whatever the
     * caller's stack held.
     */
    {
        int given = 1;
        for (i = 0; i < a->n; i++) {
            if (!(a->cfg.axis_hi[i] > a->cfg.axis_lo[i])) {
                given = 0;
            }
        }
        if (given) {
            if (bake_all(a, scratch, tab, a->tab_n, a->cfg.axis_lo,
                         a->cfg.axis_hi) != 0) {
                return -1;
            }
            design(a);
            ag_amp_reset(a);
            return 0;
        }
    }

    /* First bake fitted to the curves, coarse, which is what the chain has to be
     * run with before anything can be measured about the signal. */
    if (bake_all(a, scratch, tab, fit_n, 0, 0) != 0) {
        return -1;
    }
    design(a);
    ag_amp_reset(a);
    for (i = 0; i < a->n; i++) {
        /* Where the curve itself stops moving, as an outer bound on the fit:
         * beyond this the axis buys nothing whatever the signal does. */
        cap_lo[i] = a->tube[i].lo;
        cap_hi[i] = a->tube[i].hi;
        lo[i] = 0.0f;
        hi[i] = 0.0f;
    }
    if (probe == 0 || probe_n <= 0) {
        return 0;
    }

    /*
     * Start the fit from a narrow window rather than from that curve-fitted one.
     *
     * The curve-fitted axis runs to a hundred volts and more, and a coarse table
     * across it has cells most of a volt wide - which is wider than the whole
     * knee, so the first pass would be measuring the excursions of a model whose
     * gain is two straight lines.  Starting narrow is safe in a way that starting
     * wide is not, because the union only ever widens: an underestimate is
     * corrected by the next pass, while lost resolution is not corrected at all.
     *
     * The window goes into this bake and *not* into lo/hi, which stay at zero.
     * Seeding the union with it instead made the window a floor the axis could
     * never come back under: V1a ended up with an axis of -2.9..+6.0 V while
     * using six tenths of a volt of it.
     */
    {
        float seed_lo[AG_AMP_STAGES], seed_hi[AG_AMP_STAGES];
        for (i = 0; i < a->n; i++) {
            seed_lo[i] = cap_lo[i] > -6.0f ? cap_lo[i] : -6.0f;
            seed_hi[i] = cap_hi[i] < 6.0f ? cap_hi[i] : 6.0f;
        }
        if (bake_all(a, scratch, tab, fit_n, seed_lo, seed_hi) != 0) {
            return -1;
        }
    }
    design(a);
    ag_amp_reset(a);

    for (pass = 0; pass < AMP_FIT_PASSES; pass++) {
        int j;
        ag_amp_reset(a);
        for (j = 0; j < probe_n; j++) {
            (void)ag_amp_tick(a, probe[j]);
        }
        for (i = 0; i < a->n; i++) {
            /*
             * Union, never replace.  Any driving point the signal has been seen
             * to reach is reachable, so the range can only grow - and that is
             * what turns this into a fixed point rather than a chase.
             */
            const float m = (a->tube[i].seen_hi - a->tube[i].seen_lo) * AMP_MARGIN;
            float       l = a->tube[i].seen_lo - (m > 0.05f ? m : 0.05f);
            float       h = a->tube[i].seen_hi + (m > 0.05f ? m : 0.05f);
            if (l < lo[i]) {
                lo[i] = l;
            }
            if (h > hi[i]) {
                hi[i] = h;
            }
            if (lo[i] < cap_lo[i]) {
                lo[i] = cap_lo[i];
            }
            if (hi[i] > cap_hi[i]) {
                hi[i] = cap_hi[i];
            }
        }
        /* Coarse while still fitting, full size on the last pass. */
        if (bake_all(a, scratch, tab,
                     pass == AMP_FIT_PASSES - 1 ? a->tab_n : fit_n, lo, hi) != 0) {
            return -1;
        }
        /* The cathode shelf comes out of the bake's small-signal gains, so the
         * sections are redesigned with it rather than kept from the first pass. */
        design(a);
        ag_amp_reset(a);
    }
    return 0;
}

/* Every filter chain in the amplifier, by index, so that one loop can walk them
 * all rather than four places listing them. */
/*
 * Every biquad chain in the amplifier, for the state lifting in apply_cfg.
 *
 * **A new chain has to be added here and in chain_at, or its delay line is
 * zeroed on every keypress.**  That is not a hypothetical: an experimental
 * dispersion chain of eight high-q all-pass sections was added and not
 * registered, and it clicked audibly on every knob press while the note was
 * sounding - eight resonators holding milliseconds of it, dumped to zero.  The
 * failure is silent because chain_at returns the tone chain for any index it
 * does not recognise, so the count and the switch disagree without a warning.
 */
#define AMP_CHAINS (2 * AG_AMP_STAGES + 2)

static ag_biq_chain_t *chain_at(ag_amp_t *a, int k)
{
    if (k < AG_AMP_STAGES) {
        return &a->pre[k];
    }
    if (k == AG_AMP_STAGES) {
        return &a->f_out;
    }
    if (k < 2 * AG_AMP_STAGES + 1) {
        return &a->voice[k - AG_AMP_STAGES - 1];
    }
    return &a->tone;
}

/*
 * Everything a knob can move, applied to a chain that is already built.
 *
 * Split out of ag_amp_set_voicing because the two callers want opposite things
 * from the state.  A tool that re-voices between measurements wants the filters
 * cleared, so that what it measures is not the tail of the previous setting; a
 * listener turning a knob wants the state left exactly where it is, because
 * zeroing eight biquads under a ringing note is a click and a dropout.
 */
static int apply_cfg(ag_amp_t *a, const ag_amp_cfg_t *cfg, int keep)
{
    int i;

    if (a == 0 || cfg == 0 || a->n < 1) {
        return -1;
    }
    /* fs is baked into nothing here, but changing it changes every coefficient
     * and the caller almost certainly means to rebuild, so refuse it. */
    if (cfg->fs != a->cfg.fs) {
        return -1;
    }
    a->cfg = *cfg;
    if (a->cfg.os != 1 && a->cfg.os != 2 && a->cfg.os != 4 &&
        a->cfg.os != 8) {
        a->cfg.os = 1;
    }
    /*
     * The gains are cached out of the config, so a re-voice has to refresh them
     * or the drive knob is dead after the first build - which is exactly what
     * happened, silently, the first time a preset was played back.
     */
    for (i = 0; i < AG_AMP_STAGES; i++) {
        a->linear[i] = a->cfg.linear[i];
        a->gain[i] = a->cfg.gain[i] > 0.0f ? a->cfg.gain[i] : 1.0f;
        a->vtrim[i] = trim_lin(a->cfg.vtrim[i]);
    }
    a->gain[0] = a->cfg.drive;
    if (a->n > 1) {
        a->gain[1] = a->cfg.g12;
    }
    for (i = 0; i < a->n; i++) {
        /*
         * Both arming calls clear what they arm - the antialiasing forgets its
         * previous sample and the coupling capacitor is discharged - which is
         * right when the chain is about to be reset anyway and wrong under a
         * knob.  Discharging the capacitor in particular is not a click but a
         * jump in the bias: the stage comes back at a different operating point
         * for the next hundred milliseconds.  So on the knob path the three
         * state fields are lifted over the call and the coefficients are the
         * only thing that changes.
         */
        const float   vc = a->tube[i].vc;
        const float   vcp = a->tube[i].vc_peak;
        const uint8_t hp = a->tube[i].have_prev;
        const float   pp = a->tube[i].prev_p;

        (void)ag_tube_set_adaa(&a->tube[i], a->cfg.adaa);
        /* Re-armed rather than left alone: the capacitor's constants are per
         * sample, so changing the oversampling changes them. */
        (void)ag_tube_set_blocking(
            &a->tube[i], &a->spec[i],
            a->cfg.fs * (float)(a->cfg.os < 1 ? 1 : a->cfg.os), a->cfg.blocking);
        if (keep) {
            a->tube[i].vc = vc;
            a->tube[i].vc_peak = vcp;
            a->tube[i].have_prev = hp;
            a->tube[i].prev_p = pp;
        }
    }
    /*
     * design() rebuilds every section from scratch, which zeroes the delay line
     * along with the coefficients.  On the knob path the delay line is the note
     * that is currently sounding, so it is lifted over the rebuild and put back
     * section by section.
     *
     * By index, and the sections can move: a band whose gain crosses zero is not
     * pushed at all, so the chain in front of the last stage is three sections
     * long or four depending on the mid control.  When the count changes the
     * extra section starts empty, which is a step at one knob position out of
     * however many - against a click at every one, which is what not doing this
     * costs.
     */
    if (keep) {
        float z[AMP_CHAINS][AG_BIQ_MAX][4];
        int   was[AMP_CHAINS];
        int   c, j;

        for (c = 0; c < AMP_CHAINS; c++) {
            const ag_biq_chain_t *ch = chain_at(a, c);
            was[c] = ch->n;
            for (j = 0; j < ch->n; j++) {
                z[c][j][0] = ch->s[j].x1;
                z[c][j][1] = ch->s[j].x2;
                z[c][j][2] = ch->s[j].y1;
                z[c][j][3] = ch->s[j].y2;
            }
        }
        design(a);
        for (c = 0; c < AMP_CHAINS; c++) {
            ag_biq_chain_t *ch = chain_at(a, c);
            const int       lim = ch->n < was[c] ? ch->n : was[c];
            for (j = 0; j < lim; j++) {
                ch->s[j].x1 = z[c][j][0];
                ch->s[j].x2 = z[c][j][1];
                ch->s[j].y1 = z[c][j][2];
                ch->s[j].y2 = z[c][j][3];
            }
        }
    } else {
        design(a);
    }
    return 0;
}

int ag_amp_set_voicing(ag_amp_t *a, const ag_amp_cfg_t *cfg)
{
    const int rc = apply_cfg(a, cfg, 0);

    if (rc == 0) {
        ag_amp_reset(a);
    }
    return rc;
}

int ag_amp_set_knobs(ag_amp_t *a, const ag_amp_cfg_t *cfg)
{
    /*
     * The oversampling is the one setting that cannot be moved without clearing
     * the state, because the halfbands' history is at the old rate and the
     * blocking capacitor's constants are per sample.  Everything else - the
     * gains, the two voicing banks, the antialiasing and the blocking flag -
     * only changes coefficients, and coefficients may change under a running
     * filter.
     */
    if (a == 0 || cfg == 0 || cfg->os != a->cfg.os) {
        return -1;
    }
    return apply_cfg(a, cfg, 1);
}

void ag_amp_reset(ag_amp_t *a)
{
    int i;

    if (a == 0) {
        return;
    }
    for (i = 0; i < AG_AMP_STAGES; i++) {
        ag_tube_reset(&a->tube[i]);
    }
    for (i = 0; i < AG_AMP_STAGES; i++) {
        ag_biq_chain_reset(&a->pre[i]);
    }
    ag_biq_chain_reset(&a->f_out);
    for (i = 0; i < AG_AMP_STAGES; i++) {
        ag_biq_chain_reset(&a->voice[i]);
    }
    ag_biq_chain_reset(&a->tone);
    ag_tone_reset(&a->stack);
    ag_os8_reset(&a->os);
    a->peak_out = 0.0f;
    a->samples = 0;
}

/*
 * One pass through every stage at the oversampled rate.
 *
 * The first stage.s filter block and gain are applied outside this, at the base
 * rate, because they sit outside the oversampled region; from the second stage
 * on, both belong here.  A stage with no table is its gain and its filters and
 * nothing else, which is what a clean makeup stage is - the runtime never asks
 * what kind of device a table came from, only whether there is one.
 *
 * Each valve carries its own coupling capacitor.s charge, so blocking is inside
 * ag_tube_tick rather than here.
 */
static float run_stages(ag_amp_t *a, float v)
{
    int i;
    if (a->tube[0].t != 0) {
        v = ag_tube_tick(&a->tube[0], v);
    }
    for (i = 1; i < a->n; i++) {
        v = ag_biq_chain_tick(&a->pre[i], v) * a->gain[i];
        v = ag_biq_chain_tick(&a->voice[i], v) * a->vtrim[i];
        if (a->tube[i].t != 0) {
            v = ag_tube_tick(&a->tube[i], v);
        }
    }
    return v;
}

float ag_amp_tick(ag_amp_t *a, float x)
{
    float v, y, p;

    if (a == 0 || a->n < 1) {
        return 0.0f;
    }
    v = ag_biq_chain_tick(&a->voice[0],
                          ag_biq_chain_tick(&a->pre[0], x) * a->gain[0]) *
        a->vtrim[0];

    if (a->cfg.os == 8) {
        float u[8], w[8];
        int   i;
        ag_os8_up(&a->os, v, u);
        for (i = 0; i < 8; i++) {
            w[i] = run_stages(a, u[i]);
        }
        y = ag_os8_down(&a->os, w);
    } else if (a->cfg.os == 4) {
        float u[4], w[4];
        int   i;
        ag_os4_up(&a->os.four, v, u);
        for (i = 0; i < 4; i++) {
            w[i] = run_stages(a, u[i]);
        }
        y = ag_os4_down(&a->os.four, w);
    } else if (a->cfg.os == 2) {
        float u[2], w[2];
        ag_os2_up(&a->os.four.outer, v, u);
        w[0] = run_stages(a, u[0]);
        w[1] = run_stages(a, u[1]);
        y = ag_os2_down(&a->os.four.outer, w[0], w[1]);
    } else {
        y = run_stages(a, v);
    }

    /*
     * After the valves, in the order a 2203 has them: the output coupling
     * capacitor, then the passive tone stack, then the matching bank, then the
     * master.
     *
     * The first two are circuit and the third is a fit, and the order of the
     * three does not change the response at all - everything here is linear, so
     * they commute - but it does say which is which to anybody reading it.
     */
    y = ag_biq_chain_tick(&a->f_out, y);
    if (a->stack_on) {
        y = ag_tone_tick(&a->stack, y);
    }
    y = ag_biq_chain_tick(&a->tone, y) * a->cfg.master;

    p = y < 0.0f ? -y : y;
    if (p > a->peak_out) {
        a->peak_out = p;
    }
    a->samples++;
    return y;
}

uint32_t ag_amp_clamped(const ag_amp_t *a)
{
    uint32_t n = 0;
    int      i;
    if (a == 0) {
        return 0;
    }
    for (i = 0; i < AG_AMP_STAGES; i++) {
        n += a->tube[i].clamped;
    }
    return n;
}

/* ------------------------------------------------------------------------ */
/* Presets                                                                   */
/* ------------------------------------------------------------------------ */

/*
 * The header, and then the tables.  Everything before the tables is fixed size,
 * so the tables start at a known offset and can be read straight into place.
 */
typedef struct amp_preset_head {
    uint32_t magic, ver;
    uint32_t cfg_size, spec_size; /* refuse a struct that has moved on */
    uint32_t n_stages, tab_n;
    ag_amp_cfg_t   cfg;
    ag_tube_spec_t spec[AG_AMP_STAGES];
    /* Per stage, what the bake found and the tables alone do not say. */
    float axis_lo[AG_AMP_STAGES], axis_hi[AG_AMP_STAGES];
    float vq_plate[AG_AMP_STAGES], vq_cath[AG_AMP_STAGES];
    float gain_byp[AG_AMP_STAGES], gain_open[AG_AMP_STAGES];
    uint32_t has_table[AG_AMP_STAGES];
} amp_preset_head_t;

uint32_t ag_amp_preset_size(int n_stages, int tab_n)
{
    if (n_stages < 1 || n_stages > AG_AMP_STAGES || tab_n < 8) {
        return 0;
    }
    return (uint32_t)sizeof(amp_preset_head_t) +
           (uint32_t)(n_stages * 3 * tab_n) * (uint32_t)sizeof(float);
}

uint32_t ag_amp_preset_save(const ag_amp_t *a, void *buf, uint32_t cap)
{
    amp_preset_head_t *h = (amp_preset_head_t *)buf;
    float             *out;
    uint32_t           need;
    int                i;

    if (a == 0 || buf == 0 || a->n < 1) {
        return 0;
    }
    need = ag_amp_preset_size(a->n, a->tab_n);
    if (need == 0 || cap < need) {
        return 0;
    }
    h->magic = AG_AMP_PRESET_MAGIC;
    h->ver = AG_AMP_PRESET_VER;
    h->cfg_size = (uint32_t)sizeof(ag_amp_cfg_t);
    h->spec_size = (uint32_t)sizeof(ag_tube_spec_t);
    h->n_stages = (uint32_t)a->n;
    h->tab_n = (uint32_t)a->tab_n;
    h->cfg = a->cfg;
    for (i = 0; i < AG_AMP_STAGES; i++) {
        h->spec[i] = a->spec[i];
        h->axis_lo[i] = a->tube[i].lo;
        h->axis_hi[i] = a->tube[i].hi;
        h->vq_plate[i] = a->tube[i].vq_plate;
        h->vq_cath[i] = a->tube[i].vq_cath;
        h->gain_byp[i] = a->tube[i].gain_bypassed;
        h->gain_open[i] = a->tube[i].gain_unbypassed;
        h->has_table[i] = (i < a->n && a->tube[i].t != 0) ? 1u : 0u;
    }
    out = (float *)(void *)(h + 1);
    for (i = 0; i < a->n; i++) {
        const int   nn = a->tab_n;
        float      *d = out + (3 * i) * nn;
        int         k;
        if (!h->has_table[i]) {
            for (k = 0; k < 3 * nn; k++) {
                d[k] = 0.0f;
            }
            continue;
        }
        for (k = 0; k < nn; k++) {
            d[k] = a->tube[i].t[k];
            d[nn + k] = a->tube[i].h != 0 ? a->tube[i].h[k] : 0.0f;
            d[2 * nn + k] = a->tube[i].g != 0 ? a->tube[i].g[k] : 0.0f;
        }
    }
    return need;
}

int ag_amp_preset_load(ag_amp_t *a, const void *buf, uint32_t n, float *tab,
                       float fs)
{
    const amp_preset_head_t *h = (const amp_preset_head_t *)buf;
    const float             *in;
    int                      i;

    if (a == 0 || buf == 0 || tab == 0 || n < sizeof(amp_preset_head_t)) {
        return -1;
    }
    if (h->magic != AG_AMP_PRESET_MAGIC || h->ver != AG_AMP_PRESET_VER ||
        h->cfg_size != sizeof(ag_amp_cfg_t) ||
        h->spec_size != sizeof(ag_tube_spec_t)) {
        return -1;
    }
    if (h->n_stages < 1 || h->n_stages > AG_AMP_STAGES ||
        n < ag_amp_preset_size((int)h->n_stages, (int)h->tab_n)) {
        return -1;
    }

    a->cfg = h->cfg;
    a->cfg.fs = fs > 0.0f ? fs : h->cfg.fs;
    a->n = (int)h->n_stages;
    a->tab_n = (int)h->tab_n;
    for (i = 0; i < AG_AMP_STAGES; i++) {
        a->spec[i] = h->spec[i];
        a->linear[i] = a->cfg.linear[i];
        a->gain[i] = a->cfg.gain[i] > 0.0f ? a->cfg.gain[i] : 1.0f;
        /* Cached the same way the build caches it - a preset that carried the
         * trims in cfg and left the multipliers at whatever the allocation held
         * came back **silent**, which is what the round-trip test is for. */
        a->vtrim[i] = trim_lin(a->cfg.vtrim[i]);
    }
    a->gain[0] = a->cfg.drive;
    if (a->n > 1) {
        a->gain[1] = a->cfg.g12;
    }

    in = (const float *)(const void *)(h + 1);
    for (i = 0; i < a->n; i++) {
        const int nn = a->tab_n;
        float    *d = tab + (3 * i) * nn;
        int       k;
        for (k = 0; k < 3 * nn; k++) {
            d[k] = in[(3 * i) * nn + k];
        }
        ag_tube_init(&a->tube[i]);
        if (!h->has_table[i]) {
            a->linear[i] = 1;
            continue;
        }
        if (ag_tube_attach(&a->tube[i], d, d + nn, d + 2 * nn, nn, h->axis_lo[i],
                           h->axis_hi[i]) != 0) {
            return -1;
        }
        /* The bake knew these and the tables do not carry them: the operating
         * point for reporting, and the two small-signal gains whose ratio is the
         * cathode shelf that design() is about to build. */
        a->tube[i].vq_plate = h->vq_plate[i];
        a->tube[i].vq_cath = h->vq_cath[i];
        a->tube[i].gain_bypassed = h->gain_byp[i];
        a->tube[i].gain_unbypassed = h->gain_open[i];
        (void)ag_tube_set_adaa(&a->tube[i], a->cfg.adaa);
        (void)ag_tube_set_blocking(
            &a->tube[i], &a->spec[i],
            a->cfg.fs * (float)(a->cfg.os < 1 ? 1 : a->cfg.os), a->cfg.blocking);
    }
    ag_os8_init(&a->os);
    design(a);
    ag_amp_reset(a);
    return 0;
}

int ag_amp_blend_stage(ag_amp_t *a, ag_ckt_t *scratch, int stage, float mix,
                       float *work)
{
    float *live, *A, *B;
    int    n;

    if (a == 0 || work == 0 || stage < 0 || stage >= a->n || a->tab_n < 8) {
        return -1;
    }
    n = a->tab_n;
    live = (float *)(void *)a->tube[stage].t;
    if (live == 0) {
        return -1; /* a stage with no table has nothing to blend */
    }
    A = work;
    B = work + 3 * n;

    if (scratch != 0) {
        int k;
        /* The valve curve as it stands, kept so the knob can come back to it. */
        for (k = 0; k < n; k++) {
            A[k] = a->tube[stage].t[k];
            A[n + k] = a->tube[stage].h != 0 ? a->tube[stage].h[k] : 0.0f;
            A[2 * n + k] = a->tube[stage].g != 0 ? a->tube[stage].g[k] : 0.0f;
        }
        /*
         * And the clipper over the same axis.  10k in series is a Tube Screamer's
         * order of magnitude and 0.35 V is silicon; both belong in a preset once
         * there is somewhere to put them.
         */
        {
            ag_tube_t di;
            /* The slope of the curve that is actually in the table: an
             * unbypassed stage was swept with its cathode resistor in circuit,
             * so its gain is the open one, and matching the bypassed figure
             * would make the knob a volume control on that stage. */
            const float slope = a->spec[stage].ccath > 0.0f
                                    ? a->tube[stage].gain_bypassed
                                    : a->tube[stage].gain_unbypassed;
            ag_tube_init(&di);
            if (ag_tube_bake_clipper(&di, scratch, 10.0e3f, 0.35f, slope, B, B + n,
                                     B + 2 * n, n, a->tube[stage].lo,
                                     a->tube[stage].hi) != 0) {
                return -1;
            }
        }
        /*
         * And matched for loudness, not only for slope.
         *
         * The bake matches the small-signal gain, which makes the quiet parts
         * identical - and the loud parts are not, because the two ceilings are:
         * a diode pair stops at a fraction of a volt times the stage gain while
         * the valve's plate has a hundred to give, so at full mix the diode side
         * came back noticeably quieter, which reads as a level knob when it is
         * supposed to be a shape knob.  So the diode curve is scaled once, at
         * bake, to the same RMS over the axis as the valve curve.  The axis was
         * fitted to the signal, so its extent is where the music actually goes,
         * which is what makes an RMS over it a loudness and not an abstraction.
         * Scaling t and h by the same constant keeps h an exact antiderivative,
         * so the antialiasing stays exact; the grid current is not scaled,
         * because it is the valve's and the diode has none.
         */
        {
            double sa = 0.0, sb = 0.0;
            float  m;
            int    k;
            for (k = 0; k < n; k++) {
                sa += (double)A[k] * (double)A[k];
                sb += (double)B[k] * (double)B[k];
            }
            if (sb > 1e-20) {
                m = (float)ag_sqrtf((float)(sa / sb));
                for (k = 0; k < 2 * n; k++) {
                    B[k] *= m;
                }
            }
        }
    }
    {
        ag_tube_t va = a->tube[stage], di = a->tube[stage];
        va.t = A;
        va.h = A + n;
        va.g = A + 2 * n;
        di.t = B;
        di.h = B + n;
        di.g = B + 2 * n;
        ag_tube_blend(&a->tube[stage], &va, &di, mix, live, live + n,
                      live + 2 * n);
    }
    return 0;
}
