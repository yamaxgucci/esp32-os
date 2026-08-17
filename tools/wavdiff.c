/*
 * wavdiff - how far apart two renders are.
 *
 *   build-host/wavdiff <a.wav> <b.wav> [diff-out.wav]
 *
 * Exists because the useful question about a change to an audio model is
 * almost never "what is its noise floor" but "is this the same sound".  Two
 * renders of one take, one knob apart, answer that directly - and the answer
 * lands on the harmonics, where a noise measurement cannot see it.
 *
 * Both files are peak-normalised before the comparison, since every render in
 * this tree is written at -3 dBFS and a level difference would otherwise
 * swamp the thing being looked for.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

/* Left channel only, as double, peak normalised to 1. */
static double *load(const char *path, uint32_t *n_out, uint32_t *rate_out)
{
    FILE    *f = fopen(path, "rb");
    uint8_t *buf;
    long     size;
    uint32_t pos = 12, data_off = 0, data_len = 0, rate = 0;
    uint16_t ch = 0, bits = 0;
    double  *x, peak = 0.0;
    uint32_t i, n;

    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)size);
    if (buf == NULL || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    while (pos + 8u <= (uint32_t)size) {
        const uint32_t clen = rd_u32(buf + pos + 4);
        if (memcmp(buf + pos, "fmt ", 4) == 0) {
            ch = rd_u16(buf + pos + 10);
            rate = rd_u32(buf + pos + 12);
            bits = rd_u16(buf + pos + 22);
        } else if (memcmp(buf + pos, "data", 4) == 0) {
            data_off = pos + 8u;
            data_len = clen;
        }
        pos += 8u + clen + (clen & 1u);
    }
    if (bits != 16 || ch == 0 || data_len == 0) {
        fprintf(stderr, "%s: need 16-bit PCM\n", path);
        free(buf);
        return NULL;
    }
    n = data_len / (2u * ch);
    x = (double *)malloc(sizeof(double) * n);
    for (i = 0; i < n; i++) {
        const double v = (double)(int16_t)rd_u16(buf + data_off + i * 2u * ch);
        x[i] = v;
        if (v < 0.0 ? -v > peak : v > peak) {
            peak = v < 0.0 ? -v : v;
        }
    }
    if (peak > 0.0) {
        for (i = 0; i < n; i++) {
            x[i] /= peak;
        }
    }
    free(buf);
    *n_out = n;
    *rate_out = rate;
    return x;
}

/*
 * The average spectrum of one file, in third-octave-ish bands, plus two
 * numbers about its top end.
 *
 * Comparing two different amplifiers sample by sample says nothing - they are
 * not trying to be the same waveform.  What can be compared is where each one
 * puts its energy, and what the energy above 4 kHz looks like: fizz that is
 * steady and fizz that is a stream of little impulses measure the same in a
 * band level and sound nothing alike.  The crest factor tells them apart.
 */
#define SPEC_N 32768

/* Set from the command line when both files are one known tone. */
static double g_tone = 0.0;
/* And the second one, when the test signal is a pair. */
static double g_tone2 = 0.0;

/* Hann window, forward transform, power spectrum.  Shared so that two
 * measurements cannot silently disagree about how the block was taken. */
