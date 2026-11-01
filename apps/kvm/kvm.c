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
#include "ag_vid.h"
#include "ag_kvmws.h"
#include "kvm_page.h"

AG_APP("KVM", "1.0", "argon", AG_AXE_NEEDS_NET);

#define KVM_PORT 80u

#define REQ_MAX 1024u  /* an HTTP request that does not fit is not one of ours */
#define WS_RX 4096u    /* frames in flight; an event is a dozen bytes, a
                        * clipboard paste rather more                        */
#define FRAMES_MAX 12  /* reports one line can produce                        */
#define VID_CHUNK 1024u /* one read off the serial pipe                       */

/*
 * How many picture messages may be forwarded before going round the loop
 * again.  There has to be a limit: the far machine can produce tiles faster
 * than anything here consumes them, and a loop that forwarded until the pipe
 * ran dry would stop reading the phone's keystrokes while it did.  Input is
 * the thing that must never wait.
 */
#define VID_BURST 8

/* How long the loop sleeps when nothing at all happened.  Short, because it
 * is also the worst case a keystroke waits before anything looks at it. */
#define IDLE_MS 4u
#define REQ_TIMEOUT_MS 3000u
#define SEND_TIMEOUT_MS 3000u
#define STATUS_MS 1000u

/*
 * How long a picture message may take to reach the phone before the phone is
 * declared gone.  Long, on purpose: see the note above ws_send_ms.
 */
#define VID_SEND_MS 15000u

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
    ag_handle_t vid;  /* /dev/usbcdc0, or -1 when this build has no pipe    */
    ag_handle_t lfd;  /* the listening socket                               */
    ag_handle_t ws;   /* the phone, or -1                                   */

    uint8_t *rx;    /* WS_RX                                                */
    char    *req;   /* REQ_MAX                                              */
    uint8_t *vbuf;  /* AG_VID_MAX_MSG - the framer's buffer                 */
    uint8_t *vchunk; /* VID_CHUNK                                           */
    void    *mem;

    size_t rx_n;

    ag_vid_rx_t vrx;

    ag_kvm_state_t st;
    uint32_t       lines;
    uint32_t       reports;
    uint32_t       refused;
    uint32_t       tiles;   /* picture messages forwarded to the phone      */

    /* Config the agent sets and the board keeps in /sys/KVMCFG.TXT, so a
     * dongle remembers its network and password across unplugging. */
    char ssid[64];
    char wpass[64];
    char webpass[64]; /* the page's password; empty means the page is open  */
    bool have_cfg;    /* a network has been configured                      */
    bool authed;      /* this phone has given the password (or none is set) */
    bool panel;       /* the board's own screen is ours to draw on          */

    /*
     * Relay (reverse tunnel).  With a public IP nowhere near it, the board
     * cannot be dialled from a phone on the far side of a home router.  So it
     * dials out instead: it opens a connection to a VPS that has a public IP
     * and parks it there, and the relay glues a phone's connection to that
     * parked one.  No port open at home, no always-on machine - just the board
     * calling out, which a home router passes unaided.  Off unless the file
     * C:\KVMRLY.TXT (host, port, secret - one per line) is present; the LAN
     * listener stays up alongside it as a fallback.
     */
    bool        use_relay;
    uint32_t    relay_addr;      /* VPS IPv4, first octet in the high byte     */
    uint16_t    relay_port;      /* the relay's board port                     */
    char        relay_secret[64];
    ag_handle_t relay;           /* the parked outbound socket, or -1          */
    uint32_t    relay_next_dial; /* backoff: do not redial before this         */
    uint32_t    relay_born;      /* when the parked socket was dialled         */
} kvm_t;

/* The phone went away, for whatever reason; declared here because the picture
 * pump finds out about it as often as the socket loop does. */
static void ws_drop(kvm_t *k, const char *why);

/* Credentials off the pipe; declared here because the picture pump is where a
 * config message is noticed, above where the handler is defined. */
static void kvm_apply_config(kvm_t *k, const uint8_t *p, uint32_t len);

/* ------------------------------------------------------------------------ */
/* Sockets                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * A whole buffer out, or a failure.  The sockets here are non-blocking, so a
 * short send is normal and not an error; what is an error is giving up on half
 * a WebSocket frame, because the far end has no way to resynchronise.
 */
static bool send_all_ms(ag_handle_t s, const void *buf, size_t len,
                        uint32_t ms)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t         off = 0;
    const uint32_t deadline = ag_millis() + ms;

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

static bool send_all(ag_handle_t s, const void *buf, size_t len)
{
    return send_all_ms(s, buf, len, SEND_TIMEOUT_MS);
}

