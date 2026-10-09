/*
 * ArgonOS - the network provider indirection.  See argon/netprov.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/config.h>

#if CONFIG_ARGON_ENABLE_NET

#include <argon/netprov.h>

#include <stddef.h>
#include <string.h>

#include <argon/device.h>
#include <argon/log.h>
#include <argon/net.h>
#include <argon/power.h>

#include <argon/port/net.h>
#include <argon/port/task.h>
#include <argon/port/time.h>
#if AG_PORT_HAS_WIFI
#include <argon/port/wifi.h>
#endif

/* ------------------------------------------------------------------------ */
/* The built-in provider: the port, wrapped so it has the ag_net_ops_t shape. */
/* The `dev` argument is ignored - the port keeps its own singleton state.    */
/* ------------------------------------------------------------------------ */

static ag_err_t bi_start(ag_device_t *dev)
{
    (void)dev;
    return ag_port_net_start();
}
static bool bi_ready(ag_device_t *dev)
{
    (void)dev;
    return ag_port_net_ready();
}
static ag_err_t bi_ifaddr(ag_device_t *dev, uint32_t *addr)
{
    (void)dev;
    return ag_port_net_ifaddr(addr);
}
static ag_err_t bi_resolve(ag_device_t *dev, const char *host, uint32_t *addr)
{
    (void)dev;
    return ag_port_net_resolve(host, addr);
}
static int bi_listen(ag_device_t *dev, uint16_t port)
{
    (void)dev;
    return ag_port_net_listen(port);
}
static int bi_accept(ag_device_t *dev, int lfd, uint32_t timeout_ms)
{
    (void)dev;
    return ag_port_net_accept(lfd, timeout_ms);
}
static int bi_connect(ag_device_t *dev, uint32_t addr, uint16_t port,
                      uint32_t timeout_ms)
{
    (void)dev;
    return ag_port_net_connect(addr, port, timeout_ms);
}
static int32_t bi_send(ag_device_t *dev, int fd, const void *buf, size_t len)
{
    (void)dev;
    return ag_port_net_send(fd, buf, len);
}
static int32_t bi_recv(ag_device_t *dev, int fd, void *buf, size_t len)
{
    (void)dev;
    return ag_port_net_recv(fd, buf, len);
}
static void bi_close(ag_device_t *dev, int fd)
{
    (void)dev;
    ag_port_net_close(fd);
}
static ag_err_t bi_nonblock(ag_device_t *dev, int fd, bool on)
{
    (void)dev;
    return ag_port_net_nonblock(fd, on);
}
static int bi_wait_readable(ag_device_t *dev, int fd, uint32_t timeout_ms)
{
    (void)dev;
    return ag_port_net_wait_readable(fd, timeout_ms);
}
static int32_t bi_recv_now(ag_device_t *dev, int fd, void *buf, size_t len)
{
    (void)dev;
    return ag_port_net_recv_now(fd, buf, len);
}

#if AG_PORT_HAS_WIFI
/*
 * The built-in station, in the ABI's shape.  This conversion used to live in
 * the ABI table (src/loader/api.c) because that was its only caller; it is here
 * now because the shell needs the same answer from whichever radio is bound,
 * and there must be exactly one place that turns a port status into an ABI one.
 *
 * The ABI cannot name the GPL port types, so the two are field-for-field
 * parallel and copied across - the same arrangement as before, moved.
 */
static ag_err_t bi_radio_up(void)
{
    ag_powerctl_bus_needed();
    ag_port_wifi_status_t st;
    if (ag_port_wifi_status(&st) != AG_OK || st.state == AG_WIFI_OFF) {
        return ag_net_init();
    }
    return AG_OK;
}

static ag_err_t bi_wifi_start(ag_device_t *dev)
{
    (void)dev;
    return bi_radio_up();
}

static ag_err_t bi_wifi_stop(ag_device_t *dev)
{
    (void)dev;
    return ag_port_wifi_stop();
}

