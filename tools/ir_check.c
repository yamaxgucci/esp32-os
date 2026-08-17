/*
 * ir_check - how close the fixed-point convolution engine is to the
 * convolution it claims to be.
 *
 *   build-host/ir_check <input.wav> [outdir]
 *
 * Runs a recording through ag_ir with a cabinet impulse, and through the same
 * impulse convolved in double precision, and reports the difference.  That
 * difference is the whole of what the engine adds: there is nothing else in
 * the path.  Reported per window as well as overall, because a noise floor
 * that does not follow the signal is inaudible under a loud chord and
 * obvious under a decaying one - an average over the file hides exactly the
 * defect worth finding.
 *
 * The same measurement is repeated with the input attenuated, since a fixed
 * shift on the input spectrum shows up as a floor that stays where it is
 * while the music goes down past it.
 *
 * Writes the engine output, the reference, and the difference amplified by
 * 36 dB, so the error can be listened to as well as counted.
 *
 * Built by host-tests/CMakeLists.txt.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ag_dsp.h"
#include "ag_fft.h"
#include "ag_ir.h"

/* ------------------------------------------------------------------------ */
/* WAV                                                                       */
/* ------------------------------------------------------------------------ */

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static int16_t *wav_read(const char *path, uint32_t *frames_out,
                         uint32_t *rate_out)
{
    FILE    *f = fopen(path, "rb");
    uint8_t *buf;
    long     size;
    uint32_t pos = 12;
    uint16_t channels = 0, bits = 0, format = 0;
    uint32_t rate = 0, data_off = 0, data_len = 0;
    int16_t *out;
    uint32_t i, frames, stride;

    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 44) {
        fclose(f);
        return NULL;
    }
    buf = (uint8_t *)malloc((size_t)size);
    if (buf == NULL || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "%s: not a RIFF/WAVE file\n", path);
        free(buf);
        return NULL;
    }
    while (pos + 8u <= (uint32_t)size) {
        const uint32_t clen = rd_u32(buf + pos + 4);
        if (memcmp(buf + pos, "fmt ", 4) == 0 && clen >= 16u) {
            format = rd_u16(buf + pos + 8);
            channels = rd_u16(buf + pos + 10);
            rate = rd_u32(buf + pos + 12);
            bits = rd_u16(buf + pos + 22);
        } else if (memcmp(buf + pos, "data", 4) == 0) {
            data_off = pos + 8u;
            data_len = clen;
            if (data_off + data_len > (uint32_t)size) {
                data_len = (uint32_t)size - data_off;
            }
        }
        pos += 8u + clen + (clen & 1u);
    }
    if (data_len == 0u || channels == 0u || (bits != 16u && bits != 24u &&
                                             bits != 32u)) {
        fprintf(stderr, "%s: unsupported (fmt %u, %u bit, %u ch)\n", path,
                format, bits, channels);
        free(buf);
        return NULL;
    }
    stride = (uint32_t)channels * (bits / 8u);
    frames = data_len / stride;
    out = (int16_t *)malloc(sizeof(int16_t) * frames);
    if (out == NULL) {
        free(buf);
        return NULL;
    }
    for (i = 0; i < frames; i++) {
        const uint8_t *s = buf + data_off + (size_t)i * stride;
        double         acc = 0.0;
        uint32_t       c;
        for (c = 0; c < channels; c++) {
            const uint8_t *q = s + (size_t)c * (bits / 8u);
            if (bits == 16u) {
                acc += (double)(int16_t)rd_u16(q);
            } else if (bits == 24u) {
                int32_t v = (int32_t)((uint32_t)q[0] << 8 |
                                      (uint32_t)q[1] << 16 |
                                      (uint32_t)q[2] << 24);
                acc += (double)(v >> 8) / 256.0;
            } else if (format == 3u) {
                float fv;
                memcpy(&fv, q, 4);
                acc += (double)fv * 32767.0;
            } else {
                acc += (double)(int32_t)rd_u32(q) / 65536.0;
            }
        }
        acc /= (double)channels;
        if (acc > 32767.0) {
            acc = 32767.0;
        }
        if (acc < -32768.0) {
            acc = -32768.0;
        }
        out[i] = (int16_t)(acc >= 0.0 ? acc + 0.5 : acc - 0.5);
    }
    free(buf);
    *frames_out = frames;
    *rate_out = rate;
    return out;
}

