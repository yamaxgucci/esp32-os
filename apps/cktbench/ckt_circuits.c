/*
 * Netlists for the benchmark and for the host tests.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ckt_circuits.h"

int ckt_build_preamp(ag_ckt_t *k, float fs, int stages, float bypass_uf)
{
    ag_triode_model_t m;
    int               s;

    if (k == 0 || stages < 1) {
        return 0;
    }
    ag_triode_model_12ax7(&m);
    ag_ckt_init(k, fs);

    if (ag_ckt_add_vin(k, CKT_N_IN, 0) != 0) {
        return 0;
    }
    if (ag_ckt_add_vdc(k, CKT_N_SUPPLY, 0, 250.0f) != 0) {
        return 0;
    }

    for (s = 0; s < stages; s++) {
        const int couple = CKT_COUPLE(s);
        const int grid = CKT_GRID(s);
        const int plate = CKT_PLATE(s);
        const int cath = CKT_CATHODE(s);
        const int src = (s == 0) ? CKT_N_IN : CKT_PLATE(s - 1);

        if (cath > AG_CKT_MAX_NODES) {
            return 0;
        }
        /* Coupling in, then the stopper/divider pair down to the grid. */
        if (ag_ckt_add_c(k, src, couple, 22.0e-9f) != 0) {
            return 0;
        }
        if (ag_ckt_add_r(k, couple, grid, 470.0e3f) != 0) {
            return 0;
        }
        if (ag_ckt_add_r(k, grid, 0, 100.0e3f) != 0) {
            return 0;
        }
        /* Plate load and cathode bias. */
        if (ag_ckt_add_r(k, CKT_N_SUPPLY, plate, 100.0e3f) != 0) {
            return 0;
        }
        if (ag_ckt_add_r(k, cath, 0, 1500.0f) != 0) {
            return 0;
        }
        if (bypass_uf > 0.0f &&
            ag_ckt_add_c(k, cath, 0, bypass_uf * 1.0e-6f) != 0) {
            return 0;
        }
        if (ag_ckt_add_triode(k, grid, plate, cath, &m) != 0) {
            return 0;
        }
    }

    if (ag_ckt_build(k) != 0) {
        return 0;
    }
    return CKT_PLATE(stages - 1);
}

/*
 * One stage on its own, with the next stage's input network hung off the plate
 * through a coupling capacitor so that the loading is still modelled.  Nodes:
 * 1 input, 2 supply, 3 coupling, 4 grid, 5 plate, 6 cathode, 7 load.
 */
static int build_one_stage(ag_ckt_t *k, float fs)
{
    ag_triode_model_t m;
    ag_triode_model_12ax7(&m);

    ag_ckt_init(k, fs);
    if (ag_ckt_add_vin(k, 1, 0) != 0 || ag_ckt_add_vdc(k, 2, 0, 250.0f) != 0 ||
        ag_ckt_add_c(k, 1, 3, 22.0e-9f) != 0 ||
        ag_ckt_add_r(k, 3, 4, 470.0e3f) != 0 ||
        ag_ckt_add_r(k, 4, 0, 100.0e3f) != 0 ||
        ag_ckt_add_r(k, 2, 5, 100.0e3f) != 0 ||
        ag_ckt_add_r(k, 6, 0, 1500.0f) != 0 ||
        ag_ckt_add_c(k, 6, 0, 22.0e-6f) != 0 ||
        ag_ckt_add_triode(k, 4, 5, 6, &m) != 0) {
        return 0;
    }
    /* The next stage's 470k + 100k, seen through its coupling cap.  DC is
     * blocked, so this loads the plate at audio and not at the bias point. */
    if (ag_ckt_add_c(k, 5, 7, 22.0e-9f) != 0 ||
        ag_ckt_add_r(k, 7, 0, 570.0e3f) != 0) {
        return 0;
    }
    if (ag_ckt_build(k) != 0) {
        return 0;
    }
    return 5; /* the plate */
}

int ckt_build_stage_alone(ag_ckt_t *k, float fs) { return build_one_stage(k, fs); }

/*
 * Nodes: 1 source, 2 supply, 3 behind rsrc, 4 grid leak, 5 grid, 6 plate,
 * 7 cathode, 8 load.  Three capacitors, one valve.
 */
