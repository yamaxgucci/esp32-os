/*
 * ArgonOS - KVM.AXE: a phone types on the computer this board is plugged into.
 *
 *   run kvm.axe            serve on port 80
 *   run kvm.axe 8080       somewhere else
 *
 * The board is two things at once and neither of them knows about the other.
 * On its OTG port it is a USB keyboard and mouse, which is what /dev/usbhid0
 * is; on its radio it is a web server, which is what this file mostly is.  In
 * between there is one line of text per event and a table that turns it into a
 * HID report (apps/common/kvm).
 *
 * What this buys over any remote desktop is one thing and it is the whole
 * point: the far machine runs nothing.  No agent, no driver, no port open, no
 * account - it sees a keyboard.  So it works at the BIOS, at the boot menu, at
 * the login screen, at a blue screen and at a desktop that has stopped
 * answering, which is the entire list of moments when a remote desktop is what
 * you actually needed and cannot have.
 *
 * What it does not buy, and will not: a picture.  A PC does not send its screen
 * out over USB - there is no such thing as a USB display class, and everything
 * sold as one carries a driver for the far machine.  That is M2's problem and
 * it is solved by different means; see docs/14-usb-kvm.md.
 *
 * The connection is a WebSocket rather than a request per event.  A finger on a
 * trackpad produces something like sixty events a second, and sixty sets of
 * HTTP headers and sixty round trips a second is the difference between a
 * pointer that follows a finger and one that arrives after it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/argon.h>
#include <argon/libc.h>

#include <stdio.h>
#include <string.h>

#include "ag_kvm.h"
#include "ag_ws.h"
#include "kvm_page.h"

AG_APP("KVM", "1.0", "argon", AG_AXE_NEEDS_NET);

#define KVM_PORT 80u

#define REQ_MAX 1024u  /* an HTTP request that does not fit is not one of ours */
#define WS_RX 1024u    /* frames in flight; an event is about a dozen bytes    */
#define FRAMES_MAX 12  /* reports one line can produce                        */

#define ACCEPT_MS 15u
#define REQ_TIMEOUT_MS 3000u
#define SEND_TIMEOUT_MS 3000u
#define STATUS_MS 1000u

/*
 * How long a report may wait for the endpoint.  One report per interface per
 * millisecond is what a full-speed interrupt endpoint gives, so a burst of a
 * dozen needs a dozen milliseconds; beyond that something is wrong with the
 * cable rather than busy, and holding the socket while it sorts itself out
 * would make the next event later still.
 */
#define WRITE_BUDGET_MS 250u

typedef struct {
    ag_handle_t dev;  /* /dev/usbhid0                                       */
    ag_handle_t lfd;  /* the listening socket                               */
    ag_handle_t ws;   /* the phone, or -1                                   */

    uint8_t *rx;   /* WS_RX  */
    char    *req;  /* REQ_MAX */
    void    *mem;

    size_t rx_n;

    ag_kvm_state_t st;
    uint32_t       lines;
    uint32_t       reports;
    uint32_t       refused;
} kvm_t;

/* ------------------------------------------------------------------------ */
/* Sockets                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * A whole buffer out, or a failure.  The sockets here are non-blocking, so a
 * short send is normal and not an error; what is an error is giving up on half
 * a WebSocket frame, because the far end has no way to resynchronise.
 */
static bool send_all(ag_handle_t s, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t         off = 0;
    const uint32_t deadline = ag_millis() + SEND_TIMEOUT_MS;

    while (off < len) {
        const int32_t n = ag_net_send(s, p + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n == -AG_EAGAIN) {
            if ((int32_t)(ag_millis() - deadline) >= 0) {
                return false;
            }
            ag_delay(2);
            continue;
        }
        return false;
    }
    return true;
}

static bool ws_send(kvm_t *k, uint8_t opcode, const void *payload, size_t len)
{
    uint8_t        hdr[4];
    const uint32_t hn = ag_ws_header(hdr, opcode, len);

    if (k->ws < 0 || hn == 0) {
        return false;
    }
    if (!send_all(k->ws, hdr, hn)) {
        return false;
    }
    return len == 0 || send_all(k->ws, payload, len);
}

/* ------------------------------------------------------------------------ */
/* The device                                                               */
/* ------------------------------------------------------------------------ */

