/*
 * ArgonOS - host tests for the NAM capture player (tools/nam.c).
 *
 * What is actually being tested here, and what cannot be.
 *
 * The correctness of the inference is not something a unit test can establish:
 * the model is 12 000 numbers from somebody else's trainer and the only way to
 * know it is being played right is to compare against an implementation that is
 * known to play it right.  That comparison exists and it is exact - a render of
 * `Mars Gain 8.nam` over the DI take comes back **bit for bit** identical to the
 * one the Go reference made - and `test_capture` below is that comparison,
 * shortened to the first three blocks so it costs a tenth of a second.  It skips
 * itself when the captures are absent, because they are third-party files and
 * are not in the repository.
 *
 * The rest are the properties that hold whatever the weights are, and each one
 * catches a specific way this code could be wrong while looking right:
 *
 * - **Silence is steady.**  After the warm-up, a zero input must give the same
 *   output sample forever, bit for bit.  A model has an output on silence
 *   because it has biases; if the history buffers are stitched together wrongly
 *   the value changes at a block or rewind boundary, which is a click in a quiet
 *   passage and nothing at all in a measurement of a loud one.
 * - **The block size does not matter.**  The same take in blocks of 4096 and of
 *   333 must give bit-identical output, across a buffer rewind.  This is the
 *   check on the whole windowing scheme: the rewind moves the data to a different
 *   place in the allocation at a different moment in the two runs.
 * - **A misread topology is refused.**  The loader is asked to accept a model
 *   with a feature it does not implement, and one whose parameter count is off by
 *   one.  Both must fail: a model that loads with the wrong shape does not sound
 *   broken, it sounds like a different amplifier.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nam.h"

/* ------------------------------------------------------------------------ */
/* a model small enough to write out here                                    */
/* ------------------------------------------------------------------------ */

/*
 * Two channels, two layers, a two-tap head: 52 parameters, counted the same way
 * the loader counts them - 2 for the input rechannel, 22 for each layer
 * (2*2*3 + 2 front, 2 mixin, 2*2 + 2 post), 5 for the head, and one for the
 * output scale.
 */
#define SYN_WEIGHTS 52

static double syn_weight(int i)
{
    /* Deterministic, spread across both signs, and nothing round enough to hide
     * an indexing mistake by symmetry. */
    return (double)((i * 37) % 41 - 20) / 40.0;
}

static const char *tmp_path(const char *name)
{
    static char buf[512];
    const char *dir = getenv("TEMP");
    if (dir == NULL) {
        dir = getenv("TMPDIR");
    }
    if (dir == NULL) {
        dir = ".";
    }
    (void)snprintf(buf, sizeof(buf), "%s/%s", dir, name);
    return buf;
}

/* `extra` weights more than the architecture needs, and `gated` to turn on
 * something the player refuses. */
static int write_syn(const char *path, int extra, int gated)
{
    FILE *f = fopen(path, "wb");
    int   i;

    if (f == NULL) {
        return -1;
    }
    (void)fprintf(f,
                  "{\"version\":\"0.7.0\",\"architecture\":\"WaveNet\","
                  "\"sample_rate\":48000,\"config\":{\"head\":null,"
                  "\"head_scale\":0.5,\"layers\":[{"
                  "\"input_size\":1,\"condition_size\":1,\"channels\":2,"
                  "\"kernel_sizes\":[3,3],\"dilations\":[1,2],"
                  "\"head\":{\"out_channels\":1,\"kernel_size\":2,"
                  "\"bias\":true},"
                  "\"activation\":[{\"type\":\"LeakyReLU\","
                  "\"negative_slope\":0.01},{\"type\":\"LeakyReLU\","
                  "\"negative_slope\":0.01}],"
                  "\"gated\":%s}]},\"weights\":[",
                  gated ? "true" : "false");
    for (i = 0; i < SYN_WEIGHTS + extra; i++) {
        (void)fprintf(f, "%s%.9g", i ? "," : "", syn_weight(i));
    }
    (void)fprintf(f, "]}");
    (void)fclose(f);
    return 0;
}

/* ------------------------------------------------------------------------ */

static void test_loads(void)
{
    const char  *path = tmp_path("argon_nam_syn.nam");
    nam_model_t *m;

    AG_CHECK_INT(write_syn(path, 0, 0), 0);
    m = nam_load(path, 0, 0);
    AG_CHECK(m != NULL);
    if (m == NULL) {
        printf("  nam: %s\n", nam_err());
        return;
    }
    /* 1 + (3-1)*1 + (3-1)*2 - the zero-indexed receptive field plus the sample
     * itself, which is what the reference warms up by. */
    AG_CHECK_INT(nam_receptive_field(m), 7);
    AG_CHECK_INT(nam_sample_rate(m), 48000);
    nam_free(m);
}