int ckt_build_spec_stage(ag_ckt_t *k, float fs, const ckt_stage_spec_t *sp)
{
    ag_triode_model_t m;

    if (k == 0 || sp == 0) {
        return 0;
    }
    ag_triode_model_12ax7(&m);
    ag_ckt_init(k, fs);

    if (ag_ckt_add_vin(k, 1, 0) != 0 ||
        ag_ckt_add_vdc(k, 2, 0, sp->vsupply) != 0 ||
        ag_ckt_add_r(k, 1, 3, sp->rsrc) != 0 ||
        ag_ckt_add_c(k, 3, 4, sp->ccouple) != 0 ||
        ag_ckt_add_r(k, 4, 0, sp->rgrid) != 0 ||
        ag_ckt_add_r(k, 4, 5, sp->rstop) != 0 ||
        ag_ckt_add_r(k, 2, 6, sp->rplate) != 0 ||
        ag_ckt_add_r(k, 7, 0, sp->rcath) != 0) {
        return 0;
    }
    /*
     * An unbypassed cathode is a real design choice - it trades gain for a
     * looser, less compressed feel - so zero has to mean "no capacitor"
     * rather than "capacitor of zero farads", which the solver would reject.
     */
    if (sp->ccath > 0.0f && ag_ckt_add_c(k, 7, 0, sp->ccath) != 0) {
        return 0;
    }
    /*
     * Miller and stray capacitance at the grid.  Before the stopper, so what
     * charges it is the impedance of whatever drives the stage - which is
     * where a valve's top end is actually lost.
     */
    if (sp->cmiller > 0.0f && ag_ckt_add_c(k, 4, 0, sp->cmiller) != 0) {
        return 0;
    }
    if (ag_ckt_add_triode(k, 5, 6, 7, &m) != 0) {
        return 0;
    }
    /* The valve's own capacitances, across the valve rather than to ground. */
    if (sp->cgp > 0.0f && ag_ckt_add_c(k, 5, 6, sp->cgp) != 0) {
        return 0;
    }
    if (sp->cgk > 0.0f && ag_ckt_add_c(k, 5, 7, sp->cgk) != 0) {
        return 0;
    }
    if (sp->cpk > 0.0f && ag_ckt_add_c(k, 6, 7, sp->cpk) != 0) {
        return 0;
    }
    if (sp->cplate > 0.0f && ag_ckt_add_c(k, 6, 0, sp->cplate) != 0) {
        return 0;
    }
    if (ag_ckt_add_c(k, 6, 8, sp->cload) != 0) {
        return 0;
    }
    if (!sp->tonestack) {
        if (ag_ckt_add_r(k, 8, 0, sp->rload) != 0) {
            return 0;
        }
        if (ag_ckt_build(k) != 0) {
            return 0;
        }
        return 6; /* the plate */
    }

    /*
     * The tone network, inside the same matrix: 8 is its input, 9 the slope
     * node, 10 and 12 the capacitor outputs, 11 the wiper that the next stage
     * sees.  The 1 M at node 9 is the DC reference the whole stack hangs on -
     * without it nodes 8 and 9 reach ground through capacitors only, and a
     * node with no resistive path is a node the solver cannot pin down.
     */
    {
        const float t = sp->ts_treble > 0.02f ? sp->ts_treble : 0.02f;
        const float b = sp->ts_bass > 0.02f ? sp->ts_bass : 0.02f;
        if (ag_ckt_add_r(k, 8, 9, 33.0e3f) != 0 ||     /* slope           */
            ag_ckt_add_r(k, 9, 0, 1.0e6f) != 0 ||      /* DC reference    */
            ag_ckt_add_c(k, 8, 10, 250.0e-12f) != 0 || /* treble cap      */
            ag_ckt_add_r(k, 10, 11, 250.0e3f / t) != 0 ||  /* treble pot  */
            ag_ckt_add_c(k, 9, 12, 22.0e-9f) != 0 ||   /* bass cap        */
            ag_ckt_add_r(k, 12, 11, 250.0e3f / b) != 0 ||  /* bass pot    */
            ag_ckt_add_c(k, 12, 0, 6.8e-9f) != 0 ||    /* mid shaping     */
            ag_ckt_add_r(k, 11, 0, 100.0e3f) != 0) {   /* mid pot, output */
            return 0;
        }
    }
    if (ag_ckt_build(k) != 0) {
        return 0;
    }
    return 11; /* the wiper */
}

