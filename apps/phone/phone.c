/*
 * ArgonOS - this machine's screen and keyboard, on a phone.
 *
 * A board with no glass and no keys is still a machine somebody has to look at
 * and type into.  REMDISP answered that with two soldered wires to a board that
 * has a panel (docs/09-esp32-cyd.md); this answers it with the screen and
 * keyboard almost everybody already carries.  The phone joins the board's Wi-Fi
 * - or the board joins the house's - and opens a page the board itself serves.
 *
 * It is the same idea as REMDISP and deliberately the same shape: a display
 * device whose panel is a socket, and nothing above it can tell the difference.
 * Both ways in lead here, because the kernel hands a presented blit to every
 * display device that has a blit_rect, and the console reaches a panel with no
 * framebuffer as characters.  Applications need no change and do not know.
 *
 * Three things make this different from the wire, and each one is a decision:
 *
 *   The client is served from here.  Not a file to copy onto a phone, not a
 *   store listing - a GET of "/" returns the page, and the page is in this
 *   image.  A tool that has to be installed somewhere else before it can reach
 *   the machine is not a tool the machine has.  It costs about twenty
 *   kilobytes of flash, which is the resource this system has the most of.
 *
 *   The transport is a WebSocket, because a page cannot open a TCP socket and
 *   a phone is a page unless somebody installs something.  The framing is in
 *   apps/common/ws; there is no socket in that file and every parse in it is
 *   checked on the PC.
 *
 *   There is a task.  REMDISP could not have one when it was written, so
 *   hashing, trimming, encoding and the write all happened on whichever task
 *   drew the frame - and the game paid three hundred milliseconds for a screen
 *   it did not ask to be remote.  Since ABI 0.48 a module can own a task
 *   (sys->module_task), so blit_rect here does the one thing that must happen
 *   while the caller's pixels still exist - copies the rectangle into a frame
 *   of this driver's own - and marks the damage.  Everything else is the task's.
 *
 * What is NOT here, on purpose:
 *
 *   No authentication.  Anybody who can reach the port gets the screen and the
 *   keyboard, which is the same standing telnet has in this system and is said
 *   in the same place: it is off until somebody turns it on, and what turns it
 *   on is loading this driver.  On a board that matters, put it behind the
 *   board's own access point and nothing else.
 *
 *   No printing from the display callbacks.  The kernel calls them with the
 *   device registry held, while the console task takes the console first and
 *   the registry second - a driver that reaches for the console from inside one
 *   closes the ring and stops the machine.  Everything this has to say, it says
 *   from ag_driver_init or from its own task.
 *
 *   No retry and no acknowledgement above TCP.  A band that does not go out is
 *   a stale rectangle until something redraws it, and the text path repairs
 *   itself on a sweep.  A driver that blocked waiting for a phone to agree
 *   would trade a smear for a stopped machine.
 *
 * Build (see tools/apps.json for the authoritative line):
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc \
 *       --include sdk/include --include apps/common/pixband \
 *       --include apps/common/ws --include apps/phone \
 *       -o build/apps/PHONE.SYS apps/phone/phone.c \
 *       apps/common/pixband/ag_pixband.c apps/common/ws/ag_ws.c
 *
 *   drv install a:\phone.sys
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/keys.h>

#include <stdio.h>
#include <string.h>

#include "ag_pixband.h"
#include "ag_ws.h"
#include "page.h"

AG_DRV("PHONE", "1.0", "argon");

/* ------------------------------------------------------------------------ */
/* Sizes, and why each one is what it is                                     */
/* ------------------------------------------------------------------------ */

/*
 * Not 80, and not 8080 either.  Both are ports something else on a network
 * expects to mean something, and HTTPD.AXE in this tree already takes 80 by
 * default - two servers on one port is one of them silently not starting.
 */
#define PHONE_PORT_DEFAULT 8765u

/*
 * A band is at most eight rows and five hundred and twelve columns.
 *
 * Rows for the reason REMDISP gives - a receiver draws a band as one window
 * setting, and a band of one row spends more time setting up than drawing.
 * Columns because a surface here can be wider than a panel: the soft
 * framebuffer in QEMU is 640 across, and a band the full width of it would need
 * a scratch buffer sized for a width nobody has measured.  Splitting sideways
 * costs one extra header per eight rows and removes the limit entirely.
 */
#define BAND_ROWS 8u
#define BAND_MAX_W 512u
#define BAND_MAX_PX (BAND_ROWS * BAND_MAX_W)

/* Worst case for PackBits, plus the geometry header this transport prepends. */
#define BAND_HDR 8u
#define BAND_CAP (BAND_HDR + BAND_MAX_PX * 2u + BAND_MAX_PX / 64u + 64u)

/*
 * The console mirror.  Capped rather than allocated to whatever the console
 * turns out to be, because this is two bytes a cell held for the life of the
 * driver and a console larger than this is not a phone screen.
 */
#define TEXT_MAX_COLS 100u
#define TEXT_MAX_ROWS 40u

/* One HTTP request's headers.  Longer than this is not a request from a page. */
#define REQ_MAX 1536u

/* A client's WebSocket frame.  Input messages are tens of bytes. */
#define WS_IN_MAX 512u

/* Events waiting to be read out of /dev/pkbd0 by an application that wants them. */
#define KEY_RING 64u

/* ------------------------------------------------------------------------ */
/* The protocol inside the WebSocket                                         */
/* ------------------------------------------------------------------------ */

/*
 * Each WebSocket binary message is one op byte and its payload.  The framing
 * below the op is the WebSocket's, so there is no magic, no length and no
 * checksum here - a message either arrives whole or does not arrive.
 *
 * Board to phone:
 *   'M'  u16 surf_w, u16 surf_h, u8 cols, u8 rows, u8 cell_w, u8 cell_h,
 *        u8 flags
 *   'R'  u8 row, u8 count, count x (ch, attr)
 *   'C'  u8 col, u8 row, u8 ch, u8 attr, u8 visible
 *   'B'  u16 x, u16 y, u16 w, u16 h, pixels RGB565 as they are
 *   'P'  the same geometry, then PackBits over the pixels
 *   'I'  the same geometry, then a palette and packed indices
 *
 *   'A'  16 bytes of nonce - "say who you are before I show you anything"
 *
 * Phone to board:
 *   'H'  (nothing) - "I have just arrived": send the geometry and everything
 *   'K'  u8 kind (1 down, 2 up), u8 mods, u16 hid, u16 unicode, u8 repeat
 *   'P'  u8 kind (1 down, 2 up, 3 move, 4 wheel), u8 buttons, i16 x, i16 y,
 *        i8 wheel
 *   'T'  UTF-8 text - what a phone's own keyboard produces, where there is no
 *        key to name
 *   'A'  20 bytes - SHA1(nonce || password), the answer to the challenge
 *
 * 'B' / 'P' / 'I' are ag_pixband's own op bytes, so the encoder's choice is
 * the wire's op with nothing in between.
 */
#define OP_INFO 'M'
#define OP_ROW 'R'
#define OP_CURSOR 'C'
#define OP_AUTH 'A'

