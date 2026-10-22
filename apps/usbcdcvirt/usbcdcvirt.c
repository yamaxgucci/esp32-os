/*
 * ArgonOS - USBCDCVIRT (.SYS): /dev/usbcdc0 with a socket where the cable goes.
 *
 * The companion of USBHIDVIRT, and the same trade.  QEMU models no OTG
 * peripheral, so the serial pipe to the far machine cannot exist there; what
 * can exist is the same device with a TCP socket in place of the wire.  The
 * agent that would run on the far machine connects to it and behaves exactly as
 * it would over USB - it is the same program, sending the same bytes - so
 * everything above the port is verified without a board, and the picture is on
 * the phone while the cable does not exist.
 *
 * Bytes both ways, no framing: that is what the real device is.  With nothing
 * connected a read is -AG_EAGAIN and a write is -AG_ENODEV, which is what
 * happens on a desk when the board is plugged in and nobody has started the
 * agent.
 *
 *   drv install h:\usbcdcvirt.sys
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_DRV("USBCDCVIRT", "1.0", "argon");

#define USBCDCVIRT_PORT 5563u

typedef struct {
    ag_handle_t listen;
    ag_handle_t conn;
    uint32_t    rx;
    uint32_t    tx;
} usbcdcvirt_state_t;

static usbcdcvirt_state_t s_st;

static void close_conn(usbcdcvirt_state_t *st, const char *why)
{
    if (st->conn >= 0) {
        ag_log(AG_LOG_INFO, "usbcdcvirt", "agent gone (%s)", why ? why : "?");
        (void)ag_net_close(st->conn);
        st->conn = -1;
    }
}

static void ensure_listen(usbcdcvirt_state_t *st)
{
    if (st->listen >= 0 || !ag_net_is_ready()) {
        return;
    }
    st->listen = ag_tcp_listen(USBCDCVIRT_PORT);
    if (st->listen < 0) {
        st->listen = -1;
        return;
    }
    (void)ag_net_set_nonblock(st->listen, true);
    ag_log(AG_LOG_INFO, "usbcdcvirt", "listen :%u", (unsigned)USBCDCVIRT_PORT);
}

static void try_accept(usbcdcvirt_state_t *st)
{
    ensure_listen(st);
    if (st->listen < 0) {
        return;
    }
    const ag_handle_t peer = ag_tcp_accept(st->listen, 0u);
    if (peer < 0) {
        return;
    }
    if (st->conn >= 0) {
        close_conn(st, "replaced by new peer");
    }
    st->conn = peer;
    (void)ag_net_set_nonblock(st->conn, true);
    st->rx = 0;
    st->tx = 0;
    ag_log(AG_LOG_INFO, "usbcdcvirt", "agent connected");
}

static ag_err_t cdc_open(ag_device_t *dev, uint32_t flags)
{
    usbcdcvirt_state_t *st = (usbcdcvirt_state_t *)ag_dev_priv(dev);
    (void)flags;
    if (st == NULL) {
        return -AG_ENODEV;
    }
    try_accept(st);
    return AG_OK;
}

static ag_err_t cdc_close(ag_device_t *dev)
{
    (void)dev;
    return AG_OK;
}

static int32_t cdc_read(ag_device_t *dev, void *buf, size_t len, uint64_t off)
{
    usbcdcvirt_state_t *st = (usbcdcvirt_state_t *)ag_dev_priv(dev);
    (void)off;

    if (st == NULL || buf == NULL || len == 0) {
        return -AG_EINVAL;
    }
    try_accept(st);
    if (st->conn < 0) {
        return -AG_EAGAIN; /* enumerated, nobody talking - not a failure */
    }

    const int32_t n = ag_net_recv(st->conn, buf, len);
    if (n == -AG_EAGAIN) {
        return -AG_EAGAIN;
    }
    if (n <= 0) {
        close_conn(st, "peer closed");
        return -AG_EAGAIN;
    }
    st->rx += (uint32_t)n;
    return n;
}

static int32_t cdc_write(ag_device_t *dev, const void *buf, size_t len,
                         uint64_t off)
{
    usbcdcvirt_state_t *st = (usbcdcvirt_state_t *)ag_dev_priv(dev);
    (void)off;

    if (st == NULL || buf == NULL || len == 0) {
        return -AG_EINVAL;
    }
    try_accept(st);
    if (st->conn < 0) {
        return -AG_ENODEV;
    }

    const int32_t n = ag_net_send(st->conn, buf, len);
    if (n == -AG_EAGAIN) {
        return -AG_EAGAIN;
    }
    if (n <= 0) {
        close_conn(st, "send failed");
        return -AG_ENODEV;
    }
    st->tx += (uint32_t)n;
    return n;
}

static ag_err_t cdc_ioctl(ag_device_t *dev, uint32_t cmd, void *arg,
                          size_t arglen)
{
    usbcdcvirt_state_t *st = (usbcdcvirt_state_t *)ag_dev_priv(dev);

    if (st == NULL) {
        return -AG_ENODEV;
    }
    if (cmd != AG_IOC_USBCDC_STATUS) {
        return -AG_ENOTSUP;
    }
    if (arg == NULL || arglen < sizeof(ag_usbcdc_status_t)) {
        return -AG_EINVAL;
    }
    try_accept(st);

    ag_usbcdc_status_t out;
    memset(&out, 0, sizeof(out));
    out.state = (st->conn >= 0) ? (uint32_t)AG_USBCDC_OPEN
                                : (uint32_t)AG_USBCDC_IDLE;
    out.rx = st->rx;
    out.tx = st->tx;
    memcpy(arg, &out, sizeof(out));
    return AG_OK;
}

static const ag_dev_ops_t k_ops = {
    .open = cdc_open,
    .close = cdc_close,
    .read = cdc_read,
    .write = cdc_write,
    .ioctl = cdc_ioctl,
};

static void cdc_fini(void)
{
    close_conn(&s_st, "unload");
    if (s_st.listen >= 0) {
        (void)ag_net_close(s_st.listen);
        s_st.listen = -1;
        ag_log(AG_LOG_INFO, "usbcdcvirt", "fini: listen closed");
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
    ag_module_on_unload(cdc_fini);

    const ag_dev_add_t desc = {
        .name = "usbcdc0",
        .driver = "USBCDCVIRT",
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
