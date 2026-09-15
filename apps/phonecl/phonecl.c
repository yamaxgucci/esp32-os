/*
 * ArgonOS - the phone link, from the other end.
 *
 *   phonecl 192.168.4.1 -pass argon -screen
 *   phonecl 192.168.4.1 -pass argon -type "dir" -enter -wait 800 -screen
 *   phonecl 192.168.4.1 -pass argon -key alt+2 -screen
 *   phonecl 192.168.4.1 -get
 *
 * What PHONE.SYS serves to a browser, read by a board instead.
 *
 * Everything this link does was verified by a person holding a phone, and that
 * turned out to be the expensive part of a day: "the page does not load"
 * arrives hours after the state that caused it, "switching slots does not
 * work" cannot be repeated on demand, and every build needs somebody to press
 * a key again.  None of the five faults found on 15 Sep 2026 were where they
 * looked, and each cost a round trip through a human being.
 *
 * So: a second board joins the first one's access point and speaks the same
 * protocol.  A scenario becomes a script.  What it cannot replace is how the
 * page looks and whether it is usable with a thumb - that is still a person's
 * judgement, and this does not pretend otherwise.
 *
 * The framing is apps/common/ws, the same file the server half uses, read the
 * other way round.  The password is the same challenge and response: the board
 * sends a nonce, both sides hash it with the password, only the digest
 * crosses.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/keys.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ag_ws.h"

/*
 * Six kilobytes of stack and two of arena, named and kept small on purpose.
 *
 * This allocates nothing - the screen and the receive buffer are statics - so
 * the arena is only the floor a process needs to exist.  The stack matters
 * more than it looks: the interesting test is this client running *beside*
 * PHONE.SYS on one board, and there the driver turns visitors away below
 * 16 KB free.  The first build asked for twelve and eight, took thirty
 * kilobytes with its data, and was refused by the very driver it had come to
 * test - which is a fair measurement of nothing at all.
 */
AG_APP_SIZED("PHONECL", "1.0", "argon", AG_AXE_NEEDS_NET, 6 * 1024, 2 * 1024);

/* The protocol, which is PHONE.SYS's and documented in apps/phone/phone.c. */
#define OP_INFO 'M'
#define OP_ROW 'R'
#define OP_CURSOR 'C'
#define OP_AUTH 'A'

#define IN_HELLO 'H'
#define IN_KEY 'K'
#define IN_PTR 'P'
#define IN_TEXT 'T'
#define IN_AUTH 'A'

#define NONCE_LEN 16u
#define DIGEST_LEN 20u

#define MOD_SHIFT 1u
#define MOD_CTRL 2u
#define MOD_ALT 4u

/*
 * The biggest console worth mirroring here, which is not the biggest one
 * possible: eighty-four by thirty-two is five kilobytes of cells, and every
 * kilobyte is one the driver on the same board does not have.
 */
#define MAX_COLS 84u
#define MAX_ROWS 32u
#define RX_CAP 2048u

static struct {
    ag_handle_t sock;
    char        pass[64];
    bool        authed;

    /* The screen as it has arrived: one byte of character, one of attribute. */
    uint8_t  cell[MAX_COLS * MAX_ROWS * 2u];
    uint32_t cols, rows;
    uint32_t cur_x, cur_y;
    bool     cur_on;
    uint32_t rows_seen; /* how many row messages have landed, ever */

    /* Frames arrive in pieces; this is what has arrived and not been used. */
    uint8_t  rx[RX_CAP];
    uint32_t rx_len;
    /*
     * Bytes of a frame too big for that buffer, still to be thrown away.
     *
     * A band of pixels is about five kilobytes and this buffer is two, so
     * without this the first picture wedges the client for ever: the buffer
     * fills, the frame can never be complete, nothing is consumed and the loop
     * spins.  Found by holding the link for two minutes and watching the
     * client still be there afterwards.
     *
     * Thrown away rather than made to fit: this is the text client, it has no
     * use for pixels, and a buffer sized for them would be five kilobytes it
     * takes from the board it is testing.
     */
    uint32_t skip;
} s;