static void test_silence_is_steady(void)
{
    const char  *path = tmp_path("argon_nam_syn.nam");
    nam_model_t *m;
    float        in[NAM_BLOCK], out[NAM_BLOCK];
    float        first = 0.0f;
    int          b, i, bad = 0;

    if (write_syn(path, 0, 0) != 0) {
        AG_CHECK(0);
        return;
    }
    m = nam_load(path, 0, 0);
    AG_CHECK(m != NULL);
    if (m == NULL) {
        return;
    }
    memset(in, 0, sizeof(in));
    /* Past 65536 samples, so a rewind happens inside this loop. */
    for (b = 0; b < 20; b++) {
        nam_process(m, in, out, NAM_BLOCK);
        for (i = 0; i < NAM_BLOCK; i++) {
            if (b == 0 && i == 0) {
                first = out[0];
            } else if (out[i] != first) {
                bad++;
            }
        }
    }
    AG_CHECK_INT(bad, 0);
    /* And it is a real value rather than a zero that would make the check
     * vacuous: the biases give the model an output on silence. */
    AG_CHECK(first != 0.0f);
    nam_free(m);
}

static void test_block_size_does_not_matter(void)
{
    const char  *path = tmp_path("argon_nam_syn.nam");
    nam_model_t *a, *b;
    float       *in, *ya, *yb;
    const int    n = 70000; /* past the 65536-column buffer, so both rewind */
    int          i, bad = 0;

    if (write_syn(path, 0, 0) != 0) {
        AG_CHECK(0);
        return;
    }
    a = nam_load(path, 0, 0);
    b = nam_load(path, 0, 0);
    in = (float *)malloc(sizeof(float) * (size_t)n);
    ya = (float *)malloc(sizeof(float) * (size_t)n);
    yb = (float *)malloc(sizeof(float) * (size_t)n);
    AG_CHECK(a != NULL && b != NULL && in != NULL && ya != NULL && yb != NULL);
    if (a == NULL || b == NULL || in == NULL || ya == NULL || yb == NULL) {
        nam_free(a);
        nam_free(b);
        free(in);
        free(ya);
        free(yb);
        return;
    }
    for (i = 0; i < n; i++) {
        in[i] = 0.3f * (float)sin(0.037 * (double)i) +
                0.1f * (float)sin(0.31 * (double)i);
    }
    for (i = 0; i + NAM_BLOCK <= n; i += NAM_BLOCK) {
        nam_process(a, in + i, ya + i, NAM_BLOCK);
    }
    if (i < n) {
        nam_process(a, in + i, ya + i, n - i);
    }
    for (i = 0; i + 333 <= n; i += 333) {
        nam_process(b, in + i, yb + i, 333);
    }
    if (i < n) {
        nam_process(b, in + i, yb + i, n - i);
    }
    for (i = 0; i < n; i++) {
        if (ya[i] != yb[i]) {
            bad++;
        }
    }
    AG_CHECK_INT(bad, 0);
    /* Not silence, or the comparison above would prove nothing. */
    AG_CHECK(ya[n / 2] != ya[n / 2 + 1]);
    nam_free(a);
    nam_free(b);
    free(in);
    free(ya);
    free(yb);
}

static void test_refusals(void)
{
    const char  *path = tmp_path("argon_nam_bad.nam");
    nam_model_t *m;

    AG_CHECK_INT(write_syn(path, 0, 1), 0);
    m = nam_load(path, 0, 0);
    AG_CHECK(m == NULL);
    AG_CHECK(strstr(nam_err(), "gated") != NULL);
    nam_free(m);

    /* One number too many is a topology that was read wrongly, not a file with
     * something to spare. */
    AG_CHECK_INT(write_syn(path, 1, 0), 0);
    m = nam_load(path, 0, 0);
    AG_CHECK(m == NULL);
    AG_CHECK(strstr(nam_err(), "parameters") != NULL);
    nam_free(m);

    AG_CHECK_INT(write_syn(path, -1, 0), 0);
    m = nam_load(path, 0, 0);
    AG_CHECK(m == NULL);
    nam_free(m);

    m = nam_load(tmp_path("argon_nam_absent.nam"), 0, 0);
    AG_CHECK(m == NULL);
    nam_free(m);
}

