/*
 * ArgonOS Mega Drive — mix PSG+FM to /dev/pcm* or a WAV file.
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "md_sound.h"

#include <argon/argon.h>

#include "audio_out.h"

#define WAV_HDR 44u
#ifdef ARGON_MD_SMALL
/*
 * The CYD: one channel - its DAC is one 8-bit pin, and the driver averaged the
 * two anyway - mixed a line at a time straight into a chunk that goes to the
 * device when it is full.  A few hundred bytes where the stereo frame below
 * takes 4.4 KB, on a board whose last 4 KB is all the YM2612 has.  A chunk is
 * 128 samples, 6 ms: three writes a frame, each a trip through the registry.
 */
#define MD_SOUND_CH    1u
#define MD_SOUND_CHUNK 128
#define MD_SOUND_PIECE 8 /* samples a line makes, at most, mixed in one go */
/*
 * And the chips at half the rate, each sample given twice.  The YM2612 from
 * the hot part still took 5.2 ms of a 16.7 ms frame at 22 050 (Sonic, 30 Oct
 * 2026) - the game at 96-115% of real time, short in its busy scenes - and
 * what comes out is an 8-bit converter behind a small speaker.  `hifi` puts
 * the whole rate back.
 */
static unsigned s_div = 2u;
#else
#define MD_SOUND_CH 2u
#define MD_SOUND_MAX_FRAME 512 /* 22050/50 + slack */
#endif

enum {
    MD_SINK_DEV = 0,
    MD_SINK_WAV = 1,
};

static char        s_path[AG_PATH_MAX];
static int         s_sink = MD_SINK_DEV;
static int         s_active;
static int         s_write_err_reported;
static ag_handle_t s_fd = -1;
static uint32_t    s_data_bytes;
static int         s_lines;
static int         s_frame_samples;
static int         s_emitted;

#ifdef ARGON_MD_SMALL
static int16_t s_chunk[MD_SOUND_CHUNK];
static int16_t s_ym[MD_SOUND_PIECE];
static int16_t s_psg[MD_SOUND_PIECE];
static int     s_fill;
#else
static int16_t s_left[MD_SOUND_MAX_FRAME];
static int16_t s_right[MD_SOUND_MAX_FRAME];
static int16_t s_psg_l[MD_SOUND_MAX_FRAME];
static int16_t s_psg_r[MD_SOUND_MAX_FRAME];
static int16_t s_stereo[MD_SOUND_MAX_FRAME * 2];
#endif

static void mem_copy(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) {
        *d++ = *s++;
    }
}

