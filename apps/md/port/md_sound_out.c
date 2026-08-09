/*
 * ArgonOS Mega Drive — mix PSG+FM and write WAV, TCP stream, or discard.
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "md_sound.h"

#include <argon/argon.h>
#include <argon/keys.h>

#define WAV_HDR 44u
#define MD_SOUND_MAX_FRAME 512 /* 22050/50 + slack */

enum {
    MD_SINK_MOCK = 0,
    MD_SINK_WAV = 1,
    MD_SINK_NET = 2,
};

static char        s_path[AG_PATH_MAX];
static int         s_sink = MD_SINK_MOCK;
static int         s_active;
static int         s_write_err_reported;
static ag_handle_t s_fd = -1;      /* WAV file */
static ag_handle_t s_listen = -1;  /* TCP listen */
static ag_handle_t s_conn = -1;    /* TCP stream */
static uint16_t    s_net_port = MD_SOUND_NET_PORT_DEFAULT;
static uint32_t    s_data_bytes;
static int         s_lines;
static int         s_frame_samples;
static int         s_emitted;

static int16_t s_left[MD_SOUND_MAX_FRAME];
static int16_t s_right[MD_SOUND_MAX_FRAME];
static int16_t s_psg_l[MD_SOUND_MAX_FRAME];
static int16_t s_psg_r[MD_SOUND_MAX_FRAME];
static int16_t s_stereo[MD_SOUND_MAX_FRAME * 2]; /* per-line (WAV) or frame (net) */

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

static int str_ieq(const char *s, const char *lit)
{
    if (s == NULL || lit == NULL) {
        return 0;
    }
    for (; *lit && *s; lit++, s++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (*lit != c) {
            return 0;
        }
    }
    return *lit == '\0' && *s == '\0';
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
    put_u16(h + 22, 2u);
    put_u32(h + 24, MD_SOUND_RATE);
    put_u32(h + 28, MD_SOUND_RATE * 4u);
    put_u16(h + 32, 4u);
    put_u16(h + 34, 16u);
    mem_copy(h + 36, "data", 4);
    put_u32(h + 40, data_bytes);
}

static void write_wav_header_file(uint32_t data_bytes)
{
    uint8_t h[WAV_HDR];

    fill_wav_header(h, data_bytes);
    if (s_fd < 0) {
        return;
    }
    (void)ag_seek(s_fd, 0, AG_SEEK_SET);
    (void)ag_write(s_fd, h, WAV_HDR);
    (void)ag_seek(s_fd, (int64_t)(WAV_HDR + data_bytes), AG_SEEK_SET);
}

