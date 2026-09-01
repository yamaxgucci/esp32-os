/*
 * ag_os - halfband oversampling for the nonlinear part of the chain.
 *
 * Why this exists at all, since a lookup table works with amplitudes and knows
 * nothing about frequency: because a sequence of samples is a frequency-domain
 * object whether the code treats it as one or not.  A nonlinearity *creates*
 * frequencies that were not in the input - two clipping valves put significant
 * energy out to the twentieth harmonic - and a frequency above Nyquist does not
 * go missing, it comes back somewhere else.  At 22.05 kHz the twelfth harmonic
 * of a 1 kHz note is 12 kHz and arrives at 10.05 kHz, fifty hertz away from the
 * real tenth harmonic, and the two beat.  Every sample of that is computed
 * correctly; it is the signal they represent that is wrong.
 *
 * Measured on four baked stages at a 3.7 kHz tone (docs/08): non-harmonic
 * products sit at -13.4 dB with no oversampling, -33.4 dB at 4x, and -56.4 dB
 * at 4x with the antialiasing in ag_tube.  Thirteen decibels is a fifth of the
 * signal being rubbish, and it gets worse with gain, since more gain means
 * higher harmonics - so the artefact arrives exactly where the amplifier is
 * supposed to sound best.
 *
 * Halfband, because a windowed sinc with its cutoff at a quarter of the
 * oversampled rate has an exact zero at every even tap - sinc(n/2) = 0 for even
 * n other than zero - so half the multiplies are structurally absent rather
 * than optimised away.
 *
 * The cutoff is a quarter of the oversampled rate, which is half of the target
 * Nyquist, and that is worth writing down because getting it wrong once cost a
 * day: a decimator whose corner sat at 0.22 of the oversampled rate rather than
 * 0.25 filtered at 42 kHz instead of 22, removed nothing, and measured 8 dB
 * *more* aliasing than no oversampling at all (rake 8 in docs/08).
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#ifndef AG_OS_H
#define AG_OS_H

/*
 * Half the filter length, and it must be even for the halfband zeros to land on
 * the even taps.  Twenty is a compromise with three sides, all of them
 * measured by test_tube.c rather than argued:
 *
 *   - Transition width.  A Hamming window gives about 3.3/N, so N = 41 puts the
 *     passband edge near 9.2 kHz and the stopband edge near 12.8 kHz at a
 *     22.05 kHz base rate, with about 53 dB of rejection.  That is enough:
 *     whatever folds lands between 8 and 11 kHz, where a guitar cabinet is
 *     already dead, and the target for the whole chain is around -50 dB.
 *
 *   - Latency.  M samples of group delay at the oversampled rate, so M/2 base
 *     samples per direction: 20 base samples round trip at 2x, 30 at 4x, which
 *     is 0.9 and 1.4 ms.  Next to the 11.6 ms one ag_ir convolution costs, that
 *     is affordable; a 122-tap Blackman halfband, which is what a 2 kHz
 *     transition would need, would not be.
 *
 *   - Passband droop.  Hamming rather than Blackman is 20 dB less stopband for
 *     a third less transition width, and here the trade is worth taking: a
 *     duller top octave is audible and 53 dB of alias rejection is not.
 */
#ifndef AG_OS_M
#define AG_OS_M 40
#endif

/*
 * And the same for the stages above it.
 *
 * Only the last decimation matters much, and that is a measurement rather than
 * an intuition.  Going from four times to eight made the products *worse* -
 * -51.7 dB against -55.1 under 5 kHz - because more oversampling generates more
 * genuine ultrasonic content and hands it to a final filter that cannot remove
 * it; doubling that filter instead bought 9.4 dB.  So length belongs where the
 * fold lands in the audio band, which is the outer stage, and the inner ones can
 * stay short because what they fold sits above 20 kHz where nothing listens.
 */
#ifndef AG_OS_M_INNER
#define AG_OS_M_INNER 20
#endif

/* History capacity, a power of two at least AG_OS_M so the ring index is a
 * mask.  Each buffer is written twice so the dot product reads a contiguous
 * window and never wraps inside the loop. */
#define AG_OS_CAP 64

typedef struct ag_os2 {
    float ho[AG_OS_M]; /* the odd taps, sum 0.5                            */
    float hc;          /* the centre tap, 0.5                              */
    int   m;           /* how many of ho are in use                        */

    float uh[2 * AG_OS_CAP];
    float de[2 * AG_OS_CAP];
    float dd[2 * AG_OS_CAP];
    int   upos, depos, ddpos;
    int   ready;
} ag_os2_t;

typedef struct ag_os4 {
    ag_os2_t outer; /* base <-> 2x */
    ag_os2_t inner; /* 2x   <-> 4x */
} ag_os4_t;

/*
 * And three of them, for the question of whether four times is enough.
 *
 * Each stage up costs twice what the one below it did, because it runs at twice
 * the rate - and so does everything inside the oversampled region, which is both
 * valves and the filters between them.  So this is not a small step, and whether
 * it buys anything is a measurement rather than a principle: tube_render "alias"
 * prints the products for every factor.
 */
typedef struct ag_os8 {
    ag_os4_t four; /* base <-> 4x, and a valid ag_os4_t on its own */
    ag_os2_t c;    /* 4x   <-> 8x                                  */
} ag_os8_t;

/* Designs the halfband of half-length `m`, which must be even and at most
 * AG_OS_M.  Cheap, but it calls sin, so not on the audio path. */
void ag_os2_init(ag_os2_t *s, int m);
void ag_os2_reset(ag_os2_t *s);

/* One input sample in, two out. */
void ag_os2_up(ag_os2_t *s, float x, float *out2);

/*
 * Two samples of the oversampled stream in, one out.  `a` is the one that lines
 * up with the output sample and `b` the one between it and the next; pass them
 * in the order ag_os2_up produced them, because the decimator's odd phase reads
 * `b` from the previous call and swapping them shifts the filter by one tap.
 */
float ag_os2_down(ag_os2_t *s, float a, float b);

void  ag_os4_init(ag_os4_t *s);
void  ag_os4_reset(ag_os4_t *s);
void  ag_os4_up(ag_os4_t *s, float x, float *out4);
float ag_os4_down(ag_os4_t *s, const float *in4);

void  ag_os8_init(ag_os8_t *s);
void  ag_os8_reset(ag_os8_t *s);
void  ag_os8_up(ag_os8_t *s, float x, float *out8);
float ag_os8_down(ag_os8_t *s, const float *in8);

/* Group delay in base-rate samples for one direction, so that a chain can
 * report what it costs in time as well as in instructions. */
float ag_os2_latency(void);
float ag_os4_latency(void);
float ag_os8_latency(void);

#endif /* AG_OS_H */
