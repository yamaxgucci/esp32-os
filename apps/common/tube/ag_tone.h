/*
 * ag_tone - the passive tone stack, as a circuit with its knobs still attached.
 *
 * This is a **knob block** in the sense of README.md's first section: a control
 * and the network around it, live at run time, in the position the schematic puts
 * it.  It is not a tone-matching filter and must not be used as one - the fit has
 * its own blocks, in front of the stages and in the impulse after them, and the
 * whole point of keeping the two apart is that a component value is arguable and
 * a fitted band is not.
 *
 *          in ──┬──[Rslope]──┬──[Cbass]── bass top ──┐
 *               │            └──[Cmid]───────────────┼── mid top ──[Rmid]── gnd
 *               └──[Ctreb]── treb top                │
 *                              │                     │
 *                        [(1-t)Rtreb]                │
 *                              ├─── OUT              │
 *                          [t*Rtreb]                 │
 *                              └─────────────────────┘
 *
 * Three capacitors, so the network is third order, and every one of its numbers
 * is a component: the pots are the only things that move, as fractions from 0 to
 * 1, and **0.5 is what "at noon" means** - the position every model is fitted at.
 *
 * WHAT IT COSTS, AND WHY IT IS NOT THE SOLVER
 *
 * ag_ckt could run this netlist directly - it has a linear fast path, two
 * triangular solves and no iteration - and that is how it is verified
 * (test_tube.c builds the same network both ways and compares sample for
 * sample).  It is not how it is run: it would put the whole circuit solver back
 * into an application that plays presets, which ag_amp.h's preset format exists
 * to avoid, and it is dearer.
 *
 * What this costs is measured rather than counted, because counting it was
 * wrong by a factor of five.  The estimate was "sixteen multiply-adds, call it
 * 45 instructions"; the guest under -icount said **249** for the loop written as
 * four by four with two-dimensional indexing - fifteen instructions per
 * multiply-add, nearly all of it address arithmetic and loop counters.  Written
 * out flat it comes to what the arithmetic actually is.  The measured figures
 * are in ag_amp.c beside cfg->os, in the same table as everything else.
 *
 * The specialisation is exact rather than approximate.  The network is linear, so
 * discretising it the way ag_ckt does - companion conductance C*fs/theta with a
 * current source carrying the previous state - leaves a constant matrix.  Solving
 * that matrix once per knob position, for the four things that drive it (the
 * input and the three capacitor sources), collapses the whole per-sample job to
 * one 4x4 multiply.  Same integration rule, same theta, same answer.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#ifndef AG_TONE_H
#define AG_TONE_H

/*
 * The component values.  Everything here is a part on a board; nothing is a
 * setting.
 *
 * `rsrc` is the impedance of whatever drives the stack - a plate load against
 * the valve's own rp - and `rload` is what the stack drives, usually the next
 * grid leak or a master volume.  Both belong to the stack because a passive
 * network's response depends on them: the same three capacitors into 1 M and
 * into 100 k are two different tone controls.
 */
typedef struct ag_tone_spec {
    float rsrc;   /* what drives it                                          */
    float rslope; /* the slope resistor, the one that sets the mid dip        */
    float ctreb;  /* treble capacitor                                        */
    float rtreb;  /* treble pot, end to end                                  */
    float cbass;
    float rbass;
    float cmid;
    float rmid;
    float rload; /* what it drives                                           */
} ag_tone_spec_t;

typedef struct ag_tone {
    /* [vc_treb, vc_bass, vc_mid, vout] = k * [in, ieq_treb, ieq_bass, ieq_mid] */
    float k[4][4];
    float geq[3];
    float vpre[3], ipre[3];
    /* Kept so that a tool can print what the knobs are without a second copy of
     * them living somewhere else. */
    float treble, mid, bass;
} ag_tone_t;

/*
 * The Marshall values, which is what a 2203 has after V1b.
 *
 * PROVENANCE, because the rule about it is in README.md: 33 k slope, 500 pF
 * treble, 22 nF bass and mid, 250 k / 1 M / 25 k pots is the "Marshall" entry
 * every tone stack calculator carries and what the amplifier forums quote for
 * the 2203.  It has **not** been read off a Marshall schematic in this tree, and
 * until it is, that is what this comment is for.  The topology is not in the same
 * position: a three-capacitor TMB is textbook, and it is checked here against an
 * independent build of the same network in the circuit solver.
 */
void ag_tone_marshall(ag_tone_spec_t *out);

/*
 * Design for a sample rate and a set of knob positions, each 0 to 1 with 0.5 at
 * noon.  Cheap enough to be a knob: one six by six solve, four times, in double,
 * which is microseconds.  Returns 0 on success.
 *
 * Does not touch the state, so a knob may be turned under a sounding note - the
 * capacitor voltages are physical and carry across, exactly as they do in a real
 * amplifier when a pot is moved.
 */
int ag_tone_design(ag_tone_t *t, const ag_tone_spec_t *sp, float fs, float treble,
                   float mid, float bass);

void  ag_tone_reset(ag_tone_t *t);
float ag_tone_tick(ag_tone_t *t, float x);

#endif /* AG_TONE_H */