static void wav_write(const char *path, const int16_t *mono, uint32_t frames,
                      uint32_t rate)
{
    FILE    *f = fopen(path, "wb");
    uint8_t  h[44];
    uint32_t data = frames * 2u;
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
    h[16] = 16; h[17] = 0; h[18] = 0; h[19] = 0;
    h[20] = 1;  h[21] = 0;
    h[22] = 1;  h[23] = 0;
    h[24] = (uint8_t)(rate & 0xFF);
    h[25] = (uint8_t)((rate >> 8) & 0xFF);
    h[26] = (uint8_t)((rate >> 16) & 0xFF);
    h[27] = (uint8_t)((rate >> 24) & 0xFF);
    h[28] = (uint8_t)((rate * 2u) & 0xFF);
    h[29] = (uint8_t)(((rate * 2u) >> 8) & 0xFF);
    h[30] = (uint8_t)(((rate * 2u) >> 16) & 0xFF);
    h[31] = (uint8_t)(((rate * 2u) >> 24) & 0xFF);
    h[32] = 2;  h[33] = 0;
    h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4);
    h[40] = (uint8_t)(data & 0xFF);
    h[41] = (uint8_t)((data >> 8) & 0xFF);
    h[42] = (uint8_t)((data >> 16) & 0xFF);
    h[43] = (uint8_t)((data >> 24) & 0xFF);
    fwrite(h, 1, 44, f);
    fwrite(mono, 2, frames, f);
    fclose(f);
}

/* ------------------------------------------------------------------------ */
/* The impulse response, built here so the reference has the same taps        */
/* ------------------------------------------------------------------------ */

static uint32_t s_rng;

static int16_t rnd16(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return (int16_t)((int32_t)(s_rng >> 16) - 32768);
}

/*
 * The same 20 ms cabinet ag_ir_load_preset(4) builds, generated here so the
 * double-precision reference can use the identical taps.
 */
static uint32_t make_cab(int16_t *h, uint32_t rate, uint32_t max_n)
{
    uint32_t n = (rate * 20u) / 1000u;
    uint32_t i;
    int32_t  lp = 0;

    s_rng = 0xA5F1523Du + 4u * 0x9E3779B9u;
    if (n < 32u) {
        n = 32u;
    }
    if (n > max_n) {
        n = max_n;
    }
    h[0] = 24000;
    for (i = 1; i < n; i++) {
        int32_t noise = (int32_t)rnd16() >> 4;
        int32_t env = (int32_t)((n - i) * 20000u / n);
        int32_t x = (noise * env) >> 15;
        lp += ((x - lp) * 48) >> 7;
        h[i] = ag_sat16(lp);
    }
    return n;
}

/*
 * ag_ir_load rescales the impulse response before it uses it, and rounds the
 * result back to int16.  The reference has to convolve with the taps the
 * engine ended up with, not the ones handed to it: a first attempt compared
 * against the taps before rescaling and reported 40 dB of "engine error" that
 * was entirely a different impulse response.  So this is the engine's own
 * arithmetic, copied, and it has to stay copied exactly.
 */
static uint32_t isqrt64(uint64_t v)
{
    uint64_t rem = v;
    uint64_t root = 0;
    uint64_t bit = 1ull << 62;
    while (bit > rem) {
        bit >>= 2;
    }
    while (bit != 0ull) {
        if (rem >= root + bit) {
            rem -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)root;
}

static void normalize_peak(int16_t *x, uint32_t n, int32_t target_peak)
{
    int32_t  peak = 1;
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t a = x[i] < 0 ? -(int32_t)x[i] : (int32_t)x[i];
        if (a > peak) {
            peak = a;
        }
    }
    if (peak <= target_peak) {
        return;
    }
    for (i = 0; i < n; i++) {
        x[i] = ag_sat16(((int32_t)x[i] * target_peak) / peak);
    }
}

static void normalize_energy(int16_t *x, uint32_t n, int32_t target)
{
    uint64_t e2 = 0;
    uint32_t i, rms;
    for (i = 0; i < n; i++) {
        const int32_t v = x[i];
        e2 += (uint64_t)(v * v);
    }
    rms = isqrt64(e2);
    if (rms < 1u) {
        return;
    }
    for (i = 0; i < n; i++) {
        x[i] = ag_sat16((int32_t)(((int64_t)x[i] * target) / (int64_t)rms));
    }
    normalize_peak(x, n, 32000);
}

/* ------------------------------------------------------------------------ */

static double db(double v)
{
    if (v < 1e-12) {
        return -240.0;
    }
    return 20.0 * log10(v);
}

static int16_t sat16d(double v)
{
    if (v > 32767.0) {
        return 32767;
    }
    if (v < -32768.0) {
        return -32768;
    }
    return (int16_t)(v >= 0.0 ? v + 0.5 : v - 0.5);
}