/*
 * A frame out.  The deadline is a parameter because the two kinds of traffic
 * want different patience, and the difference is not a detail.
 *
 * A status line is small and if it will not go the phone is gone.  A picture
 * message is the far machine's screen arriving faster than the phone can draw
 * it, and that is not a failure - it is the flow control working.  The socket
 * fills, this blocks, the board stops reading the cable, the cable's buffer
 * fills, and the agent's own write blocks: back-pressure all the way to the
 * machine producing the pixels, which is exactly where it should end up.
 * Giving up early instead - which this did, with three seconds - drops the
 * phone under load and reconnects it, forever.
 *
 * A frame is never half-sent.  Once the header is out the far end is waiting
 * for exactly that many bytes, so there is no such thing as abandoning the
 * payload and keeping the connection.
 */
static bool ws_send_ms(kvm_t *k, uint8_t opcode, const void *payload,
                       size_t len, uint32_t ms)
{
    uint8_t        hdr[4];
    const uint32_t hn = ag_kvmws_header(hdr, opcode, len);

    if (k->ws < 0 || hn == 0) {
        return false;
    }
    if (!send_all_ms(k->ws, hdr, hn, ms)) {
        return false;
    }
    return len == 0 || send_all_ms(k->ws, payload, len, ms);
}

static bool ws_send(kvm_t *k, uint8_t opcode, const void *payload, size_t len)
{
    return ws_send_ms(k, opcode, payload, len, SEND_TIMEOUT_MS);
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

    /* "P <password>": the phone proving it may drive.  The reply tells the
     * page whether to go on or ask again.  A board with no password set is
     * open, and says so. */
    if (len >= 1 && line[0] == 'P' && (len == 1 || line[1] == ' ')) {
        const char  *pw = (len >= 2) ? line + 2 : "";
        const size_t pwn = (len >= 2) ? len - 2 : 0;
        const size_t wl = strlen(k->webpass);
        k->authed = (wl == 0) || (pwn == wl && memcmp(pw, k->webpass, wl) == 0);
        (void)ws_send(k, AG_KVMWS_TEXT, k->authed ? "A 1" : "A 0", 3);
        return;
    }
    /* Everything else is control, and control waits for the password. */
    if (!k->authed) {
        return;
    }

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
/* The picture                                                              */
/* ------------------------------------------------------------------------ */

/*
 * Bytes off the serial pipe, messages on to the phone.  Nothing here looks
 * inside a message, and that is the whole design: the far machine compresses,
 * the phone decodes, and this board is a length prefix and a socket.  An ESP32
 * that never touches a pixel needs no frame buffer, no codec and no CPU worth
 * measuring, so the wire is the limit rather than we are.
 *
 * Returns true when it moved something, which is how the main loop knows
 * whether it may sleep.
 */
static bool pump_video(kvm_t *k)
{
    if (k->vid < 0) {
        return false;
    }

    bool moved = false;

    for (int burst = 0; burst < VID_BURST; burst++) {
        /*
         * Drain first, read second, and never read more than there is room
         * for.  The order is the whole of it: a read carries about two
         * messages, so a loop that alternated one read with one message would
         * take in twice what it gave out, fill the buffer, and start throwing
         * away the tail of every read.  That is not a slow picture, it is no
         * picture at all - the framer spends the rest of its life hunting for
         * a header - and it is what this did until the emulator showed a
         * resync counter in the hundreds beside two messages delivered.
         */
        ag_vid_msg_t msg;
        while (ag_vid_rx_next(&k->vrx, &msg)) {
            moved = true;
            if (msg.type == AG_VID_CONFIG) {
                /* Not a picture: the agent is configuring the dongle.  Handle
                 * it here and never forward it on to the phone. */
                kvm_apply_config(k, msg.payload, msg.len);
                continue;
            }
            if (k->ws < 0 || !k->authed) {
                continue; /* no phone, or one that has not given the password */
            }
            if (!ws_send_ms(k, AG_KVMWS_BIN, msg.raw, msg.raw_len,
                            VID_SEND_MS)) {
                ws_drop(k, "the picture could not be sent");
                return moved;
            }
            k->tiles++;
        }

        uint32_t room = ag_vid_rx_room(&k->vrx);
        if (room == 0) {
            break; /* cannot happen after a drain, and not worth trusting */
        }
        if (room > VID_CHUNK) {
            room = VID_CHUNK;
        }
        const int32_t n = ag_dev_read(k->vid, k->vchunk, room);
        if (n <= 0) {
            break; /* -AG_EAGAIN with nothing there, or no cable */
        }
        (void)ag_vid_rx_feed(&k->vrx, k->vchunk, (uint32_t)n);
        moved = true;
    }
    return moved;
}

/* A message from the phone that is not input: clipboard text, or a request for
 * a whole screen.  It goes to the far machine's agent as it stands. */
static void video_out(kvm_t *k, const uint8_t *data, uint32_t len)
{
    if (k->vid < 0 || data == NULL || len == 0) {
        return;
    }
    uint32_t       off = 0;
    const uint32_t deadline = ag_millis() + 200u;

    while (off < len) {
        const int32_t w = ag_dev_write(k->vid, data + off, len - off);
        if (w > 0) {
            off += (uint32_t)w;
            continue;
        }
        if (w == -AG_EAGAIN) {
            if ((int32_t)(ag_millis() - deadline) >= 0) {
                break;
            }
            ag_delay(1);
            continue;
        }
        break; /* nothing at the other end */
    }
}

/* ------------------------------------------------------------------------ */
/* Configuration: the one thing the far machine cannot supply through a      */
/* keyboard, because the keyboard is the board.  The agent sends which Wi-Fi */
/* to join and a password for the page; the board keeps it in a file so an   */
/* unplugged dongle remembers, and joins on its own on the next boot.        */
/* ------------------------------------------------------------------------ */

#define KVM_CFG_PATH "c:\\KVMCFG.TXT"
#define KVM_RELAY_PATH "c:\\KVMRLY.TXT"
#define RELAY_RECYCLE_MS 90000u /* redial an unused parked socket this often,
                                 * to keep the NAT mapping alive and to notice
                                 * a connection the router has quietly dropped */
#define RELAY_RETRY_MS 3000u    /* and do not hammer a VPS that is down        */
#define REQ_NOTREADY ((size_t)-1) /* read_relay_request: no phone yet, keep it */

/* Copy one NUL-separated field out of a bounded buffer, advancing past it. */
static const char *take_field(char *dst, size_t cap, const char *s,
                              uint32_t *rem)
{
    uint32_t i = 0;
    while (i < *rem && s[i] != 0) {
        i++;
    }
    size_t L = (i < cap) ? i : cap - 1;
    memcpy(dst, s, L);
    dst[L] = 0;
    const uint32_t adv = (i < *rem) ? i + 1 : i; /* skip the NUL if present */
    *rem -= adv;
    return s + adv;
}

static void kvm_join(kvm_t *k)
{
    if (k->have_cfg && k->ssid[0] != 0 && ag_wifi_available()) {
        (void)ag_wifi_connect(k->ssid, k->wpass, NULL);
    }
}

static void kvm_read_cfg(kvm_t *k)
{
    const ag_handle_t h = ag_open(KVM_CFG_PATH, AG_O_RDONLY);
    if (h < 0) {
        return;
    }
    char          buf[256];
    const int32_t n = ag_read(h, buf, sizeof(buf) - 1);
    (void)ag_close(h);
    if (n <= 0) {
        return;
    }
    buf[n] = 0;
    /* Three lines: ssid, pass, webpass. */
    char        *into[3] = {k->ssid, k->wpass, k->webpass};
    const size_t cap[3] = {sizeof(k->ssid), sizeof(k->wpass),
                           sizeof(k->webpass)};
    char *p = buf;
    for (int i = 0; i < 3 && p != NULL; i++) {
        char  *nl = strchr(p, '\n');
        size_t L = nl ? (size_t)(nl - p) : strlen(p);
        if (L > 0 && p[L - 1] == '\r') {
            L--;
        }
        if (L >= cap[i]) {
            L = cap[i] - 1;
        }
        memcpy(into[i], p, L);
        into[i][L] = 0;
        p = nl ? nl + 1 : NULL;
    }
    k->have_cfg = (k->ssid[0] != 0);
    k->authed = (k->webpass[0] == 0);
}

static void kvm_write_cfg(kvm_t *k)
{
    const ag_handle_t h =
        ag_open(KVM_CFG_PATH, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (h < 0) {
        ag_printf("could not save config (%d)\n", (int)h);
        return;
    }
    char      buf[256];
    const int n = snprintf(buf, sizeof(buf), "%s\n%s\n%s\n", k->ssid, k->wpass,
                           k->webpass);
    if (n > 0) {
        (void)ag_write(h, buf, (size_t)n);
    }
    (void)ag_close(h);
}

/* "a.b.c.d" to a uint32 with the first octet in the high byte - the order
 * ag_tcp_connect and the kernel's own ifaddr formatting both use. */
static bool parse_ipv4(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    for (int parts = 0; parts < 4; parts++) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        uint32_t oct = 0;
        int      d = 0;
        while (*s >= '0' && *s <= '9') {
            oct = oct * 10u + (uint32_t)(*s - '0');
            s++;
            if (++d > 3 || oct > 255u) {
                return false;
            }
        }
        v = (v << 8) | oct;
        if (parts < 3) {
            if (*s != '.') {
                return false;
            }
            s++;
        }
    }
    if (*s != 0) {
        return false;
    }
    *out = v;
    return true;
}

/* C:\KVMRLY.TXT, three lines: VPS host (IPv4), board port, secret.  Present
 * means "reach me through this relay"; absent means LAN only, as before. */
static void kvm_read_relay(kvm_t *k)
{
    const ag_handle_t h = ag_open(KVM_RELAY_PATH, AG_O_RDONLY);
    if (h < 0) {
        return;
    }
    char          buf[192];
    const int32_t n = ag_read(h, buf, sizeof(buf) - 1);
    (void)ag_close(h);
    if (n <= 0) {
        return;
    }
    buf[n] = 0;

    char         host[64] = {0};
    char         ports[16] = {0};
    char        *into[3] = {host, ports, k->relay_secret};
    const size_t cap[3] = {sizeof(host), sizeof(ports),
                           sizeof(k->relay_secret)};
    char *p = buf;
    for (int i = 0; i < 3 && p != NULL; i++) {
        char  *nl = strchr(p, '\n');
        size_t L = nl ? (size_t)(nl - p) : strlen(p);
        if (L > 0 && p[L - 1] == '\r') {
            L--;
        }
        if (L >= cap[i]) {
            L = cap[i] - 1;
        }
        memcpy(into[i], p, L);
        into[i][L] = 0;
        p = nl ? nl + 1 : NULL;
    }

    uint32_t addr = 0;
    long     port = 0;
    for (const char *c = ports; *c >= '0' && *c <= '9'; c++) {
        port = port * 10 + (*c - '0');
    }
    if (!parse_ipv4(host, &addr) || port <= 0 || port > 65535 ||
        k->relay_secret[0] == 0) {
        ag_printf("relay config bad (%s), ignoring it\n", KVM_RELAY_PATH);
        k->relay_secret[0] = 0;
        return;
    }
    k->relay_addr = addr;
    k->relay_port = (uint16_t)port;
    k->use_relay = true;
}

/* The agent sent credentials: remember them, join, and make the page ask for
 * the new password. */
static void kvm_apply_config(kvm_t *k, const uint8_t *p, uint32_t len)
{
    const char *s = (const char *)p;
    uint32_t    rem = len;
    s = take_field(k->ssid, sizeof(k->ssid), s, &rem);
    s = take_field(k->wpass, sizeof(k->wpass), s, &rem);
    (void)take_field(k->webpass, sizeof(k->webpass), s, &rem);

    k->have_cfg = (k->ssid[0] != 0);
    k->authed = (k->webpass[0] == 0);
    kvm_write_cfg(k);
    ag_printf("configured: joining %s\n", k->ssid);
    kvm_join(k);
}

/* Tell the agent whether a network is set and, once joined, the address to
 * open on the phone.  The agent shows this to whoever plugged the dongle in. */
static void send_cfg_state(kvm_t *k)
{
    if (k->vid < 0) {
        return;
    }
    uint8_t  buf[AG_VID_HDR + 1 + 16];
    uint32_t addr = 0;
    char     ip[16] = "0.0.0.0";
    const bool ready = ag_net_is_ready();
    if (ready && ag_net_ifaddr(&addr) == AG_OK) {
        (void)snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                       (unsigned)((addr >> 24) & 0xFFu),
                       (unsigned)((addr >> 16) & 0xFFu),
                       (unsigned)((addr >> 8) & 0xFFu),
                       (unsigned)(addr & 0xFFu));
    }
    const uint8_t state = ready ? AG_VID_CFG_READY
                                : (k->have_cfg ? AG_VID_CFG_JOINING
                                               : AG_VID_CFG_NEED);
    const size_t   iplen = strlen(ip) + 1;
    const uint32_t plen = 1u + (uint32_t)iplen;
    (void)ag_vid_header(buf, AG_VID_CFG_STATE, 0, plen);
    buf[AG_VID_HDR] = state;
    memcpy(buf + AG_VID_HDR + 1, ip, iplen);
    /* One non-blocking write, no retry: this runs on a timer whether or not an
     * agent is draining the pipe, and the keyboard must not wait behind it.  A
     * dropped state message is picked up by the next tick 700 ms later.  The
     * message is far smaller than one endpoint buffer, so it goes whole or not
     * at all. */
    (void)ag_dev_write(k->vid, buf, AG_VID_HDR + plen);
}