/* ------------------------------------------------------------------------ */
/* the real thing, against the reference render                              */
/* ------------------------------------------------------------------------ */

/* Mono 24-bit, the only form the renders under build/nam take. */
static int32_t *read24(const char *path, uint32_t *frames)
{
    FILE     *f = fopen(path, "rb");
    uint8_t  *buf;
    long      len;
    uint32_t  pos = 12, off = 0, dlen = 0, i;
    int32_t  *out;

    if (f == NULL) {
        return NULL;
    }
    (void)fseek(f, 0, SEEK_END);
    len = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    buf = len > 44 ? (uint8_t *)malloc((size_t)len) : NULL;
    if (buf == NULL || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        (void)fclose(f);
        free(buf);
        return NULL;
    }
    (void)fclose(f);
    while (pos + 8u <= (uint32_t)len) {
        const uint32_t sz = (uint32_t)buf[pos + 4] |
                            ((uint32_t)buf[pos + 5] << 8) |
                            ((uint32_t)buf[pos + 6] << 16) |
                            ((uint32_t)buf[pos + 7] << 24);
        if (memcmp(buf + pos, "data", 4) == 0) {
            off = pos + 8u;
            dlen = sz;
        }
        pos += 8u + sz + (sz & 1u);
    }
    if (off == 0 || dlen % 3u != 0u) {
        free(buf);
        return NULL;
    }
    *frames = dlen / 3u;
    out = (int32_t *)malloc(sizeof(int32_t) * (*frames ? *frames : 1u));
    if (out == NULL) {
        free(buf);
        return NULL;
    }
    for (i = 0; i < *frames; i++) {
        const uint8_t *p = buf + off + (size_t)3u * i;
        out[i] = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 |
                           (uint32_t)p[2] << 24) >> 8;
    }
    free(buf);
    return out;
}

#ifndef AG_TREE_ROOT
#define AG_TREE_ROOT "."
#endif

static void test_capture(void)
{
    const char  *cap = AG_TREE_ROOT "/assets/audio/guitar-di/Mars Gain 8.nam";
    const char  *dry = AG_TREE_ROOT "/build/nam/GuitarClean2_48k24.wav";
    const char  *wet = AG_TREE_ROOT "/build/nam/GuitarClean2_mars.wav";
    const int    n = 3 * NAM_BLOCK;
    nam_model_t *m;
    int32_t     *xi, *yi;
    uint32_t     xn = 0, yn = 0;
    float       *x, *y;
    int          i, bad = 0;

    xi = read24(dry, &xn);
    yi = read24(wet, &yn);
    m = nam_load(cap, 0, 0);
    if (m == NULL || xi == NULL || yi == NULL || xn < (uint32_t)n ||
        yn < (uint32_t)n) {
        printf("  nam: skipping the reference comparison - %s\n",
               m == NULL ? "no capture (they are not in the repository)"
                         : "no rendered take under build/nam");
        nam_free(m);
        free(xi);
        free(yi);
        return;
    }
    x = (float *)malloc(sizeof(float) * (size_t)n);
    y = (float *)malloc(sizeof(float) * (size_t)n);
    AG_CHECK(x != NULL && y != NULL);
    if (x == NULL || y == NULL) {
        nam_free(m);
        free(xi);
        free(yi);
        free(x);
        free(y);
        return;
    }
    for (i = 0; i < n; i++) {
        x[i] = (float)xi[i] / 8388608.0f;
    }
    for (i = 0; i + NAM_BLOCK <= n; i += NAM_BLOCK) {
        nam_process(m, x + i, y + i, NAM_BLOCK);
    }
    /*
     * Compared as the stored file stores them - clipped and truncated toward
     * zero - because that is the only form in which the reference's answer
     * survives on disk.  Every one of these samples has to match exactly; a
     * tolerance here would let a wrong weight order through as a small error.
     */
    for (i = 0; i < n; i++) {
        float v = y[i];
        if (v > 1.0f) {
            v = 1.0f;
        } else if (v < -1.0f) {
            v = -1.0f;
        }
        if ((int32_t)(v * 8388608.0f) != yi[i]) {
            bad++;
        }
    }
    AG_CHECK_INT(bad, 0);
    nam_free(m);
    free(xi);
    free(yi);
    free(x);
    free(y);
}

void run_nam_tests(void)
{
    test_loads();
    test_silence_is_steady();
    test_block_size_does_not_matter();
    test_refusals();
    test_capture();
}
