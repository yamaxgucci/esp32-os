/*
 * ArgonOS - USBHIDVIRT (.SYS): /dev/usbhid0 with a socket where the cable goes.
 *
 * The emulator models no USB device peripheral - it models no OTG peripheral at
 * all - so the one thing the gate cannot be is a run in QEMU of the real
 * gadget.  Everything else on the path can be: the application, the web page it
 * serves, the wire format the phone speaks, the frames that come out of it and
 * the order they come out in.  This driver is where they end up.
 *
 * It publishes the same /dev/usbhid0 the kernel publishes on a board with
 * CONFIG_ARGON_USB_DEVICE, answers the same ioctl, takes the same frames, and
 * forwards them to tools/usbhidvirt.py over TCP :5562, which prints them as
 * keystrokes and pointer moves a person can read.  What it does not do is
 * pretend the cable is in when it is not: with no host tool connected a write
 * fails with -AG_ENODEV, exactly as the real device does when nothing is
 * plugged into the OTG port, so the application's unplugged path is exercised
 * here too rather than only on a desk.
 *
 * A .SYS and not a build-time stub because that is how this system already
 * tests the things a board owns - MOUSEVIRT, PCMVIRT, MIDIVIRT - and because a
 * driver can be swapped for the real one without rebuilding anything.
 *
 *   drv install h:\usbhidvirt.sys
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_DRV("USBHIDVIRT", "1.0", "argon");

#define USBHIDVIRT_PORT 5562u

typedef struct {
    ag_handle_t listen;
    ag_handle_t conn;
    uint32_t    sent;
    uint32_t    dropped;
    uint8_t     leds;
} usbhidvirt_state_t;

static usbhidvirt_state_t s_st;

static void close_conn(usbhidvirt_state_t *st, const char *why)
{
    if (st->conn >= 0) {
        ag_log(AG_LOG_INFO, "usbhidvirt", "cable out (%s)", why ? why : "?");
        (void)ag_net_close(st->conn);
        st->conn = -1;
    }
}

static void ensure_listen(usbhidvirt_state_t *st)
{
    if (st->listen >= 0 || !ag_net_is_ready()) {
        return;
    }
    st->listen = ag_tcp_listen(USBHIDVIRT_PORT);
    if (st->listen < 0) {
        st->listen = -1;
        return;
    }
    (void)ag_net_set_nonblock(st->listen, true);
    ag_log(AG_LOG_INFO, "usbhidvirt", "listen :%u",
           (unsigned)USBHIDVIRT_PORT);
}

static void try_accept(usbhidvirt_state_t *st)
{
    ensure_listen(st);
    if (st->listen < 0) {
        return;
    }
    const ag_handle_t peer = ag_tcp_accept(st->listen, 0u);
    if (peer < 0) {
        return;
    }
    /* Newest peer wins, the same rule the other virtual devices use: a
     * half-closed one left in place makes every reconnect look like a refusal
     * to the host tool. */
    if (st->conn >= 0) {
        close_conn(st, "replaced by new peer");
    }
    st->conn = peer;
    (void)ag_net_set_nonblock(st->conn, true);
    st->sent = 0;
    st->dropped = 0;
    ag_log(AG_LOG_INFO, "usbhidvirt", "cable in");
}

/*
 * The host tool's only job upstream is the lock lamps, so that Caps Lock can be
 * tested at all: one byte, whenever it feels like sending one.
 */
static void drain_leds(usbhidvirt_state_t *st)
{
    if (st->conn < 0) {
        return;
    }
    for (;;) {
        uint8_t       b;
        const int32_t n = ag_net_recv(st->conn, &b, 1);
        if (n == -AG_EAGAIN) {
            return;
        }
        if (n <= 0) {
            close_conn(st, "peer closed");
            return;
        }
        st->leds = b;
    }
}