/* ------------------------------------------------------------------------ */
/* The board's own screen                                                   */
/* ------------------------------------------------------------------------ */

/*
 * Paint the dongle's state onto its soldered panel, so it can be read without a
 * phone: the name, the address once it is on the network, and whether it has
 * been set up.  The panel is taken once (acquire suspends the console on it)
 * and redrawn only when a line would change, so it is not fighting the picture
 * for the SPI bus every tick.  A board with no panel - QEMU, a bare module -
 * just skips it.
 */
/* Panel geometry and the rate meter, learnt once the glass is acquired. */
static uint16_t s_pw, s_ph;
static uint32_t s_rate_tiles, s_rate_ms, s_fps;

/* The colour of the one-glance status light: red not set up, amber joining,
 * blue ready and waiting for a phone, green a phone is driving. */
static uint32_t dot_colour(const kvm_t *k, bool ready, bool phone)
{
    if (!k->have_cfg) return 0xFF5040;
    if (!ready)       return 0xFFC640;
    return phone ? 0x40FF80 : 0x33CCFF;
}

/*
 * The stats line - picture frames a second to the phone, and resyncs - redrawn
 * every tick over its own strip so the number moves without the whole card
 * flickering.  Only meaningful while a phone is linked; otherwise the strip is
 * a hint line.
 */