void ckt_jcm800_stage(ckt_stage_spec_t *out, int index)
{
    if (out == 0) {
        return;
    }
    if (index == 0) {
        /*
         * V1a, hot-rodded.  The 68k grid stopper and 1M leak are the Marshall
         * input; the input capacitor is not in the real amplifier - there the
         * guitar sees the grid leak directly - and 100 nF against 1 M puts its
         * corner at 1.6 Hz, below anything a guitar produces.
         *
         * The cathode is 820R rather than the stock 2k7, and the plate 220k
         * rather than 100k.  Stock, this valve stays clean and hands a big
         * clean signal to V1b, which does all the distorting; hot, it runs at
         * a warmer bias with more gain and starts clipping itself.  Two valves
         * clipping do not sound like one valve clipping twice as hard - the
         * first one's compression is already in the signal the second one
         * sees - and that is the difference between more gain here and another
         * stage on the end.
         */
        out->rsrc = 10.0e3f; /* a guitar pickup, roughly            */
        out->ccouple = 100.0e-9f;
        out->rgrid = 1.0e6f;
        out->rstop = 68.0e3f;
        out->vsupply = 330.0f;
        out->rplate = 220.0e3f;
        out->rcath = 820.0f;
        out->ccath = 0.68e-6f;
        out->cload = 22.0e-9f;
        out->rload = 470.0e3f;
        out->cmiller = 20.0e-12f;  /* wiring at the grid, not Miller */
        out->cgp = 1.7e-12f;
        /* Grid-cathode and plate-cathode are in the datasheet and measured
         * -65 dB of difference on a guitar take: at 96 kHz a 0.46 pF stamps
         * 9e-8 siemens, nine orders under the resistor it sits across.  They
         * are left here named and at zero, because the next person will ask
         * the same question and deserves the answer rather than the silence. */
        out->cgk = 0.0f;
        out->cpk = 0.0f;
        out->cplate = 15.0e-12f;
        out->ts_treble = 1.0f;
        out->ts_bass = 1.0f;
        out->tonestack = 0;
    } else if (index == 1) {
        /*
         * V1b, driven straight from V1a with no volume control between them -
         * that is what makes the 2203 a high-gain amplifier rather than a
         * Plexi.  The 820R cathode runs it hotter, so this is the valve that
         * clips first and hardest.
         */
        out->rsrc = 38.0e3f; /* 100k plate load against the valve's rp */
        /*
         * 2.2 nF, not the 22 nF a Plexi uses, and this is the one component
         * value in here that was chosen by listening rather than copied.
         *
         * Against the half megohm it sees, 22 nF corners at 14 Hz - the whole
         * of the low end arrives at the grid of the valve that does the
         * clipping, and intermodulates with everything above it into mush
         * that no tone control after the distortion can take back out.  2.2 nF
         * corners at 143 Hz, below the open low E's 82 Hz second harmonic and
         * above its fundamental.  Every high gain amplifier ever built does
         * some version of this; a 5150 uses 1 nF here, a Soldano less.
         */
        out->ccouple = 2.2e-9f;
        out->rgrid = 470.0e3f;
        out->rstop = 1.0e3f;
        out->vsupply = 330.0f;
        out->rplate = 100.0e3f;
        out->rcath = 820.0f;
        out->ccath = 0.68e-6f;
        out->cload = 22.0e-9f;
        out->rload = 1.0e6f;
        /*
         * This valve is driven hardest, so its Miller capacitance is the
         * largest in the amplifier - and it is the stage whose bandwidth
         * decides how the distortion sounds, since everything after it is
         * already clipped.  Against the 38k driving it, 150 pF is a corner
         * at 28 kHz: a parasitic, not a tone control.  Anything lower than
         * that is voicing, and belongs to whoever is voicing the amplifier.
         */
        out->cmiller = 20.0e-12f;  /* wiring at the grid, not Miller */
        out->cgp = 1.7e-12f;
        /* Grid-cathode and plate-cathode are in the datasheet and measured
         * -65 dB of difference on a guitar take: at 96 kHz a 0.46 pF stamps
         * 9e-8 siemens, nine orders under the resistor it sits across.  They
         * are left here named and at zero, because the next person will ask
         * the same question and deserves the answer rather than the silence. */
        out->cgk = 0.0f;
        out->cpk = 0.0f;
        out->cplate = 15.0e-12f;
        out->ts_treble = 1.0f;
        out->ts_bass = 1.0f;
        out->tonestack = 1; /* the stack hangs off this plate        */
    } else {
        /*
         * V2a, the gain recovery stage after the tone stack and the master
         * volume.  In a 2203 this is what turns a preamp that merely clips
         * into one that sustains: by the time the signal reaches it the stack
         * has thrown away most of the level, so this valve gets driven by
         * something already square and squares it again.
         */
        out->rsrc = 47.0e3f; /* the master volume, at about halfway   */
        out->ccouple = 22.0e-9f;
        out->rgrid = 470.0e3f;
        out->rstop = 1.0e3f;
        out->vsupply = 330.0f;
        out->rplate = 100.0e3f;
        out->rcath = 820.0f;
        out->ccath = 0.68e-6f;
        out->cload = 22.0e-9f;
        out->rload = 1.0e6f;
        out->cmiller = 20.0e-12f;  /* wiring at the grid, not Miller */
        out->cgp = 1.7e-12f;
        /* Grid-cathode and plate-cathode are in the datasheet and measured
         * -65 dB of difference on a guitar take: at 96 kHz a 0.46 pF stamps
         * 9e-8 siemens, nine orders under the resistor it sits across.  They
         * are left here named and at zero, because the next person will ask
         * the same question and deserves the answer rather than the silence. */
        out->cgk = 0.0f;
        out->cpk = 0.0f;
        out->cplate = 15.0e-12f;
        out->ts_treble = 1.0f;
        out->ts_bass = 1.0f;
        out->tonestack = 0;
    }
}

