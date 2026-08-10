/*
 * ArgonOS - virtual PCM output (.SYS): /dev/pcmvirt → TCP WAV stream.
 *
 * Listen socket stays up whenever the net stack is ready.  Host may connect
 * anytime (tools/pcmplay.py via QEMU hostfwd :5558).  write() never waits:
 * no client → samples dropped; client present → non-blocking send.
 *
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc \
 *       --include sdk/include -o build\apps\PCMVIRT.SYS apps/pcmvirt/pcmvirt.c
 *   drv install h:\pcmvirt.sys
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_DRV("PCMVIRT", "1.0", "argon");

#define PCMVIRT_PORT 5558u
#define WAV_HDR      44u

typedef struct {
    ag_audio_fmt_t fmt;
    int            fmt_set;
    ag_handle_t    listen;
    ag_handle_t    conn;
} pcmvirt_state_t;

static pcmvirt_state_t s_st;

static void default_fmt(ag_audio_fmt_t *out)
{
    out->rate = 22050u;
    out->channels = 2;
    out->bits = 16;
}

static int fmt_ok(const ag_audio_fmt_t *fmt)
{
    return fmt != NULL && fmt->rate >= 8000u && fmt->rate <= 48000u &&
           fmt->bits == 16u && (fmt->channels == 1u || fmt->channels == 2u);
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

static void fill_wav_header(uint8_t *h, const ag_audio_fmt_t *fmt)
{
    const uint16_t ch = fmt->channels;
    const uint32_t rate = fmt->rate;
    const uint16_t bps = 16u;
    const uint16_t block = (uint16_t)(ch * (bps / 8u));
    const uint32_t data_bytes = 0x7fffffffu;

    memcpy(h, "RIFF", 4);
    put_u32(h + 4, 36u + data_bytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put_u32(h + 16, 16u);
    put_u16(h + 20, 1u);
    put_u16(h + 22, ch);
    put_u32(h + 24, rate);
    put_u32(h + 28, rate * (uint32_t)block);
    put_u16(h + 32, block);
    put_u16(h + 34, bps);
    memcpy(h + 36, "data", 4);
    put_u32(h + 40, data_bytes);
}

static void close_conn(pcmvirt_state_t *st)
{
    if (st->conn >= 0) {
        (void)ag_net_close(st->conn);
        st->conn = -1;
    }
}

static void ensure_listen(pcmvirt_state_t *st)
{
    if (st->listen >= 0) {
        return;
    }
    if (!ag_net_is_ready()) {
        return; /* retry on later write/open — never block boot/load */
    }
    st->listen = ag_tcp_listen(PCMVIRT_PORT);
    if (st->listen < 0) {
        st->listen = -1;
        return;
    }
    /* Keep accept() pollable even if a caller passes a blocking timeout. */
    (void)ag_net_set_nonblock(st->listen, true);
}

static int send_all_nb_header(ag_handle_t sock, const uint8_t *buf, size_t len)
{
    size_t left = len;
    int    spins = 32;

    while (left > 0 && spins-- > 0) {
        const int32_t n = ag_net_send(sock, buf, left);
        if (n == -AG_EAGAIN) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        buf += (size_t)n;
        left -= (size_t)n;
    }
    return left == 0 ? 0 : -1;
}

/* Non-blocking: take at most one pending peer; listen stays open. */
static void try_accept(pcmvirt_state_t *st)
{
    ag_handle_t peer;
    uint8_t     hdr[WAV_HDR];

    ensure_listen(st);
    if (st->listen < 0) {
        return;
    }

    peer = ag_tcp_accept(st->listen, 0u);
    if (peer < 0) {
        return;
    }

    close_conn(st);
    st->conn = peer;
    (void)ag_net_set_nonblock(st->conn, true);
    if (!st->fmt_set) {
        default_fmt(&st->fmt);
        st->fmt_set = 1;
    }
    fill_wav_header(hdr, &st->fmt);
    if (send_all_nb_header(st->conn, hdr, WAV_HDR) != 0) {
        close_conn(st);
    }
}