/* ------------------------------------------------------------------------ */
/* Plumbing                                                                  */
/* ------------------------------------------------------------------------ */

static bool parse_ip(const char *text, uint32_t *out)
{
    uint32_t v = 0, part = 0, digits = 0, parts = 0;

    for (const char *p = text;; p++) {
        if (*p >= '0' && *p <= '9') {
            part = part * 10u + (uint32_t)(*p - '0');
            if (part > 255u || ++digits > 3u) {
                return false;
            }
            continue;
        }
        if (*p == '.' || *p == '\0') {
            if (digits == 0u) {
                return false;
            }
            v = (v << 8) | part;
            part = 0;
            digits = 0;
            if (++parts == 4u && *p == '\0') {
                *out = v;
                return true;
            }
            if (*p == '\0') {
                return false;
            }
            continue;
        }
        return false;
    }
}

/* Everything sent goes out whole or the run is over; there is no recovery
 * worth writing for a test client that can simply be started again. */
static bool send_all(const void *buf, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t       at = 0;

    while (at < len) {
        const int32_t n = ag_net_send(s.sock, p + at, len - at);
        if (n > 0) {
            at += (uint32_t)n;
            continue;
        }
        if (n != -AG_EAGAIN && n != -AG_ENOMEM && n != 0) {
            return false;
        }
        ag_delay(2);
    }
    return true;
}

/*
 * One masked frame.  A client must mask, and the mask is not a secret - it
 * exists so a proxy cannot be fooled into seeing an attacker's bytes as a
 * request - so any varying value will do.  ag_micros is one.
 */
static bool ws_send(uint8_t op, const void *body, uint32_t len)
{
    uint8_t  hdr[14];
    uint8_t  mask[4];
    uint8_t  buf[256];
    const uint32_t total = len + 1u;

    if (total > sizeof(buf)) {
        return false;
    }
    const uint32_t t = (uint32_t)ag_micros();
    mask[0] = (uint8_t)t;
    mask[1] = (uint8_t)(t >> 8);
    mask[2] = (uint8_t)(t >> 16);
    mask[3] = (uint8_t)(t >> 24);

    const uint32_t n = ag_ws_hdr_build_masked(hdr, AG_WS_BIN, total, true,
                                              mask);
    buf[0] = op;
    if (len != 0u && body != NULL) {
        memcpy(buf + 1, body, len);
    }
    ag_ws_mask(buf, total, mask);
    return send_all(hdr, n) && send_all(buf, total);
}

/* Pull whatever has arrived into the buffer.  False means the far end went. */
static bool rx_pump(uint32_t wait_ms)
{
    const uint64_t until = (uint64_t)ag_micros() + (uint64_t)wait_ms * 1000ull;

    for (;;) {
        if (s.rx_len < RX_CAP) {
            const int32_t n = ag_net_recv(s.sock, s.rx + s.rx_len,
                                          RX_CAP - s.rx_len);
            if (n > 0) {
                s.rx_len += (uint32_t)n;
                return true;
            }
            if (n == 0 || (n != -AG_EAGAIN && n != -AG_ENOMEM)) {
                return false;
            }
        }
        if ((uint64_t)ag_micros() >= until) {
            return true;
        }
        ag_delay(5);
    }
}

static void rx_drop(uint32_t n)
{
    if (n >= s.rx_len) {
        s.rx_len = 0;
        return;
    }
    memmove(s.rx, s.rx + n, s.rx_len - n);
    s.rx_len -= n;
}

/* ------------------------------------------------------------------------ */
/* The two handshakes: HTTP, then the password                               */
/* ------------------------------------------------------------------------ */

/*
 * Connect, and do not let a quiet moment look like a hang.
 *
 * A blocking socket was this client's first real bug, and it hid well:
 * everything worked for as long as the board had something to say, and the
 * first silent second wedged it for ever inside ag_net_recv.  The hold
 * reported "10/40 s" and then nothing, which is a fair description of a
 * program waiting for a screen that is not changing.
 */
static bool connect_to(uint32_t addr, uint16_t port)
{
    s.sock = ag_tcp_connect(addr, port, 4000);
    if ((int32_t)s.sock < 0) {
        ag_printf("connect failed: %d\n", (int)(int32_t)s.sock);
        return false;
    }
    (void)ag_net_set_nonblock(s.sock, true);
    return true;
}