/*
 * The same stage with a tone stack hung on its plate, inside the same matrix.
 *
 * This is the interesting case for "do tone controls between stages cost
 * extra".  As a separate block a tone stack is its own linear solve; folded
 * into the valve stage it is not a solve at all, because the baked model does
 * not care how big the linear network around the device is - it cares only how
 * many capacitors there are, since those are the state.  Three more capacitors
 * and five more resistors turn into a few more multiply-adds per sample rather
 * than into a second matrix.
 *
 * Nodes 8..11 are the stack, driven from the plate through the coupling
 * capacitor that node 7 already provides.
 */
int ckt_build_stage_with_tonestack(ag_ckt_t *k, float fs)
{
    ag_triode_model_t m;
    ag_triode_model_12ax7(&m);

    ag_ckt_init(k, fs);
    if (ag_ckt_add_vin(k, 1, 0) != 0 || ag_ckt_add_vdc(k, 2, 0, 250.0f) != 0 ||
        ag_ckt_add_c(k, 1, 3, 22.0e-9f) != 0 ||
        ag_ckt_add_r(k, 3, 4, 470.0e3f) != 0 ||
        ag_ckt_add_r(k, 4, 0, 100.0e3f) != 0 ||
        ag_ckt_add_r(k, 2, 5, 100.0e3f) != 0 ||
        ag_ckt_add_r(k, 6, 0, 1500.0f) != 0 ||
        ag_ckt_add_c(k, 6, 0, 22.0e-6f) != 0 ||
        ag_ckt_add_triode(k, 4, 5, 6, &m) != 0) {
        return 0;
    }
    /* Coupling out of the plate into the stack. */
    if (ag_ckt_add_c(k, 5, 7, 22.0e-9f) != 0) {
        return 0;
    }
    /*
     * Passive tone network, controls at noon: a slope resistor feeding the
     * bass path, a small capacitor feeding the treble path, both landing on
     * the wiper.  Node 7 is the stack input, 8 the slope node, 9 the treble
     * cap's output, 11 the bass one's, 10 the wiper.
     *
     * The 1 M from node 8 to ground is not decoration.  Without it nodes 7 and
     * 8 reach ground only through capacitors - the plate's coupling capacitor
     * on one side, the bass capacitor on the other - and a node with no
     * resistive path anywhere is a node the solver has nothing to pin down.
     * It showed up as 18 590 unconverged samples, and it is the same class of
     * mistake as leaving out a grid leak: in a real amplifier the volume
     * control that follows the stack provides exactly this.
     */
    if (ag_ckt_add_r(k, 7, 8, 33.0e3f) != 0 ||     /* slope             */
        ag_ckt_add_r(k, 8, 0, 1.0e6f) != 0 ||      /* DC reference      */
        ag_ckt_add_c(k, 7, 9, 250.0e-12f) != 0 ||  /* treble cap        */
        ag_ckt_add_r(k, 9, 10, 250.0e3f) != 0 ||   /* treble pot        */
        ag_ckt_add_c(k, 8, 11, 22.0e-9f) != 0 ||   /* bass cap          */
        ag_ckt_add_r(k, 11, 10, 250.0e3f) != 0 ||  /* bass pot          */
        ag_ckt_add_c(k, 11, 0, 6.8e-9f) != 0 ||    /* mid shaping       */
        ag_ckt_add_r(k, 10, 0, 100.0e3f) != 0) {   /* mid pot / output  */
        return 0;
    }
    if (ag_ckt_build(k) != 0) {
        return 0;
    }
    return 10; /* the wiper, which is what the next stage sees */
}

