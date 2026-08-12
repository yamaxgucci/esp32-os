/*
 * In-memory MP3 decode via minimp3 + ArgonOS VFS.
 * Small files are slurped entirely so playback never touches HostFS.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_mp3.h"

#include <string.h>

#include <argon/argon.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "minimp3.h"

#define AG_MP3_PCM_MAX     MINIMP3_MAX_SAMPLES_PER_FRAME
#define AG_MP3_LOAD_CHUNK  2048
#define AG_MP3_MAX_LOAD    (512u * 1024u) /* leave heap for PCM/UI */

struct ag_mp3 {
    uint8_t    *file;
    size_t      file_len;
    size_t      pos; /* decode cursor */
    size_t      data_off;
    mp3dec_t    dec;
    int16_t     pcm[AG_MP3_PCM_MAX];
    int         pcm_frames;
    int         pcm_off;
    uint32_t    rate;
    uint8_t     channels;
    uint32_t    bitrate;
    uint32_t    frames_out;
    char        title[64];
};

static void set_title_basename(char *title, size_t title_len, const char *path)
{
    const char *base;
    size_t i, n;
    if (title == NULL || title_len == 0) {
        return;
    }
    title[0] = '\0';
    if (path == NULL) {
        return;
    }
    base = path;
    for (i = 0; path[i]; i++) {
        if (path[i] == '/' || path[i] == '\\' || path[i] == ':') {
            base = path + i + 1;
        }
    }
    n = 0;
    while (base[n] && n + 1 < title_len) {
        title[n] = base[n];
        n++;
    }
    title[n] = '\0';
}

static size_t skip_id3v2_mem(const uint8_t *buf, size_t len)
{
    uint32_t size;
    if (len < 10u) {
        return 0;
    }
    if (buf[0] != 'I' || buf[1] != 'D' || buf[2] != '3') {
        return 0;
    }
    size = ((uint32_t)(buf[6] & 0x7f) << 21) | ((uint32_t)(buf[7] & 0x7f) << 14) |
           ((uint32_t)(buf[8] & 0x7f) << 7) | (uint32_t)(buf[9] & 0x7f);
    if ((size_t)(10u + size) > len) {
        return 0;
    }
    return 10u + (size_t)size;
}

static int load_entire(ag_handle_t fd, uint8_t **out, size_t *out_len)
{
    size_t   cap = 64u * 1024u;
    size_t   len = 0;
    uint8_t *buf;

    if (cap > AG_MP3_MAX_LOAD) {
        cap = AG_MP3_MAX_LOAD;
    }
    buf = (uint8_t *)ag_malloc(cap);
    if (buf == NULL) {
        return -1;
    }
    for (;;) {
        size_t  want;
        int32_t n;
        if (len >= AG_MP3_MAX_LOAD) {
            /* More data remains — refuse (keeps heap bounded). */
            ag_free(buf);
            return -1;
        }
        if (len == cap) {
            size_t   ncap = cap * 2u;
            uint8_t *nbuf;
            if (ncap > AG_MP3_MAX_LOAD) {
                ncap = AG_MP3_MAX_LOAD;
            }
            if (ncap <= cap) {
                ag_free(buf);
                return -1;
            }
            nbuf = (uint8_t *)ag_realloc(buf, ncap);
            if (nbuf == NULL) {
                ag_free(buf);
                return -1;
            }
            buf = nbuf;
            cap = ncap;
        }
        want = cap - len;
        if (want > AG_MP3_LOAD_CHUNK) {
            want = AG_MP3_LOAD_CHUNK;
        }
        ag_heartbeat();
        n = ag_read(fd, buf + len, want);
        if (n < 0) {
            ag_free(buf);
            return -1;
        }
        if (n == 0) {
            break;
        }
        len += (size_t)n;
    }
    if (len == 0) {
        ag_free(buf);
        return -1;
    }
    *out = buf;
    *out_len = len;
    return 0;
}

int ag_mp3_open(ag_mp3_t **out, const char *path)
{
    ag_mp3_t   *m;
    ag_handle_t fd;

    if (out == NULL || path == NULL) {
        return -1;
    }
    *out = NULL;
    m = (ag_mp3_t *)ag_malloc(sizeof(*m));
    if (m == NULL) {
        return -1;
    }
    memset(m, 0, sizeof(*m));
    set_title_basename(m->title, sizeof(m->title), path);

    fd = ag_open(path, AG_O_RDONLY);
    if (fd < 0) {
        ag_free(m);
        return -1;
    }
    /*
     * Slurp with small chunked reads (no SEEK_END — that wedges HostFS on
     * QEMU for some files). Playback then never touches the VFS again.
     */
    if (load_entire(fd, &m->file, &m->file_len) != 0) {
        (void)ag_close(fd);
        ag_free(m);
        return -1;
    }
    (void)ag_close(fd);

    m->data_off = skip_id3v2_mem(m->file, m->file_len);
    m->pos = m->data_off;
    mp3dec_init(&m->dec);
    ag_printf("ag_mp3: loaded %u bytes (data@%u)\n", (unsigned)m->file_len,
              (unsigned)m->data_off);
    *out = m;
    return 0;
}

void ag_mp3_close(ag_mp3_t *m)
{
    if (m == NULL) {
        return;
    }
    if (m->file) {
        ag_free(m->file);
        m->file = NULL;
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
    size_t bytes;
    if (m == NULL || m->bitrate == 0 || m->file_len <= m->data_off) {
        return 0;
    }
    bytes = m->file_len - m->data_off;
    return (uint32_t)(((uint64_t)bytes * 8000ull) / (uint64_t)m->bitrate);
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

static int decode_one(ag_mp3_t *m)
{
    mp3dec_frame_info_t info;
    int samples;
    int skipped = 0;
    int empty_frames = 0;
    int guard = 0;

    for (;;) {
        size_t avail;
        if (++guard > 256) {
            return m->pcm_frames > 0 ? 1 : -1;
        }
        if (m->pos >= m->file_len) {
            return 0;
        }
        avail = m->file_len - m->pos;
        samples = mp3dec_decode_frame(&m->dec, m->file + m->pos, (int)avail,
                                      m->pcm, &info);
        if (info.frame_bytes > 0) {
            m->pos += (size_t)info.frame_bytes;
        } else {
            /* Resync */
            size_t drop = 64;
            if (drop > avail) {
                drop = avail;
            }
            m->pos += drop;
            skipped += (int)drop;
            if (skipped > 8192) {
                return -1;
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
            return 1;
        }
        if (++empty_frames > 32) {
            return -1;
        }
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
    size_t span;
    if (m == NULL || m->file == NULL || m->file_len <= m->data_off) {
        return -1;
    }
    if (permille < 0) {
        permille = 0;
    }
    if (permille > 1000) {
        permille = 1000;
    }
    span = m->file_len - m->data_off;
    m->pos = m->data_off + (span * (size_t)permille) / 1000u;
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
