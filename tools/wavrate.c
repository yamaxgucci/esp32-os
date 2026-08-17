/*
 * wavrate - rational sample rate conversion for the listening tests.
 *
 * The NAM captures are 48 kHz and everything else here is 44.1, so the two
 * have to meet somewhere.  A resampler that colours the signal would be worse
 * than useless in a test whose whole subject is a faint high-frequency buzz,
 * so this one is deliberately expensive: polyphase sinc, Kaiser beta 12,
 * sixty-four taps per phase.  At 44.1 <-> 48 that is 160 phases of 64, a ten
 * thousand tap prototype, and the stopband is below -110 dB.
 *
 *   wavrate in.wav out.wav <out_rate>
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TAPS_PER_PHASE 64

static double *wav_read(const char *path, uint32_t *n, uint32_t *rate)
{
    FILE    *f = fopen(path, "rb");
    uint8_t  h[12], c[8];
    uint32_t sz, i;
    uint16_t ch = 1, bits = 16;
    double  *x = NULL;

    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) != 0) {
        fclose(f);
        fprintf(stderr, "%s is not a RIFF file\n", path);
        return NULL;
    }
    while (fread(c, 1, 8, f) == 8) {
        sz = (uint32_t)c[4] | ((uint32_t)c[5] << 8) | ((uint32_t)c[6] << 16) |
             ((uint32_t)c[7] << 24);
        if (memcmp(c, "fmt ", 4) == 0) {
            uint8_t fmt[40];
            uint32_t want = sz < sizeof(fmt) ? sz : (uint32_t)sizeof(fmt);
            if (fread(fmt, 1, want, f) != want) break;
            ch = (uint16_t)(fmt[2] | (fmt[3] << 8));
            *rate = (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) |
                    ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            bits = (uint16_t)(fmt[14] | (fmt[15] << 8));
            if (sz > want) fseek(f, (long)(sz - want), SEEK_CUR);
        } else if (memcmp(c, "data", 4) == 0) {
            const uint32_t bytes = bits / 8u;
            uint32_t       frames = sz / (bytes * ch);
            uint8_t       *raw = (uint8_t *)malloc(sz);
            if (raw == NULL || fread(raw, 1, sz, f) != sz) {
                free(raw);
                break;
            }
            x = (double *)malloc(frames * sizeof(double));
            for (i = 0; i < frames; i++) {
                const uint8_t *p = raw + (size_t)i * bytes * ch;
                if (bits == 16) {
                    x[i] = (double)(int16_t)(p[0] | (p[1] << 8)) / 32768.0;
                } else if (bits == 24) {
                    int32_t v = (int32_t)((uint32_t)p[0] << 8 |
                                          (uint32_t)p[1] << 16 |
                                          (uint32_t)p[2] << 24);
                    x[i] = (double)v / 2147483648.0;
                } else if (bits == 32) {
                    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                    float fv;
                    memcpy(&fv, &u, 4);
                    x[i] = (double)fv;
                } else {
                    x[i] = 0.0;
                }
            }
            free(raw);
            *n = frames;
            fclose(f);
            return x;
        } else {
            fseek(f, (long)sz, SEEK_CUR);
        }
        if (sz & 1u) fseek(f, 1, SEEK_CUR);
    }
    fclose(f);
    fprintf(stderr, "%s has no data chunk\n", path);
    return NULL;
}

static void wav_write(const char *path, const double *x, uint32_t n,
                      uint32_t rate, uint32_t bits)
{
    FILE          *f = fopen(path, "wb");
    uint8_t        h[44];
    const uint32_t bytes = bits / 8u;
    const uint32_t br = rate * bytes;
    uint32_t       data = n * bytes, i;

    if (f == NULL) {
        fprintf(stderr, "cannot write %s\n", path);
        return;
    }
    memcpy(h, "RIFF", 4);
    h[4] = (uint8_t)((36u + data) & 0xFF);
    h[5] = (uint8_t)(((36u + data) >> 8) & 0xFF);
    h[6] = (uint8_t)(((36u + data) >> 16) & 0xFF);
    h[7] = (uint8_t)(((36u + data) >> 24) & 0xFF);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[17] = h[18] = h[19] = 0;
    h[20] = 1; h[21] = 0; h[22] = 1; h[23] = 0;
    h[24] = (uint8_t)(rate & 0xFF);
    h[25] = (uint8_t)((rate >> 8) & 0xFF);
    h[26] = (uint8_t)((rate >> 16) & 0xFF);
    h[27] = (uint8_t)((rate >> 24) & 0xFF);
    h[28] = (uint8_t)(br & 0xFF);
    h[29] = (uint8_t)((br >> 8) & 0xFF);
    h[30] = (uint8_t)((br >> 16) & 0xFF);
    h[31] = (uint8_t)((br >> 24) & 0xFF);
    h[32] = (uint8_t)bytes; h[33] = 0;
    h[34] = (uint8_t)bits; h[35] = 0;
    memcpy(h + 36, "data", 4);
    h[40] = (uint8_t)(data & 0xFF);
    h[41] = (uint8_t)((data >> 8) & 0xFF);
    h[42] = (uint8_t)((data >> 16) & 0xFF);
    h[43] = (uint8_t)((data >> 24) & 0xFF);
    fwrite(h, 1, 44, f);
    for (i = 0; i < n; i++) {
        const double full = bits == 24 ? 8388607.0 : 32767.0;
        double       v = x[i] * full;
        int32_t      s;
        if (v > full) v = full;
        if (v < -full - 1.0) v = -full - 1.0;
        s = (int32_t)(v < 0.0 ? v - 0.5 : v + 0.5);
        fputc(s & 0xFF, f);
        fputc((s >> 8) & 0xFF, f);
        if (bits == 24) fputc((s >> 16) & 0xFF, f);
    }
    fclose(f);
}

static double bessel_i0(double x)
{
    double s = 1.0, t = 1.0;
    int    i;
    for (i = 1; i < 80; i++) {
        t *= (x / (2.0 * i)) * (x / (2.0 * i));
        s += t;
        if (t < s * 1e-18) break;
    }
    return s;
}

static uint32_t gcd_u(uint32_t a, uint32_t b)
{
    while (b != 0) {
        const uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

int main(int argc, char **argv)
{
    uint32_t n = 0, rate = 0, out_rate, l, m, g, ntaps, i, out_n, bits;
    double  *x, *h, *y;
    double   fc, gain, sum = 0.0, peak = 0.0;
    int64_t  k;

    if (argc < 4) {
        fprintf(stderr, "usage: wavrate <in.wav> <out.wav> <out_rate> [bits]\n");
        return 2;
    }
    out_rate = (uint32_t)strtoul(argv[3], NULL, 10);
    bits = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 10) : 16u;
    if (bits != 16u && bits != 24u) bits = 16u;
    x = wav_read(argv[1], &n, &rate);
    if (x == NULL) return 1;
    if (rate == out_rate) {
        wav_write(argv[2], x, n, rate, bits);
        printf("wavrate: already %u Hz, copied %u frames\n", rate, n);
        return 0;
    }
    g = gcd_u(rate, out_rate);
    l = out_rate / g;
    m = rate / g;
    ntaps = l * TAPS_PER_PHASE;
    if ((ntaps & 1u) == 0) ntaps++;

    /* Cut off below both Nyquists, in the L-times-upsampled domain. */
    fc = 0.47 / (double)(l > m ? l : m);
    gain = (double)l;
    h = (double *)malloc(ntaps * sizeof(double));
    for (i = 0; i < ntaps; i++) {
        const double t = (double)i - (double)(ntaps - 1) / 2.0;
        const double s = (t == 0.0) ? 2.0 * fc
                                    : sin(2.0 * M_PI * fc * t) / (M_PI * t);
        const double r = (2.0 * i) / (double)(ntaps - 1) - 1.0;
        const double w = bessel_i0(12.0 * sqrt(1.0 - r * r)) / bessel_i0(12.0);
        h[i] = s * w;
        sum += h[i];
    }
    for (i = 0; i < ntaps; i++) {
        h[i] *= gain / sum;
    }

    out_n = (uint32_t)(((uint64_t)n * l) / m);
    y = (double *)calloc(out_n, sizeof(double));
    /*
     * Output sample j sits at input position j*m/l.  Only every l-th tap of
     * the prototype lines up with a real input sample, so the inner loop walks
     * one phase of it - sixty-four multiplies, not ten thousand.
     */
    for (i = 0; i < out_n; i++) {
        const uint64_t pos = (uint64_t)i * m + (ntaps - 1) / 2;
        const int64_t  base = (int64_t)(pos / l);
        const uint32_t phase = (uint32_t)(pos % l);
        double         acc = 0.0;
        for (k = 0; phase + (uint64_t)k * l < ntaps; k++) {
            const int64_t idx = base - k;
            if (idx < 0) break;
            if (idx < (int64_t)n) {
                acc += h[phase + (uint64_t)k * l] * x[idx];
            }
        }
        y[i] = acc;
        if (fabs(acc) > peak) peak = fabs(acc);
    }
    printf("wavrate: %u Hz -> %u Hz (%u/%u), %u taps, %u -> %u frames, "
           "peak %.3f\n", rate, out_rate, l, m, ntaps, n, out_n, peak);
    if (peak > 0.999) {
        printf("  peak over full scale, scaling by %.4f\n", 0.999 / peak);
        for (i = 0; i < out_n; i++) y[i] *= 0.999 / peak;
    }
    wav_write(argv[2], y, out_n, out_rate, bits);
    return 0;
}