static int fetch_page(uint32_t addr, uint16_t port)
{
    if (!connect_to(addr, port)) {
        return 1;
    }

    static const char req[] = "GET / HTTP/1.1\r\nHost: board\r\n"
                              "Connection: close\r\n\r\n";
    if (!send_all(req, (uint32_t)(sizeof(req) - 1u))) {
        ag_printf("send failed\n");
        ag_net_close(s.sock);
        return 1;
    }

    /*
     * Counted, not stored.  The page is fifteen kilobytes and this board has
     * no reason to hold it - what a test needs to know is the status line and
     * whether the body arrived whole, which is the failure that actually
     * happened here (8042 bytes of 14780, with nothing said).
     */
    uint32_t got = 0, body = 0;
    char     status[64];
    uint32_t status_len = 0;
    bool     have_status = false, in_body = false;
    uint8_t  buf[512];

    for (;;) {
        const int32_t n = ag_net_recv(s.sock, buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (n == -AG_EAGAIN || n == -AG_ENOMEM) {
                ag_delay(5);
                continue;
            }
            break;
        }
        for (int32_t i = 0; i < n; i++) {
            const uint8_t c = buf[i];
            if (!have_status) {
                if (c == '\r') {
                    have_status = true;
                    status[status_len] = 0;
                } else if (status_len + 1u < sizeof(status)) {
                    status[status_len++] = (char)c;
                }
            }
            if (!in_body) {
                /* The blank line, found by counting the run of CR LF CR LF. */
                static const char k_end[] = "\r\n\r\n";
                static uint32_t   at = 0;
                if (c == (uint8_t)k_end[at]) {
                    if (++at == 4u) {
                        in_body = true;
                        at = 0;
                    }
                } else {
                    at = (c == '\r') ? 1u : 0u;
                }
            } else {
                body++;
            }
        }
        got += (uint32_t)n;
    }
    ag_net_close(s.sock);
    s.sock = (ag_handle_t)-1;

    ag_printf("page: %s\n", have_status ? status : "(no status line)");
    ag_printf("page: %u bytes of body, %u total\n", (unsigned)body,
              (unsigned)got);
    return 0;
}

