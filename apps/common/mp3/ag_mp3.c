/*
 * Streaming MP3 via minimp3 + ArgonOS VFS.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_mp3.h"

#include <string.h>

#include <argon/argon.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "minimp3.h"

#define AG_MP3_BUF     16384
#define AG_MP3_PCM_MAX MINIMP3_MAX_SAMPLES_PER_FRAME

struct ag_mp3 {
    ag_handle_t fd;
    mp3dec_t    dec;
    uint8_t     in[AG_MP3_BUF];
    int         in_len;
    int         eof;
    int16_t     pcm[AG_MP3_PCM_MAX];
    int         pcm_frames; /* remaining frames in pcm[] */
    int         pcm_off;    /* frame offset into pcm[] */
    uint32_t    rate;
    uint8_t     channels;
    int64_t     file_size;
    int64_t     data_off; /* first audio byte (after ID3) */
    uint32_t    bitrate;  /* last frame bitrate */
    uint32_t    frames_out;
    char        title[64];
};

static int skip_id3v2(ag_handle_t fd, char *title, size_t title_len)
{
    uint8_t hdr[10];
    int64_t pos;
    uint32_t size;

    if (title && title_len) {
        title[0] = '\0';
    }
    pos = ag_seek(fd, 0, AG_SEEK_SET);
    if (pos < 0) {
        return -1;
    }
    if (ag_read(fd, hdr, 10) != 10) {
        (void)ag_seek(fd, 0, AG_SEEK_SET);
        return 0;
    }
    if (hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3') {
        (void)ag_seek(fd, 0, AG_SEEK_SET);
        return 0;
    }
    size = ((uint32_t)(hdr[6] & 0x7f) << 21) | ((uint32_t)(hdr[7] & 0x7f) << 14) |
           ((uint32_t)(hdr[8] & 0x7f) << 7) | (uint32_t)(hdr[9] & 0x7f);
    /* Best-effort TIT2 scan in first 2 KiB of tag. */
    if (title && title_len > 1 && size > 0) {
        uint8_t scratch[2048];
        uint32_t n = size;
        if (n > sizeof(scratch)) {
            n = (uint32_t)sizeof(scratch);
        }
        if (ag_read(fd, scratch, n) == (int32_t)n) {
            uint32_t i;
            for (i = 0; i + 11 < n; i++) {
                if (scratch[i] == 'T' && scratch[i + 1] == 'I' &&
                    scratch[i + 2] == 'T' && scratch[i + 3] == '2') {
                    uint32_t tsz =
                        ((uint32_t)scratch[i + 4] << 24) |
                        ((uint32_t)scratch[i + 5] << 16) |
                        ((uint32_t)scratch[i + 6] << 8) | scratch[i + 7];
                    uint8_t enc = scratch[i + 10];
                    uint32_t src = i + 11;
                    uint32_t j = 0;
                    if (tsz > 1 && enc == 0) {
                        while (j + 1 < title_len && src < i + 10 + tsz &&
                               scratch[src] >= 32) {
                            title[j++] = (char)scratch[src++];
                        }
                        title[j] = '\0';
                    }
                    break;
                }
            }
        }
    }
    if (ag_seek(fd, (int64_t)(10 + size), AG_SEEK_SET) < 0) {
        return -1;
    }
    return 0;
}

static void try_id3v1(ag_handle_t fd, int64_t file_size, char *title,
                      size_t title_len)
{
    uint8_t tag[128];
    if (!title || title_len < 2 || file_size < 128) {
        return;
    }
    if (title[0] != '\0') {
        return;
    }
    if (ag_seek(fd, file_size - 128, AG_SEEK_SET) < 0) {
        return;
    }
    if (ag_read(fd, tag, 128) != 128) {
        return;
    }
    if (tag[0] == 'T' && tag[1] == 'A' && tag[2] == 'G') {
        size_t j = 0;
        size_t i;
        for (i = 3; i < 33 && j + 1 < title_len; i++) {
            char c = (char)tag[i];
            if (c == '\0') {
                break;
            }
            if (c >= 32) {
                title[j++] = c;
            }
        }
        while (j > 0 && title[j - 1] == ' ') {
            j--;
        }
        title[j] = '\0';
    }
}

int ag_mp3_open(ag_mp3_t **out, const char *path)
{
    ag_mp3_t *m;
    int64_t   sz;

    if (out == NULL || path == NULL) {
        return -1;
    }
    *out = NULL;
    m = (ag_mp3_t *)ag_malloc(sizeof(*m));
    if (m == NULL) {
        return -1;
    }
    memset(m, 0, sizeof(*m));
    m->fd = ag_open(path, AG_O_RDONLY);
    if (m->fd < 0) {
        ag_free(m);
        return -1;
    }
    sz = ag_seek(m->fd, 0, AG_SEEK_END);
    if (sz < 0) {
        (void)ag_close(m->fd);
        ag_free(m);
        return -1;
    }
    m->file_size = sz;
    try_id3v1(m->fd, sz, m->title, sizeof(m->title));
    if (skip_id3v2(m->fd, m->title, sizeof(m->title)) != 0) {
        (void)ag_close(m->fd);
        ag_free(m);
        return -1;
    }
    m->data_off = ag_seek(m->fd, 0, AG_SEEK_CUR);
    if (m->data_off < 0) {
        (void)ag_close(m->fd);
        ag_free(m);
        return -1;
    }
    mp3dec_init(&m->dec);
    *out = m;
    return 0;
}