static ag_err_t hid_open(ag_device_t *dev, uint32_t flags)
{
    usbhidvirt_state_t *st = (usbhidvirt_state_t *)ag_dev_priv(dev);
    (void)flags;
    if (st == NULL) {
        return -AG_ENODEV;
    }
    try_accept(st);
    return AG_OK;
}

static ag_err_t hid_close(ag_device_t *dev)
{
    (void)dev;
    return AG_OK;
}

static int32_t hid_write(ag_device_t *dev, const void *buf, size_t len,
                         uint64_t off)
{
    usbhidvirt_state_t *st = (usbhidvirt_state_t *)ag_dev_priv(dev);
    (void)off;

    if (st == NULL || buf == NULL) {
        return -AG_EINVAL;
    }
    if (len == 0 || (len % AG_USBHID_FRAME) != 0) {
        return -AG_EINVAL;
    }

    drain_leds(st);
    try_accept(st);
    if (st->conn < 0) {
        return -AG_ENODEV; /* nothing plugged in - the same answer the real one gives */
    }

    const uint8_t *p = (const uint8_t *)buf;
    size_t         done = 0;

    while (done < len) {
        const int32_t n = ag_net_send(st->conn, p + done, AG_USBHID_FRAME);
        if (n == -AG_EAGAIN) {
            /*
             * The socket is full.  On the board this is the endpoint being
             * busy, and the answer is the same one: how much went, and come
             * back for the rest.
             */
            st->dropped++;
            break;
        }
        if (n != (int32_t)AG_USBHID_FRAME) {
            close_conn(st, "send failed");
            return done > 0 ? (int32_t)done : -AG_EIO;
        }
        st->sent++;
        done += AG_USBHID_FRAME;
    }
    return done > 0 ? (int32_t)done : -AG_EAGAIN;
}

static ag_err_t hid_ioctl(ag_device_t *dev, uint32_t cmd, void *arg,
                          size_t arglen)
{
    usbhidvirt_state_t *st = (usbhidvirt_state_t *)ag_dev_priv(dev);

    if (st == NULL) {
        return -AG_ENODEV;
    }
    if (cmd != AG_IOC_USBHID_STATUS) {
        return -AG_ENOTSUP;
    }
    if (arg == NULL || arglen < sizeof(ag_usbhid_status_t)) {
        return -AG_EINVAL;
    }

    drain_leds(st);
    try_accept(st);

    ag_usbhid_status_t out;
    memset(&out, 0, sizeof(out));
    out.state = (st->conn >= 0) ? (uint32_t)AG_USBHID_READY
                                : (uint32_t)AG_USBHID_ATTACHED;
    out.sent = st->sent;
    out.dropped = st->dropped;
    out.leds = st->leds;
    out.protocol = 0;
    memcpy(arg, &out, sizeof(out));
    return AG_OK;
}

static const ag_dev_ops_t k_ops = {
    .open = hid_open,
    .close = hid_close,
    .read = NULL,
    .write = hid_write,
    .ioctl = hid_ioctl,
};

static void hid_fini(void)
{
    close_conn(&s_st, "unload");
    if (s_st.listen >= 0) {
        (void)ag_net_close(s_st.listen);
        s_st.listen = -1;
        ag_log(AG_LOG_INFO, "usbhidvirt", "fini: listen closed");
    }
}

ag_err_t ag_driver_init(void)
{
    if (!AG_HAS(ag_api()->dev, add)) {
        return -AG_ENOSYS;
    }

    memset(&s_st, 0, sizeof(s_st));
    s_st.listen = -1;
    s_st.conn = -1;
    ag_module_on_unload(hid_fini);

    const ag_dev_add_t desc = {
        .name = "usbhid0",
        .driver = "USBHIDVIRT",
        .cls = AG_DEV_CHAR,
        .flags = AG_DEVF_EXCLUSIVE,
        .ops = &k_ops,
        .priv = &s_st,
    };
    const ag_err_t err = ag_dev_add(&desc);
    if (err != AG_OK) {
        return err;
    }

    ensure_listen(&s_st);
    return AG_OK;
}