static void fft_pow(const double *x, uint32_t off, double *p)
{
    static double re[SPEC_N], im[SPEC_N];
    int           i, len, half, a, j;

    /*
     * Blackman-Harris, not Hann.
     *
     * Hann's first sidelobe is 31 dB down and its skirt is wide; measuring
     * "everything that is not a harmonic" of a low note through it counts the
     * window's own leakage as the model's noise, and reports about -25 dB no
     * matter what is being measured.  This one is 92 dB down, which is below
     * anything being looked for here.  The main lobe is wider in exchange, so
     * the harmonic windows below have to be wider too.
     */
    for (i = 0; i < SPEC_N; i++) {
        const double t = (double)i / SPEC_N;
        const double w = 0.35875 - 0.48829 * cos(2.0 * 3.14159265358979 * t) +
                         0.14128 * cos(4.0 * 3.14159265358979 * t) -
                         0.01168 * cos(6.0 * 3.14159265358979 * t);
        re[i] = x[off + (uint32_t)i] * w;
        im[i] = 0.0;
    }
    for (a = 1, j = 0; a < SPEC_N; a++) {
        int m = SPEC_N >> 1;
        for (; m >= 1 && j >= m; m >>= 1) {
            j -= m;
        }
        j += m;
        if (a < j) {
            double t;
            t = re[a]; re[a] = re[j]; re[j] = t;
            t = im[a]; im[a] = im[j]; im[j] = t;
        }
    }
    for (len = 2; len <= SPEC_N; len <<= 1) {
        half = len >> 1;
        for (a = 0; a < SPEC_N; a += len) {
            for (j = 0; j < half; j++) {
                const double ang = -2.0 * 3.14159265358979 * j / len;
                const double wr = cos(ang), wi = sin(ang);
                const int    i0 = a + j, i1 = i0 + half;
                const double tr = re[i1] * wr - im[i1] * wi;
                const double ti = re[i1] * wi + im[i1] * wr;
                re[i1] = re[i0] - tr;
                im[i1] = im[i0] - ti;
                re[i0] += tr;
                im[i0] += ti;
            }
        }
    }
    for (i = 0; i < SPEC_N / 2; i++) {
        p[i] = re[i] * re[i] + im[i] * im[i];
    }
}

/*
 * How much of the top end stands on the note, and how much stands between the
 * notes.
 *
 * This is the difference between fizz that belongs to the sound and fizz that
 * does not.  A valve making harmonics puts its energy at multiples of what is
 * being played; intermodulation, aliasing and arithmetic noise put it
 * everywhere else.  Both measure the same in a band level - which is why the
 * band levels of a model and a real amplifier can agree while one of them
 * buzzes.
 *
 * The fundamental is taken per block as the strongest bin below 500 Hz, which
 * is enough for a single note and honest about nothing else: a chord has no
 * one fundamental and this number would mean nothing for one.
 */
static void harmonic_fraction(const double *x, uint32_t n, uint32_t rate,
                              const char *name)
{
    static double p[SPEC_N / 2];
    double        harm = 0.0, between = 0.0;
    uint32_t      k, blocks = 0;

    for (k = 0; k + SPEC_N <= n; k += SPEC_N / 2) {
        double f0 = 0.0, best = 0.0, total = 0.0;
        int    i, f0bin = 0;

        fft_pow(x, k, p);
        for (i = 1; i < SPEC_N / 2; i++) {
            total += p[i];
        }
        if (total <= 0.0) {
            continue;
        }
        for (i = 1; i < SPEC_N / 2; i++) {
            const double hz = (double)i * rate / SPEC_N;
            if (hz < 60.0 || hz > 500.0) {
                continue;
            }
            if (p[i] > best) {
                best = p[i];
                f0bin = i;
            }
        }
        if (f0bin == 0) {
            continue;
        }
        f0 = (double)f0bin * rate / SPEC_N;
        blocks++;

        for (i = 1; i < SPEC_N / 2; i++) {
            const double hz = (double)i * rate / SPEC_N;
            double       d;
            if (hz < 2000.0) {
                continue;
            }
            d = hz / f0;
            d -= (double)(int)(d + 0.5);
            if (d < 0.0) {
                d = -d;
            }
            if (d < 0.2) {
                harm += p[i];
            } else {
                between += p[i];
            }
        }
    }
    if (blocks == 0u) {
        return;
    }
    printf("     above 2 kHz: %+.1f dB of it sits between the harmonics"
           "  (%u blocks)\n",
           10.0 * log10((between + 1e-30) / (harm + 1e-30)), blocks);
    (void)name;
}

/*
 * The harmonic series produced by a known tone.
 *
 * On music, two amplifiers can agree on every average and still not sound
 * alike.  On one steady note they cannot hide: the series is the distortion,
 * and how fast it falls away is most of what "harsh" and "warm" mean.
 */