static void send_frames(kvm_t *k, const ag_usbhid_frame_t *f, int n)
{
    if (n <= 0) {
        return;
    }

    const uint8_t *p = (const uint8_t *)f;
    const size_t   total = (size_t)n * AG_USBHID_FRAME;
    size_t         off = 0;
    const uint32_t deadline = ag_millis() + WRITE_BUDGET_MS;

    while (off < total) {
        const int32_t w = ag_dev_write(k->dev, p + off, total - off);
        if (w > 0) {
            off += (size_t)w;
            k->reports += (uint32_t)(w / (int32_t)AG_USBHID_FRAME);
            continue;
        }
        if (w == -AG_EAGAIN) {
            if ((int32_t)(ag_millis() - deadline) >= 0) {
                break;
            }
            ag_delay(1);
            continue;
        }
        /*
         * -AG_ENODEV: the cable is out, or the far machine has not enumerated
         * us.  Not worth a message per event - the phone sees it in the status
         * line, which is where a person is already looking.
         */
        break;
    }
    if (off < total) {
        k->refused += (uint32_t)((total - off) / AG_USBHID_FRAME);
    }
}

/* Everything up: sent when the phone goes away, however it goes away. */
static void release_all(kvm_t *k)
{
    ag_usbhid_frame_t f[FRAMES_MAX];
    const int         n = ag_kvm_parse("R", 1, &k->st, f, FRAMES_MAX);
    if (n > 0) {
        send_frames(k, f, n);
    }
}

/* ------------------------------------------------------------------------ */
/* One line from the phone                                                  */
/* ------------------------------------------------------------------------ */

/*
 * "T <text>" is handled here rather than in the parser because it is the one
 * line that produces an unbounded number of reports: a pasted password is two
 * frames per character and there is no buffer worth sizing for the longest
 * thing somebody might paste.  So it is a loop over ag_kvm_type_frames, which
 * stops when the buffer is full and says where it got to.
 */
static void type_text(kvm_t *k, const char *text, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ag_usbhid_frame_t f[FRAMES_MAX];
        size_t            used = 0;
        const int n = ag_kvm_type_frames(&k->st.kbd, text + off, len - off, f,
                                         FRAMES_MAX, &used);
        if (n <= 0 || used == 0) {
            break;
        }
        send_frames(k, f, n);
        off += used;
    }
}

static void handle_line(kvm_t *k, const char *line, size_t len)
{
    k->lines++;

    if (len >= 2 && line[0] == 'T' && line[1] == ' ') {
        type_text(k, line + 2, len - 2);
        return;
    }

    ag_usbhid_frame_t f[FRAMES_MAX];
    const int         n = ag_kvm_parse(line, len, &k->st, f, FRAMES_MAX);
    if (n > 0) {
        send_frames(k, f, n);
    }
}

/* ------------------------------------------------------------------------ */
/* HTTP                                                                     */
/* ------------------------------------------------------------------------ */

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/*
 * The value of one header, without a copy.  Header names are case insensitive
 * and browsers do not agree on which case they use, so the comparison is too.
 * `name` is lower case and includes the colon.
 */
static const char *hdr_value(const char *buf, size_t len, const char *name,
                             size_t *out_len)
{
    const size_t nlen = strlen(name);

    for (size_t i = 0; i + nlen <= len; i++) {
        if (i > 0 && buf[i - 1] != '\n') {
            continue;
        }
        size_t j = 0;
        while (j < nlen && lower(buf[i + j]) == name[j]) {
            j++;
        }
        if (j != nlen) {
            continue;
        }
        size_t v = i + nlen;
        while (v < len && (buf[v] == ' ' || buf[v] == '\t')) {
            v++;
        }
        size_t e = v;
        while (e < len && buf[e] != '\r' && buf[e] != '\n') {
            e++;
        }
        *out_len = e - v;
        return buf + v;
    }
    *out_len = 0;
    return NULL;
}

/* Reads until the blank line that ends the headers.  Returns the length, or 0. */
static size_t read_request(ag_handle_t c, char *buf, size_t cap)
{
    size_t         n = 0;
    const uint32_t deadline = ag_millis() + REQ_TIMEOUT_MS;

    while (n + 1 < cap) {
        const int32_t r = ag_net_recv(c, buf + n, cap - 1 - n);
        if (r > 0) {
            n += (size_t)r;
            buf[n] = '\0';
            if (n >= 4 && strstr(buf, "\r\n\r\n") != NULL) {
                return n;
            }
            continue;
        }
        if (r == -AG_EAGAIN) {
            if ((int32_t)(ag_millis() - deadline) >= 0) {
                return 0;
            }
            ag_delay(2);
            continue;
        }
        return 0;
    }
    return 0;
}