/*
 * One pass at one input level.  Both paths get the identical input samples,
 * so any difference between them is the engine's.
 */
static void run_level(const int16_t *src, uint32_t frames, uint32_t rate,
                      const int16_t *h, uint32_t hn, double atten,
                      const char *tag, const char *outdir)
{
    const uint32_t gain = 64u, wetmix = 127u;
    const uint32_t win = rate / 5u; /* 200 ms */
    ag_ir_t   ir;
    int16_t  *in, *eng, *ref, *err, *hn2;
    double   *hd;
    uint32_t  i, k, blocks, w, nwin;
    double    tot_s = 0.0, tot_e = 0.0, worst = -240.0, peak_err = 0.0;
    double    worst_sig = 0.0;
    char      path[512];
    int16_t   stereo[AG_IR_BLOCK * 2u];

    blocks = frames / AG_IR_BLOCK;
    if (blocks == 0u) {
        return;
    }
    frames = blocks * AG_IR_BLOCK;

    in = (int16_t *)malloc(sizeof(int16_t) * frames);
    eng = (int16_t *)malloc(sizeof(int16_t) * frames);
    ref = (int16_t *)malloc(sizeof(int16_t) * frames);
    err = (int16_t *)malloc(sizeof(int16_t) * frames);
    hn2 = (int16_t *)malloc(sizeof(int16_t) * hn);
    hd = (double *)malloc(sizeof(double) * hn);
    if (in == NULL || eng == NULL || ref == NULL || err == NULL ||
        hn2 == NULL || hd == NULL) {
        fprintf(stderr, "out of memory\n");
        return;
    }
    for (i = 0; i < frames; i++) {
        in[i] = sat16d((double)src[i] * atten);
    }
    memcpy(hn2, h, sizeof(int16_t) * hn);
    normalize_energy(hn2, hn, 16384);
    for (i = 0; i < hn; i++) {
        hd[i] = (double)hn2[i] / 32768.0;
    }

    /* The engine. */
    if (ag_ir_init(&ir, rate) != 0 ||
        ag_ir_load(&ir, h, hn, rate) != 0) {
        fprintf(stderr, "ir init failed\n");
        return;
    }
    ag_ir_set_gain(&ir, (uint8_t)gain);
    ag_ir_set_wet(&ir, (uint8_t)wetmix);
    for (k = 0; k < blocks; k++) {
        ag_ir_process_block(&ir, in + k * AG_IR_BLOCK, stereo);
        for (i = 0; i < AG_IR_BLOCK; i++) {
            eng[k * AG_IR_BLOCK + i] = stereo[i * 2u];
        }
    }
    ag_ir_free(&ir);

    /*
     * The reference, with the engine's own mix arithmetic in double so that
     * only the convolution differs.  Direct form: slow, and beyond argument.
     */
    for (i = 0; i < frames; i++) {
        double conv = 0.0;
        double wet, m, dry = (double)in[i];
        uint32_t kk;
        uint32_t lim = (i + 1u) < hn ? (i + 1u) : hn;
        for (kk = 0; kk < lim; kk++) {
            conv += hd[kk] * (double)in[i - kk];
        }
        wet = floor((conv * (double)gain + 32.0) / 64.0);
        if (wet > 32767.0) {
            wet = 32767.0;
        }
        if (wet < -32768.0) {
            wet = -32768.0;
        }
        m = dry + floor(((wet - dry) * (double)wetmix + 64.0) / 128.0);
        ref[i] = sat16d(m);
    }

    /*
     * Skip the first partition's worth: the engine's delay line starts empty
     * and its first block is a genuine transient, not an error.
     */
    nwin = 0;
    for (i = 0; i < frames; i++) {
        const double d = (double)eng[i] - (double)ref[i];
        double ad = d < 0.0 ? -d : d;
        double amp = d * 64.0;
        err[i] = sat16d(amp);
        if (i < AG_IR_BLOCK) {
            continue;
        }
        if (ad > peak_err) {
            peak_err = ad;
        }
        tot_s += (double)ref[i] * (double)ref[i];
        tot_e += d * d;
    }

    for (w = AG_IR_BLOCK; w + win <= frames; w += win) {
        double s = 0.0, e = 0.0, r;
        for (i = w; i < w + win; i++) {
            const double d = (double)eng[i] - (double)ref[i];
            s += (double)ref[i] * (double)ref[i];
            e += d * d;
        }
        if (s < 1.0) {
            continue; /* silence: nothing to be relative to */
        }
        r = db(sqrt(e / s));
        nwin++;
        if (r > worst) {
            worst = r;
            worst_sig = db(sqrt(s / (double)win) / 32768.0);
        }
    }

    printf("  %-8s signal %7.1f dBFS   error %7.1f dBFS   "
           "err/sig %7.1f dB   worst window %7.1f dB (at %.0f dBFS)\n",
           tag,
           db(sqrt(tot_s / (double)(frames - AG_IR_BLOCK)) / 32768.0),
           db(sqrt(tot_e / (double)(frames - AG_IR_BLOCK)) / 32768.0),
           db(sqrt(tot_e / tot_s)), worst, worst_sig);
    printf("           peak error %.1f LSB over %u windows\n", peak_err, nwin);

    snprintf(path, sizeof(path), "%s/ir_%s_engine.wav", outdir, tag);
    wav_write(path, eng, frames, rate);
    snprintf(path, sizeof(path), "%s/ir_%s_ref.wav", outdir, tag);
    wav_write(path, ref, frames, rate);
    snprintf(path, sizeof(path), "%s/ir_%s_err_x64.wav", outdir, tag);
    wav_write(path, err, frames, rate);

    free(in);
    free(eng);
    free(ref);
    free(err);
    free(hn2);
    free(hd);
}

