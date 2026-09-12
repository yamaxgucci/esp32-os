/*
 * ArgonOS - virtual keyboard (.SYS): TCP :5561 → KEY_DOWN/UP via inp->inject.
 *
 * Pump on open/read only (same as MOUSEVIRT / MIDIVIRT).  No resident thread.
 * Apps that want host keys must open /dev/kbd0 and read() each frame (DOOM does).
 *
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc \
 *       --include sdk/include -o build\apps\KBDVIRT.SYS apps/kbdvirt/kbdvirt.c
 *   drv install a:\kbdvirt.sys
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/keys.h>
#include <argon/libc.h>

AG_DRV("KBDVIRT", "1.0", "argon");

#define KBDVIRT_PORT 5561u
#define PKT_SIZE     8u
#define RING_EV      64u
#define RX_CHUNK     64u

/*
 * Packet (little-endian):
 *   [0] type: 1=down, 2=up
 *   [1] mods (AG_MOD_*)
 *   [2..3] HID usage id (uint16)
 *   [4..5] unicode (uint16, 0 if none)
 *   [6] repeat
 *   [7] pad
 */
typedef struct {
    uint8_t  type;
    uint8_t  mods;
    uint16_t hid;
    uint16_t unicode;
    uint8_t  repeat;
    uint8_t  _pad;
} kbdvirt_pkt_t;

typedef struct {
    ag_handle_t  listen;
    ag_handle_t  conn;
    kbdvirt_pkt_t ring[RING_EV];
    uint16_t     head;
    uint16_t     tail;
    uint16_t     count;
    uint8_t      rx_buf[PKT_SIZE];
    uint8_t      rx_n;
    uint8_t      drop_stale;
    uint8_t      listen_moaned; /* the failed-to-listen warning is said once */
} kbdvirt_state_t;

static kbdvirt_state_t s_st;

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void ring_clear(kbdvirt_state_t *st)
{
    st->head = 0;
    st->tail = 0;
    st->count = 0;
}

static void ring_push(kbdvirt_state_t *st, const kbdvirt_pkt_t *pkt)
{
    if (st->count >= RING_EV) {
        st->tail = (uint16_t)((st->tail + 1u) % RING_EV);
        st->count--;
    }
    st->ring[st->head] = *pkt;
    st->head = (uint16_t)((st->head + 1u) % RING_EV);
    st->count++;
}

static void inject_key(const kbdvirt_pkt_t *pkt)
{
    ag_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = (pkt->type == 2u) ? AG_EV_KEY_UP : AG_EV_KEY_DOWN;
    ev.key.keycode = pkt->hid;
    ev.key.unicode = pkt->unicode;
    ev.key.mods = pkt->mods;
    ev.key.repeat = pkt->repeat != 0;
    (void)ag_inject_event(&ev);
}

static void handle_pkt(kbdvirt_state_t *st, const kbdvirt_pkt_t *pkt)
{
    if (pkt->type != 1u && pkt->type != 2u) {
        return;
    }
    if (pkt->hid == 0u) {
        return;
    }
    /* Host Ctrl+C / F12 are supervisor stop keys, not Doom fire/menu. */
    if ((pkt->mods & AG_MOD_CTRL) && pkt->hid == (uint16_t)AG_KEY_C) {
        return;
    }
    if (pkt->hid == (uint16_t)AG_KEY_F12) {
        return;
    }
    ring_push(st, pkt);
    inject_key(pkt);
}

static void feed_bytes(kbdvirt_state_t *st, const uint8_t *buf, int32_t n)
{
    int32_t i;
    for (i = 0; i < n; i++) {
        st->rx_buf[st->rx_n++] = buf[i];
        if (st->rx_n >= PKT_SIZE) {
            kbdvirt_pkt_t pkt;
            pkt.type = st->rx_buf[0];
            pkt.mods = st->rx_buf[1];
            pkt.hid = rd_u16(st->rx_buf + 2);
            pkt.unicode = rd_u16(st->rx_buf + 4);
            pkt.repeat = st->rx_buf[6];
            pkt._pad = st->rx_buf[7];
            st->rx_n = 0;
            handle_pkt(st, &pkt);
        }
    }
}

static void drain_rx(kbdvirt_state_t *st)
{
    uint8_t dump[RX_CHUNK];
    int     spins = 0;
    if (st->conn < 0) {
        return;
    }
    st->rx_n = 0;
    ring_clear(st);
    while (spins++ < 64) {
        const int32_t n = ag_net_recv(st->conn, dump, sizeof(dump));
        if (n == -AG_EAGAIN || n <= 0) {
            break;
        }
    }
}

static void close_conn(kbdvirt_state_t *st, const char *why)
{
    if (st->conn >= 0) {
        ag_log(AG_LOG_INFO, "kbdvirt", "close conn (%s)", why ? why : "?");
        (void)ag_net_close(st->conn);
        st->conn = -1;
    }
    st->rx_n = 0;
}

