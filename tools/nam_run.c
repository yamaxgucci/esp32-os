/*
 * nam_run - a capture, a take, a render.  The same job as the Go `namrun`, in
 * this tree, so that a tone match can be checked without a binary in %TEMP%.
 *
 *   build-host/nam_run model.nam in.wav out.wav [weights]
 *
 * WHY THE WAV HANDLING IS SO LITERAL
 *
 * This program exists to be compared against `namrun` byte for byte, and a
 * comparison that has to allow for a rounding difference in the file writer says
 * nothing about the model.  So the format is the reference's format and not this
 * tree's: mono, 48 kHz, 24-bit, scaled by 2^23, clipped to +-1 and **truncated**
 * toward zero rather than rounded, because that is what a Go float32-to-int32
 * conversion does.  Every one of those is a decision the reference made and this
 * one copies.
 *
 * 16-bit input is accepted too, because a take in this tree is usually 16-bit,
 * and the values are exact either way - the reference refuses it, which is the
 * only place the two differ on purpose.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nam.h"

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t rd_u16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

/*
 * Mono float, first channel only, 16 or 24 bit.
 *
 * The scaling is by the format's own full scale - 2^15 or 2^23 - and both are
 * exact in float32, so what the model sees is exactly what the file holds.  That
 * matters: the reference divides by 2^23 after an integer-to-float32 conversion
 * that is itself exact, and any other route in would make the two renders differ
 * before the model had done anything.
 */
static float *read_wav(const char *path, uint32_t *frames, uint32_t *rate,
                       uint32_t *bits_out)
{
    FILE    *f = fopen(path, "rb");
    uint8_t *buf;
    long     len;
    uint32_t pos = 12, ch = 1, bits = 16, off = 0, dlen = 0, i;
    float   *out;

    if (f == NULL) {
        printf("  cannot read %s\n", path);
        return NULL;
    }
    (void)fseek(f, 0, SEEK_END);
    len = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    if (len < 44) {
        (void)fclose(f);
        printf("  %s is not a wav\n", path);
        return NULL;
    }
    buf = (uint8_t *)malloc((size_t)len);
    if (buf == NULL || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        (void)fclose(f);
        free(buf);
        return NULL;
    }
    (void)fclose(f);
    while (pos + 8u <= (uint32_t)len) {
        const uint32_t sz = rd_u32(buf + pos + 4);
        if (memcmp(buf + pos, "fmt ", 4) == 0) {
            ch = rd_u16(buf + pos + 10);
            *rate = rd_u32(buf + pos + 12);
            bits = rd_u16(buf + pos + 22);
        } else if (memcmp(buf + pos, "data", 4) == 0) {
            off = pos + 8u;
            dlen = sz;
        }
        pos += 8u + sz + (sz & 1u);
    }
    if (off == 0 || ch == 0 || (bits != 16 && bits != 24)) {
        printf("  %s: need 16- or 24-bit PCM (got %u bits, %u channels)\n", path,
               bits, ch);
        free(buf);
        return NULL;
    }
    *bits_out = bits;
    {
        const uint32_t bps = bits / 8u;
        *frames = dlen / (bps * ch);
        out = (float *)malloc(sizeof(float) * (*frames ? *frames : 1u));
        if (out == NULL) {
            free(buf);
            return NULL;
        }
        for (i = 0; i < *frames; i++) {
            const uint8_t *p = buf + off + (size_t)bps * ch * i;
            if (bits == 16) {
                out[i] = (float)(int16_t)rd_u16(p) / 32768.0f;
            } else {
                const int32_t s =
                    (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 |
                              (uint32_t)p[2] << 24);
                out[i] = (float)(s >> 8) / 8388608.0f;
            }
        }
    }
    free(buf);
    return out;
}

static void wr32(FILE *f, uint32_t v)
{
    uint8_t b[4];
    b[0] = (uint8_t)(v & 0xFFu);
    b[1] = (uint8_t)((v >> 8) & 0xFFu);
    b[2] = (uint8_t)((v >> 16) & 0xFFu);
    b[3] = (uint8_t)((v >> 24) & 0xFFu);
    (void)fwrite(b, 1, 4, f);
}

static void wr16(FILE *f, uint16_t v)
{
    uint8_t b[2];
    b[0] = (uint8_t)(v & 0xFFu);
    b[1] = (uint8_t)((v >> 8) & 0xFFu);
    (void)fwrite(b, 1, 2, f);
}