static void screen_stats(kvm_t *k, int16_t y, bool ready, bool phone)
{
    ag_gfx_fill_rect(0, y, s_pw, 18, 0x001018);
    char line[24];
    if (ready && phone) {
        (void)snprintf(line, sizeof(line), "%u/s  rs %u",
                       (unsigned)s_fps, (unsigned)k->vrx.resyncs);
        ag_gfx_text(4, y, line, 0x9AA5AD, AG_GFX_TRANS);
    } else if (ready) {
        ag_gfx_text(4, y, "open on the phone", 0x9AA5AD, AG_GFX_TRANS);
    } else if (!k->have_cfg) {
        ag_gfx_text(4, y, "run the agent", 0x9AA5AD, AG_GFX_TRANS);
    }
    ag_gfx_flush(0, (uint16_t)y, s_pw, 18);
}

static void kvm_screen(kvm_t *k)
{
    uint32_t   addr = 0;
    char       ip[16] = "";
    const bool ready = ag_net_is_ready() && ag_net_ifaddr(&addr) == AG_OK;
    const bool phone = (k->ws >= 0);
    if (ready) {
        (void)snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                       (unsigned)((addr >> 24) & 0xFFu),
                       (unsigned)((addr >> 16) & 0xFFu),
                       (unsigned)((addr >> 8) & 0xFFu),
                       (unsigned)(addr & 0xFFu));
    }

    if (!k->panel) {
        if (g_ag_api->gfx == NULL) {
            return; /* a build with no display at all */
        }
        ag_gfxinfo_t gi;
        if (ag_gfx_acquire(&gi) != AG_OK) {
            return; /* no panel, or something else holds it */
        }
        s_pw = gi.width;
        s_ph = gi.height;
        s_rate_tiles = k->tiles;
        s_rate_ms = ag_millis();
        k->panel = true;
    }

    /* Picture frames a second to the phone, measured across the redraw tick. */
    const uint32_t nowms = ag_millis();
    const uint32_t dt = nowms - s_rate_ms;
    if (dt >= 400u) {
        s_fps = ((k->tiles - s_rate_tiles) * 1000u) / dt;
        s_rate_tiles = k->tiles;
        s_rate_ms = nowms;
    }

    /* The small 160x80 dongle panel gets its own compact card; a larger glass
     * (the S3-CAM's 320x240) keeps the roomier layout.  What would change the
     * static part of the card is tracked so it is redrawn only on a real
     * change; the stats strip repaints every tick on top. */
    const bool  small = (s_pw <= 200u || s_ph <= 120u);
    static char shown[40] = "\x01";
    char        now[40];
    (void)snprintf(now, sizeof(now), "%d|%d|%s", k->have_cfg ? 1 : 0,
                   phone ? 1 : 0, ip);
    const bool changed = (strcmp(now, shown) != 0);
    if (changed) {
        strncpy(shown, now, sizeof(shown) - 1);
        shown[sizeof(shown) - 1] = 0;
    }

    if (small) {
        const int16_t stats_y = (int16_t)(s_ph - 18);
        if (changed) {
            ag_gfx_clear(0x001018);
            ag_gfx_fill_rect(0, 0, s_pw, 18, 0x12283C);
            ag_gfx_text(3, 1, "ArgonOS KVM", 0x33CCFF, AG_GFX_TRANS);
            ag_gfx_fill_circle((int16_t)(s_pw - 10), 9, 4,
                               dot_colour(k, ready, phone));
            if (ready) {
                ag_gfx_text(4, 24, ip, 0x40FF80, AG_GFX_TRANS);
                ag_gfx_text(4, 42, phone ? "phone linked" : "waiting phone",
                            phone ? 0x40FF80 : 0x9AA5AD, AG_GFX_TRANS);
            } else if (k->have_cfg) {
                ag_gfx_text(4, 30, "joining Wi-Fi...", 0xFFC640, AG_GFX_TRANS);
            } else {
                ag_gfx_text(4, 30, "not set up", 0xFFC640, AG_GFX_TRANS);
            }
            ag_gfx_flush(0, 0, s_pw, s_ph);
        }
        screen_stats(k, stats_y, ready, phone);
        return;
    }

    /* The roomier card for a large panel. */
    if (!changed) {
        return;
    }
    ag_gfx_clear(0x001018);
    ag_gfx_text(10, 12, "ArgonOS  KVM", 0x33CCFF, AG_GFX_TRANS);
    if (ready) {
        char line[40];
        (void)snprintf(line, sizeof(line), "http://%s/", ip);
        ag_gfx_text(10, 44, line, 0x40FF80, AG_GFX_TRANS);
        ag_gfx_text(10, 68, phone ? "phone linked" : "open on the phone",
                    0x9AA5AD, AG_GFX_TRANS);
    } else if (k->have_cfg) {
        ag_gfx_text(10, 44, "joining Wi-Fi...", 0xFFC640, AG_GFX_TRANS);
    } else {
        ag_gfx_text(10, 44, "not set up yet", 0xFFC640, AG_GFX_TRANS);
        ag_gfx_text(10, 68, "run the agent to set me up", 0x9AA5AD, AG_GFX_TRANS);
    }
    ag_gfx_flush(0, 0, s_pw, s_ph);
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

