/*
 * ag_pcm — PCM device / WAV file sink with wall-clock pacing.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_pcm.h"

#include "audio_out.h"

#define WAV_HDR 44u

static int ends_wav(const char *s)
{
    int n = 0;
    if (s == 0) {
        return 0;
    }
    while (s[n]) {
        n++;
    }
    if (n < 4) {
        return 0;
    }
    return (s[n - 4] == '.' &&
            (s[n - 3] == 'w' || s[n - 3] == 'W') &&
            (s[n - 2] == 'a' || s[n - 2] == 'A') &&
            (s[n - 1] == 'v' || s[n - 1] == 'V'));
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void wav_write_header(ag_pcm_t *o, uint32_t data_bytes)
{
    uint8_t  h[WAV_HDR];
    uint32_t br;
    uint16_t align;
    unsigned i;
    const char *riff = "RIFF";
    const char *wave = "WAVE";
    const char *fmt = "fmt ";
    const char *data = "data";

    if (o == 0 || o->fd < 0 || o->sink != AG_PCM_SINK_WAV) {
        return;
    }
    br = o->rate * (uint32_t)o->channels * 2u;
    align = (uint16_t)(o->channels * 2u);
    for (i = 0; i < 4; i++) {
        h[i] = (uint8_t)riff[i];
        h[8 + i] = (uint8_t)wave[i];
        h[12 + i] = (uint8_t)fmt[i];
        h[36 + i] = (uint8_t)data[i];
    }
    put_u32(h + 4, 36u + data_bytes);
    put_u32(h + 16, 16u);
    put_u16(h + 20, 1u);
    put_u16(h + 22, o->channels);
    put_u32(h + 24, o->rate);
    put_u32(h + 28, br);
    put_u16(h + 32, align);
    put_u16(h + 34, 16u);
    put_u32(h + 40, data_bytes);
    (void)ag_seek(o->fd, 0, AG_SEEK_SET);
    (void)ag_write(o->fd, h, WAV_HDR);
    (void)ag_seek(o->fd, (int64_t)(WAV_HDR + data_bytes), AG_SEEK_SET);
}

static void pcm_clear(ag_pcm_t *o)
{
    unsigned i;
    if (o == 0) {
        return;
    }
    o->fd = -1;
    o->path[0] = '\0';
    o->rate = 22050u;
    o->chunk = AG_PCM_CHUNK_DEFAULT;
    o->chunk_us = 0;
    o->channels = 2;
    o->sink = AG_PCM_SINK_DEV;
    o->next_due = 0;
    o->render_us = 0;
    o->send_us = 0;
    o->load_pct = 0;
    o->late = 0;
    o->drop = 0;
    o->resync = 0;
    o->wav_bytes = 0;
    o->stats_ok = 0;
    for (i = 0; i < sizeof(o->stats); i++) {
        ((uint8_t *)&o->stats)[i] = 0;
    }
}

static void set_chunk_us(ag_pcm_t *o)
{
    if (o->rate < 1u) {
        o->rate = 22050u;
    }
    if (o->chunk < 1u) {
        o->chunk = AG_PCM_CHUNK_DEFAULT;
    }
    o->chunk_us = (uint32_t)(((uint64_t)o->chunk * 1000000ull) / o->rate);
}

int ag_pcm_open(ag_pcm_t *o, const char *arg, uint32_t rate, uint8_t ch)
{
    int is_dev;

    if (o == 0 || rate < 8000u || (ch != 1u && ch != 2u)) {
        return -1;
    }
    pcm_clear(o);
    o->rate = rate;
    o->channels = ch;
    set_chunk_us(o);

    if (arg != 0 && ends_wav(arg)) {
        size_t n = 0;
        o->sink = AG_PCM_SINK_WAV;
        while (arg[n] && n + 1u < sizeof(o->path)) {
            o->path[n] = arg[n];
            n++;
        }
        o->path[n] = '\0';
        o->fd = ag_open(o->path, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
        if (o->fd < 0) {
            return -1;
        }
        wav_write_header(o, 0);
        return 0;
    }

    is_dev = ag_audio_out_resolve(arg, o->path, sizeof(o->path));
    if (!is_dev) {
        /* Unresolved non-wav path: try as device name anyway. */
        if (arg != 0 && arg[0] != '\0') {
            ag_audio_out_copy(o->path, sizeof(o->path), arg);
        } else {
            ag_audio_out_copy(o->path, sizeof(o->path), "/dev/pcmnull");
        }
    }
    o->sink = AG_PCM_SINK_DEV;
    o->fd = ag_audio_out_open_dev(o->path, rate, ch);
    if (o->fd < 0) {
        (void)ag_audio_out_resolve("pcmnull", o->path, sizeof(o->path));
        o->fd = ag_audio_out_open_dev(o->path, rate, ch);
        if (o->fd < 0) {
            return -1;
        }
    }
    return 0;
}