static void mem_zero(void *p, size_t n)
{
    unsigned char *d = (unsigned char *)p;
    while (n--) {
        *d++ = 0;
    }
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

static void fill_wav_header(uint8_t *h, uint32_t data_bytes)
{
    mem_copy(h, "RIFF", 4);
    put_u32(h + 4, 36u + data_bytes);
    mem_copy(h + 8, "WAVE", 4);
    mem_copy(h + 12, "fmt ", 4);
    put_u32(h + 16, 16u);
    put_u16(h + 20, 1u);
    put_u16(h + 22, (uint16_t)MD_SOUND_CH);
    put_u32(h + 24, MD_SOUND_RATE);
    put_u32(h + 28, MD_SOUND_RATE * 2u * MD_SOUND_CH);
    put_u16(h + 32, (uint16_t)(2u * MD_SOUND_CH));
    put_u16(h + 34, 16u);
    mem_copy(h + 36, "data", 4);
    put_u32(h + 40, data_bytes);
}

static void write_wav_header_file(uint32_t data_bytes)
{
    uint8_t h[WAV_HDR];

    fill_wav_header(h, data_bytes);
    if (s_fd < 0 || s_sink != MD_SINK_WAV) {
        return;
    }
    (void)ag_seek(s_fd, 0, AG_SEEK_SET);
    (void)ag_write(s_fd, h, WAV_HDR);
    (void)ag_seek(s_fd, (int64_t)(WAV_HDR + data_bytes), AG_SEEK_SET);
}

uint32_t md_sound_synth_rate(void)
{
#ifdef ARGON_MD_SMALL
    return MD_SOUND_RATE / s_div;
#else
    return MD_SOUND_RATE;
#endif
}

void md_sound_set_hifi(int on)
{
#ifdef ARGON_MD_SMALL
    s_div = on ? 1u : 2u;
#else
    (void)on;
#endif
}

void md_sound_set_net_port(uint16_t port)
{
    (void)port; /* virt port is owned by PCMVIRT.SYS */
}

void md_sound_set_path(const char *path)
{
    if (ag_audio_out_resolve(path, s_path, sizeof(s_path))) {
        s_sink = MD_SINK_DEV;
    } else {
        s_sink = MD_SINK_WAV;
    }
}

void md_sound_init(void)
{
    s_active = 0;
    s_data_bytes = 0;
    s_write_err_reported = 0;
    if (s_fd >= 0) {
        if (s_sink == MD_SINK_DEV) {
            (void)ag_dev_close(s_fd);
        } else {
            ag_close(s_fd);
        }
        s_fd = -1;
    }
    if (s_path[0] == '\0') {
        md_sound_set_path("mock");
    }

    if (s_sink == MD_SINK_DEV) {
        s_fd = ag_audio_out_open_dev(s_path, MD_SOUND_RATE, MD_SOUND_CH);
        if (s_fd < 0) {
            ag_printf("md: %s: %s\n", s_path, ag_strerror((ag_err_t)s_fd));
            md_sound_set_path("mock");
            s_fd = ag_audio_out_open_dev(s_path, MD_SOUND_RATE, MD_SOUND_CH);
            if (s_fd < 0) {
                return;
            }
        }
        s_active = 1;
        ag_printf("md: sound = %s @ %u Hz, chips at %u\n", s_path,
                  (unsigned)MD_SOUND_RATE, (unsigned)md_sound_synth_rate());
        return;
    }

    s_fd = ag_open(s_path, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (s_fd < 0) {
        ag_printf("md: %s: %s\n", s_path, ag_strerror((ag_err_t)s_fd));
        return;
    }
    write_wav_header_file(0);
    s_active = 1;
    ag_printf("md: sound = wav %s @ %u Hz\n", s_path, (unsigned)MD_SOUND_RATE);
}

#ifdef ARGON_MD_SMALL
static void flush_chunk(void)
{
    if (s_fill <= 0 || s_fd < 0) {
        s_fill = 0;
        return;
    }
    const size_t  bytes = (size_t)s_fill * sizeof(int16_t);
    const int32_t n = (s_sink == MD_SINK_DEV) ? ag_dev_write(s_fd, s_chunk, bytes)
                                              : ag_write(s_fd, s_chunk, bytes);
    if (n > 0) {
        s_data_bytes += (uint32_t)n;
    } else if (n < 0 && !s_write_err_reported) {
        s_write_err_reported = 1;
        ag_printf("md: %s: %s\n", s_path, ag_strerror((ag_err_t)n));
    }
    s_fill = 0;
}
#endif

void md_sound_close(void)
{
#ifdef ARGON_MD_SMALL
    flush_chunk();
#endif
    if (s_fd >= 0) {
        if (s_sink == MD_SINK_WAV) {
            write_wav_header_file(s_data_bytes);
            ag_close(s_fd);
            ag_printf("md: wav closed, %u bytes PCM\n", (unsigned)s_data_bytes);
        } else {
            (void)ag_dev_close(s_fd);
        }
        s_fd = -1;
    }
    s_active = 0;
}

void md_sound_begin_frame(int lines_per_frame)
{
    s_lines = lines_per_frame > 0 ? lines_per_frame : 262;
    s_frame_samples = (int)(md_sound_synth_rate() / (s_lines >= 300 ? 50u : 60u));
#ifndef ARGON_MD_SMALL
    if (s_frame_samples > MD_SOUND_MAX_FRAME) {
        s_frame_samples = MD_SOUND_MAX_FRAME;
    }
#endif
    s_emitted = 0;
    md_ym_frame_begin(s_lines);
}

void md_sound_line(int line, int lines_per_frame)
{
    int want;
    int n;
    int i;

    if (!s_active || lines_per_frame <= 0) {
        return;
    }
    want = (int)(((int64_t)(line + 1) * s_frame_samples) / lines_per_frame);
    n = want - s_emitted;
    if (n <= 0) {
        return;
    }
#ifdef ARGON_MD_SMALL
    s_emitted = want;
    while (n > 0) {
        int k = (n > MD_SOUND_PIECE) ? MD_SOUND_PIECE : n;
        if (s_fill + k * (int)s_div > MD_SOUND_CHUNK) {
            flush_chunk();
        }
        /* one buffer for both sides: the cores write the same sample twice */
        md_ym_render(s_ym, s_ym, k);
        md_psg_render(s_psg, s_psg, k);
        for (i = 0; i < k; i++) {
            int32_t v = (int32_t)s_ym[i] + (int32_t)s_psg[i];
            if (v > 32767) {
                v = 32767;
            }
            if (v < -32768) {
                v = -32768;
            }
            for (unsigned d = 0; d < s_div; d++) {
                s_chunk[s_fill++] = (int16_t)v;
            }
        }
        n -= k;
    }
    return;
#else
    if (s_emitted + n > MD_SOUND_MAX_FRAME) {
        n = MD_SOUND_MAX_FRAME - s_emitted;
    }
    if (n <= 0) {
        return;
    }

    mem_zero(s_left, (size_t)n * sizeof(int16_t));
    mem_zero(s_right, (size_t)n * sizeof(int16_t));
    mem_zero(s_psg_l, (size_t)n * sizeof(int16_t));
    mem_zero(s_psg_r, (size_t)n * sizeof(int16_t));

    md_ym_render(s_left, s_right, n);
    md_psg_render(s_psg_l, s_psg_r, n);

    for (i = 0; i < n; i++) {
        int32_t l = (int32_t)s_left[i] + (int32_t)s_psg_l[i];
        int32_t r = (int32_t)s_right[i] + (int32_t)s_psg_r[i];
        if (l > 32767) {
            l = 32767;
        }
        if (l < -32768) {
            l = -32768;
        }
        if (r > 32767) {
            r = 32767;
        }
        if (r < -32768) {
            r = -32768;
        }
        if (s_sink == MD_SINK_DEV) {
            s_stereo[(s_emitted + i) * 2] = (int16_t)l;
            s_stereo[(s_emitted + i) * 2 + 1] = (int16_t)r;
        } else {
            s_stereo[i * 2] = (int16_t)l;
            s_stereo[i * 2 + 1] = (int16_t)r;
        }
    }

    if (s_sink == MD_SINK_WAV && s_fd >= 0) {
        const size_t  bytes = (size_t)n * 4u;
        const int32_t wr = ag_write(s_fd, s_stereo, bytes);
        if (wr > 0) {
            s_data_bytes += (uint32_t)wr;
        } else if (wr < 0 && !s_write_err_reported) {
            s_write_err_reported = 1;
            ag_printf("md: %s: %s\n", s_path, ag_strerror((ag_err_t)wr));
        }
    }
    s_emitted = want;
#endif
}

void md_sound_end_frame(void)
{
    if (s_active && s_lines > 0 && s_emitted < s_frame_samples) {
        md_sound_line(s_lines - 1, s_lines);
    }
    /* the CYD's chunks go out as they fill, across frames */
#ifndef ARGON_MD_SMALL
    if (s_sink == MD_SINK_DEV && s_fd >= 0 && s_emitted > 0) {
        const size_t  bytes = (size_t)s_emitted * 4u;
        const int32_t n = ag_dev_write(s_fd, s_stereo, bytes);
        if (n > 0) {
            s_data_bytes += (uint32_t)n;
        } else if (n < 0 && !s_write_err_reported) {
            s_write_err_reported = 1;
            ag_printf("md: %s: %s\n", s_path, ag_strerror((ag_err_t)n));
        }
    }
#endif
}