int ckt_build_chain(ckt_chain_t *c, float fs, int stages)
{
    int s;

    if (c == 0 || stages < 1 || stages > CKT_MAX_CHAIN) {
        return 0;
    }
    c->n = stages;
    for (s = 0; s < stages; s++) {
        c->out[s] = build_one_stage(&c->stage[s], fs);
        if (!c->out[s]) {
            return 0;
        }
    }
    return 1;
}

/*
 * What crosses the boundary is the plate voltage as it stands, bias included -
 * about 170 V of it.  Nothing subtracts that here on purpose: the next stage's
 * coupling capacitor is what removes the DC in the real circuit and in this
 * model too, and it has to be the capacitor rather than a constant, because
 * the bias itself moves when the valve is driven into grid current.  That
 * movement is blocking distortion, and subtracting a fixed number would delete
 * it.
 */
float ckt_chain_tick(ckt_chain_t *c, float vin)
{
    int   s;
    float v = vin;

    for (s = 0; s < c->n; s++) {
        v = ag_ckt_tick(&c->stage[s], v, c->out[s]);
    }
    return v;
}

unsigned ckt_chain_iters(const ckt_chain_t *c)
{
    unsigned t = 0;
    int      s;
    for (s = 0; s < c->n; s++) {
        t += (unsigned)c->stage[s].iters_total;
    }
    return t;
}

unsigned ckt_chain_noncvg(const ckt_chain_t *c)
{
    unsigned t = 0;
    int      s;
    for (s = 0; s < c->n; s++) {
        t += (unsigned)c->stage[s].nonconverged;
    }
    return t;
}

void ckt_chain_reset_stats(ckt_chain_t *c)
{
    int s;
    for (s = 0; s < c->n; s++) {
        c->stage[s].iters_total = 0;
        c->stage[s].nonconverged = 0;
    }
}

/*
 * Baking a chain, stage by stage, in two passes each.
 *
 * The first pass bakes over a range wide enough to be certainly sufficient and
 * certainly too coarse, and runs the probe signal through it - not to get the
 * sound, but to find out where the driving point actually goes.  The second
 * pass bakes tightly around that, which is where the resolution comes from:
 * the same 128 points spread over three volts instead of two hundred.
 *
 * Each stage is fitted to the signal the stages before it actually produce,
 * because the first valve sees a guitar and the fourth sees a square wave, and
 * a table fitted to the wrong one of those is a table of the wrong circuit.
 */
/* Silence until the bias stops moving, then take that as the reset state. */
static void chain_find_dc(ckt_baked_chain_t *c, int settle)
{
    int i;
    ckt_baked_chain_reset(c);
    for (i = 0; i < settle; i++) {
        (void)ckt_baked_chain_tick(c, 0.0f);
    }
    for (i = 0; i < c->n; i++) {
        ag_stage_mark_dc(&c->stage[i]);
    }
    ckt_baked_chain_reset(c);
}

