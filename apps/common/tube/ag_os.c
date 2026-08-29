/*
 * ag_os - see ag_os.h.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include "ag_os.h"

#include "ag_biq.h" /* ag_sincosf */

#define OS_MASK (AG_OS_CAP - 1)

static void push(float *buf, int *pos, float x)
{
    const int p = *pos;
    buf[p] = x;
    buf[p + AG_OS_CAP] = x;
    *pos = (p + 1) & OS_MASK;
}

/* The window of the last AG_OS_CAP samples, oldest first, so that x[k-p] is
 * win[AG_OS_CAP - 1 - p]. */
static const float *win(const float *buf, int pos) { return buf + pos; }

void ag_os2_init(ag_os2_t *s, int m)
{
    int n;
    float     sum;
    int       p;

    if (s == 0) {
        return;
    }
    if (m < 2 || m > AG_OS_M) {
        m = AG_OS_M;
    }
    m &= ~1; /* even, or the halfband zeros do not land on the even taps */
    s->m = m;
    n = 2 * m + 1;

    /*
     * The taps, from the sinc that a quarter-rate cutoff gives.
     *
     * The odd ones have a closed form and need no trigonometry at all:
     * sinc(d/2) with d odd is 2*(-1)^((d-1)/2)/(pi*d).  Only the Hamming window
     * needs a cosine, and that is what ag_sincosf is for.
     */
    sum = 0.0f;
    for (p = 0; p < m; p++) {
        const int   d = 2 * p + 1 - m; /* offset from the centre, odd */
        const int   k = d > 0 ? d : -d;
        const int   q = ((k - 1) / 2) & 1;
        /* |d| in the denominator, not d: sinc is even, and signing it twice
         * mirrors the filter and quietly turns it into a high pass. */
        const float sinc = (q ? -2.0f : 2.0f) / (3.14159265f * (float)k);
        float       cs, sn;
        ag_sincosf(6.28318531f * (float)(2 * p + 1) / (float)(n - 1), &sn, &cs);
        s->ho[p] = sinc * (0.54f - 0.46f * cs);
        sum += s->ho[p];
    }
    /*
     * Normalised so the whole filter has unity DC gain, which is what the
     * decimator wants; the interpolator multiplies by two, because stuffing
     * zeros between samples halves the average and the filter has to put it
     * back.  Doing it this way round means the centre tap comes out at exactly
     * 0.5 and the even phase of the interpolator is exactly a delay.
     */
    s->hc = 0.5f;
    if (sum > 1.0e-9f || sum < -1.0e-9f) {
        const float g = 0.5f / sum;
        for (p = 0; p < m; p++) {
            s->ho[p] *= g;
        }
    }
    s->ready = 1;
    ag_os2_reset(s);
}

void ag_os2_reset(ag_os2_t *s)
{
    int i;
    if (s == 0) {
        return;
    }
    for (i = 0; i < 2 * AG_OS_CAP; i++) {
        s->uh[i] = 0.0f;
        s->de[i] = 0.0f;
        s->dd[i] = 0.0f;
    }
    s->upos = 0;
    s->depos = 0;
    s->ddpos = 0;
}

void ag_os2_up(ag_os2_t *s, float x, float *out2)
{
    const float *w;
    float        acc = 0.0f;
    int          p;

    if (s == 0 || out2 == 0) {
        return;
    }
    push(s->uh, &s->upos, x);
    w = win(s->uh, s->upos);

    /* The even phase is the single non-zero even tap, so it is a pure delay of
     * M/2 input samples.  The odd phase is the M odd taps against the same
     * history: y[2k+1] = sum_p ho[p] * x[k-p]. */
    for (p = 0; p < s->m; p++) {
        acc += s->ho[p] * w[AG_OS_CAP - 1 - p];
    }
    out2[0] = 2.0f * s->hc * w[AG_OS_CAP - 1 - (s->m / 2)];
    out2[1] = 2.0f * acc;
}