/* 24-bit mono, the reference's scaling and its truncation. */
static int write_wav24(const char *path, const float *x, uint32_t n,
                       uint32_t rate, uint32_t *clipped)
{
    FILE    *f = fopen(path, "wb");
    uint32_t i;

    if (f == NULL) {
        return -1;
    }
    (void)fwrite("RIFF", 1, 4, f);
    wr32(f, 36u + n * 3u);
    (void)fwrite("WAVEfmt ", 1, 8, f);
    wr32(f, 16);
    wr16(f, 1);
    wr16(f, 1);
    wr32(f, rate);
    wr32(f, rate * 3u);
    wr16(f, 3);
    wr16(f, 24);
    (void)fwrite("data", 1, 4, f);
    wr32(f, n * 3u);
    for (i = 0; i < n; i++) {
        float   v = x[i];
        int32_t s;
        uint8_t b[3];
        if (v > 1.0f) {
            v = 1.0f;
            (*clipped)++;
        } else if (v < -1.0f) {
            v = -1.0f;
            (*clipped)++;
        }
        s = (int32_t)(v * 8388608.0f);
        b[0] = (uint8_t)((uint32_t)s & 0xFFu);
        b[1] = (uint8_t)(((uint32_t)s >> 8) & 0xFFu);
        b[2] = (uint8_t)(((uint32_t)s >> 16) & 0xFFu);
        (void)fwrite(b, 1, 3, f);
    }
    (void)fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    const char *model, *in_path, *out_path;
    nam_model_t *m;
    float       *in, *out;
    uint32_t     frames = 0, rate = 0, bits = 0, clipped = 0, i;
    int          want = 0;
    clock_t      t0;

    if (argc < 4) {
        printf("usage: nam_run model.nam in.wav out.wav [weights]\n"
               "  weights picks a container submodel by parameter count;"
               " 0 or absent takes the largest\n");
        return 2;
    }
    model = argv[1];
    in_path = argv[2];
    out_path = argv[3];
    if (argc > 4) {
        want = atoi(argv[4]);
    }

    printf("\n  nam_run\n");
    m = nam_load(model, want, 1);
    if (m == NULL) {
        printf("  %s: %s\n", model, nam_err());
        return 1;
    }
    printf("  receptive field %d samples, capture rate %d Hz\n",
           nam_receptive_field(m), nam_sample_rate(m));

    in = read_wav(in_path, &frames, &rate, &bits);
    if (in == NULL || frames == 0) {
        nam_free(m);
        return 1;
    }
    if (nam_sample_rate(m) != 0 && (uint32_t)nam_sample_rate(m) != rate) {
        /*
         * A warning rather than an error, because it is a real thing to want -
         * but it is not the amplifier: every filter the capture learned sits at
         * a fixed fraction of *its* sample rate, so playing it at another rate
         * moves all of them.  tools/wavrate.c is the way in and out.
         */
        printf("  WARNING the capture is %d Hz and the take is %u - resample the"
               " take first (tools/wavrate.c) or this is a different amplifier\n",
               nam_sample_rate(m), rate);
    }
    out = (float *)calloc((size_t)frames, sizeof(float));
    if (out == NULL) {
        nam_free(m);
        free(in);
        return 1;
    }

    printf("  take: %s, %u frames at %u Hz, %u-bit, %.2f s\n", in_path, frames,
           rate, bits, (double)frames / (double)(rate ? rate : 1u));
    t0 = clock();
    for (i = 0; i + (uint32_t)NAM_BLOCK <= frames; i += (uint32_t)NAM_BLOCK) {
        nam_process(m, in + i, out + i, NAM_BLOCK);
    }
    if (i < frames) {
        nam_process(m, in + i, out + i, (int)(frames - i));
    }
    printf("  rendered in %.1f s, %.1fx realtime\n",
           (double)(clock() - t0) / (double)CLOCKS_PER_SEC,
           ((double)frames / (double)(rate ? rate : 1u)) /
               ((double)(clock() - t0) / (double)CLOCKS_PER_SEC + 1e-9));

    if (write_wav24(out_path, out, frames, rate, &clipped) != 0) {
        printf("  cannot write %s\n", out_path);
        nam_free(m);
        free(in);
        free(out);
        return 1;
    }
    printf("  wrote %s (%u samples, 24-bit%s)\n\n", out_path, frames,
           clipped ? ", CLIPPED" : "");
    nam_free(m);
    free(in);
    free(out);
    return 0;
}