static void harmonic_series(const double *x, uint32_t n, uint32_t rate,
                            double f0)
{
    static double p[SPEC_N / 2], acc[SPEC_N / 2];
    uint32_t      i, k, blocks = 0;
    int           h;
    double        first = 0.0;

    for (i = 0; i < SPEC_N / 2; i++) {
        acc[i] = 0.0;
    }
    /*
     * Only the blocks that are actually the note.
     *
     * A render carries silence after the tone, and the block where the tone
     * stops is a step: broadband, and louder than anything being measured.
     * Averaging it in smears the peak into a pedestal thirty decibels wide
     * and reports that as the model's noise.  Not hypothetical - it is what
     * this function did before the gate, and it was wrong by sixty decibels.
     */
    {
        double loudest = 0.0;
        for (k = 0; k + SPEC_N <= n; k += SPEC_N / 2) {
            double e = 0.0;
            for (i = 0; i < SPEC_N; i++) {
                e += x[k + i] * x[k + i];
            }
            if (e > loudest) {
                loudest = e;
            }
        }
        for (k = 0; k + SPEC_N <= n; k += SPEC_N / 2) {
            double e = 0.0;
            for (i = 0; i < SPEC_N; i++) {
                e += x[k + i] * x[k + i];
            }
            if (e < loudest * 0.98) {
                continue;
            }
            fft_pow(x, k, p);
            for (i = 0; i < SPEC_N / 2; i++) {
                acc[i] += p[i];
            }
            blocks++;
        }
    }
    if (blocks == 0u) {
        return;
    }
    printf("     %u steady blocks used\n", blocks);
    /*
     * Everything that is not within a few bins of a multiple of the tone.
     *
     * On a single sine an amplifier is only entitled to harmonics.  Whatever
     * else is there was invented by the model - intermodulation with its own
     * state, aliasing, arithmetic - and no amount of tone shaping will remove
     * it, because it is between the harmonics rather than above them.
     */
    {
        double onH = 0.0, offH = 0.0, worst = 0.0;
        int    worst_bin = 0;
        for (i = 1; i < SPEC_N / 2; i++) {
            const double hz = (double)i * rate / SPEC_N;
            double       d;
            if (hz < f0 * 0.5) {
                continue;
            }
            d = hz / f0;
            d -= (double)(int)(d + 0.5);
            if (d < 0.0) {
                d = -d;
            }
            /* Four bins either side of each harmonic, expressed as a
             * fraction of the spacing, so the window scales with the note. */
            if (d * f0 * SPEC_N / rate < 7.5) {
                onH += acc[i];
            } else {
                offH += acc[i];
                if (acc[i] > worst) {
                    worst = acc[i];
                    worst_bin = (int)i;
                }
            }
        }
        printf("     not on a harmonic: %+.1f dB of the harmonics; "
               "loudest stray %.0f Hz\n",
               10.0 * log10((offH + 1e-30) / (onH + 1e-30)),
               (double)worst_bin * rate / SPEC_N);

        /*
         * The neighbourhood of the fundamental, bin by bin.  A single number
         * cannot tell a pair of discrete sidebands from a continuous skirt,
         * and those mean completely different things: one is something
         * modulating, the other is noise or a window artefact.
         */
        {
            const int b0 = (int)(f0 * 0.4 * SPEC_N / rate);
            const int b1 = (int)(f0 * 2.6 * SPEC_N / rate);
            double    top = 0.0;
            int       q;
            for (q = b0; q < b1 && q < SPEC_N / 2; q++) {
                if (acc[q] > top) {
                    top = acc[q];
                }
            }
            printf("       around the note, dB from the peak, every 2 Hz:\n        ");
            for (q = b0; q < b1 && q < SPEC_N / 2; q++) {
                const double hz = (double)q * rate / SPEC_N;
                if ((int)(hz / 2.0) == (int)(((double)(q - 1) * rate / SPEC_N) / 2.0)) {
                    continue;
                }
                printf(" %.0f:%.0f", hz, 10.0 * log10((acc[q] + 1e-30) / top));
            }
            printf("\n");
        }

        /*
         * And where it lives.  Sidebands hugging the fundamental mean the
         * output is being modulated by something slow; a flat carpet across
         * the spectrum means noise; a rise towards the top means aliasing.
         * The three want completely different fixes.
         */
        {
            static const double be[] = { 0, 200, 600, 2000, 6000, 24000 };
            int   q;
            printf("       and where it is:");
            for (q = 0; q < 5; q++) {
                double e = 0.0;
                for (i = 1; i < SPEC_N / 2; i++) {
                    const double hz = (double)i * rate / SPEC_N;
                    double       d;
                    if (hz < f0 * 0.5 || hz < be[q] || hz >= be[q + 1]) {
                        continue;
                    }
                    d = hz / f0;
                    d -= (double)(int)(d + 0.5);
                    if (d < 0.0) {
                        d = -d;
                    }
                    if (d * f0 * SPEC_N / rate >= 7.5) {
                        e += acc[i];
                    }
                }
                printf(" %.0f-%.0fHz:%+.0f", be[q], be[q + 1],
                       10.0 * log10((e + 1e-30) / (onH + 1e-30)));
            }
            printf("\n");
        }
    }

    printf("     harmonics of %.0f Hz:", f0);
    for (h = 1; h <= 40; h++) {
        const double hz = f0 * h;
        const int    b = (int)(hz * SPEC_N / rate + 0.5);
        double       e = 0.0;
        int          q;
        if (b + 4 >= SPEC_N / 2) {
            break;
        }
        for (q = b - 4; q <= b + 4; q++) {
            e += acc[q];
        }
        if (h == 1) {
            first = e;
            continue;
        }
        if (h <= 12 || h % 4 == 0) {
            printf(" %d:%.0f", h, 10.0 * log10((e + 1e-30) / (first + 1e-30)));
        }
    }
    printf("\n");
}