float ag_os2_down(ag_os2_t *s, float a, float b)
{
    const float *we;
    const float *wo;
    float        acc = 0.0f;
    int          p;

    if (s == 0) {
        return 0.0f;
    }
    /*
     * w[k] = hc*v[2(k - M/2)] + sum_p ho[p]*v[2(k-p) - 1].
     *
     * So the even-indexed samples and the odd-indexed ones are two separate
     * histories, and the odd one is read a sample behind: at output k the term
     * wanted is v[2k-1], which arrived as `b` on the previous call.  Hence the
     * order here - push the even sample, read, then push the odd one for next
     * time.  Getting that backwards shifts the filter by one tap at the
     * oversampled rate, which is not a crash and not obvious, only slightly
     * wrong forever.
     */
    push(s->de, &s->depos, a);
    we = win(s->de, s->depos);
    wo = win(s->dd, s->ddpos);
    for (p = 0; p < s->m; p++) {
        acc += s->ho[p] * wo[AG_OS_CAP - 1 - p];
    }
    acc += s->hc * we[AG_OS_CAP - 1 - (s->m / 2)];
    push(s->dd, &s->ddpos, b);
    return acc;
}

void ag_os4_init(ag_os4_t *s)
{
    if (s == 0) {
        return;
    }
    ag_os2_init(&s->outer, AG_OS_M);
    ag_os2_init(&s->inner, AG_OS_M_INNER);
}

void ag_os4_reset(ag_os4_t *s)
{
    if (s == 0) {
        return;
    }
    ag_os2_reset(&s->outer);
    ag_os2_reset(&s->inner);
}

void ag_os4_up(ag_os4_t *s, float x, float *out4)
{
    float mid[2];
    if (s == 0 || out4 == 0) {
        return;
    }
    ag_os2_up(&s->outer, x, mid);
    ag_os2_up(&s->inner, mid[0], out4);
    ag_os2_up(&s->inner, mid[1], out4 + 2);
}

float ag_os4_down(ag_os4_t *s, const float *in4)
{
    float d0, d1;
    if (s == 0 || in4 == 0) {
        return 0.0f;
    }
    d0 = ag_os2_down(&s->inner, in4[0], in4[1]);
    d1 = ag_os2_down(&s->inner, in4[2], in4[3]);
    return ag_os2_down(&s->outer, d0, d1);
}

/* M samples at the oversampled rate is M/2 at the base rate. */
float ag_os2_latency(void) { return (float)AG_OS_M * 0.5f; }

/* The outer stage costs M/2 base samples; the inner one costs M/2 samples of
 * the 2x stream, which is M/4 base samples. */
float ag_os4_latency(void)
{
    return (float)AG_OS_M * 0.5f + (float)AG_OS_M_INNER * 0.25f;
}

void ag_os8_init(ag_os8_t *s)
{
    if (s == 0) {
        return;
    }
    ag_os4_init(&s->four);
    ag_os2_init(&s->c, AG_OS_M_INNER);
}

void ag_os8_reset(ag_os8_t *s)
{
    if (s == 0) {
        return;
    }
    ag_os4_reset(&s->four);
    ag_os2_reset(&s->c);
}

void ag_os8_up(ag_os8_t *s, float x, float *out8)
{
    float quad[4];
    if (s == 0 || out8 == 0) {
        return;
    }
    ag_os4_up(&s->four, x, quad);
    ag_os2_up(&s->c, quad[0], out8);
    ag_os2_up(&s->c, quad[1], out8 + 2);
    ag_os2_up(&s->c, quad[2], out8 + 4);
    ag_os2_up(&s->c, quad[3], out8 + 6);
}

float ag_os8_down(ag_os8_t *s, const float *in8)
{
    float q[4];
    if (s == 0 || in8 == 0) {
        return 0.0f;
    }
    q[0] = ag_os2_down(&s->c, in8[0], in8[1]);
    q[1] = ag_os2_down(&s->c, in8[2], in8[3]);
    q[2] = ag_os2_down(&s->c, in8[4], in8[5]);
    q[3] = ag_os2_down(&s->c, in8[6], in8[7]);
    return ag_os4_down(&s->four, q);
}

/* M/2 base samples for the outer stage, M/4 for the next and M/8 for the last. */
float ag_os8_latency(void)
{
    return ag_os4_latency() + (float)AG_OS_M_INNER * 0.125f;
}
