/*
 * ArgonOS - virtual mouse (.SYS): TCP :5560 → POINTER_* via inp->inject.
 *
 * Pump on open/read only (same as MIDIVIRT).  No resident thread: a 4 KB
 * FreeRTOS stack at drv install stole internal DRAM and made GRAIN fail
 * with "no memory for a 12288 byte stack".  Apps that want mouse must
 * open /dev/mouse0 and read() each frame (GRAIN does).
 *
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc \
 *       --include sdk/include -o build\apps\MOUSEVIRT.SYS apps/mousevirt/mousevirt.c
 *   drv install h:\mousevirt.sys
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_DRV("MOUSEVIRT", "1.1", "argon");

#define MOUSEVIRT_PORT 5560u
#define PKT_SIZE       8u
#define RING_EV        64u
#define RX_CHUNK       64u

/*
 * Packet (little-endian):
 *   [0] type: 1=abs, 2=btn edge (unused; abs carries buttons), 3=wheel
 *   [1] buttons bitmask (bit0=L, bit1=R, bit2=M)
 *   [2..3] x int16
 *   [4..5] y int16
 *   [6] wheel delta (int8)
 *   [7] pad
 */
typedef struct {
    uint8_t type;
    uint8_t buttons;
    int16_t x;
    int16_t y;
    int8_t  wheel;
    uint8_t _pad;
} mousevirt_pkt_t;

typedef struct {
    ag_handle_t listen;
    ag_handle_t conn;
    mousevirt_pkt_t ring[RING_EV];
    uint16_t    head;
    uint16_t    tail;
    uint16_t    count;
    uint8_t     buttons;
    int16_t     x;
    int16_t     y;
    uint8_t     rx_buf[PKT_SIZE];
    uint8_t     rx_n;
} mousevirt_state_t;

static mousevirt_state_t s_st;

static int16_t rd_i16(const uint8_t *p)
{
    return (int16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void ring_clear(mousevirt_state_t *st)
{
    st->head = 0;
    st->tail = 0;
    st->count = 0;
}

static void ring_push(mousevirt_state_t *st, const mousevirt_pkt_t *pkt)
{
    if (st->count >= RING_EV) {
        st->tail = (uint16_t)((st->tail + 1u) % RING_EV);
        st->count--;
    }
    st->ring[st->head] = *pkt;
    st->head = (uint16_t)((st->head + 1u) % RING_EV);
    st->count++;
}

static void inject_ptr(mousevirt_state_t *st, ag_event_type_t type, int8_t wheel)
{
    ag_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.ptr.x = st->x;
    ev.ptr.y = st->y;
    ev.ptr.buttons = st->buttons;
    ev.ptr.dx = 0;
    ev.ptr.dy = (int16_t)wheel;
    ev.ptr.slot = 0;
    (void)ag_inject_event(&ev);
}

static void handle_pkt(mousevirt_state_t *st, const mousevirt_pkt_t *pkt)
{
    uint8_t prev = st->buttons;
    uint8_t cur;

    st->x = pkt->x;
    st->y = pkt->y;
    cur = pkt->buttons;
    st->buttons = cur;
    ring_push(st, pkt);

    if (pkt->type == 3u || pkt->wheel != 0) {
        inject_ptr(st, AG_EV_WHEEL, pkt->wheel);
    }

    if ((cur & 1u) && !(prev & 1u)) {
        inject_ptr(st, AG_EV_POINTER_DOWN, 0);
    } else if (!(cur & 1u) && (prev & 1u)) {
        inject_ptr(st, AG_EV_POINTER_UP, 0);
    } else {
        inject_ptr(st, AG_EV_POINTER_MOVE, 0);
    }
}

static void feed_bytes(mousevirt_state_t *st, const uint8_t *buf, int32_t n)
{
    int32_t i;
    for (i = 0; i < n; i++) {
        st->rx_buf[st->rx_n++] = buf[i];
        if (st->rx_n >= PKT_SIZE) {
            mousevirt_pkt_t pkt;
            pkt.type = st->rx_buf[0];
            pkt.buttons = st->rx_buf[1];
            pkt.x = rd_i16(st->rx_buf + 2);
            pkt.y = rd_i16(st->rx_buf + 4);
            pkt.wheel = (int8_t)st->rx_buf[6];
            pkt._pad = st->rx_buf[7];
            st->rx_n = 0;
            if (pkt.type >= 1u && pkt.type <= 3u) {
                handle_pkt(st, &pkt);
            }
        }
    }
}

static void close_conn(mousevirt_state_t *st, const char *why)
{
    if (st->conn >= 0) {
        ag_log(AG_LOG_INFO, "mousevirt", "close conn (%s)", why ? why : "?");
        (void)ag_net_close(st->conn);
        st->conn = -1;
    }
    st->rx_n = 0;
}

static void ensure_listen(mousevirt_state_t *st)
{
    if (st->listen >= 0) {
        return;
    }
    if (!ag_net_is_ready()) {
        return;
    }
    st->listen = ag_tcp_listen(MOUSEVIRT_PORT);
    if (st->listen < 0) {
        st->listen = -1;
        return;
    }
    (void)ag_net_set_nonblock(st->listen, true);
    ag_log(AG_LOG_INFO, "mousevirt", "listen :%u", (unsigned)MOUSEVIRT_PORT);
}

static void try_accept(mousevirt_state_t *st)
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
    st->rx_n = 0;
    ag_log(AG_LOG_INFO, "mousevirt", "host connected");
}

static void pump_rx(mousevirt_state_t *st)
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
        feed_bytes(st, buf, n);
    }
}