int ckt_bake_chain(ckt_baked_chain_t *c, ag_ckt_t *scratch, float fs,
                   int stages, const float *probe, int probe_n, int settle,
                   int variant, int adaa)
{
    float p1lo[CKT_MAX_CHAIN], p1hi[CKT_MAX_CHAIN];
    float p2lo[CKT_MAX_CHAIN], p2hi[CKT_MAX_CHAIN];
    int   s, i;
    int (*build)(ag_ckt_t *, float) =
        (variant == CKT_VARIANT_TONESTACK) ? ckt_build_stage_with_tonestack
                                           : ckt_build_stage_alone;

    if (c == 0 || scratch == 0 || probe == 0 || probe_n < 1 || stages < 1 ||
        stages > CKT_MAX_CHAIN) {
        return -1;
    }
    for (s = 0; s < CKT_MAX_CHAIN; s++) {
        ag_stage_init(&c->stage[s]);
    }
    c->n = stages;

    /*
     * Fitting the tables is a fixed-point problem, and pretending otherwise
     * does not work.
     *
     * The range a stage visits depends on the signal reaching it, which
     * depends on the tables in the stages before it, which depend on their
     * ranges.  A single wide-then-tight pass is enough only when the wide
     * table is fine enough to show the signal at all: over 300 V in 256 points
     * a stage whose driving point moves by a tenth of a volt sits inside one
     * cell, the model produces almost nothing, and the range comes back nearly
     * empty.  The tight tables built from it then make the real signal bigger
     * than anything the fit ever saw - measured, 55 211 lookups off the edge,
     * all of them on the plate axis: the table held 188.4..191.9 V and the run
     * wanted 187.0..193.1.
     *
     * So: iterate, and only ever widen.  Any driving point seen in any pass is
     * reachable, so the accumulated range is a union, never a replacement -
     * which makes the loop monotone and stops it from chasing its own tail.
     */
    for (s = 0; s < stages; s++) {
        const int out_node = build(scratch, fs);
        if (!out_node ||
            ag_stage_bake(&c->stage[s], scratch, out_node, c->tab[s], 0,
                          CKT_BAKE_N1, CKT_BAKE_N2, -80.0f, 40.0f, 0.0f,
                          320.0f) != 0) {
            return -1;
        }
        p1lo[s] = 1.0e30f;
        p1hi[s] = -1.0e30f;
        p2lo[s] = 1.0e30f;
        p2hi[s] = -1.0e30f;
    }

    for (i = 0; i < CKT_BAKE_FIT_PASSES; i++) {
        int j;

        chain_find_dc(c, settle * 8);

        /*
         * Measure from the first sample of signal, not from a settled one.  A
         * valve stage driven into grid current shifts its own bias over tens
         * of milliseconds - that shift is blocking distortion, it is half of
         * what a cranked amplifier sounds like, and the driving points it
         * passes through on the way are exactly the ones a player creates
         * every time they hit the strings.  Fitting only to the settled state
         * leaves the attack of every note off the edge of the table.
         */
        for (j = 0; j < settle + probe_n * 8; j++) {
            (void)ckt_baked_chain_tick(c, probe[j % probe_n]);
            for (s = 0; s < stages; s++) {
                const float a = c->stage[s].last_p1;
                const float b = c->stage[s].last_p2;
                if (a < p1lo[s]) {
                    p1lo[s] = a;
                }
                if (a > p1hi[s]) {
                    p1hi[s] = a;
                }
                if (b < p2lo[s]) {
                    p2lo[s] = b;
                }
                if (b > p2hi[s]) {
                    p2hi[s] = b;
                }
            }
        }

        for (s = 0; s < stages; s++) {
            const int   out_node = build(scratch, fs);
            const float m1 = CKT_BAKE_MARGIN * (p1hi[s] - p1lo[s]) + 1.0f;
            const float m2 = CKT_BAKE_MARGIN * (p2hi[s] - p2lo[s]) + 1.0f;
            const int   last = (i == CKT_BAKE_FIT_PASSES - 1);
            if (!out_node ||
                ag_stage_bake(&c->stage[s], scratch, out_node, c->tab[s],
                              (adaa && last) ? c->tabH[s] : 0, CKT_BAKE_N1,
                              CKT_BAKE_N2, p1lo[s] - m1, p1hi[s] + m1,
                              p2lo[s] - m2, p2hi[s] + m2) != 0) {
                return -1;
            }
            if (adaa && last && !ag_stage_set_adaa(&c->stage[s], 1)) {
                return -1;
            }
        }
    }

    chain_find_dc(c, settle * 8);
    return 0;
}

float ckt_baked_chain_tick(ckt_baked_chain_t *c, float vin)
{
    int   s;
    float v = vin;
    for (s = 0; s < c->n; s++) {
        v = ag_stage_tick(&c->stage[s], v);
    }
    return v;
}