static ag_err_t bi_wifi_scan(ag_device_t *dev, ag_wifi_ap_t *out, uint32_t max,
                             uint32_t *found)
{
    (void)dev;
    if (out == NULL || max == 0u) {
        return -AG_EINVAL;
    }
    const ag_err_t up = bi_radio_up();
    if (up != AG_OK) {
        return up;
    }
    /*
     * No heap.  The port fills at most sixteen records from a stack buffer of
     * its own, so a matching stack buffer converts them without an allocation -
     * which matters because with the radio up and an application resident, a few
     * hundred spare bytes of heap is exactly what there is not, and a scan that
     * failed only because its conversion buffer would not allocate reads as
     * -AG_ENOMEM for a scan that would have worked.
     */
    ag_port_wifi_ap_t tmp[16];
    const uint32_t    cap = (max < 16u) ? max : 16u;
    uint32_t          n = 0;
    const ag_err_t    err = ag_port_wifi_scan(tmp, cap, &n);
    if (err != AG_OK) {
        return err;
    }
    const uint32_t copy = (n < cap) ? n : cap;
    for (uint32_t i = 0; i < copy; i++) {
        memcpy(out[i].ssid, tmp[i].ssid, sizeof(out[i].ssid));
        memcpy(out[i].bssid, tmp[i].bssid, 6u);
        out[i].rssi = tmp[i].rssi;
        out[i].channel = tmp[i].channel;
        out[i].auth = (uint8_t)tmp[i].auth;
    }
    if (found != NULL) {
        *found = n; /* every access point the radio heard, not every one copied */
    }
    return AG_OK;
}

static ag_err_t bi_wifi_connect(ag_device_t *dev, const char *ssid,
                                const char *pass, const uint8_t bssid[6])
{
    (void)dev;
    ag_powerctl_bus_needed();
    return ag_port_wifi_connect(ssid, pass, bssid);
}

static ag_err_t bi_wifi_disconnect(ag_device_t *dev)
{
    (void)dev;
    return ag_port_wifi_disconnect();
}

static ag_err_t bi_wifi_status(ag_device_t *dev, ag_wifi_status_t *out)
{
    (void)dev;
    if (out == NULL) {
        return -AG_EINVAL;
    }
    ag_port_wifi_status_t st;
    const ag_err_t        err = ag_port_wifi_status(&st);
    if (err != AG_OK) {
        return err;
    }
    memset(out, 0, sizeof(*out));
    out->state = (uint8_t)st.state;
    memcpy(out->ssid, st.ssid, sizeof(out->ssid));
    memcpy(out->bssid, st.bssid, 6u);
    out->pinned = st.pinned;
    out->rssi = st.rssi;
    out->channel = st.channel;
    out->attempts = st.attempts;
    out->last_reason = (int32_t)st.last_reason;
    return AG_OK;
}
#endif /* AG_PORT_HAS_WIFI */

static const ag_net_ops_t s_builtin = {
    .size = sizeof(ag_net_ops_t),
    .start = bi_start,
    .ready = bi_ready,
    .ifaddr = bi_ifaddr,
    .resolve = bi_resolve,
    .listen = bi_listen,
    .accept = bi_accept,
    .connect = bi_connect,
    .send = bi_send,
    .recv = bi_recv,
    .net_close = bi_close,
    .nonblock = bi_nonblock,
    .wait_readable = bi_wait_readable,
    .recv_now = bi_recv_now,
#if AG_PORT_HAS_WIFI
    .wifi_start = bi_wifi_start,
    .wifi_stop = bi_wifi_stop,
    .wifi_scan = bi_wifi_scan,
    .wifi_connect = bi_wifi_connect,
    .wifi_disconnect = bi_wifi_disconnect,
    .wifi_status = bi_wifi_status,
#endif
    /* Without a radio on this chip the six stay NULL, and the AG_HAS probe
     * below reports no station - the same answer a GSM modem gets. */
};

/* ------------------------------------------------------------------------ */
/* Active provider.  NULL device means "the built-in one".                   */
/* ------------------------------------------------------------------------ */

static const ag_net_ops_t *s_ops = &s_builtin;
static ag_device_t        *s_dev; /* NULL for built-in */
static char                s_name[AG_DEV_NAME_MAX] = "builtin";

/*
 * The provider, held for one call into it: an unload waits until it is let go
 * (ag_dev_call_begin), as linkprov's wires are.  on_revoke only swings the
 * pointers back to the built-in stack, and a task that read them a moment
 * earlier - an accept or a recv sits inside the radio's code for its whole
 * timeout - was still in the image when `drv unload` gave it back.
 *
 * The pointers are read once and checked again once held, so a provider
 * switched in between is not called with the other one's ops.  The built-in
 * stack is part of the kernel and is not counted; a NULL device with ops that
 * are not the built-in ones is a switch half-seen, and is refused like any
 * other.  Even the AG_HAS probes hold: the ops table is the driver's own data.
 */
