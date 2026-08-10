/*
 * ArgonOS - virtual MIDI input (.SYS): TCP :5559 → /dev/midivirt.
 *
 * Host (tools/midikbd.py via QEMU hostfwd) connects and sends raw MIDI
 * channel voice messages.  Apps non-blocking read() packed 4-byte events.
 *
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc \
 *       --include sdk/include -o build\apps\MIDIVIRT.SYS apps/midivirt/midivirt.c
 *   drv install h:\midivirt.sys
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_DRV("MIDIVIRT", "1.0", "argon");

#define MIDIVIRT_PORT 5559u
#define RING_EV       64u
#define EV_SIZE       4u
#define RX_CHUNK      64u

typedef struct {
    uint8_t status;
    uint8_t d1;
    uint8_t d2;
    uint8_t _pad;
} midivirt_ev_t;

typedef struct {
    ag_handle_t  listen;
    ag_handle_t  conn;
    midivirt_ev_t ring[RING_EV];
    uint16_t     head; /* next write */
    uint16_t     tail; /* next read */
    uint16_t     count;
    /* MIDI parser */
    uint8_t      run_status;
    uint8_t      need; /* data bytes still needed */
    uint8_t      d0;
    uint8_t      d1;
    uint8_t      in_sysex;
} midivirt_state_t;

static midivirt_state_t s_st;

static void ring_clear(midivirt_state_t *st)
{
    st->head = 0;
    st->tail = 0;
    st->count = 0;
}

static void ring_push(midivirt_state_t *st, uint8_t status, uint8_t d1,
                      uint8_t d2)
{
    midivirt_ev_t *e;
    if (st->count >= RING_EV) {
        /* drop oldest */
        st->tail = (uint16_t)((st->tail + 1u) % RING_EV);
        st->count--;
    }
    e = &st->ring[st->head];
    e->status = status;
    e->d1 = d1;
    e->d2 = d2;
    e->_pad = 0;
    st->head = (uint16_t)((st->head + 1u) % RING_EV);
    st->count++;
}

static int data_bytes_for(uint8_t status)
{
    uint8_t hi = (uint8_t)(status & 0xf0u);
    if (hi == 0xc0u || hi == 0xd0u) {
        return 1;
    }
    if (hi >= 0x80u && hi <= 0xe0u) {
        return 2;
    }
    return 0;
}

static void parser_reset(midivirt_state_t *st)
{
    st->run_status = 0;
    st->need = 0;
    st->d0 = 0;
    st->d1 = 0;
    st->in_sysex = 0;
}

static void feed_byte(midivirt_state_t *st, uint8_t b)
{
    if (st->in_sysex) {
        if (b == 0xf7u) {
            st->in_sysex = 0;
        }
        return;
    }

    if (b >= 0xf8u) {
        return; /* realtime: ignore */
    }
    if (b == 0xf0u) {
        st->in_sysex = 1;
        st->need = 0;
        return;
    }
    if (b >= 0xf1u && b <= 0xf7u) {
        st->need = 0;
        return; /* other system common */
    }

    if (b & 0x80u) {
        int n = data_bytes_for(b);
        if (n == 0) {
            st->need = 0;
            return;
        }
        st->run_status = b;
        st->need = (uint8_t)n;
        st->d0 = 0;
        st->d1 = 0;
        return;
    }

    /* data byte */
    if (st->need == 0u) {
        if (st->run_status == 0u) {
            return;
        }
        st->need = (uint8_t)data_bytes_for(st->run_status);
        if (st->need == 0u) {
            return;
        }
    }

    if (st->need == 2u) {
        st->d0 = b;
        st->need = 1u;
        return;
    }
    if (st->need == 1u) {
        uint8_t st_byte = st->run_status;
        uint8_t d1, d2;
        if (data_bytes_for(st_byte) == 1) {
            d1 = b;
            d2 = 0;
        } else {
            d1 = st->d0;
            d2 = b;
        }
        st->need = 0;
        /* note-on vel 0 → note-off */
        if ((st_byte & 0xf0u) == 0x90u && d2 == 0u) {
            st_byte = (uint8_t)(0x80u | (st_byte & 0x0fu));
        }
        ring_push(st, st_byte, d1, d2);
    }
}

static void close_conn(midivirt_state_t *st, const char *why)
{
    if (st->conn >= 0) {
        ag_log(AG_LOG_INFO, "midivirt", "close conn (%s)", why ? why : "?");
        (void)ag_net_close(st->conn);
        st->conn = -1;
    }
    parser_reset(st);
}