/*
 * A single-tap impulse response: the convolution then has to hand back what
 * it was given, at half scale, and every sample that differs is the engine's
 * own arithmetic with nothing else mixed in.
 */
/*
 * Forward then inverse on the same block: whatever comes back changed is the
 * transform's own rounding, before any of the convolution's scaling gets a
 * chance to make it worse.
 */
static void diag_fft(uint32_t rate, int lift)
{
    static int32_t re[AG_IR_FFT], im[AG_IR_FFT];
    int32_t  src[AG_IR_FFT];
    double   ph = 0.0, e2 = 0.0, s2 = 0.0, worst = 0.0;
    uint32_t i;

    for (i = 0; i < AG_IR_FFT; i++) {
        ph += 2.0 * 3.14159265358979 * 220.0 / (double)rate;
        src[i] = i < AG_IR_BLOCK ? (int32_t)(20000.0 * sin(ph)) : 0;
        re[i] = src[i] << lift;
        im[i] = 0;
    }
    (void)ag_fft_cplx_i32(re, im, (int)AG_IR_FFT, 1);
    (void)ag_fft_cplx_i32(re, im, (int)AG_IR_FFT, 0);
    for (i = 0; i < AG_IR_FFT; i++) {
        const double got = (double)re[i] / (double)AG_IR_FFT /
                           (double)(1 << lift);
        const double d = got - (double)src[i];
        const double ad = d < 0.0 ? -d : d;
        if (ad > worst) {
            worst = ad;
        }
        e2 += d * d;
        s2 += (double)src[i] * (double)src[i];
    }
    printf("  fft round trip, lift %2d: err/sig %6.1f dB, peak %8.3f LSB\n",
           lift, db(sqrt(e2 / s2)), worst);
}

static void diag_delta(uint32_t rate, double amp)
{
    ag_ir_t  ir;
    int16_t  h[64];
    int16_t  in[AG_IR_BLOCK];
    int16_t  stereo[AG_IR_BLOCK * 2u];
    uint32_t i, k;
    double   worst = 0.0, e2 = 0.0, s2 = 0.0, sum = 0.0;
    double   ph = 0.0;
    uint32_t n = 0;

    memset(h, 0, sizeof(h));
    h[0] = 32767;
    if (ag_ir_init(&ir, rate) != 0 || ag_ir_load(&ir, h, 64u, rate) != 0) {
        return;
    }
    ag_ir_set_gain(&ir, 64);
    ag_ir_set_wet(&ir, 127);

    for (k = 0; k < 40u; k++) {
        for (i = 0; i < AG_IR_BLOCK; i++) {
            ph += 2.0 * 3.14159265358979 * 220.0 / (double)rate;
            in[i] = sat16d(amp * sin(ph));
        }
        ag_ir_process_block(&ir, in, stereo);
        if (k < 2u) {
            continue; /* delay line filling */
        }
        for (i = 0; i < AG_IR_BLOCK; i++) {
            /* h[0] normalizes to 16384, so wet is exactly half the input;
             * then 1/128 of dry leaks back through the wet=127 mix. */
            const double wet = (double)in[i] * 16384.0 / 32768.0;
            const double ref = floor((double)in[i] +
                                     ((wet - (double)in[i]) * 127.0 + 64.0) /
                                         128.0);
            const double d = (double)stereo[i * 2u] - ref;
            const double ad = d < 0.0 ? -d : d;
            if (ad > worst) {
                worst = ad;
            }
            e2 += d * d;
            s2 += ref * ref;
            sum += d;
            n++;
        }
    }
    printf("    amp %6.0f: err/sig %6.1f dB   error rms %6.2f LSB   "
           "mean %6.2f   peak %4.0f\n",
           amp, db(sqrt(e2 / s2)), sqrt(e2 / (double)n), sum / (double)n,
           worst);
    ag_ir_free(&ir);
}

