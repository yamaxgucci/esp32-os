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

AG_DRV("MOUSEVIRT", "1.5", "argon");

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
    uint8_t     pending_move;
    uint8_t     listen_moaned; /* the failed-to-listen warning is said once */
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

static void flush_move(mousevirt_state_t *st)
{
    if (st->pending_move) {
        inject_ptr(st, AG_EV_POINTER_MOVE, 0);
        st->pending_move = 0;
    }
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
        flush_move(st);
        inject_ptr(st, AG_EV_POINTER_DOWN, 0);
    } else if (!(cur & 1u) && (prev & 1u)) {
        flush_move(st);
        inject_ptr(st, AG_EV_POINTER_UP, 0);
    } else {
        /* Coalesce moves: a slow guest would overflow the 64-event queue. */
        st->pending_move = 1;
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
    /*
     * Two tasks call this and neither may spoil the other's work.
     *
     * The kernel's input tick asks this driver for events every ten
     * milliseconds, and the shell asks it when a device is opened, and both can
     * read listen < 0 before either has stored its handle.  One of them then
     * gets -AG_EBUSY from a port the other has just bound - and the version
     * that wrote that straight into st->listen left the port shut for the rest
     * of the boot, with a log saying it had opened it a millisecond earlier.
     *
     * From the outside: a host tool reporting "nothing listening", a guest
     * reporting a healthy driver, and one run in three or four failing.  So a
     * refusal never clears a handle somebody else installed, and a handle won
     * twice has the loser's copy closed rather than leaked.
     */
    const int32_t rc = ag_tcp_listen(MOUSEVIRT_PORT);
    if (rc < 0) {
        if (st->listen < 0 && !st->listen_moaned) {
            st->listen_moaned = 1;
            ag_log(AG_LOG_WARN, "mousevirt", "cannot listen on :%u (%d)",
                   (unsigned)MOUSEVIRT_PORT, (int)rc);
        }
        return;
    }
    if (st->listen >= 0) {
        (void)ag_net_close(rc);
        return;
    }
    st->listen = rc;
    st->listen_moaned = 0;
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
    /*
     * One host tool.  Always take the newest peer (same as pcmvirt/midivirt).
     * Rejecting while a half-closed zombie still sat in st->conn made the host
     * see connect → WinError 10053 on every --reconnect attempt.
     */
    if (st->conn >= 0) {
        close_conn(st, "replaced by new peer");
    }
    st->conn = peer;
    (void)ag_net_set_nonblock(st->conn, true);
    st->rx_n = 0;
    ag_log(AG_LOG_INFO, "mousevirt", "host connected");
}

/* Non-blocking drain; closes st->conn on peer error/EOF. */
static void recv_available(mousevirt_state_t *st)
{
    uint8_t buf[RX_CHUNK];
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

static void pump_rx(mousevirt_state_t *st)
{
    /* Drop a dead peer before accept so reconnect can bind cleanly. */
    recv_available(st);
    try_accept(st);
    recv_available(st);
    flush_move(st);
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

/*
 * The kernel's service tick (ABI 0.28), and what it is for here.
 *
 * A driver on this system has no thread, so being called is the only chance it
 * gets to look at its socket.  Until this existed the only call that ever came
 * was read(), which meant an application had to open /dev/mouse0 and read it
 * every frame *even though the events arrive through ag_poll_event* - and one
 * that did not, having no reason to think a pointer needed opening, saw a
 * driver that accepted the host's connection, never read it, and let it be
 * reset.  From the outside that is a mouse that does not work, and it has been
 * diagnosed twice from scratch: once for Doom, once for the desktop shell.
 *
 * ag_inputpoll_tick calls this every ten milliseconds whatever the foreground
 * application is doing, which is exactly the tick that was missing.
 *
 * It returns no events, and that is deliberate rather than lazy.  The events
 * are injected from here in the surface's own pixels, which is what a pointer
 * is measured in (ABI 0.42); the kernel's poll path takes cells and rescales
 * them, so handing them over would quantise a mouse to the 8x16 grid of the
 * text console.  The tick is the service; the injection is the delivery.
 */
static int32_t mouse_poll(ag_handle_t h, ag_event_t *out, uint32_t max)
{
    (void)h;
    (void)out;
    (void)max;
    if (s_st.listen < 0 && s_st.conn < 0) {
        ensure_listen(&s_st);
    }
    pump_rx(&s_st);
    return 0;
}

static const ag_input_ops_t k_input_ops = {
    .size = sizeof(ag_input_ops_t),
    .poll = mouse_poll,
};

static void mouse_fini(void)
{
    if (s_st.conn >= 0) {
        (void)ag_net_close(s_st.conn);
        s_st.conn = -1;
    }
    if (s_st.listen >= 0) {
        (void)ag_net_close(s_st.listen);
        s_st.listen = -1;
        ag_log(AG_LOG_INFO, "mousevirt", "fini: listen closed");
    }
}

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
    ag_module_on_unload(mouse_fini);

    {
        const ag_dev_add_t desc = {
            .name = "mouse0",
            .driver = "MOUSEVIRT",
            .cls = AG_DEV_INPUT,
            .flags = 0,
            .ops = &k_ops,
            .class_ops = &k_input_ops,
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