void ag_mp3_close(ag_mp3_t *m)
{
    if (m == NULL) {
        return;
    }
    if (m->fd >= 0) {
        (void)ag_close(m->fd);
    }
    ag_free(m);
}

uint32_t ag_mp3_rate(const ag_mp3_t *m)
{
    return m ? m->rate : 0;
}

uint8_t ag_mp3_channels(const ag_mp3_t *m)
{
    return m ? m->channels : 0;
}

uint32_t ag_mp3_duration_ms(const ag_mp3_t *m)
{
    int64_t bytes;
    if (m == NULL || m->bitrate == 0 || m->file_size <= m->data_off) {
        return 0;
    }
    bytes = m->file_size - m->data_off;
    /* bitrate is bits/s; ms = bytes*8*1000 / bitrate */
    return (uint32_t)((bytes * 8000ll) / (int64_t)m->bitrate);
}

uint32_t ag_mp3_position_ms(const ag_mp3_t *m)
{
    if (m == NULL || m->rate == 0) {
        return 0;
    }
    return (uint32_t)((m->frames_out * 1000ull) / m->rate);
}

const char *ag_mp3_title(const ag_mp3_t *m)
{
    return m ? m->title : "";
}

static int refill(ag_mp3_t *m)
{
    int32_t n;
    if (m->eof) {
        return m->in_len;
    }
    if (m->in_len < AG_MP3_BUF) {
        n = ag_read(m->fd, m->in + m->in_len,
                    (size_t)(AG_MP3_BUF - m->in_len));
        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            m->eof = 1;
        } else {
            m->in_len += (int)n;
        }
    }
    return m->in_len;
}

static int decode_one(ag_mp3_t *m)
{
    mp3dec_frame_info_t info;
    int samples;
    int free_format_bytes = 0;

    for (;;) {
        if (refill(m) < 0) {
            return -1;
        }
        if (m->in_len <= 0) {
            return 0;
        }
        samples = mp3dec_decode_frame(&m->dec, m->in, m->in_len, m->pcm, &info);
        if (info.frame_bytes > 0) {
            memmove(m->in, m->in + info.frame_bytes,
                    (size_t)(m->in_len - info.frame_bytes));
            m->in_len -= info.frame_bytes;
        } else if (m->eof) {
            return 0;
        } else if (m->in_len == AG_MP3_BUF) {
            /* stuck — drop a byte */
            memmove(m->in, m->in + 1, (size_t)(m->in_len - 1));
            m->in_len--;
            free_format_bytes++;
            if (free_format_bytes > 4096) {
                return -1;
            }
            continue;
        } else {
            /* need more data */
            if (m->eof) {
                return 0;
            }
            continue;
        }
        if (samples > 0) {
            if (m->rate == 0) {
                m->rate = (uint32_t)info.hz;
                m->channels = (uint8_t)info.channels;
            }
            if (info.bitrate_kbps > 0) {
                m->bitrate = (uint32_t)info.bitrate_kbps * 1000u;
            }
            m->pcm_frames = samples;
            m->pcm_off = 0;
            /* minimp3 returns samples per channel */
            return 1;
        }
        /* frame consumed but no samples (skipped) — loop */
    }
}

int ag_mp3_read(ag_mp3_t *m, int16_t *out, int max_frames)
{
    int written = 0;
    if (m == NULL || out == NULL || max_frames <= 0) {
        return -1;
    }
    while (written < max_frames) {
        int ch;
        int left;
        int n;
        int i;
        if (m->pcm_frames <= 0) {
            int r = decode_one(m);
            if (r < 0) {
                return written > 0 ? written : -1;
            }
            if (r == 0) {
                break;
            }
        }
        ch = m->channels ? (int)m->channels : 2;
        left = m->pcm_frames - m->pcm_off;
        n = max_frames - written;
        if (n > left) {
            n = left;
        }
        if (ch == 1) {
            for (i = 0; i < n; i++) {
                int16_t s = m->pcm[m->pcm_off + i];
                out[(written + i) * 2] = s;
                out[(written + i) * 2 + 1] = s;
            }
        } else {
            memcpy(out + written * 2, m->pcm + m->pcm_off * 2,
                   (size_t)n * 2u * sizeof(int16_t));
        }
        m->pcm_off += n;
        written += n;
        m->frames_out += (uint32_t)n;
        if (m->pcm_off >= m->pcm_frames) {
            m->pcm_frames = 0;
            m->pcm_off = 0;
        }
    }
    return written;
}

int ag_mp3_seek_permille(ag_mp3_t *m, int permille)
{
    int64_t span;
    int64_t off;
    if (m == NULL || m->file_size <= m->data_off) {
        return -1;
    }
    if (permille < 0) {
        permille = 0;
    }
    if (permille > 1000) {
        permille = 1000;
    }
    span = m->file_size - m->data_off;
    off = m->data_off + (span * (int64_t)permille) / 1000;
    if (ag_seek(m->fd, off, AG_SEEK_SET) < 0) {
        return -1;
    }
    m->in_len = 0;
    m->eof = 0;
    m->pcm_frames = 0;
    m->pcm_off = 0;
    mp3dec_init(&m->dec);
    if (m->rate > 0) {
        uint32_t dur = ag_mp3_duration_ms(m);
        m->frames_out = (uint32_t)(((uint64_t)dur * m->rate) / 1000ull *
                                   (uint32_t)permille / 1000ull);
    } else {
        m->frames_out = 0;
    }
    return 0;
}