typedef struct {
    const ag_net_ops_t *ops;
    ag_device_t        *dev;
} held_t;

static bool hold(held_t *h)
{
    h->ops = s_ops;
    h->dev = s_dev;
    if (h->dev == NULL) {
        return h->ops == &s_builtin;
    }
    if (h->ops == NULL || !ag_dev_call_begin(h->dev)) {
        return false;
    }
    if (s_ops != h->ops || s_dev != h->dev) {
        ag_dev_call_end(h->dev);
        return false;
    }
    return true;
}

static void let_go(held_t *h) { ag_dev_call_end(h->dev); } /* NULL: no-op */

/* ------------------------------------------------------------------------ */
/* Dispatch.  One indirect call each; the client above cannot tell which     */
/* provider it reached.  A provider that went away mid-call answers ENODEV.   */
/* ------------------------------------------------------------------------ */

ag_err_t ag_netprov_start(void)
{
    held_t h;
    if (!hold(&h)) {
        return -AG_ENODEV;
    }
    const ag_err_t e = h.ops->start(h.dev);
    let_go(&h);
    return e;
}
bool ag_netprov_ready(void)
{
    held_t h;
    if (!hold(&h)) {
        return false;
    }
    const bool r = h.ops->ready(h.dev);
    let_go(&h);
    return r;
}
ag_err_t ag_netprov_ifaddr(uint32_t *addr)
{
    held_t h;
    if (!hold(&h)) {
        return -AG_ENODEV;
    }
    const ag_err_t e = h.ops->ifaddr(h.dev, addr);
    let_go(&h);
    return e;
}
ag_err_t ag_netprov_resolve(const char *host, uint32_t *addr)
{
    held_t h;
    if (!hold(&h)) {
        return -AG_ENODEV;
    }
    const ag_err_t e = h.ops->resolve(h.dev, host, addr);
    let_go(&h);
    return e;
}
int ag_netprov_listen(uint16_t port)
{
    held_t h;
    if (!hold(&h)) {
        return -AG_ENODEV;
    }
    const int r = h.ops->listen(h.dev, port);
    let_go(&h);
    return r;
}
int ag_netprov_accept(int lfd, uint32_t timeout_ms)
{
    held_t h;
    if (!hold(&h)) {
        return -AG_ENODEV;
    }
    const int r = h.ops->accept(h.dev, lfd, timeout_ms);
    let_go(&h);
    return r;
}
int ag_netprov_connect(uint32_t addr, uint16_t port, uint32_t timeout_ms)
{
    held_t h;
    if (!hold(&h)) {
        return -AG_ENODEV;
    }
    const int r = h.ops->connect(h.dev, addr, port, timeout_ms);
    let_go(&h);
    return r;
}
int32_t ag_netprov_send(int fd, const void *buf, size_t len)
{
    held_t h;
    if (!hold(&h)) {
        return -AG_ENODEV;
    }
    const int32_t r = h.ops->send(h.dev, fd, buf, len);
    let_go(&h);
    return r;
}
int32_t ag_netprov_recv(int fd, void *buf, size_t len)
{
    held_t h;
    if (!hold(&h)) {
        return -AG_ENODEV;
    }
    const int32_t r = h.ops->recv(h.dev, fd, buf, len);
    let_go(&h);
    return r;
}
/*
 * Also reached from on_revoke, through ag_net_reset_sockets, with the registry
 * lock held: the watchers run before the slot is marked revoked, so the hold
 * still succeeds there and the going radio closes its own sockets, as before.
 */
void ag_netprov_close(int fd)
{
    held_t h;
    if (!hold(&h)) {
        return;
    }
    h.ops->net_close(h.dev, fd);
    let_go(&h);
}
/* net->reset (ABI 0.50): a connection thrown away rather than closed.  The
 * built-in stack can; a provider across a wire has only its close. */
void ag_netprov_close_hard(int fd)
{
    if (ag_netprov_is_builtin()) {
        ag_port_net_close_hard(fd);
        return;
    }
    ag_netprov_close(fd);
}
ag_err_t ag_netprov_nonblock(int fd, bool on)
{
    held_t h;
    if (!hold(&h)) {
        return -AG_ENODEV;
    }
    const ag_err_t e = h.ops->nonblock(h.dev, fd, on);
    let_go(&h);
    return e;
}
int ag_netprov_wait_readable(int fd, uint32_t timeout_ms)
{
    held_t h;
    if (!hold(&h)) {
        return -AG_ENODEV;
    }
    const int r = h.ops->wait_readable(h.dev, fd, timeout_ms);
    let_go(&h);
    return r;
}
int32_t ag_netprov_recv_now(int fd, void *buf, size_t len)
{
    held_t h;
    if (!hold(&h)) {
        return -AG_ENODEV;
    }
    const int32_t r = h.ops->recv_now(h.dev, fd, buf, len);
    let_go(&h);
    return r;
}