static void serve_page(ag_handle_t c)
{
    char         head[160];
    const size_t body = sizeof(k_page) - 1u;
    const int    hn = snprintf(head, sizeof(head),
                               "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/html; charset=utf-8\r\n"
                               "Content-Length: %u\r\n"
                               "Cache-Control: no-store\r\n"
                               "Connection: close\r\n\r\n",
                               (unsigned)body);
    if (hn <= 0) {
        return;
    }
    if (send_all(c, head, (size_t)hn)) {
        (void)send_all(c, k_page, body);
    }
}

static void serve_404(ag_handle_t c)
{
    static const char msg[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                              "Connection: close\r\n\r\n";
    (void)send_all(c, msg, sizeof(msg) - 1u);
}

/* ------------------------------------------------------------------------ */

static void ws_drop(kvm_t *k, const char *why)
{
    if (k->ws < 0) {
        return;
    }
    ag_printf("phone gone (%s)\n", why);
    release_all(k);
    (void)ag_net_close(k->ws);
    k->ws = -1;
    k->rx_n = 0;
}

static void ws_adopt(kvm_t *k, ag_handle_t c)
{
    if (k->ws >= 0) {
        /* One phone at a time, and the newest wins: a page left open on a
         * laptop must not lock out the phone in somebody's hand. */
        ws_drop(k, "replaced");
    }
    k->ws = c;
    k->rx_n = 0;
    ag_kvm_reset(&k->st);
    ag_printf("phone connected\n");
}

static void ws_service(kvm_t *k)
{
    if (k->ws < 0) {
        return;
    }

    for (;;) {
        if (k->rx_n < WS_RX) {
            const int32_t r =
                ag_net_recv(k->ws, k->rx + k->rx_n, WS_RX - k->rx_n);
            if (r > 0) {
                k->rx_n += (size_t)r;
            } else if (r == -AG_EAGAIN) {
                break;
            } else {
                ws_drop(k, "closed");
                return;
            }
        }

        /* Take whole frames off the front until what is left is a fragment. */
        size_t off = 0;
        for (;;) {
            ag_ws_frame_t f;
            const int32_t used = ag_ws_decode(k->rx + off, k->rx_n - off, &f);
            if (used == 0) {
                break;
            }
            if (used < 0) {
                ws_drop(k, "bad frame");
                return;
            }
            switch (f.opcode) {
            case AG_WS_TEXT:
                handle_line(k, (const char *)f.payload, f.len);
                break;
            case AG_WS_PING:
                (void)ws_send(k, AG_WS_PONG, f.payload, f.len);
                break;
            case AG_WS_CLOSE:
                (void)ws_send(k, AG_WS_CLOSE, NULL, 0);
                ws_drop(k, "goodbye");
                return;
            default:
                break; /* a pong, or something we do not speak */
            }
            off += (size_t)used;
        }
        if (off > 0) {
            memmove(k->rx, k->rx + off, k->rx_n - off);
            k->rx_n -= off;
        } else if (k->rx_n >= WS_RX) {
            /* A frame larger than the buffer cannot arrive: nothing this
             * protocol sends is that big, so the peer is not one of ours. */
            ws_drop(k, "frame too large");
            return;
        }
    }
}

static void push_status(kvm_t *k)
{
    ag_usbhid_status_t st;
    char               line[64];

    memset(&st, 0, sizeof(st));
    if (ag_dev_ioctl(k->dev, AG_IOC_USBHID_STATUS, &st, sizeof(st)) != AG_OK) {
        st.state = AG_USBHID_OFF;
    }
    const int n = snprintf(line, sizeof(line), "S %u %u %u %u",
                           (unsigned)st.state, (unsigned)k->reports,
                           (unsigned)k->refused, (unsigned)st.leds);
    if (n > 0) {
        (void)ws_send(k, AG_WS_TEXT, line, (size_t)n);
    }
}

/* ------------------------------------------------------------------------ */

int ag_main(int argc, char **argv)
{
    uint16_t port = KVM_PORT;

    for (int i = 1; i < argc; i++) {
        long p = 0;
        for (const char *c = argv[i]; *c >= '0' && *c <= '9'; c++) {
            p = p * 10 + (*c - '0');
        }
        if (p <= 0 || p > 65535) {
            ag_printf("usage: kvm [port]\n");
            return 1;
        }
        port = (uint16_t)p;
    }

    kvm_t k;
    memset(&k, 0, sizeof(k));
    k.dev = -1;
    k.lfd = -1;
    k.ws = -1;
    ag_kvm_reset(&k.st);

    k.dev = ag_dev_open("usbhid0");
    if (k.dev < 0) {
        ag_printf("no /dev/usbhid0 (%d)\n", (int)k.dev);
        ag_printf("this image has no USB device stack, or something holds it.\n");
        ag_printf("in the emulator: drv install h:\\usbhidvirt.sys\n");
        return 1;
    }

    if (!ag_net_is_ready() && ag_net_wait_ready(8000) != AG_OK) {
        ag_printf("no network (is the radio up and joined?)\n");
        (void)ag_dev_close(k.dev);
        return 1;
    }

    k.mem = ag_malloc(WS_RX + REQ_MAX);
    if (k.mem == NULL) {
        ag_printf("not enough memory\n");
        (void)ag_dev_close(k.dev);
        return 1;
    }
    k.rx = (uint8_t *)k.mem;
    k.req = (char *)k.mem + WS_RX;

    k.lfd = ag_tcp_listen(port);
    if (k.lfd < 0) {
        ag_printf("port %u is not available (%d)\n", (unsigned)port,
                  (int)k.lfd);
        ag_free(k.mem);
        (void)ag_dev_close(k.dev);
        return 1;
    }

    {
        uint32_t addr = 0;
        char     ip[16] = "0.0.0.0";
        /* Four bytes, most significant first - the kernel's ag_ipv4_str reads
         * it the same way.  Written out here rather than linking netmsg for
         * one line of formatting. */
        if (ag_net_ifaddr(&addr) == AG_OK) {
            (void)snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                           (unsigned)((addr >> 24) & 0xFFu),
                           (unsigned)((addr >> 16) & 0xFFu),
                           (unsigned)((addr >> 8) & 0xFFu),
                           (unsigned)(addr & 0xFFu));
        }
        ag_printf("keyboard and mouse for the PC on the OTG port\n");
        ag_printf("open http://%s:%u/ on the phone\n", ip, (unsigned)port);
        ag_printf("Ctrl+C to stop\n");
    }

    uint32_t next_status = ag_millis();

    while (!ag_interrupted()) {
        const ag_handle_t c = ag_tcp_accept(k.lfd, ACCEPT_MS);
        if (c >= 0) {
            (void)ag_net_set_nonblock(c, true);

            const size_t n = read_request(c, k.req, REQ_MAX);
            bool         adopted = false;

            if (n > 0) {
                size_t      klen = 0;
                const char *key =
                    hdr_value(k.req, n, "sec-websocket-key:", &klen);
                if (key != NULL && klen > 0 && klen < 64) {
                    char accept[AG_WS_ACCEPT_LEN];
                    char resp[200];
                    if (ag_ws_accept_key(key, klen, accept)) {
                        const int rn =
                            snprintf(resp, sizeof(resp),
                                     "HTTP/1.1 101 Switching Protocols\r\n"
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Accept: %s\r\n\r\n",
                                     accept);
                        if (rn > 0 && send_all(c, resp, (size_t)rn)) {
                            ws_adopt(&k, c);
                            adopted = true;
                        }
                    }
                } else if (strncmp(k.req, "GET / ", 6) == 0 ||
                           strncmp(k.req, "GET /index.html ", 16) == 0) {
                    serve_page(c);
                } else {
                    serve_404(c);
                }
            }
            if (!adopted) {
                (void)ag_net_close(c);
            }
        }

        ws_service(&k);

        if ((int32_t)(ag_millis() - next_status) >= 0) {
            next_status = ag_millis() + STATUS_MS;
            if (k.ws >= 0) {
                push_status(&k);
            }
        }
    }

    ws_drop(&k, "stopping");
    (void)ag_net_close(k.lfd);
    ag_free(k.mem);
    (void)ag_dev_close(k.dev);
    ag_printf("stopped: %u lines, %u reports, %u lost\n", (unsigned)k.lines,
              (unsigned)k.reports, (unsigned)k.refused);
    return 0;
}