/*
 * Every preset through the same recording, level only.  The point is the long
 * ones: a 500 ms tail is 94 partitions, which is where the partition count
 * drives the shifts hardest, and a scale that comes out wrong there shows up
 * as a level that is nowhere near the short ones'.
 */
static void diag_presets(const int16_t *src, uint32_t frames, uint32_t rate)
{
    static const char *names[5] = {"room", "hall", "spring", "cab dark",
                                   "cab bright"};
    int16_t  stereo[AG_IR_BLOCK * 2u];
    uint32_t blocks = frames / AG_IR_BLOCK;
    int      preset;

    if (blocks > 2000u) {
        blocks = 2000u; /* ~11 s is plenty to settle a 500 ms tail */
    }
    for (preset = 0; preset < 5; preset++) {
        ag_ir_t  ir;
        double   e2 = 0.0;
        int32_t  peak = 0;
        uint32_t k, i, n = 0;
        if (ag_ir_init(&ir, rate) != 0 || ag_ir_load_preset(&ir, preset) != 0) {
            continue;
        }
        ag_ir_set_gain(&ir, 64);
        ag_ir_set_wet(&ir, 127);
        for (k = 0; k < blocks; k++) {
            ag_ir_process_block(&ir, src + k * AG_IR_BLOCK, stereo);
            for (i = 0; i < AG_IR_BLOCK; i++) {
                const int32_t v = stereo[i * 2u];
                const int32_t a = v < 0 ? -v : v;
                if (a > peak) {
                    peak = a;
                }
                e2 += (double)v * (double)v;
                n++;
            }
        }
        printf("  preset %d %-11s %3u parts, h_pre %2u, h_shift %2u, "
               "p_shift %u   out %6.1f dBFS rms, %6.1f dBFS peak\n",
               preset, names[preset], ir.parts, ir.h_pre, ir.h_shift,
               ir.p_shift,
               db(sqrt(e2 / (double)n) / 32768.0), db((double)peak / 32768.0));
        ag_ir_free(&ir);
    }
}

int main(int argc, char **argv)
{
    const char *outdir = argc > 2 ? argv[2] : "build/listen";
    int16_t    *src;
    int16_t    *h;
    uint32_t    frames = 0, rate = 0, hn;

    if (argc < 2) {
        fprintf(stderr, "usage: ir_check <input.wav> [outdir]\n");
        return 2;
    }
    src = wav_read(argv[1], &frames, &rate);
    if (src == NULL) {
        return 1;
    }
    if (rate < 8000u || rate > 48000u) {
        fprintf(stderr, "rate %u out of range for ag_ir\n", rate);
        return 1;
    }
    h = (int16_t *)malloc(sizeof(int16_t) * rate);
    if (h == NULL) {
        return 1;
    }
    hn = make_cab(h, rate, rate);

    printf("ir_check: %s, %u frames at %u Hz, cabinet %u taps (%.0f ms)\n",
           argv[1], frames, rate, hn, 1000.0 * (double)hn / (double)rate);
    printf("  engine vs the same convolution in double precision\n");

    /*
     * Lift 0 only.  Lifting the input here does not measure what it measures
     * inside the engine: a round trip runs the unnormalized inverse straight
     * off the forward, and that alone multiplies by 512, so anything lifted
     * overflows on the way back.  The engine rescales between the two, which
     * is exactly why it can afford the lift and this cannot.
     */
    diag_fft(rate, 0);
    printf("  delta IR (engine must hand back half of what it was given)\n");
    diag_delta(rate, 20000.0);
    diag_delta(rate, 2000.0);
    diag_delta(rate, 200.0);
    diag_presets(src, frames, rate);

    run_level(src, frames, rate, h, hn, 1.0, "0dB", outdir);
    run_level(src, frames, rate, h, hn, 0.1, "-20dB", outdir);
    run_level(src, frames, rate, h, hn, 0.01, "-40dB", outdir);

    free(h);
    free(src);
    return 0;
}