/*
 * Two tones in, and everything that is neither of them nor a harmonic of
 * either is intermodulation.
 *
 * This is the test a single note cannot do.  A symmetric nonlinearity makes
 * odd products, which land near the notes and sound like the notes; an
 * asymmetric one makes even products - sums and differences of pairs - which
 * land where nothing was played.  Two amplifiers can produce the same
 * harmonic series on one tone and completely different amounts of this, and
 * it is this that music exposes and a sine does not.
 */
static void intermodulation(const double *x, uint32_t n, uint32_t rate,
                            double f1, double f2)
{
    static double p[SPEC_N / 2], acc[SPEC_N / 2];
    double        tones = 0.0, harm = 0.0, imd = 0.0, even = 0.0, odd = 0.0;
    uint32_t      i, k, blocks = 0;

    for (i = 0; i < SPEC_N / 2; i++) {
        acc[i] = 0.0;
    }
    for (k = n / 3u; k + SPEC_N <= n; k += SPEC_N / 2) {
        fft_pow(x, k, p);
        for (i = 0; i < SPEC_N / 2; i++) {
            acc[i] += p[i];
        }
        blocks++;
    }
    if (blocks == 0u) {
        return;
    }
    for (i = 1; i < SPEC_N / 2; i++) {
        const double hz = (double)i * rate / SPEC_N;
        double       d1, d2;
        int          isTone, isHarm;
        if (hz < 30.0) {
            continue;
        }
        d1 = hz / f1;
        d2 = hz / f2;
        isTone = (d1 > 0.97 && d1 < 1.03) || (d2 > 0.97 && d2 < 1.03);
        isHarm = 0;
        {
            const double r1 = d1 - (double)(int)(d1 + 0.5);
            const double r2 = d2 - (double)(int)(d2 + 0.5);
            if ((r1 < 0.02 && r1 > -0.02 && d1 > 1.5) ||
                (r2 < 0.02 && r2 > -0.02 && d2 > 1.5)) {
                isHarm = 1;
            }
        }
        if (isTone) {
            tones += acc[i];
        } else if (isHarm) {
            harm += acc[i];
        } else {
            imd += acc[i];
            /*
             * Products of even order sit at m*f1 + n*f2 with m+n even - the
             * sum and the difference are the loudest of them.  Near either is
             * enough to tell the two families apart.
             */
            if ((hz < f1 - 20.0) || (hz > f1 + f2 - 60.0 && hz < f1 + f2 + 60.0)) {
                even += acc[i];
            } else {
                odd += acc[i];
            }
        }
    }
    printf("     two tones: harmonics %+.1f dB, intermodulation %+.1f dB "
           "(of the tones)\n",
           10.0 * log10((harm + 1e-30) / (tones + 1e-30)),
           10.0 * log10((imd + 1e-30) / (tones + 1e-30)));
    printf("       of that intermodulation, %.0f%% is below the lower tone or "
           "at the sum\n",
           100.0 * even / (even + odd + 1e-30));
}