static ag_err_t pcm_open(ag_device_t *dev, uint32_t flags)
{
    pcmvirt_state_t *st = (pcmvirt_state_t *)ag_dev_priv(dev);
    (void)flags;
    if (st == NULL) {
        return -AG_ENODEV;
    }
    if (!st->fmt_set) {
        default_fmt(&st->fmt);
        st->fmt_set = 1;
    }
    ensure_listen(st);
    try_accept(st); /* late host attach before first write */
    return AG_OK;
}

static ag_err_t pcm_close(ag_device_t *dev)
{
    (void)dev;
    /* Keep listen (+ optional conn) across open/close. */
    return AG_OK;
}

static ag_err_t pcm_ioctl(ag_device_t *dev, uint32_t cmd, void *arg,
                          size_t arglen)
{
    pcmvirt_state_t *st = (pcmvirt_state_t *)ag_dev_priv(dev);
    if (st == NULL) {
        return -AG_ENODEV;
    }
    if (cmd == AG_IOC_AUDIO_GETFMT) {
        if (arg == NULL || arglen < sizeof(ag_audio_fmt_t)) {
            return -AG_EINVAL;
        }
        if (!st->fmt_set) {
            default_fmt(&st->fmt);
            st->fmt_set = 1;
        }
        *(ag_audio_fmt_t *)arg = st->fmt;
        return AG_OK;
    }
    if (cmd == AG_IOC_AUDIO_SETFMT) {
        const ag_audio_fmt_t *fmt;
        if (arg == NULL || arglen < sizeof(ag_audio_fmt_t)) {
            return -AG_EINVAL;
        }
        fmt = (const ag_audio_fmt_t *)arg;
        if (!fmt_ok(fmt)) {
            return -AG_EINVAL;
        }
        st->fmt = *fmt;
        st->fmt_set = 1;
        /* New format → new WAV header on next peer. */
        close_conn(st);
        return AG_OK;
    }
    if (cmd == AG_IOC_FLUSH || cmd == AG_IOC_RESET) {
        return AG_OK;
    }
    return -AG_ENOTSUP;
}

static int32_t pcm_write(ag_device_t *dev, const void *buf, size_t len,
                         uint64_t off)
{
    pcmvirt_state_t *st = (pcmvirt_state_t *)ag_dev_priv(dev);
    size_t           frame_bytes;
    size_t           frames;
    size_t           bytes;
    const uint8_t   *p;
    size_t           left;

    (void)off;
    if (st == NULL) {
        return -AG_ENODEV;
    }
    if (buf == NULL || len < 2u) {
        return -AG_EINVAL;
    }
    if (!st->fmt_set) {
        default_fmt(&st->fmt);
        st->fmt_set = 1;
    }

    frame_bytes = (size_t)st->fmt.channels * sizeof(int16_t);
    frames = len / frame_bytes;
    if (frames == 0u) {
        return -AG_EINVAL;
    }
    bytes = frames * frame_bytes;

    try_accept(st);

    if (st->conn < 0) {
        return (int32_t)bytes; /* drop — never wait for host */
    }

    p = (const uint8_t *)buf;
    left = bytes;
    while (left > 0) {
        const int32_t n = ag_net_send(st->conn, p, left);
        if (n == -AG_EAGAIN) {
            break; /* drop remainder; never stall */
        }
        if (n <= 0) {
            close_conn(st);
            break;
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
    return (int32_t)bytes;
}

static const ag_dev_ops_t k_ops = {
    .open = pcm_open,
    .close = pcm_close,
    .write = pcm_write,
    .ioctl = pcm_ioctl,
};

ag_err_t ag_driver_init(void)
{
    if (!AG_HAS(ag_api()->dev, add)) {
        return -AG_ENOSYS;
    }

    memset(&s_st, 0, sizeof(s_st));
    s_st.listen = -1;
    s_st.conn = -1;
    default_fmt(&s_st.fmt);
    s_st.fmt_set = 1;

    {
        const ag_dev_add_t desc = {
            .name = "pcmvirt",
            .driver = "PCMVIRT",
            .cls = AG_DEV_AUDIO,
            .flags = AG_DEVF_EXCLUSIVE | AG_DEVF_DMA,
            .ops = &k_ops,
            .priv = &s_st,
        };
        const ag_err_t err = ag_dev_add(&desc);
        if (err != AG_OK) {
            return err;
        }
    }

    /* If DHCP already done, bind now; otherwise open/write will retry. */
    ensure_listen(&s_st);
    return AG_OK;
}