unsigned ckt_baked_chain_clamped(const ckt_baked_chain_t *c)
{
    unsigned t = 0;
    int      s;
    for (s = 0; s < c->n; s++) {
        t += c->stage[s].clamped;
    }
    return t;
}

void ckt_baked_chain_reset(ckt_baked_chain_t *c)
{
    int s;
    for (s = 0; s < c->n; s++) {
        ag_stage_reset(&c->stage[s]);
    }
}

int ckt_build_diode_clipper(ag_ckt_t *k, float fs)
{
    ag_ckt_init(k, fs);
    if (ag_ckt_add_vin(k, 1, 0) != 0) {
        return 0;
    }
    if (ag_ckt_add_r(k, 1, 2, 4700.0f) != 0) {
        return 0;
    }
    /* 1N914: Is 2.52 nA, n 1.752, Vt 25.85 mV. */
    if (ag_ckt_add_diode_pair(k, 2, 0, 2.52e-9f, 0.0453f) != 0) {
        return 0;
    }
    if (ag_ckt_build(k) != 0) {
        return 0;
    }
    return 2;
}

int ckt_build_jfet_stage(ag_ckt_t *k, float fs)
{
    ag_jfet_model_t m;
    ag_jfet_model_j201(&m);

    ag_ckt_init(k, fs);
    if (ag_ckt_add_vin(k, 1, 0) != 0 || ag_ckt_add_vdc(k, 5, 0, 9.0f) != 0 ||
        ag_ckt_add_c(k, 1, 2, 100.0e-9f) != 0 ||
        ag_ckt_add_r(k, 2, 0, 1.0e6f) != 0 ||
        ag_ckt_add_r(k, 5, 3, 47.0e3f) != 0 ||
        ag_ckt_add_r(k, 4, 0, 2.2e3f) != 0 ||
        ag_ckt_add_c(k, 4, 0, 22.0e-6f) != 0 ||
        ag_ckt_add_jfet(k, 2, 3, 4, &m) != 0) {
        return 0;
    }
    if (ag_ckt_build(k) != 0) {
        return 0;
    }
    return 3;
}

int ckt_build_rc(ag_ckt_t *k, float fs)
{
    ag_ckt_init(k, fs);
    if (ag_ckt_add_vin(k, 1, 0) != 0 || ag_ckt_add_r(k, 1, 2, 1000.0f) != 0 ||
        ag_ckt_add_c(k, 2, 0, 159.15e-9f) != 0) {
        return 0;
    }
    if (ag_ckt_build(k) != 0) {
        return 0;
    }
    return 2;
}

/*
 * The classic passive stack, controls at noon:
 *
 *   in -1- 250n -2- treble pot -3- bass/mid to ground
 *
 * Node 2 is the treble cap junction, 3 the wiper, 4 the bass node, 5 the mid
 * node.  Values are the Marshall set (250 pF treble, 22 nF bass, 22 nF mid).
 */
int ckt_build_tonestack(ag_ckt_t *k, float fs)
{
    ag_ckt_init(k, fs);
    if (ag_ckt_add_vin(k, 1, 0) != 0) {
        return 0;
    }
    if (ag_ckt_add_r(k, 1, 2, 33.0e3f) != 0) { /* slope resistor */
        return 0;
    }
    if (ag_ckt_add_c(k, 1, 3, 250.0e-12f) != 0) { /* treble cap */
        return 0;
    }
    if (ag_ckt_add_r(k, 3, 4, 125.0e3f) != 0) { /* treble pot, half */
        return 0;
    }
    if (ag_ckt_add_r(k, 4, 3, 125.0e3f) != 0) {
        return 0;
    }
    if (ag_ckt_add_c(k, 2, 4, 22.0e-9f) != 0) { /* bass cap */
        return 0;
    }
    if (ag_ckt_add_r(k, 4, 5, 500.0e3f) != 0) { /* bass pot */
        return 0;
    }
    if (ag_ckt_add_c(k, 2, 5, 22.0e-9f) != 0) { /* mid cap */
        return 0;
    }
    if (ag_ckt_add_r(k, 5, 0, 12.5e3f) != 0) { /* mid pot */
        return 0;
    }
    if (ag_ckt_add_r(k, 4, 0, 1.0e6f) != 0) { /* load */
        return 0;
    }
    if (ag_ckt_build(k) != 0) {
        return 0;
    }
    return 4;
}