static void band_spectrum(const double *x, uint32_t n, uint32_t rate,
                          const char *name)
{
    static const double edge[] = { 60,   100,  160,   250,   400,  630,
                                   1000, 1600, 2500,  4000,  6300, 10000,
                                   16000, 22000 };
    const int nb = (int)(sizeof(edge) / sizeof(edge[0])) - 1;
    static double p[SPEC_N / 2], acc[SPEC_N / 2];
    double band[16], ref = 1.0, lo = 0.0, hi = 0.0;
    double hp_peak = 0.0, hp_e = 0.0;
    uint32_t i, k, blocks = 0;
    int b;

    for (i = 0; i < SPEC_N / 2; i++) {
        acc[i] = 0.0;
    }
    for (k = 0; k + SPEC_N <= n; k += SPEC_N / 2) {
        fft_pow(x, k, p);
        for (i = 0; i < SPEC_N / 2; i++) {
            acc[i] += p[i];
        }
        blocks++;
    }
    if (blocks == 0u) {
        return;
    }
    for (b = 0; b < nb; b++) {
        band[b] = 0.0;
        for (i = 1; i < SPEC_N / 2; i++) {
            const double hz = (double)i * rate / SPEC_N;
            if (hz >= edge[b] && hz < edge[b + 1]) {
                band[b] += acc[i];
            }
        }
        if (edge[b] <= 1000.0 && edge[b + 1] > 1000.0) {
            ref = band[b];
        }
    }
    for (i = 1; i < SPEC_N / 2; i++) {
        const double hz = (double)i * rate / SPEC_N;
        if (hz < 4000.0) {
            lo += acc[i];
        } else {
            hi += acc[i];
        }
    }

    /*
     * And the shape of the top end in time: a one-pole pair standing in for a
     * 4 kHz high pass, then peak against rms over the whole file.
     */
    {
        double s1 = 0.0, s2 = 0.0;
        const double c = 1.0 - exp(-2.0 * 3.14159265358979 * 4000.0 / rate);
        for (i = 0; i < n; i++) {
            double v;
            s1 += c * (x[i] - s1);
            s2 += c * (s1 - s2);
            v = x[i] - s2;
            if (i * 4u < n) {
                continue; /* let the filter settle */
            }
            hp_e += v * v;
            if (v < 0.0 ? -v > hp_peak : v > hp_peak) {
                hp_peak = v < 0.0 ? -v : v;
            }
        }
        hp_e = sqrt(hp_e / (double)(n - n / 4u));
    }

    printf("  %-28s", name);
    for (b = 0; b < nb; b++) {
        printf(" %+5.1f", 10.0 * log10((band[b] + 1e-30) / (ref + 1e-30)));
    }
    printf("\n     above 4 kHz: %+.1f dB of the rest;  its crest factor %.1f dB\n",
           10.0 * log10((hi + 1e-30) / (lo + 1e-30)),
           20.0 * log10((hp_peak + 1e-30) / (hp_e + 1e-30)));
    harmonic_fraction(x, n, rate, name);
    if (g_tone > 0.0 && g_tone2 <= 0.0) {
        harmonic_series(x, n, rate, g_tone);
    }
    if (g_tone > 0.0 && g_tone2 > 0.0) {
        intermodulation(x, n, rate, g_tone, g_tone2);
    }
}