static void ensure_listen(kbdvirt_state_t *st)
{
    if (st->listen >= 0) {
        return;
    }
    if (!ag_net_is_ready()) {
        return;
    }
    /*
     * A refusal never clears a handle somebody else installed.  See the long
     * note in apps/mousevirt/mousevirt.c: the kernel's input tick and the
     * shell both call this, and the loser of that race used to overwrite the
     * winner's socket with -1 and leave the port shut for the whole boot.
     */
    const int32_t rc = ag_tcp_listen(KBDVIRT_PORT);
    if (rc < 0) {
        if (st->listen < 0 && !st->listen_moaned) {
            st->listen_moaned = 1;
            ag_log(AG_LOG_WARN, "kbdvirt", "cannot listen on :%u (%d)",
                   (unsigned)KBDVIRT_PORT, (int)rc);
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
    ag_log(AG_LOG_INFO, "kbdvirt", "listen :%u", (unsigned)KBDVIRT_PORT);
}

static void try_accept(kbdvirt_state_t *st)
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
    if (st->conn >= 0) {
        close_conn(st, "replaced by new peer");
    }
    st->conn = peer;
    (void)ag_net_set_nonblock(st->conn, true);
    /*
     * Events queued from an older peer go; bytes on this brand new socket
     * stay.
     *
     * This used to drain the socket here as well, and that was a keyboard that
     * dropped keys.  The socket has just been accepted, so the only thing it
     * can hold is what *this* peer has already sent - and a host tool that
     * connects and immediately types has sent it before the guest, which only
     * accepts on its ten-millisecond tick, gets here.  Measured: a scripted F5
     * arriving one run in three, with the driver reporting a healthy
     * connection every time.
     */
    ring_clear(st);
    st->rx_n = 0;
    ag_log(AG_LOG_INFO, "kbdvirt", "host connected");
}

static void recv_available(kbdvirt_state_t *st)
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

static void pump_rx(kbdvirt_state_t *st)
{
    recv_available(st);
    try_accept(st);
    recv_available(st);
}

static ag_err_t kbd_open(ag_device_t *dev, uint32_t flags)
{
    kbdvirt_state_t *st = (kbdvirt_state_t *)ag_dev_priv(dev);
    (void)flags;
    if (st == NULL) {
        return -AG_ENODEV;
    }
    ensure_listen(st);
    /*
     * Throw away what is waiting only when a peer was already connected before
     * this open: that is a previous application's leftovers and belongs to
     * nobody.  A connection this open has just accepted has sent nothing but
     * what the caller is about to want.
     */
    const bool had_peer = (st->conn >= 0);
    try_accept(st);
    st->drop_stale = had_peer ? 1u : 0u;
    return AG_OK;
}

static ag_err_t kbd_close(ag_device_t *dev)
{
    (void)dev;
    return AG_OK;
}

static ag_err_t kbd_ioctl(ag_device_t *dev, uint32_t cmd, void *arg,
                          size_t arglen)
{
    kbdvirt_state_t *st = (kbdvirt_state_t *)ag_dev_priv(dev);
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

static int32_t kbd_read(ag_device_t *dev, void *buf, size_t len, uint64_t off)
{
    kbdvirt_state_t *st = (kbdvirt_state_t *)ag_dev_priv(dev);
    uint8_t         *out;
    size_t           max_ev, got = 0;

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

    if (st->drop_stale) {
        drain_rx(st);
        st->drop_stale = 0;
    }
    pump_rx(st);

    out = (uint8_t *)buf;
    max_ev = len / PKT_SIZE;
    while (got < max_ev && st->count > 0u) {
        const kbdvirt_pkt_t *e = &st->ring[st->tail];
        out[0] = e->type;
        out[1] = e->mods;
        out[2] = (uint8_t)(e->hid & 0xffu);
        out[3] = (uint8_t)((e->hid >> 8) & 0xffu);
        out[4] = (uint8_t)(e->unicode & 0xffu);
        out[5] = (uint8_t)((e->unicode >> 8) & 0xffu);
        out[6] = e->repeat;
        out[7] = 0;
        out += PKT_SIZE;
        st->tail = (uint16_t)((st->tail + 1u) % RING_EV);
        st->count--;
        got++;
    }
    return (int32_t)(got * PKT_SIZE);
}

static const ag_dev_ops_t k_ops = {
    .open = kbd_open,
    .close = kbd_close,
    .read = kbd_read,
    .write = NULL,
    .ioctl = kbd_ioctl,
};

/*
 * The kernel's service tick (ABI 0.28).  See the long note over mouse_poll in
 * apps/mousevirt/mousevirt.c: a driver here has no thread, and without this
 * the only service it ever got was an application reading the device - which
 * an application taking its keys from ag_poll_event has no reason to do.
 *
 * No events are returned: they are injected from the pump, already decoded.
 * The stale-drop is deliberately not run here.  It exists to throw away what a
 * previous peer left behind when an application opens the device, and running
 * it on a ten-millisecond tick would throw away every key instead.
 */
static int32_t kbd_poll(ag_handle_t h, ag_event_t *out, uint32_t max)
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
    .poll = kbd_poll,
};

static void kbd_fini(void)
{
    if (s_st.conn >= 0) {
        (void)ag_net_close(s_st.conn);
        s_st.conn = -1;
    }
    if (s_st.listen >= 0) {
        (void)ag_net_close(s_st.listen);
        s_st.listen = -1;
        ag_log(AG_LOG_INFO, "kbdvirt", "fini: listen closed");
    }
}

ag_err_t ag_driver_init(void)
{
    if (!AG_HAS(ag_api()->dev, add)) {
        return -AG_ENOSYS;
    }
    if (!AG_HAS(ag_api()->inp, inject)) {
        ag_log(AG_LOG_WARN, "kbdvirt", "inp->inject missing (need ABI 0.18+)");
        return -AG_ENOSYS;
    }

    memset(&s_st, 0, sizeof(s_st));
    s_st.listen = -1;
    s_st.conn = -1;
    ag_module_on_unload(kbd_fini);

    {
        const ag_dev_add_t desc = {
            .name = "kbd0",
            .driver = "KBDVIRT",
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
