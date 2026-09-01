/*
 * ag_biq - the linear half of a valve stage, as first and second order sections.
 *
 * The stages in ag_tube.h are static curves with no frequency in them at all.
 * Everything in a real preamp that does have a frequency - the coupling
 * capacitors, the cathode bypass, the grid stopper against the Miller
 * capacitance, the tone stack - is linear, and linear is what this file is for.
 * A chain of static curves with these between them is a Wiener-Hammerstein
 * model, which is not a memoryless model: the drive into each valve depends on
 * frequency, and the signal-dependent DC that an asymmetrically clipping stage
 * pushes onto its coupling capacitor is here too, with the right time constant.
 *
 * Why sections and not convolution, since the design started out asking for
 * impulse responses in all three places.  Two measured reasons and one that
 * follows from them:
 *
 *   - Latency.  ag_ir is uniform partitioned convolution with AG_IR_BLOCK 256,
 *     which at 22.05 kHz is 11.6 ms per convolution.  Three of them in series
 *     is 35 ms, and 35 ms is a slapback echo rather than an amplifier.  A
 *     section has none.
 *
 *   - Resolution.  A short FIR cannot do low frequencies: 64 taps at 22.05 kHz
 *     resolve about 345 Hz, and one period of 150 Hz is 147 samples.  Every
 *     feature this amplifier needs except the top cut is below 800 Hz.
 *
 *   - So the honest realisation of "a 150 Hz high pass and a mid lift" is two
 *     sections, at 25 instructions a sample against 2453 for a convolution and
 *     with no delay.  An arbitrary drawn curve is still possible with a
 *     minimum-phase FIR of 512 taps; that costs about 10% of a core each, which
 *     is a real price and belongs to whoever wants the curve.
 *
 * Minimum phase throughout, and that is not a preference either: a symmetric
 * linear-phase filter rings before the event, and pre-ringing in front of a
 * clipper adds an overshoot the circuit does not have.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#ifndef AG_BIQ_H
#define AG_BIQ_H

/*
 * Eight.  A stage's own linear network needs three - coupling high pass, cathode
 * shelf, one voicing section - and the voicing bank that shapes what reaches the
 * valves needs one per octave from 200 Hz to 5 kHz.
 */
#define AG_BIQ_MAX 8

/*
 * Direct form I.  Transposed direct form II is quieter by a bit or two, and
 * this is not the place for it: a 7 Hz high pass at 22 kHz puts its pole at
 * 0.998, where what matters is that the coefficients are exact rather than that
 * the state is small, and DF1 keeps the coefficient error where it can be seen.
 */
typedef struct ag_biq {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
} ag_biq_t;

typedef struct ag_biq_chain {
    ag_biq_t s[AG_BIQ_MAX];
    int      n;
} ag_biq_chain_t;

/*
 * Sine and cosine, which ag_mathf does not have because the circuit solver
 * never needed them.  Exposed rather than kept private because the
 * oversampler's window in ag_os.c wants them too, and two copies of a
 * polynomial is two places for a coefficient to be wrong.
 *
 * Coefficient-time only: accuracy matters (a 150 Hz corner at 22 kHz needs
 * 1 - cos(w0), which is 9e-4), speed does not.
 */
void ag_sincosf(float x, float *sn, float *cs);

/* Unity gain, no state. */
void ag_biq_bypass(ag_biq_t *s);
void ag_biq_reset(ag_biq_t *s);

/*
 * Designers.  Each returns the corner frequency it actually used, which is the
 * requested one unless it had to be pulled below 0.45*fs - a 12 kHz cut asked
 * for at a 22.05 kHz sample rate is above Nyquist, and answering that silently
 * with either a bypass or a wrong filter is worse than answering it with a
 * number the caller can print.
 */

/* First order high pass: a coupling capacitor, 6 dB an octave. */
float ag_biq_hp1(ag_biq_t *s, float fs, float f0);

/*
 * First order shelf: gain 1 above f_zero, 10^(db/20) below it, with the
 * transition ending at f_zero/gain.  That is exactly the shape of a cathode
 * resistor with a bypass capacitor across it - zero at 1/(2*pi*Rk*Ck), pole
 * where the loop gain puts it - so `db` comes from ag_tube_shelf rather than
 * from a dial.  Negative db cuts the low end, which is the direction a cathode
 * network goes.
 */
float ag_biq_shelf1(ag_biq_t *s, float fs, float f_zero, float db);

/*
 * First order high shelf: gain 1 below f0, 10^(db/20) above it, zero and pole
 * placed symmetrically about f0 in the log domain.
 *
 * The point of it is what it does *not* have.  A peaking section with +15 dB at
 * Q 1 puts its poles at radius 0.81, which rings for 1.4 ms at its centre
 * frequency - and behind a clipper that is a rattle at a fixed pitch, because
 * every clipping edge pings it twice a cycle while the note moves and the ring
 * does not.  Measured on a listener's recording of exactly that complaint: a
 * burst of 3.9 kHz every half period, the same 3.9 kHz on two different notes.
 * This has one real pole, so an edge stays an edge.
 *
 * Which is also what a real presence control is: a shelf, not a resonance.
 */
float ag_biq_hshelf1(ag_biq_t *s, float fs, float f0, float db);

/* Second order low pass, Butterworth at q = 0.70710678. */
float ag_biq_lp2(ag_biq_t *s, float fs, float f0, float q);

/* Peaking EQ, which is what the voicing controls are made of. */
float ag_biq_peak(ag_biq_t *s, float fs, float f0, float db, float q);

/*
 * Constant-skirt band pass with unity gain at the centre, `bw` in octaves.
 *
 * Not part of the amplifier: this is the analyser third-octave filter that
 * tools/tube_render.c uses to compare a render against a reference band by band.
 * It lives here because it is the same bilinear transform as everything else and
 * a second copy of that would be a second place to get it wrong.
 */
float ag_biq_bandpass(ag_biq_t *s, float fs, float f0, float bw);

float ag_biq_tick(ag_biq_t *s, float x);

void      ag_biq_chain_init(ag_biq_chain_t *c);
void      ag_biq_chain_reset(ag_biq_chain_t *c);
/* A section to design into, or NULL if the chain is full. */
ag_biq_t *ag_biq_chain_push(ag_biq_chain_t *c);
float     ag_biq_chain_tick(ag_biq_chain_t *c, float x);

/*
 * |H(f)| in dB, evaluated from the coefficients rather than measured by running
 * a sweep.  The tool prints this so that "cuts above 12 kHz" is a curve on
 * paper before it is a claim about the sound.
 */
float ag_biq_mag_db(const ag_biq_t *s, float f, float fs);
float ag_biq_chain_mag_db(const ag_biq_chain_t *c, float f, float fs);

#endif /* AG_BIQ_H */