int main(int argc, char **argv)
{
    uint32_t na = 0, nb = 0, ra = 0, rb = 0, i, n;
    double  *a, *b;
    double   ea = 0.0, ed = 0.0, worst = 0.0;
    uint32_t worst_at = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: wavdiff <a.wav> <b.wav> [tone Hz]\n");
        return 2;
    }
    if (argc > 3) {
        const char *comma = strchr(argv[3], ',');
        g_tone = atof(argv[3]);
        if (comma != NULL) {
            g_tone2 = atof(comma + 1);
        }
    }
    a = load(argv[1], &na, &ra);
    b = load(argv[2], &nb, &rb);
    if (a == NULL || b == NULL) {
        return 1;
    }
    n = na < nb ? na : nb;

    for (i = 0; i < n; i++) {
        const double d = a[i] - b[i];
        const double ad = d < 0.0 ? -d : d;
        ea += a[i] * a[i];
        ed += d * d;
        if (ad > worst) {
            worst = ad;
            worst_at = i;
        }
    }
    printf("%s vs %s\n", argv[1], argv[2]);
    printf("  %u frames at %u Hz compared\n", n, ra);

    /*
     * Spectra first, because they are the comparison that means something
     * between two different amplifiers.  The waveform difference below is
     * only meaningful for two renders of the same model.
     */
    printf("  average spectrum, dB relative to 1 kHz:\n");
    printf("  %-28s    60   100   160   250   400   630    1k  1.6k  2.5k"
           "    4k  6.3k   10k   16k\n", "");
    band_spectrum(a, na, ra, argv[1]);
    band_spectrum(b, nb, rb, argv[2]);

    /*
     * Two renders of the same model can be subtracted; two different
     * amplifiers cannot, and neither can two different sample rates.
     */
    if (ra != rb) {
        printf("  (different sample rates, %u and %u: spectra only)\n", ra, rb);
        free(a);
        free(b);
        return 0;
    }
    printf("  difference %.1f dB below the first, worst single sample %.1f dB "
           "at %.2f s\n",
           10.0 * log10((ed + 1e-30) / (ea + 1e-30)),
           20.0 * log10(worst + 1e-30), (double)worst_at / ra);

    /*
     * And the same in bands, because where two models differ says what kind
     * of difference it is: a level or tone difference sits low, a difference
     * in how the corner of the clipping is drawn sits high.
     */
    {
        static const double edge[4] = { 300.0, 1500.0, 5000.0, 24000.0 };
        int                 k;
        for (k = 0; k < 4; k++) {
            /* One-pole band split is enough to place the energy. */
            double lpa = 0.0, lpb = 0.0, hpa = 0.0, hpb = 0.0;
            double sa = 0.0, sd = 0.0;
            const double c = 1.0 - exp(-2.0 * 3.14159265358979 * edge[k] / ra);
            const double clo =
                k == 0 ? 0.0
                       : 1.0 - exp(-2.0 * 3.14159265358979 * edge[k - 1] / ra);
            for (i = 0; i < n; i++) {
                double fa, fb;
                lpa += c * (a[i] - lpa);
                lpb += c * (b[i] - lpb);
                hpa += clo * (a[i] - hpa);
                hpb += clo * (b[i] - hpb);
                fa = lpa - (k == 0 ? 0.0 : hpa);
                fb = lpb - (k == 0 ? 0.0 : hpb);
                sa += fa * fa;
                sd += (fa - fb) * (fa - fb);
            }
            printf("    %5.0f-%5.0f Hz: %6.1f dB\n",
                   k == 0 ? 0.0 : edge[k - 1], edge[k],
                   10.0 * log10((sd + 1e-30) / (sa + 1e-30)));
        }
    }
    free(a);
    free(b);
    return 0;
}
