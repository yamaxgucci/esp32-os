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
#include "ag_ws.h"
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
    const uint32_t hn = ag_ws_header(hdr, opcode, len);

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
        (void)ws_send(k, AG_WS_TEXT, k->authed ? "A 1" : "A 0", 3);
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
            if (!ws_send_ms(k, AG_WS_BIN, msg.raw, msg.raw_len,
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
static void kvm_screen(kvm_t *k)
{
    uint32_t   addr = 0;
    char       ip[16] = "";
    const bool ready = ag_net_is_ready() && ag_net_ifaddr(&addr) == AG_OK;
    if (ready) {
        (void)snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                       (unsigned)((addr >> 24) & 0xFFu),
                       (unsigned)((addr >> 16) & 0xFFu),
                       (unsigned)((addr >> 8) & 0xFFu),
                       (unsigned)(addr & 0xFFu));
    }

    /* One line that stands for what is on the glass, so a redraw happens only
     * on a real change. */
    static char shown[48] = "\x01";
    char        now[48];
    (void)snprintf(now, sizeof(now), "%d|%s", k->have_cfg ? 1 : 0, ip);
    if (strcmp(now, shown) == 0) {
        return;
    }

    if (!k->panel) {
        if (g_ag_api->gfx == NULL) {
            return; /* a build with no display at all */
        }
        ag_gfxinfo_t gi;
        if (ag_gfx_acquire(&gi) != AG_OK) {
            return; /* no panel, or something else holds it */
        }
        k->panel = true;
    }
    strncpy(shown, now, sizeof(shown) - 1);
    shown[sizeof(shown) - 1] = 0;

    ag_gfx_clear(0x001018);
    ag_gfx_text(10, 12, "ArgonOS  KVM", 0x33CCFF, AG_GFX_TRANS);
    if (ready) {
        char line[40];
        (void)snprintf(line, sizeof(line), "http://%s/", ip);
        ag_gfx_text(10, 44, line, 0x40FF80, AG_GFX_TRANS);
        ag_gfx_text(10, 68, "open on the phone", 0x9AA5AD, AG_GFX_TRANS);
    } else if (k->have_cfg) {
        ag_gfx_text(10, 44, "joining Wi-Fi...", 0xFFC640, AG_GFX_TRANS);
    } else {
        ag_gfx_text(10, 44, "not set up yet", 0xFFC640, AG_GFX_TRANS);
        ag_gfx_text(10, 68, "run the agent to set me up", 0x9AA5AD, AG_GFX_TRANS);
    }
    ag_gfx_flush(0, 0, 320, 240);
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
    k->authed = (k->webpass[0] == 0); /* a fresh phone must give the password */
    ag_kvm_reset(&k->st);
    ag_printf("phone connected\n");
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
            ag_ws_frame_t f;
            const int32_t used = ag_ws_decode(k->rx + off, k->rx_n - off, &f);
            if (used == 0) {
                break;
            }
            if (used < 0) {
                ws_drop(k, "bad frame");
                return moved;
            }
            switch (f.opcode) {
            case AG_WS_TEXT:
                handle_line(k, (const char *)f.payload, f.len);
                break;
            case AG_WS_BIN:
                /*
                 * Not input.  Input is text and goes out as HID, because that
                 * is the half that has to work when nothing runs on the far
                 * machine.  A binary frame is for the agent - clipboard text,
                 * or a request for a whole screen - and travels as it stands.
                 */
                video_out(k, f.payload, f.len);
                break;
            case AG_WS_PING:
                (void)ws_send(k, AG_WS_PONG, f.payload, f.len);
                break;
            case AG_WS_CLOSE:
                (void)ws_send(k, AG_WS_CLOSE, NULL, 0);
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
    k.vid = -1;
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
        bool              busy = false;
        const ag_handle_t c = ag_tcp_accept(k.lfd, 0u);
        if (c >= 0) {
            busy = true;
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