/*
 * Keep one connection parked at the relay, ready for the next phone.  Only when
 * no phone is on the line (k->ws < 0): one socket is enough for one phone at a
 * time, and the relay holds a new phone for a few seconds, which covers the gap
 * between serving the page on one socket and the WebSocket arriving on the next.
 * Redial is rate-limited, and a long-parked socket is recycled so a connection
 * the home router dropped in silence does not sit here looking alive.
 */
static void relay_ensure(kvm_t *k)
{
    if (!k->use_relay || k->ws >= 0) {
        return;
    }
    const uint32_t now = ag_millis();
    if (k->relay >= 0 &&
        (int32_t)(now - k->relay_born) >= (int32_t)RELAY_RECYCLE_MS) {
        (void)ag_net_close(k->relay);
        k->relay = -1;
    }
    if (k->relay >= 0) {
        return;
    }
    if ((int32_t)(now - k->relay_next_dial) < 0) {
        return;
    }
    k->relay_next_dial = now + RELAY_RETRY_MS;
    if (!k->have_cfg) {
        return; /* no network joined yet - nothing to dial over */
    }
    const ag_handle_t c = ag_tcp_connect(k->relay_addr, k->relay_port, 5000u);
    if (c < 0) {
        return;
    }
    (void)ag_net_set_nonblock(c, true);
    char      hello[80];
    const int hn = snprintf(hello, sizeof(hello), "AGKVM1 %s\n",
                            k->relay_secret);
    if (hn <= 0 || !send_all(c, hello, (size_t)hn)) {
        (void)ag_net_close(c);
        return;
    }
    k->relay = c;
    k->relay_born = now;
}