static ag_err_t mouse_open(ag_device_t *dev, uint32_t flags)
{
    mousevirt_state_t *st = (mousevirt_state_t *)ag_dev_priv(dev);
    (void)flags;
    if (st == NULL) {
        return -AG_ENODEV;
    }
    ensure_listen(st);
    try_accept(st);
    return AG_OK;
}

static ag_err_t mouse_close(ag_device_t *dev)
{
    (void)dev;
    return AG_OK;
}

static ag_err_t mouse_ioctl(ag_device_t *dev, uint32_t cmd, void *arg,
                            size_t arglen)
{
    mousevirt_state_t *st = (mousevirt_state_t *)ag_dev_priv(dev);
    (void)arg;
    (void)arglen;
    if (st == NULL) {
        return -AG_ENODEV;
    }
    if (cmd == AG_IOC_FLUSH || cmd == AG_IOC_RESET) {
        ring_clear(st);
        st->rx_n = 0;
        return AG_OK;
    }
    return -AG_ENOTSUP;
}

static int32_t mouse_read(ag_device_t *dev, void *buf, size_t len, uint64_t off)
{
    mousevirt_state_t *st = (mousevirt_state_t *)ag_dev_priv(dev);
    uint8_t           *out;
    size_t             max_ev, got = 0;

    (void)off;
    if (st == NULL) {
        return -AG_ENODEV;
    }
    if (buf == NULL) {
        return -AG_EINVAL;
    }
    if (len < PKT_SIZE) {
        return 0;
    }

    pump_rx(st);

    out = (uint8_t *)buf;
    max_ev = len / PKT_SIZE;
    while (got < max_ev && st->count > 0u) {
        const mousevirt_pkt_t *e = &st->ring[st->tail];
        out[0] = e->type;
        out[1] = e->buttons;
        out[2] = (uint8_t)(e->x & 0xff);
        out[3] = (uint8_t)((e->x >> 8) & 0xff);
        out[4] = (uint8_t)(e->y & 0xff);
        out[5] = (uint8_t)((e->y >> 8) & 0xff);
        out[6] = (uint8_t)e->wheel;
        out[7] = 0;
        out += PKT_SIZE;
        st->tail = (uint16_t)((st->tail + 1u) % RING_EV);
        st->count--;
        got++;
    }
    return (int32_t)(got * PKT_SIZE);
}

static const ag_dev_ops_t k_ops = {
    .open = mouse_open,
    .close = mouse_close,
    .read = mouse_read,
    .write = NULL,
    .ioctl = mouse_ioctl,
};

ag_err_t ag_driver_init(void)
{
    if (!AG_HAS(ag_api()->dev, add)) {
        return -AG_ENOSYS;
    }
    if (!AG_HAS(ag_api()->inp, inject)) {
        ag_log(AG_LOG_WARN, "mousevirt", "inp->inject missing (need ABI 0.18+)");
        return -AG_ENOSYS;
    }

    memset(&s_st, 0, sizeof(s_st));
    s_st.listen = -1;
    s_st.conn = -1;

    {
        const ag_dev_add_t desc = {
            .name = "mouse0",
            .driver = "MOUSEVIRT",
            .cls = AG_DEV_INPUT,
            .flags = 0,
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