static void ensure_listen(midivirt_state_t *st)
{
    if (st->listen >= 0) {
        return;
    }
    if (!ag_net_is_ready()) {
        return;
    }
    st->listen = ag_tcp_listen(MIDIVIRT_PORT);
    if (st->listen < 0) {
        st->listen = -1;
        return;
    }
    (void)ag_net_set_nonblock(st->listen, true);
}

static void try_accept(midivirt_state_t *st)
{
    ag_handle_t peer;

    ensure_listen(st);
    if (st->listen < 0) {
        return;
    }
    peer = ag_tcp_accept(st->listen, 0u);
    if (peer < 0) {
        return;
    }
    close_conn(st, "replaced by new peer");
    st->conn = peer;
    (void)ag_net_set_nonblock(st->conn, true);
    parser_reset(st);
}

static void pump_rx(midivirt_state_t *st)
{
    uint8_t buf[RX_CHUNK];

    try_accept(st);
    if (st->conn < 0) {
        return;
    }
    for (;;) {
        int32_t n = ag_net_recv(st->conn, buf, sizeof(buf));
        if (n == -AG_EAGAIN) {
            break;
        }
        if (n <= 0) {
            close_conn(st, "peer closed / recv err");
            break;
        }
        {
            int32_t i;
            for (i = 0; i < n; i++) {
                feed_byte(st, buf[i]);
            }
        }
    }
}

static ag_err_t midi_open(ag_device_t *dev, uint32_t flags)
{
    midivirt_state_t *st = (midivirt_state_t *)ag_dev_priv(dev);
    (void)flags;
    if (st == NULL) {
        return -AG_ENODEV;
    }
    ensure_listen(st);
    try_accept(st);
    return AG_OK;
}

static ag_err_t midi_close(ag_device_t *dev)
{
    (void)dev;
    return AG_OK;
}

static ag_err_t midi_ioctl(ag_device_t *dev, uint32_t cmd, void *arg,
                           size_t arglen)
{
    midivirt_state_t *st = (midivirt_state_t *)ag_dev_priv(dev);
    (void)arg;
    (void)arglen;
    if (st == NULL) {
        return -AG_ENODEV;
    }
    if (cmd == AG_IOC_FLUSH || cmd == AG_IOC_RESET) {
        ring_clear(st);
        parser_reset(st);
        return AG_OK;
    }
    return -AG_ENOTSUP;
}

static int32_t midi_read(ag_device_t *dev, void *buf, size_t len, uint64_t off)
{
    midivirt_state_t *st = (midivirt_state_t *)ag_dev_priv(dev);
    uint8_t          *out;
    size_t            max_ev;
    size_t            got = 0;

    (void)off;
    if (st == NULL) {
        return -AG_ENODEV;
    }
    if (buf == NULL) {
        return -AG_EINVAL;
    }
    if (len < EV_SIZE) {
        return 0;
    }

    pump_rx(st);

    out = (uint8_t *)buf;
    max_ev = len / EV_SIZE;
    while (got < max_ev && st->count > 0u) {
        const midivirt_ev_t *e = &st->ring[st->tail];
        out[0] = e->status;
        out[1] = e->d1;
        out[2] = e->d2;
        out[3] = e->_pad;
        out += EV_SIZE;
        st->tail = (uint16_t)((st->tail + 1u) % RING_EV);
        st->count--;
        got++;
    }
    return (int32_t)(got * EV_SIZE);
}

static const ag_dev_ops_t k_ops = {
    .open = midi_open,
    .close = midi_close,
    .read = midi_read,
    .ioctl = midi_ioctl,
};

ag_err_t ag_driver_init(void)
{
    if (!AG_HAS(ag_api()->dev, add)) {
        return -AG_ENOSYS;
    }

    memset(&s_st, 0, sizeof(s_st));
    s_st.listen = -1;
    s_st.conn = -1;
    parser_reset(&s_st);

    {
        const ag_dev_add_t desc = {
            .name = "midivirt",
            .driver = "MIDIVIRT",
            .cls = AG_DEV_INPUT,
            .flags = AG_DEVF_READONLY,
            .ops = &k_ops,
            .priv = &s_st,
        };
        const ag_err_t err = ag_dev_add(&desc);
        if (err != AG_OK) {
            return err;
        }
    }

    ensure_listen(&s_st);
    return AG_OK;
}