/* ------------------------------------------------------------------------ */
/* The station half.  AG_HAS on every call and not once at bind time: the     */
/* provider can change under a caller that is holding no lock, and the six    */
/* come as a set, so probing one of them answers for all.  The probe is made  */
/* on the held ops, so the answer is about the table that is then called.     */
/* ------------------------------------------------------------------------ */

bool ag_netprov_has_wifi(void)
{
    held_t h;
    if (!hold(&h)) {
        return false;
    }
    const bool r = AG_HAS(h.ops, wifi_status);
    let_go(&h);
    return r;
}

ag_err_t ag_netprov_wifi_start(void)
{
    held_t h;
    if (!hold(&h)) {
        return -AG_ENODEV;
    }
    const ag_err_t e =
        AG_HAS(h.ops, wifi_start) ? h.ops->wifi_start(h.dev) : -AG_ENODEV;
    let_go(&h);
    return e;
}

ag_err_t ag_netprov_wifi_stop(void)
{
    held_t h;
    if (!hold(&h)) {
        return -AG_ENODEV;
    }
    const ag_err_t e =
        AG_HAS(h.ops, wifi_stop) ? h.ops->wifi_stop(h.dev) : -AG_ENODEV;
    let_go(&h);
    return e;
}

ag_err_t ag_netprov_wifi_scan(ag_wifi_ap_t *out, uint32_t max, uint32_t *found)
{
    held_t h;
    if (!hold(&h)) {
        return -AG_ENODEV;
    }
    const ag_err_t e = AG_HAS(h.ops, wifi_scan)
                           ? h.ops->wifi_scan(h.dev, out, max, found)
                           : -AG_ENODEV;
    let_go(&h);
    return e;
}

ag_err_t ag_netprov_wifi_connect(const char *ssid, const char *pass,
                                 const uint8_t bssid[6])
{
    held_t h;
    if (!hold(&h)) {
        return -AG_ENODEV;
    }
    const ag_err_t e = AG_HAS(h.ops, wifi_connect)
                           ? h.ops->wifi_connect(h.dev, ssid, pass, bssid)
                           : -AG_ENODEV;
    let_go(&h);
    return e;
}

ag_err_t ag_netprov_wifi_disconnect(void)
{
    held_t h;
    if (!hold(&h)) {
        return -AG_ENODEV;
    }
    const ag_err_t e = AG_HAS(h.ops, wifi_disconnect)
                           ? h.ops->wifi_disconnect(h.dev)
                           : -AG_ENODEV;
    let_go(&h);
    return e;
}

ag_err_t ag_netprov_wifi_status(ag_wifi_status_t *out)
{
    held_t h;
    if (!hold(&h)) {
        return -AG_ENODEV;
    }
    const ag_err_t e = AG_HAS(h.ops, wifi_status)
                           ? h.ops->wifi_status(h.dev, out)
                           : -AG_ENODEV;
    let_go(&h);
    return e;
}

/* ------------------------------------------------------------------------ */
/* Control.                                                                   */
/* ------------------------------------------------------------------------ */

const char *ag_netprov_active(void) { return s_name; }
bool ag_netprov_is_builtin(void) { return s_dev == NULL; }

/*
 * The bound radio went away - `drv unload EXTRADIO` while `net use extradio`
 * was in force, or the hardware did.  Fall back to the built-in stack before
 * the pointer becomes an address in an image that has been given back.
 *
 * This was latent here before the node link was written and found the same
 * shape of bug next door: the sockets that were open belong to the interface
 * that has gone, so they go too, exactly as they do on a deliberate switch.
 *
 * Called with the registry lock held: drop the pointer and return, and do not
 * reach back into the registry.  ag_port_net_start is left to whoever next
 * wants an address; doing it here would be work under a lock for a board that
 * may have no built-in radio at all.
 */