#define IN_HELLO 'H'
#define IN_KEY 'K'
#define IN_PTR 'P'
#define IN_TEXT 'T'
#define IN_AUTH 'A'

#define INFO_HAS_TEXT 0x01u

/*
 * The password, and what it is and is not.
 *
 * Without one, whoever reaches the port has the screen and the keyboard.  That
 * is the same standing telnet has here, and it is fine for a board on a bench
 * and wrong for anything else - which is why `[phone] password` exists and why
 * leaving it out still means no password: a board that suddenly refused its
 * owner after an update would be worse than one that never asked.
 *
 * It is a challenge and a response, not a password sent and compared.  The
 * board makes a nonce, both sides hash it with the password, and only the
 * digest crosses - so the password itself is never on a wire that has no TLS,
 * and a digest somebody copied is worth nothing on the next connection.
 *
 * What this is NOT: it is not encryption.  Everything after the answer - the
 * screen, the keys - crosses in the clear, because a board of this size cannot
 * carry TLS under a video link.  It keeps strangers out; it does not keep a
 * listener from watching over your shoulder.  Said here rather than left to be
 * assumed.
 */
#define PASS_MAX 32
#define NONCE_LEN 16
#define DIGEST_LEN 20

/* How long a wrong answer costs.  Not a lockout - a board that locks itself is
 * a board its owner cannot use either - but enough that guessing a password
 * over a network is a week's work rather than an afternoon's. */
#define BAD_PASS_DELAY_MS 1500u

/* ------------------------------------------------------------------------ */
/* State                                                                     */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint8_t  type; /* 1 down, 2 up  */
    uint8_t  mods;
    uint16_t hid;
    uint16_t unicode;
    uint8_t  repeat;
} keyev_t;

static struct {
    volatile bool stop;
    bool          publish_text;

    uint16_t port;

    ag_mutex_t lock; /* the frame, the damage, the text shadow              */

    /*
     * The picture, as this driver's own copy of the surface.
     *
     * Two sizes, and the difference is the whole reason the first frame is not
     * lost.  cap_w/cap_h is what was allocated - at ag_driver_init, before
     * anything has drawn, from the size the display is configured for.
     * frame_w/frame_h is the surface actually in use, learned from the first
     * blit that arrives.  The rows are strided by cap_w whatever the surface
     * turns out to be, so a smaller one simply uses part of the buffer.
     *
     * The version before this allocated on seeing the first blit, which meant
     * dropping that blit - and an application that paints once and waits, which
     * is most of them, then had a black screen on the phone for ever.  Measured
     * exactly that way: gfxdemo, 256000 pixels, every one of them #000000.
     */
    uint16_t *frame;
    uint32_t  cap_w;
    uint32_t  cap_h;
    uint32_t  frame_w;
    uint32_t  frame_h;
    /* A surface larger than cap: the task grows the buffer and one frame is
     * lost, which is the rare case rather than the first one. */
    volatile uint32_t want_w;
    volatile uint32_t want_h;

    /* Damage, in surface coordinates.  x1/y1 exclusive; empty when x0 >= x1. */
    uint32_t dx0, dy0, dx1, dy1;

    /* The console mirror. */
    uint16_t      cols, rows;
    uint16_t      cell_w, cell_h;
    ag_textcell_t cells[TEXT_MAX_ROWS][TEXT_MAX_COLS];
    uint8_t       row_dirty[TEXT_MAX_ROWS];
    uint16_t      cur_col, cur_row;
    ag_textcell_t cur_under;
    bool          cur_visible;
    bool          cur_dirty;

    /* The wire. */
    ag_handle_t listen;
    ag_handle_t conn; /* an upgraded WebSocket, or -1                       */
    volatile bool     up;
    volatile bool     owe_everything;

    /* Who may look.  `password` empty means anybody. */
    char     password[PASS_MAX + 1];
    bool     authed;      /* this connection has answered                   */
    bool     challenged;  /* ...and has been asked                          */
    uint8_t  nonce[NONCE_LEN];
    uint32_t nonce_seq;

    /* Working memory, the task's alone once it is running. */
    ag_pixband_ctx_t *pix;
    uint16_t         *band;
    uint8_t          *out;

    /* What an application reading /dev/pkbd0 has not taken yet. */
    keyev_t  ring[KEY_RING];
    uint16_t head, tail, count;
} s;

static const ag_task_api_t *TASK;

static void lock(void)
{
    if (s.lock != NULL) {
        (void)TASK->mutex_lock(s.lock, 1000u);
    }
}

static void unlock(void)
{
    if (s.lock != NULL) {
        TASK->mutex_unlock(s.lock);
    }
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* ------------------------------------------------------------------------ */
/* The socket                                                                */
/* ------------------------------------------------------------------------ */

/*
 * Everything that reaches the network is here, and all of it is on the task.
 * A short write is retried against a deadline rather than dropped: a partial
 * WebSocket frame is not a stale rectangle, it is a stream the client can no
 * longer parse, and the only honest recovery from one is to close.
 */
static bool send_all(ag_handle_t h, const void *buf, uint32_t len,
                     uint32_t deadline_ms)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t       at = 0;
    const uint64_t until = ag_micros() + (uint64_t)deadline_ms * 1000ull;

    while (at < len) {
        const int32_t n = ag_net_send(h, p + at, len - at);
        if (n > 0) {
            at += (uint32_t)n;
            continue;
        }
        if (n != -AG_EAGAIN) {
            return false;
        }
        if (ag_micros() > until) {
            return false;
        }
        TASK->sleep_ms(2u);
    }
    return true;
}

/* One WebSocket binary message: the op byte and its payload, in one write. */
static bool ws_send(uint8_t op, const void *payload, uint32_t len)
{
    if (s.conn < 0) {
        return false;
    }
    uint8_t hdr[11];
    const uint32_t n = ag_ws_hdr_build(hdr, AG_WS_BIN, len + 1u, true);
    hdr[n] = op;

    if (!send_all(s.conn, hdr, n + 1u, 2000u)) {
        return false;
    }
    if (len > 0u && !send_all(s.conn, payload, len, 2000u)) {
        return false;
    }
    return true;
}

/*
 * Say goodbye before hanging up, when there is something to say.
 *
 * One client at a time is the rule here - there is one screen and one keyboard,
 * and two people typing into them is not a feature - so a second browser takes
 * the link off the first.  Closing the socket without a word makes that look
 * like a fault: the page that lost it reconnects, takes the link back, and the
 * two spin against each other about once a second for ever.  Reported as
 * "reconnecting every two seconds", and it was two browsers being polite to
 * each other.
 *
 * A WebSocket close carries a code and a reason, so the one being replaced is
 * told why and stops trying.  Best effort: the frame goes out with a short
 * deadline and the socket closes whether or not it got there.
 */
static void ws_close(const char *reason)
{
    if (s.conn < 0 || reason == NULL) {
        return;
    }
    const uint32_t n = (uint32_t)strlen(reason);
    if (n > 100u) {
        return;
    }
    uint8_t buf[128];
    const uint32_t k = ag_ws_hdr_build(buf, AG_WS_CLOSE, n + 2u, true);
    buf[k] = 0x03u; /* 1000, a normal closure */
    buf[k + 1u] = 0xe8u;
    memcpy(buf + k + 2u, reason, n);
    (void)send_all(s.conn, buf, k + 2u + n, 200u);
}