/*
 * Has the phone's request arrived on the parked socket?  Returns REQ_NOTREADY
 * when nothing is waiting (leave it parked, do not stall the loop), 0 when the
 * link died (caller discards it and redials), or the request length in k->req.
 */
static size_t read_relay_request(kvm_t *k)
{
    int32_t r = ag_net_recv(k->relay, k->req, REQ_MAX - 1);
    if (r == -AG_EAGAIN) {
        return REQ_NOTREADY;
    }
    if (r <= 0) {
        return 0;
    }
    size_t n = (size_t)r;
    k->req[n] = '\0';
    if (n >= 4 && strstr(k->req, "\r\n\r\n") != NULL) {
        return n;
    }
    const uint32_t deadline = ag_millis() + REQ_TIMEOUT_MS;
    while (n + 1 < REQ_MAX) {
        r = ag_net_recv(k->relay, k->req + n, REQ_MAX - 1 - n);
        if (r > 0) {
            n += (size_t)r;
            k->req[n] = '\0';
            if (strstr(k->req, "\r\n\r\n") != NULL) {
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
    k->authed = (k->webpass[0] == 0); /* a fresh phone must give the password */
    ag_kvm_reset(&k->st);
    ag_printf("phone connected\n");
}

/*
 * One request already read into k->req (n bytes), from the LAN or the relay -
 * the two are the same once the bytes are here.  A WebSocket upgrade is adopted
 * (returns true, caller keeps the socket); the page and 404 are served and the
 * caller closes.
 */
static bool handle_http(kvm_t *k, ag_handle_t c, size_t n)
{
    if (n == 0) {
        return false;
    }
    size_t      klen = 0;
    const char *key = hdr_value(k->req, n, "sec-websocket-key:", &klen);
    if (key != NULL && klen > 0 && klen < 64) {
        char accept[AG_KVMWS_ACCEPT_LEN];
        char resp[200];
        if (ag_kvmws_accept_key(key, klen, accept)) {
            const int rn = snprintf(resp, sizeof(resp),
                                    "HTTP/1.1 101 Switching Protocols\r\n"
                                    "Upgrade: websocket\r\n"
                                    "Connection: Upgrade\r\n"
                                    "Sec-WebSocket-Accept: %s\r\n\r\n",
                                    accept);
            if (rn > 0 && send_all(c, resp, (size_t)rn)) {
                ws_adopt(k, c);
                return true;
            }
        }
    } else if (strncmp(k->req, "GET / ", 6) == 0 ||
               strncmp(k->req, "GET /index.html ", 16) == 0) {
        serve_page(c);
    } else {
        serve_404(c);
    }
    return false;
}

/* Returns true when it read something, so the main loop knows not to sleep. */
static bool ws_service(kvm_t *k)
{
    bool moved = false;

    if (k->ws < 0) {
        return false;
    }

    for (;;) {
        if (k->rx_n < WS_RX) {
            const int32_t r =
                ag_net_recv(k->ws, k->rx + k->rx_n, WS_RX - k->rx_n);
            if (r > 0) {
                k->rx_n += (size_t)r;
                moved = true;
            } else if (r == -AG_EAGAIN) {
                break;
            } else {
                ws_drop(k, "closed");
                return moved;
            }
        }

        /* Take whole frames off the front until what is left is a fragment. */
        size_t off = 0;
        for (;;) {
            ag_kvmws_frame_t f;
            const int32_t used = ag_kvmws_decode(k->rx + off, k->rx_n - off, &f);
            if (used == 0) {
                break;
            }
            if (used < 0) {
                ws_drop(k, "bad frame");
                return moved;
            }
            switch (f.opcode) {
            case AG_KVMWS_TEXT:
                handle_line(k, (const char *)f.payload, f.len);
                break;
            case AG_KVMWS_BIN:
                /*
                 * Not input.  Input is text and goes out as HID, because that
                 * is the half that has to work when nothing runs on the far
                 * machine.  A binary frame is for the agent - clipboard text,
                 * or a request for a whole screen - and travels as it stands.
                 */
                video_out(k, f.payload, f.len);
                break;
            case AG_KVMWS_PING:
                (void)ws_send(k, AG_KVMWS_PONG, f.payload, f.len);
                break;
            case AG_KVMWS_CLOSE:
                (void)ws_send(k, AG_KVMWS_CLOSE, NULL, 0);
                ws_drop(k, "goodbye");
                return moved;
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
            return moved;
        }
    }
    return moved;
}

static void push_status(kvm_t *k)
{
    ag_usbhid_status_t st;
    ag_usbcdc_status_t vs;
    char               line[96];

    memset(&st, 0, sizeof(st));
    if (ag_dev_ioctl(k->dev, AG_IOC_USBHID_STATUS, &st, sizeof(st)) != AG_OK) {
        st.state = AG_USBHID_OFF;
    }
    memset(&vs, 0, sizeof(vs));
    if (k->vid < 0 ||
        ag_dev_ioctl(k->vid, AG_IOC_USBCDC_STATUS, &vs, sizeof(vs)) != AG_OK) {
        vs.state = AG_USBCDC_OFF;
    }
    const int n = snprintf(line, sizeof(line), "S %u %u %u %u %u %u %u",
                           (unsigned)st.state, (unsigned)k->reports,
                           (unsigned)k->refused, (unsigned)st.leds,
                           (unsigned)vs.state, (unsigned)k->tiles,
                           (unsigned)k->vrx.resyncs);
    if (n > 0) {
        (void)ws_send(k, AG_KVMWS_TEXT, line, (size_t)n);
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
    k.vid = -1;
    k.lfd = -1;
    k.ws = -1;
    k.relay = -1;
    ag_kvm_reset(&k.st);

    k.dev = ag_dev_open("usbhid0");
    if (k.dev < 0) {
        ag_printf("no /dev/usbhid0 (%d)\n", (int)k.dev);
        ag_printf("this image has no USB device stack, or something holds it.\n");
        ag_printf("in the emulator: drv install h:\\usbhidvirt.sys\n");
        return 1;
    }

    /*
     * No network is not a failure any more.  A dongle plugged into a machine
     * it has never seen has not been told a Wi-Fi yet; it waits here with the
     * pipe open, and the agent sends the credentials down it.  Once a network
     * is set the board joins on its own at boot (kvm_read_cfg + kvm_join).
     *
     * The serial pipe is likewise optional: a build without it, or a board
     * with nothing running on the far machine, is a keyboard and a mouse, and
     * that is a whole useful thing on its own.  It is opened before anything
     * network, because it is how an unconfigured dongle is configured.
     */
    k.vid = ag_dev_open("usbcdc0");

    k.mem = ag_malloc(WS_RX + REQ_MAX + AG_VID_MAX_MSG + VID_CHUNK);
    if (k.mem == NULL) {
        ag_printf("not enough memory\n");
        (void)ag_dev_close(k.dev);
        return 1;
    }
    k.rx = (uint8_t *)k.mem;
    k.req = (char *)k.mem + WS_RX;
    k.vbuf = (uint8_t *)k.mem + WS_RX + REQ_MAX;
    k.vchunk = k.vbuf + AG_VID_MAX_MSG;
    ag_vid_rx_init(&k.vrx, k.vbuf, AG_VID_MAX_MSG);

    /*
     * Bring the radio stack up ourselves.  With no [wifi] in SYSTEM.CFG the OS
     * never starts it, and then the socket calls below have no stack under them
     * - so a dongle that has not been configured must start it here.  Harmless
     * when the OS already did: start on an started radio just returns.
     */
    if (ag_wifi_available()) {
        (void)ag_wifi_start();
    }

    /* What the agent set last time.  Join now so the page is reachable as soon
     * as the radio associates; if nothing is set, wait for the agent. */
    kvm_read_cfg(&k);
    kvm_read_relay(&k);
    kvm_join(&k);

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
        if (k.use_relay) {
            ag_printf("relay: via %u.%u.%u.%u - reachable from anywhere\n",
                      (unsigned)((k.relay_addr >> 24) & 0xFFu),
                      (unsigned)((k.relay_addr >> 16) & 0xFFu),
                      (unsigned)((k.relay_addr >> 8) & 0xFFu),
                      (unsigned)(k.relay_addr & 0xFFu));
        }
        ag_printf("Ctrl+C to stop\n");
    }

    uint32_t next_status = ag_millis();
    uint32_t next_cfg = ag_millis();

    while (!ag_interrupted()) {
        /*
         * Accept without waiting, then do the work, then sleep only if there
         * was none.  Waiting inside accept was fine while the only traffic was
         * a keystroke now and then; with a picture coming down the cable it
         * would cap the whole thing at a few kilobytes a second.
         */
        bool        busy = false;
        ag_handle_t c = -1;
        size_t      n = 0;

        /*
         * A connection to handle, from the relay or straight off the LAN.  The
         * relay is tried first: a parked socket that has just been handed a
         * phone should be served before we go looking on the LAN.  Both fill
         * k.req, and from there the two paths are identical.
         */
        if (k.use_relay) {
            relay_ensure(&k);
            if (k.relay >= 0) {
                const size_t rn = read_relay_request(&k);
                if (rn != REQ_NOTREADY) {
                    c = k.relay;
                    k.relay = -1; /* consumed: served and closed, or adopted */
                    n = rn;
                }
            }
        }
        if (c < 0) {
            c = ag_tcp_accept(k.lfd, 0u);
            if (c >= 0) {
                (void)ag_net_set_nonblock(c, true);
                n = read_request(c, k.req, REQ_MAX);
            }
        }
        if (c >= 0) {
            busy = true;
            if (!handle_http(&k, c, n)) {
                (void)ag_net_close(c);
            }
        }

        busy |= ws_service(&k);
        busy |= pump_video(&k);

        if ((int32_t)(ag_millis() - next_status) >= 0) {
            next_status = ag_millis() + STATUS_MS;
            if (k.ws >= 0) {
                push_status(&k);
            }
        }

        /* Keep the agent told whether a network is set and, once joined, the
         * address to open - it is the only screen the person configuring the
         * dongle can read. */
        if ((int32_t)(ag_millis() - next_cfg) >= 0) {
            next_cfg = ag_millis() + 700u;
            send_cfg_state(&k);
            kvm_screen(&k);
        }

        if (!busy) {
            ag_delay(IDLE_MS);
        }
    }

    if (k.panel) {
        ag_gfx_release(); /* hand the glass back to the console */
    }
    ws_drop(&k, "stopping");
    if (k.relay >= 0) {
        (void)ag_net_close(k.relay);
    }
    (void)ag_net_close(k.lfd);
    ag_free(k.mem);
    (void)ag_dev_close(k.dev);
    if (k.vid >= 0) {
        (void)ag_dev_close(k.vid);
    }
    ag_printf("stopped: %u lines, %u reports, %u lost, %u picture messages, "
              "%u resyncs\n",
              (unsigned)k.lines, (unsigned)k.reports, (unsigned)k.refused,
              (unsigned)k.tiles, (unsigned)k.vrx.resyncs);
    return 0;
}