/* Blocking send (header only). */
static int send_all(ag_handle_t sock, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t         left = len;

    while (left > 0) {
        const int32_t n = ag_net_send(sock, p, left);
        if (n < 0) {
            return (int)n;
        }
        if (n == 0) {
            return -AG_EIO;
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
    return 0;
}

/*
 * Non-blocking best-effort: never stall the emulator on a full TCP window.
 * Returns bytes accepted (>= 0), or negative AG_E* on hard failure.
 */
static int32_t send_nb(ag_handle_t sock, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t         left = len;
    size_t         sent = 0;

    while (left > 0) {
        const int32_t n = ag_net_send(sock, p, left);
        if (n == -AG_EAGAIN) {
            break;
        }
        if (n < 0) {
            return n;
        }
        if (n == 0) {
            break;
        }
        p += (size_t)n;
        left -= (size_t)n;
        sent += (size_t)n;
    }
    return (int32_t)sent;
}

void md_sound_set_net_port(uint16_t port)
{
    if (port != 0) {
        s_net_port = port;
    }
}

void md_sound_set_path(const char *path)
{
    s_path[0] = '\0';
    s_sink = MD_SINK_MOCK;
    if (path == NULL || path[0] == '\0' || str_ieq(path, "mock")) {
        return;
    }
    if (str_ieq(path, "net") || str_ieq(path, "tcp")) {
        s_sink = MD_SINK_NET;
        return;
    }
    /* net:PORT */
    if ((path[0] == 'n' || path[0] == 'N') &&
        (path[1] == 'e' || path[1] == 'E') &&
        (path[2] == 't' || path[2] == 'T') && path[3] == ':') {
        uint32_t port = 0;
        const char *p = path + 4;
        while (*p >= '0' && *p <= '9') {
            port = port * 10u + (uint32_t)(*p - '0');
            p++;
        }
        if (port > 0 && port <= 65535u && *p == '\0') {
            s_net_port = (uint16_t)port;
            s_sink = MD_SINK_NET;
            return;
        }
    }
    size_t n = 0;
    while (path[n] && n + 1u < sizeof(s_path)) {
        s_path[n] = path[n];
        n++;
    }
    s_path[n] = '\0';
    s_sink = MD_SINK_WAV;
}

static int net_listen_and_accept(void)
{
    uint32_t ip = 0;
    uint8_t  hdr[WAV_HDR];

    if (ag_net_wait_ready(10000u) != AG_OK) {
        ag_printf("md: net: DHCP timeout\n");
        return -1;
    }
    (void)ag_net_ifaddr(&ip);
    ag_printf("md: net: ip %u.%u.%u.%u, listen :%u (hostfwd)\n",
              (unsigned)((ip >> 24) & 0xffu), (unsigned)((ip >> 16) & 0xffu),
              (unsigned)((ip >> 8) & 0xffu), (unsigned)(ip & 0xffu),
              (unsigned)s_net_port);

    s_listen = ag_tcp_listen(s_net_port);
    if (s_listen < 0) {
        ag_printf("md: net: listen: %s\n", ag_strerror((ag_err_t)s_listen));
        return -1;
    }

    ag_printf("md: net: waiting for host (python tools/pcmplay.py)...\n");
    for (;;) {
        s_conn = ag_tcp_accept(s_listen, 1000u);
        if (s_conn >= 0) {
            break;
        }
        if ((ag_err_t)s_conn != -AG_EAGAIN &&
            (ag_err_t)s_conn != -AG_ETIMEDOUT) {
            ag_printf("md: net: accept: %s\n",
                      ag_strerror((ag_err_t)s_conn));
            (void)ag_net_close(s_listen);
            s_listen = -1;
            return -1;
        }
        if (ag_key(AG_KEY_ESC) || ag_key(AG_KEY_Q)) {
            ag_printf("md: net: cancelled\n");
            (void)ag_net_close(s_listen);
            s_listen = -1;
            return -1;
        }
        ag_heartbeat();
    }

    /* Streaming WAV: huge data size so players keep reading. */
    fill_wav_header(hdr, 0x7fffffffu);
    if (send_all(s_conn, hdr, WAV_HDR) < 0) {
        ag_printf("md: net: header send failed\n");
        (void)ag_net_close(s_conn);
        s_conn = -1;
        (void)ag_net_close(s_listen);
        s_listen = -1;
        return -1;
    }
    /* After the header, never block the frame loop on TCP. */
    if (ag_net_set_nonblock(s_conn, true) != AG_OK) {
        ag_printf("md: net: set_nonblock failed (will risk stalls)\n");
    }
    ag_printf("md: sound = net :%u @ %u Hz (connected, non-block)\n",
              (unsigned)s_net_port, (unsigned)MD_SOUND_RATE);
    return 0;
}

void md_sound_init(void)
{
    s_active = 0;
    s_data_bytes = 0;
    s_write_err_reported = 0;
    if (s_fd >= 0) {
        ag_close(s_fd);
        s_fd = -1;
    }
    if (s_conn >= 0) {
        (void)ag_net_close(s_conn);
        s_conn = -1;
    }
    if (s_listen >= 0) {
        (void)ag_net_close(s_listen);
        s_listen = -1;
    }

    if (s_sink == MD_SINK_MOCK) {
        s_active = 1;
        ag_printf("md: sound = mock (Z80+PSG+FM, discard)\n");
        return;
    }

    if (s_sink == MD_SINK_NET) {
        if (g_ag_api->net == NULL) {
            ag_printf("md: net: api->net is NULL (rebuild with ARGON_ENABLE_NET)\n");
            s_sink = MD_SINK_MOCK;
            s_active = 1;
            return;
        }
        if (net_listen_and_accept() != 0) {
            s_sink = MD_SINK_MOCK;
            s_active = 1;
            ag_printf("md: sound = mock (net failed)\n");
            return;
        }
        s_active = 1;
        return;
    }

    if (s_path[0] == '\0') {
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

void md_sound_close(void)
{
    if (s_fd >= 0) {
        write_wav_header_file(s_data_bytes);
        ag_close(s_fd);
        s_fd = -1;
        ag_printf("md: wav closed, %u bytes PCM\n", (unsigned)s_data_bytes);
    }
    if (s_conn >= 0) {
        (void)ag_net_close(s_conn);
        s_conn = -1;
        ag_printf("md: net stream closed, %u bytes PCM\n",
                  (unsigned)s_data_bytes);
    }
    if (s_listen >= 0) {
        (void)ag_net_close(s_listen);
        s_listen = -1;
    }
    s_active = 0;
}

void md_sound_begin_frame(int lines_per_frame)
{
    s_lines = lines_per_frame > 0 ? lines_per_frame : 262;
    /* NTSC 262 lines → 60 Hz; PAL 313 → 50 Hz. */
    s_frame_samples = (int)(MD_SOUND_RATE / (s_lines >= 300 ? 50u : 60u));
    if (s_frame_samples > MD_SOUND_MAX_FRAME) {
        s_frame_samples = MD_SOUND_MAX_FRAME;
    }
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
        if (s_sink == MD_SINK_NET) {
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
}

void md_sound_end_frame(void)
{
    if (s_active && s_lines > 0 && s_emitted < s_frame_samples) {
        md_sound_line(s_lines - 1, s_lines);
    }
    /* One TCP write per frame — never block the 68000/VDP loop. */
    if (s_sink == MD_SINK_NET && s_conn >= 0 && s_emitted > 0) {
        const size_t  bytes = (size_t)s_emitted * 4u;
        const int32_t n = send_nb(s_conn, s_stereo, bytes);
        if (n > 0) {
            s_data_bytes += (uint32_t)n;
        } else if (n < 0 && n != -AG_EAGAIN && !s_write_err_reported) {
            s_write_err_reported = 1;
            ag_printf("md: net send: %s\n", ag_strerror((ag_err_t)n));
        }
    }
}