static void drop_conn(const char *why)
{
    if (s.conn >= 0) {
        (void)ag_net_close(s.conn);
        s.conn = -1;
        ag_log(AG_LOG_INFO, "phone", "client gone (%s)", why ? why : "?");
    }
}

/* ------------------------------------------------------------------------ */
/* Board to phone: the geometry, the console, the picture                    */
/* ------------------------------------------------------------------------ */

/* ---- the password ------------------------------------------------------ */

static bool needs_password(void) { return s.password[0] != '\0'; }

/*
 * A nonce that does not repeat.
 *
 * There is no random in the ABI, and this does not need one: what a challenge
 * has to be is fresh, so that a digest somebody copied off the wire is worth
 * nothing the next time.  A counter and the microsecond clock, hashed, are
 * fresh.  Guessing the nonce buys an attacker nothing anyway - without the
 * password the digest cannot be computed from it.
 */
static void make_nonce(void)
{
    struct {
        uint64_t us;
        uint32_t seq;
        uint32_t conn;
    } seed;

    seed.us = (uint64_t)ag_micros();
    seed.seq = ++s.nonce_seq;
    seed.conn = (uint32_t)s.conn;

    uint8_t digest[DIGEST_LEN];
    ag_ws_sha1(&seed, sizeof(seed), digest);
    memcpy(s.nonce, digest, NONCE_LEN);
}

static void expected_digest(uint8_t out[DIGEST_LEN])
{
    uint8_t buf[NONCE_LEN + PASS_MAX];
    const uint32_t n = (uint32_t)strlen(s.password);

    memcpy(buf, s.nonce, NONCE_LEN);
    memcpy(buf + NONCE_LEN, s.password, n);
    ag_ws_sha1(buf, NONCE_LEN + n, out);
}

static bool send_info(void)
{
    uint8_t p[9];
    lock();
    const uint32_t w = (s.frame_w != 0u) ? s.frame_w : s.cap_w;
    const uint32_t h = (s.frame_h != 0u) ? s.frame_h : s.cap_h;
    const uint16_t cols = s.cols;
    const uint16_t rows = s.rows;
    unlock();

    put16(p + 0, w);
    put16(p + 2, h);
    p[4] = (uint8_t)cols;
    p[5] = (uint8_t)rows;
    /*
     * The cell, because a tap in the text screen has to arrive as the pixels a
     * pointer event is measured in (ABI 0.42) and this driver injects directly
     * - nothing between here and the console queue scales anything.  Where the
     * console has no surface at all it says zero, and the page falls back to
     * 8x16, which is the font the kernel draws with.
     */
    p[6] = (uint8_t)s.cell_w;
    p[7] = (uint8_t)s.cell_h;
    p[8] = s.publish_text ? INFO_HAS_TEXT : 0u;
    return ws_send(OP_INFO, p, sizeof(p));
}

static bool send_text_row(uint16_t row)
{
    uint8_t p[2u + TEXT_MAX_COLS * 2u];

    lock();
    const uint16_t count = s.cols;
    p[0] = (uint8_t)row;
    p[1] = (uint8_t)count;
    for (uint16_t x = 0; x < count; x++) {
        p[2u + x * 2u] = s.cells[row][x].ch;
        p[3u + x * 2u] = s.cells[row][x].attr;
    }
    s.row_dirty[row] = 0u;
    unlock();

    return ws_send(OP_ROW, p, 2u + (uint32_t)count * 2u);
}

static bool send_cursor(void)
{
    uint8_t p[5];

    lock();
    p[0] = (uint8_t)s.cur_col;
    p[1] = (uint8_t)s.cur_row;
    p[2] = s.cur_under.ch;
    p[3] = s.cur_under.attr;
    p[4] = s.cur_visible ? 1u : 0u;
    s.cur_dirty = false;
    unlock();

    return ws_send(OP_CURSOR, p, sizeof(p));
}

/*
 * One band of the picture: taken out of the frame under the lock, encoded and
 * sent without it.
 *
 * The split matters and is the whole reason this driver has a task.  The copy
 * is a few thousand bytes and settles who may write the frame while it is being
 * read; the encode is hundreds of microseconds and the send is however long the
 * network takes, and neither is anything an application that happened to draw
 * should be made to wait for.
 */
static bool send_band(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    const uint32_t n = w * h;
    if (n == 0u || n > BAND_MAX_PX) {
        return true;
    }

    lock();
    if (s.frame == NULL || x + w > s.frame_w || y + h > s.frame_h) {
        unlock();
        return true;
    }
    /* Strided by the allocation, not by the surface: see cap_w above. */
    for (uint32_t r = 0; r < h; r++) {
        memcpy(s.band + r * w, s.frame + (size_t)(y + r) * s.cap_w + x,
               (size_t)w * sizeof(uint16_t));
    }
    unlock();

    uint8_t op = 0;
    const uint32_t len = ag_pixband_encode(s.pix, s.band, n, s.out + BAND_HDR,
                                           BAND_CAP - BAND_HDR, &op);
    if (len == 0u) {
        return true; /* refused rather than truncated: see ag_pixband.h */
    }
    put16(s.out + 0, x);
    put16(s.out + 2, y);
    put16(s.out + 4, w);
    put16(s.out + 6, h);
    return ws_send(op, s.out, BAND_HDR + len);
}

/*
 * Everything the phone is owed, in one pass, and then out to wait for more.
 *
 * Returns false when the connection has gone; the caller drops it rather than
 * carrying on into a socket that is not there.
 */