static bool ws_open(uint32_t addr, uint16_t port)
{
    if (!connect_to(addr, port)) {
        return false;
    }

    uint8_t nonce[16];
    const uint32_t t = (uint32_t)ag_micros();
    for (uint32_t i = 0; i < sizeof(nonce); i++) {
        nonce[i] = (uint8_t)(t >> ((i & 3u) * 8u)) ^ (uint8_t)(i * 37u);
    }
    char key[AG_WS_KEY_LEN];
    (void)ag_ws_client_key(nonce, key);

    char req[256];
    const int n = snprintf(req, sizeof(req),
                              "GET /ws HTTP/1.1\r\nHost: board\r\n"
                              "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                              "Sec-WebSocket-Version: 13\r\n"
                              "Sec-WebSocket-Key: %s\r\n\r\n", key);
    if (n <= 0 || !send_all(req, (uint32_t)n)) {
        ag_printf("upgrade request failed\n");
        return false;
    }

    /* The answer, up to the blank line.  Anything after it is frames. */
    char     head[512];
    uint32_t at = 0;
    const uint64_t until = (uint64_t)ag_micros() + 4000000ull;
    bool done = false;

    while (!done && (uint64_t)ag_micros() < until) {
        uint8_t c;
        const int32_t r = ag_net_recv(s.sock, &c, 1);
        if (r == 1) {
            if (at + 1u < sizeof(head)) {
                head[at++] = (char)c;
            }
            if (at >= 4u && head[at - 4u] == '\r' && head[at - 3u] == '\n' &&
                head[at - 2u] == '\r' && head[at - 1u] == '\n') {
                done = true;
            }
            continue;
        }
        if (r == 0 || (r != -AG_EAGAIN && r != -AG_ENOMEM)) {
            break;
        }
        ag_delay(2);
    }
    head[at] = 0;

    if (!done) {
        ag_printf("no upgrade answer\n");
        return false;
    }
    if (strstr(head, " 101 ") == NULL) {
        char first[64];
        uint32_t i = 0;
        while (i + 1u < sizeof(first) && head[i] != '\r' && head[i] != 0) {
            first[i] = head[i];
            i++;
        }
        first[i] = 0;
        ag_printf("upgrade refused: %s\n", first);
        return false;
    }

    /*
     * And the accept value, which is the one thing the handshake proves.  A
     * server that answers 101 with the wrong accept is not speaking this
     * protocol, and feeding it frames produces a puzzle rather than an error.
     */
    const char *acc = strstr(head, "Sec-WebSocket-Accept:");
    if (acc == NULL) {
        acc = strstr(head, "sec-websocket-accept:");
    }
    if (acc == NULL) {
        ag_printf("upgrade answer has no accept header\n");
        return false;
    }
    acc += 21;
    while (*acc == ' ') {
        acc++;
    }
    char value[AG_WS_ACCEPT_LEN];
    uint32_t vi = 0;
    while (vi + 1u < sizeof(value) && *acc != '\r' && *acc != 0) {
        value[vi++] = *acc++;
    }
    value[vi] = 0;
    if (!ag_ws_accept_ok(key, value)) {
        ag_printf("upgrade accept is wrong: %s\n", value);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* The screen                                                                */
/* ------------------------------------------------------------------------ */

static void take_message(const uint8_t *m, uint32_t len)
{
    if (len == 0u) {
        return;
    }
    const uint8_t  op = m[0];
    const uint8_t *b = m + 1;
    const uint32_t n = len - 1u;

    switch (op) {
    case OP_AUTH:
        if (n == NONCE_LEN) {
            /*
             * SHA1(nonce || password), which is the whole of the exchange.
             * The password never crosses, and a digest somebody copied is
             * worth nothing on the next connection - the nonce is new.
             */
            uint8_t  material[NONCE_LEN + sizeof(s.pass)];
            uint32_t pl = (uint32_t)strlen(s.pass);
            uint8_t  digest[DIGEST_LEN];

            memcpy(material, b, NONCE_LEN);
            memcpy(material + NONCE_LEN, s.pass, pl);
            ag_ws_sha1(material, NONCE_LEN + pl, digest);
            (void)ws_send(IN_AUTH, digest, DIGEST_LEN);
            s.authed = true;
        }
        break;

    case OP_INFO:
        if (n >= 9u) {
            s.cols = b[4];
            s.rows = b[5];
            if (s.cols > MAX_COLS) {
                s.cols = MAX_COLS;
            }
            if (s.rows > MAX_ROWS) {
                s.rows = MAX_ROWS;
            }
        }
        break;

    case OP_ROW:
        if (n >= 2u) {
            const uint32_t row = b[0];
            const uint32_t count = b[1];
            if (row < s.rows && n >= 2u + count * 2u) {
                for (uint32_t i = 0; i < count && i < s.cols; i++) {
                    const uint32_t at = (row * s.cols + i) * 2u;
                    s.cell[at] = b[2u + i * 2u];
                    s.cell[at + 1u] = b[3u + i * 2u];
                }
                s.rows_seen++;
            }
        }
        break;

    case OP_CURSOR:
        if (n >= 5u) {
            s.cur_x = b[0];
            s.cur_y = b[1];
            s.cur_on = b[4] != 0u;
        }
        break;

    default:
        /* 'B', 'P' and 'I' are pixels; a text client has no use for them. */
        break;
    }
}

/*
 * Frames out of the buffer, for as long as whole ones are in it.  A server
 * never masks, so anything masked here is not our server and is worth saying
 * rather than quietly unmasking.
 */
static bool take_frames(void)
{
    for (;;) {
        /* Finish discarding an oversized frame before looking for a header:
         * what is in the buffer is its body, not the start of anything. */
        if (s.skip != 0u) {
            const uint32_t n = (s.skip < s.rx_len) ? s.skip : s.rx_len;
            rx_drop(n);
            s.skip -= n;
            if (s.skip != 0u) {
                return true;
            }
        }

        ag_ws_hdr_t h;
        const int32_t r = ag_ws_hdr_parse(s.rx, s.rx_len, RX_CAP, &h);
        if (r == 0) {
            return true;
        }
        if (r < 0) {
            ag_printf("bad frame from the board\n");
            return false;
        }
        if (h.hdr + h.len > RX_CAP) {
            /* Bigger than anything this client can hold: step over it. */
            s.skip = h.len;
            rx_drop(h.hdr);
            continue;
        }
        if (s.rx_len < h.hdr + h.len) {
            return true; /* the body is still on its way */
        }
        if (h.opcode == AG_WS_CLOSE) {
            ag_printf("the board closed the link\n");
            return false;
        }
        if (h.opcode == AG_WS_BIN || h.opcode == AG_WS_TEXT) {
            take_message(s.rx + h.hdr, h.len);
        }
        rx_drop(h.hdr + h.len);
    }
}

static bool pump(uint32_t ms)
{
    const uint64_t until = (uint64_t)ag_micros() + (uint64_t)ms * 1000ull;

    do {
        if (!rx_pump(20)) {
            ag_printf("the link went\n");
            return false;
        }
        if (!take_frames()) {
            return false;
        }
    } while ((uint64_t)ag_micros() < until);
    return true;
}

static void print_screen(void)
{
    if (s.cols == 0u || s.rows == 0u) {
        ag_printf("(no screen arrived)\n");
        return;
    }
    ag_printf("--- %ux%u, cursor %u,%u %s, %u rows received ---\n",
              (unsigned)s.cols, (unsigned)s.rows, (unsigned)s.cur_x,
              (unsigned)s.cur_y, s.cur_on ? "on" : "off",
              (unsigned)s.rows_seen);
    for (uint32_t y = 0; y < s.rows; y++) {
        char line[MAX_COLS + 1u];
        uint32_t w = 0;
        for (uint32_t x = 0; x < s.cols; x++) {
            const uint8_t c = s.cell[(y * s.cols + x) * 2u];
            /* Printable ASCII as itself, everything else as a dot: this is a
             * log line, not a terminal, and CP437 box drawing would arrive as
             * noise in whatever encoding the reader's console happens to be. */
            line[w++] = (c >= 32u && c < 127u) ? (char)c : '.';
        }
        while (w > 0u && line[w - 1u] == ' ') {
            w--;
        }
        line[w] = 0;
        ag_printf("|%s\n", line);
    }
}

/* ------------------------------------------------------------------------ */
/* Keys                                                                      */
/* ------------------------------------------------------------------------ */

static void send_key(uint16_t hid, uint16_t unicode, uint8_t mods)
{
    uint8_t body[7];

    body[0] = 1; /* down */
    body[1] = mods;
    body[2] = (uint8_t)hid;
    body[3] = (uint8_t)(hid >> 8);
    body[4] = (uint8_t)unicode;
    body[5] = (uint8_t)(unicode >> 8);
    body[6] = 0;
    (void)ws_send(IN_KEY, body, sizeof(body));

    body[0] = 2; /* up */
    body[4] = 0;
    body[5] = 0;
    (void)ws_send(IN_KEY, body, sizeof(body));
}

/*
 * "alt+2", "ctrl+c", "f5", "enter", "esc", "up" - the spellings a scenario
 * wants to be able to type on a command line.
 */
static bool send_chord(const char *spec)
{
    uint8_t mods = 0;
    const char *p = spec;

    for (;;) {
        if (strncmp(p, "alt+", 4) == 0) {
            mods |= MOD_ALT;
            p += 4;
        } else if (strncmp(p, "ctrl+", 5) == 0) {
            mods |= MOD_CTRL;
            p += 5;
        } else if (strncmp(p, "shift+", 6) == 0) {
            mods |= MOD_SHIFT;
            p += 6;
        } else {
            break;
        }
    }

    static const struct {
        const char *name;
        uint16_t    hid;
    } k_named[] = {
        {"enter", AG_KEY_ENTER}, {"esc", AG_KEY_ESC},
        {"tab", AG_KEY_TAB},     {"back", AG_KEY_BACKSPACE},
        {"space", AG_KEY_SPACE}, {"up", AG_KEY_UP},
        {"down", AG_KEY_DOWN},   {"left", AG_KEY_LEFT},
        {"right", AG_KEY_RIGHT}, {"backslash", AG_KEY_BACKSLASH},
    };
    for (uint32_t i = 0; i < sizeof(k_named) / sizeof(k_named[0]); i++) {
        if (strcmp(p, k_named[i].name) == 0) {
            send_key(k_named[i].hid, 0, mods);
            return true;
        }
    }
    if ((p[0] == 'f' || p[0] == 'F') && p[1] >= '1' && p[1] <= '9') {
        uint32_t num = (uint32_t)(p[1] - '0');
        if (p[2] >= '0' && p[2] <= '9') {
            num = num * 10u + (uint32_t)(p[2] - '0');
        }
        if (num >= 1u && num <= 12u) {
            send_key((uint16_t)(AG_KEY_F1 + (num - 1u)), 0, mods);
            return true;
        }
    }
    if (p[0] != 0 && p[1] == 0) {
        const char c = p[0];
        uint16_t   hid = 0;
        if (c >= 'a' && c <= 'z') {
            hid = (uint16_t)(AG_KEY_A + (c - 'a'));
        } else if (c >= '1' && c <= '9') {
            hid = (uint16_t)(AG_KEY_1 + (c - '1'));
        } else if (c == '0') {
            hid = AG_KEY_0;
        }
        if (hid != 0u) {
            /* With Ctrl or Alt this is a chord, not a character - the same
             * rule the page follows, and the shell wants Ctrl+C as a keycode
             * rather than as the letter c. */
            const uint16_t uni = (mods & (MOD_CTRL | MOD_ALT)) ? 0u
                                                               : (uint16_t)c;
            send_key(hid, uni, mods);
            return true;
        }
    }
    ag_printf("do not know the key '%s'\n", spec);
    return false;
}

/*
 * A tap at a place, with whichever button.
 *
 * Here so that the right button can be tested at all: a touch screen has one,
 * the desktop wants two, and the page now arms the second with a latch - none
 * of which is checkable by looking at a phone, because a tap that arrives as
 * the wrong button looks exactly like a tap that did not arrive.
 *
 * Coordinates are pixels, which is what a driver says it deals in (ABI 0.44).
 */
static void send_tap(uint16_t x, uint16_t y, uint8_t buttons)
{
    uint8_t body[7];

    body[0] = 1; /* down */
    body[1] = buttons;
    body[2] = (uint8_t)x;
    body[3] = (uint8_t)(x >> 8);
    body[4] = (uint8_t)y;
    body[5] = (uint8_t)(y >> 8);
    body[6] = 0;
    (void)ws_send(IN_PTR, body, sizeof(body));

    body[0] = 2; /* up */
    body[1] = 0;
    (void)ws_send(IN_PTR, body, sizeof(body));
}

/* "120,80" - a place to put a finger. */
static bool parse_point(const char *spec, uint16_t *x, uint16_t *y)
{
    uint32_t v = 0;
    bool     digits = false;

    for (const char *p = spec;; p++) {
        if (*p >= '0' && *p <= '9') {
            v = v * 10u + (uint32_t)(*p - '0');
            digits = true;
            continue;
        }
        if (*p == ',' && digits) {
            *x = (uint16_t)v;
            v = 0;
            digits = false;
            continue;
        }
        if (*p == 0 && digits) {
            *y = (uint16_t)v;
            return true;
        }
        return false;
    }
}

/* ------------------------------------------------------------------------ */

int ag_main(int argc, char **argv)
{
    if (argc < 2) {
        ag_printf("usage: phonecl <ip> [-port N] [-pass P] [-get]\n");
        ag_printf("       [-type TEXT] [-enter] [-key CHORD] [-wait MS]\n");
        ag_printf("       [-screen] [-hold SECS]\n");
        return 1;
    }

    uint32_t addr = 0;
    if (!parse_ip(argv[1], &addr)) {
        ag_printf("not an address: %s\n", argv[1]);
        return 1;
    }

    uint16_t port = 8765;
    bool     want_get = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-port") == 0 && i + 1 < argc) {
            port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-pass") == 0 && i + 1 < argc) {
            snprintf(s.pass, sizeof(s.pass), "%s", argv[++i]);
        } else if (strcmp(argv[i], "-get") == 0) {
            want_get = true;
        }
    }

    if (!ag_net_is_ready()) {
        ag_printf("no network\n");
        return 1;
    }

    if (want_get && fetch_page(addr, port) != 0) {
        return 1;
    }

    if (!ws_open(addr, port)) {
        if ((int32_t)s.sock >= 0) {
            ag_net_close(s.sock);
        }
        return 1;
    }
    ag_printf("linked to %s:%u\n", argv[1], (unsigned)port);

    /*
     * The nonce arrives unasked when a password is set, so the first pump is
     * the authentication.  Then hello, which is what makes the board send the
     * geometry and the whole screen rather than only what changes.
     */
    (void)pump(600);
    (void)ws_send(IN_HELLO, NULL, 0);
    (void)pump(600);

    int rc = 0;
    for (int i = 2; i < argc && rc == 0; i++) {
        if (strcmp(argv[i], "-type") == 0 && i + 1 < argc) {
            const char *text = argv[++i];
            (void)ws_send(IN_TEXT, text, (uint32_t)strlen(text));
            if (!pump(200)) {
                rc = 1;
            }
        } else if (strcmp(argv[i], "-enter") == 0) {
            send_key(AG_KEY_ENTER, 0, 0);
            if (!pump(200)) {
                rc = 1;
            }
        } else if (strcmp(argv[i], "-key") == 0 && i + 1 < argc) {
            if (!send_chord(argv[++i])) {
                rc = 1;
            } else if (!pump(200)) {
                rc = 1;
            }
        } else if (strcmp(argv[i], "-wait") == 0 && i + 1 < argc) {
            if (!pump((uint32_t)atoi(argv[++i]))) {
                rc = 1;
            }
        } else if (strcmp(argv[i], "-hold") == 0 && i + 1 < argc) {
            /*
             * Sit on the link and report what happened to it.  This is the
             * shape of "the page dies after a while" - a question about
             * minutes, which nobody can hold a phone through.
             */
            const uint32_t secs = (uint32_t)atoi(argv[++i]);
            const uint32_t before = s.rows_seen;
            for (uint32_t t = 0; t < secs && rc == 0; t++) {
                /*
                 * Every ten seconds, because a hold that says nothing until it
                 * ends cannot be told from a hold that is never going to end -
                 * which is exactly what happened the first time this ran, and
                 * cost a rebuild to find out.
                 */
                if ((t % 10u) == 0u) {
                    ag_printf("  %u/%u s, %u rows\n", (unsigned)t,
                              (unsigned)secs, (unsigned)(s.rows_seen - before));
                }
                if (!pump(1000)) {
                    ag_printf("link lost after %u s\n", (unsigned)t);
                    rc = 1;
                }
            }
            if (rc == 0) {
                ag_printf("held %u s, %u rows arrived\n", (unsigned)secs,
                          (unsigned)(s.rows_seen - before));
            }
        } else if (strcmp(argv[i], "-tap") == 0 && i + 1 < argc) {
            uint16_t x = 0, y = 0;
            if (!parse_point(argv[++i], &x, &y)) {
                ag_printf("not a point: %s\n", argv[i]);
                rc = 1;
            } else {
                send_tap(x, y, 1);
                if (!pump(200)) {
                    rc = 1;
                }
            }
        } else if (strcmp(argv[i], "-right") == 0 && i + 1 < argc) {
            uint16_t x = 0, y = 0;
            if (!parse_point(argv[++i], &x, &y)) {
                ag_printf("not a point: %s\n", argv[i]);
                rc = 1;
            } else {
                send_tap(x, y, 2);
                if (!pump(200)) {
                    rc = 1;
                }
            }
        } else if (strcmp(argv[i], "-screen") == 0) {
            print_screen();
        }
    }

    ag_net_close(s.sock);
    return rc;
}
