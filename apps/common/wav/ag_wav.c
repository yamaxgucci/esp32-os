/*
 * Minimal RIFF PCM16 WAV → mono s16 loader.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_wav.h"

#include <argon/argon.h>

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int tag_eq(const uint8_t *p, const char *tag)
{
    return p[0] == (uint8_t)tag[0] && p[1] == (uint8_t)tag[1] &&
           p[2] == (uint8_t)tag[2] && p[3] == (uint8_t)tag[3];
}

void ag_wav_free(ag_wav_pcm_t *w)
{
    if (w == NULL) {
        return;
    }
    if (w->data != NULL) {
        ag_free(w->data);
    }
    w->data = NULL;
    w->frames = 0;
    w->rate = 0;
}

int ag_wav_load(const char *path, ag_wav_pcm_t *out)
{
    ag_handle_t h;
    uint8_t     hdr[12];
    uint16_t    audio_fmt = 0, channels = 0, bits = 0;
    uint32_t    rate = 0, data_bytes = 0;
    int64_t     data_off = -1;
    int16_t    *mono = NULL;
    uint32_t    frames = 0;
    uint32_t    i;

    if (path == NULL || out == NULL) {
        return -1;
    }
    out->data = NULL;
    out->frames = 0;
    out->rate = 0;

    h = ag_open(path, AG_O_RDONLY);
    if (h < 0) {
        return -1;
    }
    if (ag_read(h, hdr, 12) != 12 || !tag_eq(hdr, "RIFF") ||
        !tag_eq(hdr + 8, "WAVE")) {
        (void)ag_close(h);
        return -1;
    }

    for (;;) {
        uint8_t  ch[8];
        uint32_t csz;
        int64_t  next;
        if (ag_read(h, ch, 8) != 8) {
            break;
        }
        csz = rd_u32(ch + 4);
        next = ag_seek(h, 0, AG_SEEK_CUR);
        if (next < 0) {
            break;
        }
        next += (int64_t)csz + (int64_t)(csz & 1u);

        if (tag_eq(ch, "fmt ")) {
            uint8_t fmt[16];
            if (csz < 16u || ag_read(h, fmt, 16) != 16) {
                (void)ag_close(h);
                return -1;
            }
            audio_fmt = rd_u16(fmt + 0);
            channels = rd_u16(fmt + 2);
            rate = rd_u32(fmt + 4);
            bits = rd_u16(fmt + 14);
        } else if (tag_eq(ch, "data")) {
            data_off = ag_seek(h, 0, AG_SEEK_CUR);
            data_bytes = csz;
            break;
        }
        if (ag_seek(h, next, AG_SEEK_SET) < 0) {
            break;
        }
    }

    if (audio_fmt != 1u || (channels != 1u && channels != 2u) || bits != 16u ||
        rate < 8000u || rate > 48000u || data_off < 0 || data_bytes < 2u) {
        (void)ag_close(h);
        return -1;
    }
    if (ag_seek(h, data_off, AG_SEEK_SET) < 0) {
        (void)ag_close(h);
        return -1;
    }

    frames = data_bytes / ((uint32_t)channels * 2u);
    if (frames < 16u) {
        (void)ag_close(h);
        return -1;
    }
    /* Cap ~30s @ 48k to keep PSRAM polite */
    if (frames > 48000u * 30u) {
        frames = 48000u * 30u;
    }

    mono = (int16_t *)ag_malloc((size_t)frames * sizeof(int16_t));
    if (mono == NULL) {
        (void)ag_close(h);
        return -1;
    }

    if (channels == 1u) {
        int32_t need = (int32_t)(frames * 2u);
        int32_t got = ag_read(h, mono, (size_t)need);
        if (got < 32) {
            ag_free(mono);
            (void)ag_close(h);
            return -1;
        }
        frames = (uint32_t)got / 2u;
    } else {
        /* Read in chunks and mix L/R */
        enum { CHUNK = 1024 };
        int16_t tmp[CHUNK * 2];
        uint32_t done = 0;
        while (done < frames) {
            uint32_t n = frames - done;
            int32_t  got;
            uint32_t j;
            if (n > CHUNK) {
                n = CHUNK;
            }
            got = ag_read(h, tmp, (size_t)n * 4u);
            if (got < 4) {
                break;
            }
            n = (uint32_t)got / 4u;
            for (j = 0; j < n; j++) {
                int32_t m = ((int32_t)tmp[j * 2] + (int32_t)tmp[j * 2 + 1]) / 2;
                mono[done + j] = (int16_t)m;
            }
            done += n;
        }
        frames = done;
        if (frames < 16u) {
            ag_free(mono);
            (void)ag_close(h);
            return -1;
        }
    }
    (void)ag_close(h);

    /* Optional nearest resample toward 22050 if wildly different — keep native;
     * engine pitch_step accounts for buf.rate. */
    out->data = mono;
    out->frames = frames;
    out->rate = rate;
    (void)i;
    return 0;
}