static void on_revoke(ag_device_t *dev)
{
    if (dev == NULL || dev != s_dev) {
        return;
    }
    ag_net_reset_sockets();
    s_ops = &s_builtin;
    s_dev = NULL;
    strcpy(s_name, "builtin");
    ag_log(AG_LOG_WARN, "net", "the bound radio went away; back to builtin");
}

/*
 * Every function pointer the kernel actually dials must be present; a NULL one
 * would fault the first time a socket was opened, and the operator should be
 * told at bind time instead.
 *
 * The size test is against the SOCKET half only, deliberately.  It used to read
 * `>= sizeof(ag_net_ops_t)`, which was the same thing until 0.47 appended the
 * station entries - and would then have refused, at bind time, every driver
 * built before the append.  That is exactly the breakage the size field exists
 * to prevent: a table is long enough when it holds what the caller is about to
 * read, not when it matches today's header.  The station half is probed
 * separately, per call, with AG_HAS.
 */
#define NET_OPS_SOCKET_SIZE                                                   \
    (offsetof(ag_net_ops_t, recv_now) + sizeof(((ag_net_ops_t *)0)->recv_now))

static bool ops_complete(const ag_net_ops_t *o)
{
    return o != NULL && o->size >= NET_OPS_SOCKET_SIZE && o->start &&
           o->ready && o->ifaddr && o->resolve && o->listen && o->accept &&
           o->connect && o->send && o->recv && o->net_close && o->nonblock &&
           o->wait_readable && o->recv_now;
}

/* Print the address once it is up, so `net use` reads like a link coming on.
 * Bounded: a provider that never gets an address returns after the budget and
 * the operator can query later with `net`. */
static void announce_when_ready(void)
{
    const ag_port_ticks_t start = ag_port_ticks();
    const ag_port_ticks_t budget = ag_port_ms_to_ticks(4000);
    while (!ag_netprov_ready()) {
        if ((ag_port_ticks() - start) >= budget) {
            ag_log(AG_LOG_INFO, "net", "%s: no address yet", s_name);
            return;
        }
        ag_port_task_delay(ag_port_ms_to_ticks(50));
    }
    uint32_t addr = 0;
    if (ag_netprov_ifaddr(&addr) == AG_OK) {
        ag_log(AG_LOG_INFO, "net", "%s ip %u.%u.%u.%u", s_name,
               (unsigned)(addr >> 24), (unsigned)((addr >> 16) & 0xffu),
               (unsigned)((addr >> 8) & 0xffu), (unsigned)(addr & 0xffu));
    }
}

ag_err_t ag_netprov_use_builtin(void)
{
    if (s_dev == NULL) {
        return AG_OK; /* already the built-in one */
    }
    ag_net_reset_sockets();
    s_ops = &s_builtin;
    s_dev = NULL;
    strcpy(s_name, "builtin");
    return ag_port_net_start();
}

ag_err_t ag_netprov_use_device(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return -AG_EINVAL;
    }
    if (strcmp(name, "builtin") == 0) {
        return ag_netprov_use_builtin();
    }

    (void)ag_dev_on_revoke(on_revoke);

    ag_device_t *dev = ag_dev_find(name);
    if (dev == NULL) {
        return -AG_ENODEV;
    }
    if (dev->cls != AG_DEV_NET) {
        ag_log(AG_LOG_ERROR, "net", "%s is not a network device", name);
        return -AG_EINVAL;
    }
    /* The table is the driver's own data: read it counted, like a call.  If
     * the driver goes between here and the bind, on_revoke does not know it
     * yet - but the start below cannot hold it, fails, and falls back. */
    if (!ag_dev_call_begin(dev)) {
        return -AG_ENODEV;
    }
    const ag_net_ops_t *ops = (const ag_net_ops_t *)dev->class_ops;
    const bool          complete = ops_complete(ops);
    ag_dev_call_end(dev);
    if (!complete) {
        ag_log(AG_LOG_ERROR, "net", "%s: incomplete net ops", name);
        return -AG_ENOTSUP;
    }

    ag_net_reset_sockets();
    s_ops = ops;
    s_dev = dev;
    strncpy(s_name, dev->name, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';

    const ag_err_t e = ag_netprov_start();
    if (e != AG_OK) {
        ag_log(AG_LOG_ERROR, "net", "%s did not start: %d", s_name, (int)e);
        /* Fall back so the system is never left with a dead provider. */
        (void)ag_netprov_use_builtin();
        return e;
    }
    announce_when_ready();
    return AG_OK;
}

#endif /* CONFIG_ARGON_ENABLE_NET */