void ag_pcm_close(ag_pcm_t *o)
{
    if (o == 0) {
        return;
    }
    if (o->fd >= 0) {
        if (o->sink == AG_PCM_SINK_WAV) {
            wav_write_header(o, o->wav_bytes);
            (void)ag_close(o->fd);
        } else {
            (void)ag_dev_close(o->fd);
        }
        o->fd = -1;
    }
}

void ag_pcm_set_chunk(ag_pcm_t *o, uint32_t frames)
{
    if (o == 0) {
        return;
    }
    o->chunk = frames ? frames : AG_PCM_CHUNK_DEFAULT;
    set_chunk_us(o);
}

int32_t ag_pcm_write(ag_pcm_t *o, const int16_t *pcm, int32_t frames)
{
    size_t    bytes;
    int32_t   wr;
    ag_time_t t0;

    if (o == 0 || pcm == 0 || frames <= 0) {
        return 0;
    }
    bytes = (size_t)frames * (size_t)o->channels * 2u;
    if (o->fd < 0) {
        o->send_us = 0;
        return 0;
    }
    t0 = ag_micros();
    if (o->sink == AG_PCM_SINK_WAV) {
        wr = ag_write(o->fd, pcm, (int32_t)bytes);
        if (wr > 0) {
            o->wav_bytes += (uint32_t)wr;
        }
    } else {
        wr = ag_dev_write(o->fd, pcm, (int32_t)bytes);
    }
    o->send_us = (uint32_t)(ag_micros() - t0);
    if (wr < 0) {
        o->drop += (uint32_t)bytes;
        return wr;
    }
    if ((size_t)wr < bytes) {
        o->drop += (uint32_t)(bytes - (size_t)wr);
    }
    return wr;
}

void ag_pcm_mark_render(ag_pcm_t *o, uint32_t us)
{
    if (o == 0) {
        return;
    }
    o->render_us = us;
    if (o->chunk_us > 0u) {
        o->load_pct = (us * 100u + o->chunk_us / 2u) / o->chunk_us;
        if (o->load_pct == 0u && us > 0u) {
            o->load_pct = 1u;
        }
    } else {
        o->load_pct = 0;
    }
}

void ag_pcm_pace_start(ag_pcm_t *o)
{
    if (o == 0) {
        return;
    }
    set_chunk_us(o);
    o->next_due = ag_micros() + (ag_time_t)o->chunk_us;
}

void ag_pcm_pace_wait(ag_pcm_t *o)
{
    ag_time_t now;
    if (o == 0) {
        return;
    }
    /* File capture: write as fast as the renderer can; no wall-clock sleep. */
    if (o->sink == AG_PCM_SINK_WAV) {
        return;
    }
    now = ag_micros();
    if (now <= o->next_due) {
        for (;;) {
            uint32_t rem;
            now = ag_micros();
            if (now >= o->next_due) {
                break;
            }
            rem = (uint32_t)(o->next_due - now);
            if (rem > 2000u) {
                ag_delay((rem / 1000u) - 1u);
            } else if (rem > 50u) {
                ag_delay_us(rem);
            } else {
                ag_heartbeat();
            }
        }
        o->next_due += (ag_time_t)o->chunk_us;
    } else {
        o->late++;
        if (now > o->next_due + (ag_time_t)o->chunk_us) {
            o->resync++;
            o->next_due = now + (ag_time_t)o->chunk_us;
        } else {
            o->next_due += (ag_time_t)o->chunk_us;
        }
    }
}

int32_t ag_pcm_slack_us(const ag_pcm_t *o)
{
    ag_time_t now;
    if (o == 0) {
        return 0;
    }
    now = ag_micros();
    if (now >= o->next_due) {
        return 0;
    }
    return (int32_t)(o->next_due - now);
}

void ag_pcm_poll_stats(ag_pcm_t *o)
{
    if (o == 0 || o->fd < 0 || o->sink != AG_PCM_SINK_DEV) {
        if (o != 0) {
            o->stats_ok = 0;
        }
        return;
    }
    o->stats_ok = (ag_dev_ioctl(o->fd, AG_IOC_AUDIO_GETSTATS, &o->stats,
                                sizeof(o->stats)) == AG_OK)
                      ? 1
                      : 0;
}