static bool service_client(void)
{
    /*
     * Nothing before the answer.  Not the geometry, not a row of the console,
     * not a band of the picture - a screen shown to somebody who has not said
     * the password is the screen given away, whatever happens afterwards.
     */
    if (needs_password() && !s.authed) {
        if (!s.challenged) {
            make_nonce();
            s.challenged = true;
            return ws_send(OP_AUTH, s.nonce, NONCE_LEN);
        }
        return true;
    }

    if (s.owe_everything) {
        s.owe_everything = false;
        if (!send_info()) {
            return false;
        }
        lock();
        for (uint32_t r = 0; r < s.rows; r++) {
            s.row_dirty[r] = 1u;
        }
        s.cur_dirty = true;
        if (s.frame != NULL) {
            s.dx0 = 0;
            s.dy0 = 0;
            s.dx1 = s.frame_w;
            s.dy1 = s.frame_h;
        }
        unlock();
    }

    for (uint32_t r = 0; r < s.rows; r++) {
        if (s.row_dirty[r] && !send_text_row((uint16_t)r)) {
            return false;
        }
    }
    if (s.cur_dirty && !send_cursor()) {
        return false;
    }

    /*
     * The damage, taken whole and then walked in bands.  Taken rather than
     * walked in place because blit_rect goes on marking while this sends, and a
     * rectangle that keeps growing under the sender is a sender that never
     * finishes.  What arrives during the pass is marked afresh and goes out on
     * the next one, which is one frame of latency and no starvation.
     */
    lock();
    const uint32_t x0 = s.dx0, y0 = s.dy0, x1 = s.dx1, y1 = s.dy1;
    s.dx0 = 0;
    s.dy0 = 0;
    s.dx1 = 0;
    s.dy1 = 0;
    unlock();

    if (x0 >= x1 || y0 >= y1) {
        return true;
    }
    for (uint32_t y = y0; y < y1; y += BAND_ROWS) {
        uint32_t h = y1 - y;
        if (h > BAND_ROWS) {
            h = BAND_ROWS;
        }
        for (uint32_t x = x0; x < x1; x += BAND_MAX_W) {
            uint32_t w = x1 - x;
            if (w > BAND_MAX_W) {
                w = BAND_MAX_W;
            }
            if (!send_band(x, y, w, h)) {
                return false;
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* Phone to board: keys and the pointer                                      */
/* ------------------------------------------------------------------------ */

static void ring_push(const keyev_t *e)
{
    if (s.count >= KEY_RING) {
        s.tail = (uint16_t)((s.tail + 1u) % KEY_RING);
        s.count--;
    }
    s.ring[s.head] = *e;
    s.head = (uint16_t)((s.head + 1u) % KEY_RING);
    s.count++;
}

static void inject_key(uint8_t kind, uint8_t mods, uint16_t hid,
                       uint32_t unicode, bool repeat)
{
    if (hid == 0u && unicode == 0u) {
        return;
    }
    /*
     * Ctrl+C and F12 are the supervisor's, not the application's - the same
     * exclusion KBDVIRT makes and for the same reason.  A phone that can stop
     * the foreground process is a phone that stops it by accident.
     */
    if ((mods & AG_MOD_CTRL) && hid == (uint16_t)AG_KEY_C) {
        return;
    }
    if (hid == (uint16_t)AG_KEY_F12) {
        return;
    }

    ag_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = (kind == 2u) ? AG_EV_KEY_UP : AG_EV_KEY_DOWN;
    ev.key.keycode = hid;
    ev.key.unicode = unicode;
    ev.key.mods = mods;
    ev.key.repeat = repeat;
    (void)ag_inject_event(&ev);

    const keyev_t k = {kind, mods, hid, (uint16_t)unicode, repeat ? 1u : 0u};
    lock();
    ring_push(&k);
    unlock();
}

/*
 * One code point out of UTF-8, or 0 when the sequence is malformed.
 *
 * A phone's own keyboard is the reason this exists: it reports "a character was
 * composed", not "a key went down", and for anything past ASCII there is no HID
 * usage to name.  So the page sends the text and this turns it into a keystroke
 * with a unicode and no keycode, which is what every text application in this
 * system reads anyway.
 */
static uint32_t utf8_next(const uint8_t *p, uint32_t len, uint32_t *at)
{
    if (*at >= len) {
        return 0;
    }
    const uint8_t c = p[(*at)++];
    if (c < 0x80u) {
        return c;
    }
    uint32_t need;
    uint32_t cp;
    if ((c & 0xe0u) == 0xc0u) {
        need = 1u;
        cp = c & 0x1fu;
    } else if ((c & 0xf0u) == 0xe0u) {
        need = 2u;
        cp = c & 0x0fu;
    } else if ((c & 0xf8u) == 0xf0u) {
        need = 3u;
        cp = c & 0x07u;
    } else {
        return 0;
    }
    if (*at + need > len) {
        *at = len;
        return 0;
    }
    for (uint32_t i = 0; i < need; i++) {
        const uint8_t k = p[(*at)++];
        if ((k & 0xc0u) != 0x80u) {
            return 0;
        }
        cp = (cp << 6) | (uint32_t)(k & 0x3fu);
    }
    return cp;
}

static void handle_message(const uint8_t *p, uint32_t len)
{
    if (len == 0u) {
        return;
    }
    const uint8_t op = p[0];
    const uint8_t *body = p + 1;
    const uint32_t n = len - 1u;

    /*
     * Before the answer, the only thing this end listens to is the answer.  A
     * keystroke from an unauthenticated client is a keystroke at the machine's
     * shell, so it is not "harmless until the screen is shown" - it is the
     * whole of what a password is for.
     */
    if (needs_password() && !s.authed && op != IN_AUTH) {
        return;
    }

    switch (op) {
    case IN_AUTH: {
        if (!needs_password() || s.authed || !s.challenged ||
            n != DIGEST_LEN) {
            return;
        }
        uint8_t want[DIGEST_LEN];
        expected_digest(want);
        if (memcmp(want, body, DIGEST_LEN) != 0) {
            ag_log(AG_LOG_WARN, "phone", "wrong password");
            /* Slowly, and then hang up.  Slowly so that guessing over a network
             * is a week rather than an afternoon; hang up so the next attempt
             * has to take a fresh nonce. */
            TASK->sleep_ms(BAD_PASS_DELAY_MS);
            ws_close("badpass");
            drop_conn("wrong password");
            return;
        }
        s.authed = true;
        s.owe_everything = true;
        ag_log(AG_LOG_INFO, "phone", "client authenticated");
        break;
    }

    case IN_HELLO:
        s.owe_everything = true;
        break;

    case IN_KEY:
        if (n >= 7u) {
            inject_key(body[0], body[1], rd16(body + 2), rd16(body + 4),
                       body[6] != 0u);
        }
        break;

    case IN_PTR:
        if (n >= 7u) {
            ag_event_t ev;
            memset(&ev, 0, sizeof(ev));
            switch (body[0]) {
            case 1u:
                ev.type = AG_EV_POINTER_DOWN;
                break;
            case 2u:
                ev.type = AG_EV_POINTER_UP;
                break;
            case 4u:
                ev.type = AG_EV_WHEEL;
                break;
            default:
                ev.type = AG_EV_POINTER_MOVE;
                break;
            }
            ev.ptr.buttons = body[1];
            ev.ptr.x = (int16_t)rd16(body + 2);
            ev.ptr.y = (int16_t)rd16(body + 4);
            if (ev.type == AG_EV_WHEEL) {
                ev.ptr.dy = (int8_t)body[6];
            }
            (void)ag_inject_event(&ev);
        }
        break;

    case IN_TEXT: {
        uint32_t at = 0;
        while (at < n) {
            const uint32_t cp = utf8_next(body, n, &at);
            if (cp == 0u) {
                break;
            }
            inject_key(1u, 0u, 0u, cp, false);
            inject_key(2u, 0u, 0u, cp, false);
        }
        break;
    }

    default:
        break;
    }
}

/*
 * Read whatever has arrived and act on it.  False when the connection has gone
 * or said something this does not speak, which is the same answer: close.
 */
static bool pump_client(void)
{
    static uint8_t buf[WS_IN_MAX + 16u];
    static uint32_t have;

    for (;;) {
        const int32_t n = ag_net_recv(s.conn, buf + have,
                                      sizeof(buf) - have);
        if (n == -AG_EAGAIN) {
            break;
        }
        if (n <= 0) {
            have = 0;
            return false;
        }
        have += (uint32_t)n;

        for (;;) {
            ag_ws_hdr_t h;
            const int32_t hn = ag_ws_hdr_parse(buf, have, WS_IN_MAX, &h);
            if (hn == 0) {
                break; /* the rest of the header has not arrived */
            }
            if (hn < 0) {
                have = 0;
                return false;
            }
            if (have < h.hdr + h.len) {
                break; /* the payload has not arrived */
            }
            uint8_t *body = buf + h.hdr;
            if (h.masked) {
                ag_ws_unmask(body, h.len, h.mask);
            }

            switch (h.opcode) {
            case AG_WS_BIN:
            case AG_WS_TEXT:
                handle_message(body, h.len);
                break;
            case AG_WS_PING: {
                uint8_t hdr[11];
                const uint32_t k = ag_ws_hdr_build(hdr, AG_WS_PONG, h.len,
                                                   true);
                if (!send_all(s.conn, hdr, k, 500u) ||
                    (h.len > 0u && !send_all(s.conn, body, h.len, 500u))) {
                    have = 0;
                    return false;
                }
                break;
            }
            case AG_WS_CLOSE:
                have = 0;
                return false;
            case AG_WS_PONG:
                break;
            default:
                /* A continuation frame: nothing here fragments, so a peer that
                 * does is speaking something else.  See ag_ws.h. */
                have = 0;
                return false;
            }

            const uint32_t used = h.hdr + h.len;
            memmove(buf, buf + used, have - used);
            have -= used;
        }

        if (have >= sizeof(buf) - 16u) {
            /* A frame larger than this will never complete in this buffer, and
             * WS_IN_MAX already refused the ones that claim to be. */
            have = 0;
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* HTTP: the page, and the upgrade                                           */
/* ------------------------------------------------------------------------ */

/*
 * Case-insensitive search for a header, returning its value with the leading
 * spaces gone.  Written out rather than taken from the kernel's netmsg.c for
 * the reason the whole of apps/common exists: this is a .SYS and has the ABI,
 * not the kernel's internals.
 */
static const char *header_value(const char *req, const char *name, char *out,
                                uint32_t cap)
{
    const uint32_t nlen = (uint32_t)strlen(name);

    for (const char *p = req; *p != '\0'; p++) {
        if (p != req && p[-1] != '\n') {
            continue;
        }
        uint32_t i = 0;
        while (i < nlen && p[i] != '\0') {
            const char a = (char)((p[i] >= 'A' && p[i] <= 'Z') ? p[i] + 32
                                                               : p[i]);
            const char b = (char)((name[i] >= 'A' && name[i] <= 'Z')
                                      ? name[i] + 32
                                      : name[i]);
            if (a != b) {
                break;
            }
            i++;
        }
        if (i != nlen || p[i] != ':') {
            continue;
        }
        const char *v = p + nlen + 1u;
        while (*v == ' ' || *v == '\t') {
            v++;
        }
        uint32_t k = 0;
        while (v[k] != '\0' && v[k] != '\r' && v[k] != '\n' && k + 1u < cap) {
            out[k] = v[k];
            k++;
        }
        out[k] = '\0';
        return out;
    }
    return NULL;
}

static void serve_404(ag_handle_t h)
{
    static const char k_404[] = "HTTP/1.1 404 Not Found\r\n"
                                "Content-Length: 0\r\n"
                                "Connection: close\r\n\r\n";
    (void)send_all(h, k_404, (uint32_t)(sizeof(k_404) - 1u), 1000u);
}

static void serve_page(ag_handle_t h)
{
    char hdr[160];
    const int n = snprintf(hdr, sizeof(hdr),
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: text/html; charset=utf-8\r\n"
                              "Content-Length: %u\r\n"
                              "Cache-Control: no-store\r\n"
                              "Connection: close\r\n\r\n",
                              (unsigned)AG_PHONE_PAGE_LEN);
    if (n <= 0) {
        return;
    }
    if (!send_all(h, hdr, (uint32_t)n, 2000u)) {
        return;
    }
    /*
     * Ten seconds, which is generous and deliberate.  This is one write of
     * twenty kilobytes to a phone that may be three rooms away on a weak
     * signal, and the alternative to waiting is a page that arrives truncated -
     * which a browser renders as a blank screen and no error.
     */
    (void)send_all(h, ag_phone_page, AG_PHONE_PAGE_LEN, 10000u);
}

/*
 * A fresh connection: read its request, then either answer it and close, or
 * turn it into the WebSocket.
 *
 * Returns true when the handle has become the live client and must not be
 * closed by the caller.
 */
static bool serve_request(ag_handle_t h)
{
    char           req[REQ_MAX];
    uint32_t       have = 0;
    const uint64_t until = ag_micros() + 3000000ull;

    /*
     * The headers, to the blank line.  With a deadline, because a connection
     * that opens and says nothing is either a port scanner or a browser that
     * changed its mind, and this task has a screen to feed.
     */
    for (;;) {
        const int32_t n = ag_net_recv(h, req + have, sizeof(req) - 1u - have);
        if (n > 0) {
            have += (uint32_t)n;
            req[have] = '\0';
            if (strstr(req, "\r\n\r\n") != NULL || strstr(req, "\n\n") != NULL) {
                break;
            }
            if (have >= sizeof(req) - 1u) {
                serve_404(h);
                return false;
            }
            continue;
        }
        if (n != -AG_EAGAIN) {
            return false;
        }
        if (ag_micros() > until) {
            return false;
        }
        TASK->sleep_ms(5u);
    }

    char key[64];
    if (header_value(req, "Sec-WebSocket-Key", key, sizeof(key)) != NULL) {
        char accept[AG_WS_ACCEPT_LEN];
        if (!ag_ws_accept_key(key, accept)) {
            serve_404(h);
            return false;
        }
        char resp[220];
        const int n = snprintf(resp, sizeof(resp),
                                  "HTTP/1.1 101 Switching Protocols\r\n"
                                  "Upgrade: websocket\r\n"
                                  "Connection: Upgrade\r\n"
                                  "Sec-WebSocket-Accept: %s\r\n\r\n",
                                  accept);
        if (n <= 0 || !send_all(h, resp, (uint32_t)n, 2000u)) {
            return false;
        }

        ws_close("replaced");
        drop_conn("replaced");
        s.conn = h;
        s.authed = false;
        s.challenged = false;
        s.owe_everything = true;
        ag_log(AG_LOG_INFO, "phone", "client connected");
        return true;
    }

    /*
     * Anything that is not the upgrade gets the page, whatever it asked for -
     * except the paths a browser asks for on its own.  One page, one board: a
     * router that hands out a captive-portal probe and a favicon request should
     * not each be answered with twenty kilobytes.
     */
    if (strncmp(req, "GET ", 4) != 0) {
        serve_404(h);
        return false;
    }
    if (strncmp(req + 4, "/favicon.ico", 12) == 0) {
        serve_404(h);
        return false;
    }
    serve_page(h);
    return false;
}

/* ------------------------------------------------------------------------ */
/* The task                                                                  */
/* ------------------------------------------------------------------------ */

static bool ensure_listen(void)
{
    if (s.listen >= 0) {
        return true;
    }
    if (!ag_net_is_ready()) {
        return false;
    }
    const ag_handle_t h = ag_tcp_listen(s.port);
    if (h < 0) {
        return false;
    }
    s.listen = h;
    (void)ag_net_set_nonblock(s.listen, true);

    uint32_t addr = 0;
    if (ag_net_ifaddr(&addr) == AG_OK && addr != 0u) {
        ag_log(AG_LOG_INFO, "phone", "http://%u.%u.%u.%u:%u/",
               (unsigned)((addr >> 24) & 0xffu), (unsigned)((addr >> 16) & 0xffu),
               (unsigned)((addr >> 8) & 0xffu), (unsigned)(addr & 0xffu),
               (unsigned)s.port);
    } else {
        ag_log(AG_LOG_INFO, "phone", "listening on :%u", (unsigned)s.port);
    }
    return true;
}

/*
 * The working memory, taken here rather than in ag_driver_init for one reason
 * and one only: the surface's size is not known until something draws.  It is
 * taken on this task, which belongs to no process, so it comes off the system
 * heap - the same place ag_driver_init would have got it, and not out of the
 * arena of whichever application happened to be the one that drew.
 */
static bool ensure_memory(void)
{
    if (s.pix == NULL) {
        void *mem = ag_malloc(ag_pixband_size(BAND_MAX_PX));
        if (mem == NULL || !ag_pixband_init(mem, ag_pixband_size(BAND_MAX_PX),
                                            BAND_MAX_PX)) {
            ag_free(mem);
            return false;
        }
        s.pix = (ag_pixband_ctx_t *)mem;
    }
    if (s.band == NULL) {
        s.band = (uint16_t *)ag_malloc(BAND_MAX_PX * sizeof(uint16_t));
        if (s.band == NULL) {
            return false;
        }
    }
    if (s.out == NULL) {
        s.out = (uint8_t *)ag_malloc(BAND_CAP);
        if (s.out == NULL) {
            return false;
        }
    }

    uint32_t w = s.want_w;
    uint32_t h = s.want_h;
    if (w == 0u || h == 0u) {
        return true; /* nothing has asked for more than what is already held */
    }
    if (s.frame != NULL && w <= s.cap_w && h <= s.cap_h) {
        s.want_w = 0;
        s.want_h = 0;
        return true;
    }
    /* Grow rather than resize: a buffer that only ever gets larger cannot be
     * thrashed by two surfaces taking turns. */
    if (w < s.cap_w) {
        w = s.cap_w;
    }
    if (h < s.cap_h) {
        h = s.cap_h;
    }

    uint16_t *fb = (uint16_t *)ag_malloc((size_t)w * h * sizeof(uint16_t));
    if (fb == NULL) {
        ag_log(AG_LOG_WARN, "phone", "no memory for a %ux%u frame (%u KB)",
               (unsigned)w, (unsigned)h,
               (unsigned)((w * h * 2u) / 1024u));
        s.want_w = 0;
        s.want_h = 0;
        return true;
    }
    memset(fb, 0, (size_t)w * h * sizeof(uint16_t));

    lock();
    uint16_t *old = s.frame;
    s.frame = fb;
    s.cap_w = w;
    s.cap_h = h;
    /* The surface in use is whatever the next blit says; until one arrives
     * there is no picture, only a buffer to put one in. */
    s.frame_w = 0;
    s.frame_h = 0;
    s.dx0 = 0;
    s.dy0 = 0;
    s.dx1 = 0;
    s.dy1 = 0;
    s.want_w = 0;
    s.want_h = 0;
    unlock();

    ag_free(old);
    ag_log(AG_LOG_INFO, "phone", "frame %ux%u (%u KB)", (unsigned)w,
           (unsigned)h, (unsigned)((w * h * 2u) / 1024u));
    s.owe_everything = true;
    return true;
}

static void phone_task(void *arg)
{
    (void)arg;

    while (!s.stop) {
        if (!ensure_listen()) {
            TASK->sleep_ms(200u);
            continue;
        }
        if (!ensure_memory()) {
            TASK->sleep_ms(500u);
            continue;
        }

        /*
         * A new connection, whether or not one is already live.  A phone that
         * has locked its screen and come back does not close anything - it just
         * opens a second socket, and the old one sits there being written to
         * for as long as TCP takes to notice.  So the newest upgrade wins.
         */
        const ag_handle_t fresh = ag_tcp_accept(s.listen, 0u);
        if (fresh >= 0) {
            (void)ag_net_set_nonblock(fresh, true);
            if (!serve_request(fresh)) {
                (void)ag_net_close(fresh);
            }
        }

        if (s.conn < 0) {
            TASK->sleep_ms(50u);
            continue;
        }
        if (!pump_client()) {
            drop_conn("closed by peer");
            continue;
        }
        if (!service_client()) {
            drop_conn("write failed");
            continue;
        }

        /*
         * Ten milliseconds, which is the interval a screen is asked for rather
         * than the interval it changes.  Nothing here polls the picture: the
         * damage is marked by blit_rect and this only notices it, so a still
         * screen costs one wakeup a hundredth of a second and no bytes at all.
         */
        TASK->sleep_ms(10u);
    }

    if (s.conn >= 0) {
        (void)ag_net_close(s.conn);
        s.conn = -1;
    }
    if (s.listen >= 0) {
        (void)ag_net_close(s.listen);
        s.listen = -1;
    }

    /*
     * And the memory, here rather than in the unload hook, because this task is
     * the only thing that ever touches it and it is still running when the hook
     * is called.  Nothing in this system used to do this at all: REMDISP leaked
     * five kilobytes of hash grid per load, and an afternoon of `drv install`
     * had lost sixty.  A frame is a hundred and fifty.
     */
    lock();
    uint16_t *frame = s.frame;
    s.frame = NULL;
    s.frame_w = 0;
    s.frame_h = 0;
    unlock();

    ag_free(frame);
    ag_free(s.pix);
    ag_free(s.band);
    ag_free(s.out);
    s.pix = NULL;
    s.band = NULL;
    s.out = NULL;

    /*
     * The mutex is deliberately NOT given back, and that is a leak of about
     * eighty bytes per `drv install`.  It is the smaller of two wrongs: the
     * loader waits for this task and only then takes the device registry to
     * revoke what the module published, so between this line and the revoke the
     * kernel can still call blit_rect - and blit_rect takes this mutex.  Freeing
     * it here is a use-after-free on a real path; holding it is eighty bytes.
     *
     * The frame above is safe by ordering rather than by luck: it is taken away
     * under the lock, so a blit_rect already inside finishes with a frame that
     * still exists and every later one finds NULL and returns.
     */
    s.up = false;
}

/* ------------------------------------------------------------------------ */
/* The display device                                                        */
/* ------------------------------------------------------------------------ */

static ag_err_t phone_info(ag_handle_t h, ag_gfxinfo_t *out)
{
    (void)h;
    if (out == NULL) {
        return -AG_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    out->width = (uint16_t)((s.frame_w != 0u) ? s.frame_w : s.cap_w);
    out->height = (uint16_t)((s.frame_h != 0u) ? s.frame_h : s.cap_h);
    if (out->width == 0u) {
        out->width = 320;
        out->height = 240;
    }
    out->fmt = AG_PIX_RGB565;
    return AG_OK;
}

/*
 * The kernel's rectangle, copied and marked.  Nothing else happens here.
 *
 * Called on whichever task drew, with the device registry held.  The copy is
 * the only thing that has to happen while the caller's pixels still exist; the
 * encoding and the write are the task's, and that is the difference between a
 * game that pays 0.3 ms a frame for a remote screen and one that pays 22.
 */
static void phone_blit_rect(ag_handle_t h, const ag_blit_t *b)
{
    (void)h;
    if (b == NULL || b->px == NULL || b->w == 0u || b->h == 0u) {
        return;
    }

    lock();
    if (s.frame == NULL || b->surf_w > s.cap_w || b->surf_h > s.cap_h) {
        /*
         * Bigger than what is held, or nothing held at all.  Say so without the
         * lock mattering - it is two words and the task re-reads them - and
         * drop this rectangle; the task grows the buffer and the next frame
         * lands.  This is the rare path now, not the first one.
         */
        s.want_w = b->surf_w;
        s.want_h = b->surf_h;
        unlock();
        return;
    }
    if ((uint32_t)b->x + b->w > b->surf_w ||
        (uint32_t)b->y + b->h > b->surf_h) {
        unlock();
        return;
    }
    if (s.frame_w != b->surf_w || s.frame_h != b->surf_h) {
        /* A new surface in a buffer that already fits it: take the geometry,
         * and owe the client a fresh 'M' and a whole screen. */
        s.frame_w = b->surf_w;
        s.frame_h = b->surf_h;
        s.dx0 = 0;
        s.dy0 = 0;
        s.dx1 = 0;
        s.dy1 = 0;
        s.owe_everything = true;
    }

    const uint8_t *src = (const uint8_t *)b->px;
    for (uint32_t r = 0; r < b->h; r++) {
        memcpy(s.frame + (size_t)(b->y + r) * s.cap_w + b->x,
               src + (size_t)r * b->stride, (size_t)b->w * sizeof(uint16_t));
    }

    if (s.dx0 >= s.dx1 || s.dy0 >= s.dy1) {
        s.dx0 = b->x;
        s.dy0 = b->y;
        s.dx1 = (uint32_t)b->x + b->w;
        s.dy1 = (uint32_t)b->y + b->h;
    } else {
        if (b->x < s.dx0) {
            s.dx0 = b->x;
        }
        if (b->y < s.dy0) {
            s.dy0 = b->y;
        }
        if ((uint32_t)b->x + b->w > s.dx1) {
            s.dx1 = (uint32_t)b->x + b->w;
        }
        if ((uint32_t)b->y + b->h > s.dy1) {
            s.dy1 = (uint32_t)b->y + b->h;
        }
    }
    unlock();
}

/* ---- the console, as characters ---------------------------------------- */

static ag_err_t phone_text_info(ag_handle_t h, uint16_t *cols, uint16_t *rows)
{
    (void)h;
    if (cols != NULL) {
        *cols = s.cols;
    }
    if (rows != NULL) {
        *rows = s.rows;
    }
    return AG_OK;
}

static void phone_text_row(ag_handle_t h, uint16_t row,
                           const ag_textcell_t *cells, uint16_t count)
{
    (void)h;
    if (cells == NULL || row >= s.rows) {
        return;
    }
    if (count > s.cols) {
        count = s.cols;
    }
    lock();
    for (uint16_t x = 0; x < count; x++) {
        s.cells[row][x] = cells[x];
    }
    for (uint16_t x = count; x < s.cols; x++) {
        s.cells[row][x].ch = ' ';
        s.cells[row][x].attr = 0x07u;
    }
    s.row_dirty[row] = 1u;
    unlock();
}

static void phone_text_cursor(ag_handle_t h, uint16_t col, uint16_t row,
                              ag_textcell_t under, bool visible)
{
    (void)h;
    if (col >= s.cols || row >= s.rows) {
        return;
    }
    lock();
    s.cur_col = col;
    s.cur_row = row;
    s.cur_under = under;
    s.cur_visible = visible;
    s.cur_dirty = true;
    /* The cell under the caret is now what the phone has there, and the mirror
     * must agree or the next repaint would put the old character back. */
    s.cells[row][col] = under;
    unlock();
}

/* ------------------------------------------------------------------------ */
/* The input device                                                          */
/* ------------------------------------------------------------------------ */

/*
 * Keys and taps reach the console queue from the task, already injected, which
 * is what makes them work for every application without one of them knowing.
 * This device is the other road, for an application that reads its input out of
 * a handle rather than out of ag_poll_event - DOOM does - and it is the same
 * eight-byte packet KBDVIRT's /dev/kbd0 hands out, so such an application needs
 * no second code path.
 */
#define PKT_SIZE 8u

static int32_t pkbd_read(ag_device_t *dev, void *buf, size_t len, uint64_t off)
{
    (void)dev;
    (void)off;
    if (buf == NULL) {
        return -AG_EINVAL;
    }
    if (len < PKT_SIZE) {
        return 0;
    }

    uint8_t     *out = (uint8_t *)buf;
    const size_t max_ev = len / PKT_SIZE;
    size_t       got = 0;

    lock();
    while (got < max_ev && s.count > 0u) {
        const keyev_t *e = &s.ring[s.tail];
        out[0] = e->type;
        out[1] = e->mods;
        out[2] = (uint8_t)(e->hid & 0xffu);
        out[3] = (uint8_t)(e->hid >> 8);
        out[4] = (uint8_t)(e->unicode & 0xffu);
        out[5] = (uint8_t)(e->unicode >> 8);
        out[6] = e->repeat;
        out[7] = 0;
        out += PKT_SIZE;
        s.tail = (uint16_t)((s.tail + 1u) % KEY_RING);
        s.count--;
        got++;
    }
    unlock();

    return (int32_t)(got * PKT_SIZE);
}

/*
 * The kernel's input tick.  Nothing to do - the task does the reading - but the
 * device is registered as an input so that `dev` lists it as one and an
 * application can open it, and a class vtable with a NULL poll would be a
 * device the kernel skips rather than one it leaves alone.
 */
static int32_t pkbd_poll(ag_handle_t h, ag_event_t *out, uint32_t max)
{
    (void)h;
    (void)out;
    (void)max;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Loading and unloading                                                     */
/* ------------------------------------------------------------------------ */

static const ag_display_ops_t k_display_ops_text = {
    .size = sizeof(ag_display_ops_t),
    .info = phone_info,
    .acquire = NULL, /* no surface: the picture is somebody else's memory */
    .release = NULL,
    .flush = NULL,
    .swap = NULL,
    .text_info = phone_text_info,
    .text_row = phone_text_row,
    .text_cursor = phone_text_cursor,
    .blit_rect = phone_blit_rect,
};

static const ag_display_ops_t k_display_ops_pixels = {
    .size = sizeof(ag_display_ops_t),
    .info = phone_info,
    .acquire = NULL,
    .release = NULL,
    .flush = NULL,
    .swap = NULL,
    .text_info = NULL,
    .text_row = NULL,
    .text_cursor = NULL,
    .blit_rect = phone_blit_rect,
};

static const ag_dev_ops_t k_disp_dev_ops = {
    .ioctl = NULL,
};

static const ag_dev_ops_t k_kbd_dev_ops = {
    .read = pkbd_read,
};

static const ag_input_ops_t k_input_ops = {
    .size = sizeof(ag_input_ops_t),
    .poll = pkbd_poll,
    .units = AG_PTR_PIXELS,
    .span_w = 0,
    .span_h = 0,
};

static void phone_fini(void)
{
    /*
     * The flag, and nothing else.  The loader calls this first and then waits
     * for the task to return before the image is unmapped; freeing here would
     * be freeing memory the task is still inside.  See sys->module_task.
     */
    s.stop = true;
}

/*
 * Whether to offer the console as characters.
 *
 * It matters because the picture alone is not enough: the kernel hands a
 * rectangle of pixels to a driver only while an application holds the display.
 * At a shell prompt nothing holds it, nothing is presented, and a phone that
 * only speaks pixels shows a black screen with a working keyboard - which is
 * the least useful thing this could be.  The console reaches a panel as
 * characters or it does not reach it at all.
 *
 * Publishing it is therefore the default, and `[phone] text = no` is for the
 * board where somebody wants the phone to be a second screen for games and
 * nothing else.  Since the kernel now renders the console to *every* text
 * panel, doing this does not take the console off a board's own glass.
 */
static bool want_text(void)
{
    const ag_cfg_api_t *cfg = ag_api()->cfg;

    if (cfg != NULL && AG_HAS(cfg, get_bool)) {
        return cfg->get_bool("phone.text", true);
    }
    return true;
}

ag_err_t ag_driver_init(void)
{
    const ag_api_t *api = ag_api();

    if (api->net == NULL) {
        ag_printf("PHONE: this build has no network\n");
        return -AG_ENOTSUP;
    }
    if (!AG_HAS(api->sys, module_task)) {
        ag_printf("PHONE: needs ABI 0.48 (sys->module_task)\n");
        return -AG_ENOTSUP;
    }

    memset(&s, 0, sizeof(s));
    TASK = api->task;
    s.listen = -1;
    s.conn = -1;

    s.port = (uint16_t)PHONE_PORT_DEFAULT;
    if (api->cfg != NULL && AG_HAS(api->cfg, get_int)) {
        const int32_t p = api->cfg->get_int("phone.port",
                                            (int32_t)PHONE_PORT_DEFAULT);
        if (p > 0 && p < 65536) {
            s.port = (uint16_t)p;
        }
    }

    /*
     * The console's own size, asked for rather than chosen.  A panel that
     * reports a size the kernel treats it as a clamp, and a clamp that
     * disagrees with the console is columns quietly cut off; reporting what is
     * already there cannot disagree with itself.
     */
    ag_coninfo_t ci;
    memset(&ci, 0, sizeof(ci));
    ag_coninfo(&ci);
    s.cols = (ci.cols > 0u && ci.cols <= TEXT_MAX_COLS) ? ci.cols
                                                        : (uint16_t)80u;
    s.rows = (ci.rows > 0u && ci.rows <= TEXT_MAX_ROWS) ? ci.rows
                                                        : (uint16_t)25u;
    s.cell_w = (ci.cell_w > 0u && ci.cell_w < 256u) ? ci.cell_w : (uint16_t)8u;
    s.cell_h = (ci.cell_h > 0u && ci.cell_h < 256u) ? ci.cell_h : (uint16_t)16u;
    for (uint32_t r = 0; r < s.rows; r++) {
        for (uint32_t c = 0; c < s.cols; c++) {
            s.cells[r][c].ch = ' ';
            s.cells[r][c].attr = 0x07u;
        }
    }

    s.lock = TASK->mutex_create();
    if (s.lock == NULL) {
        return -AG_ENOMEM;
    }

    s.publish_text = want_text();

    if (api->cfg != NULL && AG_HAS(api->cfg, get_str)) {
        if (api->cfg->get_str("phone.password", s.password,
                              sizeof(s.password)) != AG_OK) {
            s.password[0] = '\0';
        }
    }

    /*
     * The frame, here and not on the first blit.
     *
     * Allocating on the first blit meant dropping it, and an application that
     * paints once and waits then had a black screen on the phone for ever -
     * measured as gfxdemo, 256000 pixels, all of them black.  Here there is no
     * first blit to drop, and this runs on no process, so the memory comes off
     * the system heap rather than out of whichever application happens to draw.
     *
     * The size is a guess that has to be at least right: [display] width and
     * height when the operator set them, the console's own extent otherwise -
     * a soft framebuffer sizes the console to fit, so 80x25 of 8x16 cells is
     * exactly its 640x400 - and 640x400 when neither says anything.  A guess
     * that comes out too small is not fatal: the task grows the buffer on the
     * first blit that does not fit, at the cost of that one frame.
     */
    {
        const ag_cfg_api_t *cfg = api->cfg;
        uint32_t w = 0, h = 0;
        if (cfg != NULL && AG_HAS(cfg, get_int)) {
            w = (uint32_t)cfg->get_int("display.width", 0);
            h = (uint32_t)cfg->get_int("display.height", 0);
        }
        if (w == 0u || h == 0u) {
            w = (uint32_t)s.cols * s.cell_w;
            h = (uint32_t)s.rows * s.cell_h;
        }
        if (w < 160u || h < 120u || w > 1024u || h > 768u) {
            w = 640u;
            h = 400u;
        }
        s.want_w = w;
        s.want_h = h;
        if (!ensure_memory()) {
            return -AG_ENOMEM;
        }
    }

    ag_module_on_unload(phone_fini);

    const ag_dev_add_t disp = {
        .name = "phone0",
        .driver = "PHONE",
        .cls = AG_DEV_DISPLAY,
        .ops = &k_disp_dev_ops,
        .class_ops = s.publish_text ? &k_display_ops_text
                                    : &k_display_ops_pixels,
        .priv = NULL,
    };
    ag_err_t err = ag_dev_add(&disp);
    if (err != AG_OK) {
        return err;
    }

    const ag_dev_add_t kbd = {
        .name = "pkbd0",
        .driver = "PHONE",
        .cls = AG_DEV_INPUT,
        .ops = &k_kbd_dev_ops,
        .class_ops = &k_input_ops,
        .priv = NULL,
    };
    err = ag_dev_add(&kbd);
    if (err != AG_OK) {
        return err;
    }

    /*
     * The system core, and priority five.
     *
     * Both are the lesson of the wire (docs/plans/desktop.md and the memory of
     * it in REMDISP): a driver task on the application core is a second
     * runnable task on the one core the foreground application is pinned to,
     * and a driver above the applications that does not block starves them.
     * Five is at or below what an application gets, so this cannot.
     */
    if (!ag_module_task(phone_task, NULL, "ag_phone", 8192u, 5,
                        AG_THREAD_SYS_CORE)) {
        return -AG_ENOMEM;
    }
    s.up = true;

    ag_printf("PHONE: open http://<board>:%u/ - %ux%u cells%s, %s\n",
              (unsigned)s.port, (unsigned)s.cols, (unsigned)s.rows,
              s.publish_text ? "" : " (pixels only)",
              needs_password() ? "password set"
                               : "NO PASSWORD ([phone] password to set one)");
    return AG_OK;
}
