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

#include <argon/libc.h>

#include <stdio.h>
#include <string.h>

#include "ag_pixband.h"
#include "ag_ws.h"

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

/* The geometry this transport prepends to a band.  The working buffers are
 * sized at load from the panel that actually turned up - see ensure_memory -
 * because 512 columns of scratch is a quarter of the free memory on a board
 * with no PSRAM, for a screen that is 320 across. */
#define BAND_HDR 8u

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

/*
 * Below this much free memory, the next visitor is turned away at the door.
 *
 * The number and the reason are HTTPD.AXE's, which found them first: a
 * connection accepted when lwIP or the Wi-Fi driver is out of memory does not
 * fail, it aborts the board.  Sixteen kilobytes is measured, not guessed.
 */
#define MEM_FLOOR (16u * 1024u)

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
    ag_mutex_t wire; /* one writer inside one WebSocket frame               */

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
    /*
     * Allocated to the console that is actually there, not to the largest one
     * allowed.  As a static array this was eight kilobytes of bss on every
     * board, including a forty-by-twenty-five one that needs two - and on the
     * CYD eight kilobytes is a quarter of what it has free.
     */
    ag_textcell_t *cells;
    uint8_t        row_dirty[TEXT_MAX_ROWS];
    uint16_t      cur_col, cur_row;
    ag_textcell_t cur_under;
    bool          cur_visible;
    bool          cur_dirty;

    /* The wire. */
    ag_handle_t listen;
    ag_handle_t conn;    /* an upgraded WebSocket, or -1                    */
    ag_handle_t closing; /* answered, waiting to be hung up - see defer_close */
    uint64_t    closing_until;
    volatile bool     up;
    volatile bool     owe_everything;
    /*
     * Just the geometry, without the console behind it.  A surface that
     * changes size while an application is drawing needs the page told; it
     * does not need twenty-five rows of stale console painted over the
     * picture, which is what owing "everything" would do.
     */
    volatile bool     owe_info;

    /* Where the page is, on this board's own disk. */
    char     page[64];

    /* Who may look.  `password` empty means anybody. */
    char     password[PASS_MAX + 1];
    bool     authed;      /* this connection has answered                   */
    bool     challenged;  /* ...and has been asked                          */
    uint8_t  nonce[NONCE_LEN];
    uint32_t nonce_seq;

    /* Working memory, sized to the panel (see ensure_memory). */
    ag_pixband_ctx_t *pix;
    uint16_t         *band;
    uint8_t          *out;
    uint32_t          band_w;
    uint32_t          band_px;
    uint32_t          band_cap;
    uint32_t          direct_at; /* which band the budget stopped on         */
    volatile uint8_t  stage;     /* where the task's loop last was            */
    bool              moaned_frame;
    bool              moaned_mem;
    bool              want_frame;

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
/*
 * Three answers, not two, and the difference is what keeps a link alive.
 *
 * A board can run out of memory without anybody's connection being gone: an
 * application loading takes every byte for a moment, lwIP has nothing to copy
 * into, and sends do not go.  Measured on the CYD: the desktop asking for
 * 71 KB drove the heap to 3212 bytes free, and for two seconds the screen could
 * not send a byte.  Treating that as a lost peer dropped the phone, which then
 * reconnected - which is a link that visibly breaks every time the machine is
 * busy, for no reason.
 *
 * WIRE_STALL is therefore reported only when NOTHING of the message has gone.
 * Once a byte is out the client is mid-frame and there is no way back: a
 * WebSocket frame that stops halfway is not a lost rectangle, it is a stream
 * the client can no longer parse, and the only honest answer is to close.
 */
/*
 * Compare against WIRE_OK.  Never test one of these as a truth value: success
 * is zero, so `if (send_all(...))` reads as "it worked" and means "it failed".
 * That is not hypothetical - turning this from a bool into an enum did exactly
 * that to four callers, and the board answered every page with a correct header
 * and no body at all: "200 OK, 14700 bytes ... 0 bytes in 3.8s".  The compiler
 * says nothing, because an enum converts to bool without a word.
 */
typedef enum {
    WIRE_OK = 0,
    WIRE_STALL, /* nothing sent, nothing broken - try again later */
    WIRE_GONE,  /* the connection is finished, or the stream is  */
} wire_t;

static wire_t send_all(ag_handle_t h, const void *buf, uint32_t len,
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
        /*
         * "Try again" is more than one answer.
         *
         * A non-blocking socket says EAGAIN when its window is full, and lwIP
         * on a board with little memory says ENOMEM when it has no buffer to
         * copy into - which is the same situation and clears the same way, as
         * soon as the peer acknowledges something.  Zero is the third spelling
         * of it.  Treating those as failures is what cut the page off at 8042
         * bytes of 14700 on the CYD, with nothing said: the board simply closed
         * the connection in the middle and the browser got half a page.
         *
         * Anything else really is the connection going away, and then the
         * deadline is what stops this waiting for a phone that has left.
         */
        if (n != -AG_EAGAIN && n != -AG_ENOMEM && n != 0) {
            ag_log(AG_LOG_WARN, "phone", "send failed after %u of %u: %d",
                   (unsigned)at, (unsigned)len, (int)n);
            return WIRE_GONE;
        }
        if (ag_micros() > until) {
            if (at == 0u) {
                return WIRE_STALL; /* nothing started; nothing broken */
            }
            ag_log(AG_LOG_WARN, "phone", "send stalled at %u of %u",
                   (unsigned)at, (unsigned)len);
            return WIRE_GONE;
        }
        TASK->sleep_ms(2u);
    }
    return WIRE_OK;
}

/*
 * One WebSocket binary message: the op byte and its payload, in one write.
 *
 * Under a lock of its own, because two tasks can reach here.  On a board with
 * memory to spare only the driver's task sends, and the lock costs nothing; on
 * one without - the CYD has no PSRAM and a frame it cannot hold - the drawing
 * application sends its own rectangle from blit_rect while the task is sending
 * a row of the console.  Two writers interleaving inside one WebSocket frame is
 * not a torn picture, it is a stream the client can no longer parse.
 *
 * The lock is never held across anything but this write, so the only thing that
 * can wait on it is another write.
 */
/*
 * `hdr_ms` is how long to wait for the frame to START, and `body_ms` how long
 * to finish it once it has.  They are separate because the two are different
 * promises: nothing is committed until the first byte, and everything is once
 * it has gone.
 *
 * The caller chooses them, and the choice is not cosmetic.  The task can afford
 * to wait; blit_rect cannot, because the kernel calls it holding the device
 * registry and every other driver in the machine is behind that lock.  Ten
 * seconds there is not a slow picture, it is a board that has stopped.
 */
/*
 * ONE write for the whole message, and that is why `payload` must have
 * WS_SLACK writable bytes in front of it: the header is built there, so header
 * and body leave as a single buffer.
 *
 * It used to be two writes, and that was a link that dropped itself.  A frame
 * whose header has gone commits the client to waiting for exactly so many
 * bytes, so a body that then cannot go is a stream nobody can parse and the
 * only answer is to close - which is what happened ten seconds after an
 * application started drawing: "client gone (write failed)", with no other
 * word, because a body that never started was silently called a lost peer.
 *
 * As one write there is no half-sent frame to be committed by.  Nothing
 * started is nothing broken, and the caller can simply come back to it.
 */
#define WS_SLACK 11u

static wire_t ws_send(uint8_t op, void *payload, uint32_t len,
                      uint32_t deadline_ms, uint32_t lock_ms)
{
    if (s.conn < 0) {
        return WIRE_GONE;
    }
    uint8_t *p = (uint8_t *)payload;
    uint8_t  hdr[WS_SLACK];
    const uint32_t n = ag_ws_hdr_build(hdr, AG_WS_BIN, len + 1u, true);

    /* Header and op byte immediately before the payload, in its slack. */
    memcpy(p - (n + 1u), hdr, n);
    p[-1] = (int8_t)op;

    /*
     * `lock_ms` is zero for the caller that must not wait at all.
     *
     * blit_rect is called with the device registry held, so anything this waits
     * for is waited for by the console tick, the panel and the touchscreen as
     * well.  Bounding the *send* was not enough: the wire mutex was still taken
     * with three seconds of patience, and three seconds of the registry is a
     * board that has visibly stopped - the glass froze on the loader's line and
     * never came back.  If the task is mid-message, this frame is simply not
     * this frame's to send.
     */
    if (s.wire != NULL && !TASK->mutex_lock(s.wire, lock_ms)) {
        return WIRE_STALL;
    }
    const wire_t r = send_all(s.conn, p - (n + 1u), n + 1u + len, deadline_ms);
    if (s.wire != NULL) {
        TASK->mutex_unlock(s.wire);
    }
    return r;
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

/* ---- the console's size, which is not a constant ------------------------ */

/*
 * Asked for again and again, not remembered once.
 *
 * The size this driver read at load was the size the console had *at load*, and
 * on the CYD that is the wrong one: `[console] cols = auto` resolves against
 * the panel, the panel is a loadable driver, and the kernel therefore sizes the
 * console a second time after the modules stage.  This driver loads inside that
 * stage, so it saw forty by twenty-five and the console became forty by thirty
 * a moment later - and the five rows that were now real never reached the
 * phone, because text_row refuses a row past what it thinks exists.
 *
 * Reported as "the screen is not shown in full, the text is cut".
 *
 * It is also not a one-off fix: `mode con lines=...` changes the console while
 * the system runs, and a screen on the far end of a link should follow it.  So
 * the task asks a few times a second, which is free, and re-lays the mirror
 * when the answer changes.
 */
static bool sync_console(void)
{
    ag_coninfo_t ci;
    memset(&ci, 0, sizeof(ci));
    ag_coninfo(&ci);

    uint16_t cols = (ci.cols > 0u && ci.cols <= TEXT_MAX_COLS) ? ci.cols
                                                               : (uint16_t)80u;
    uint16_t rows = (ci.rows > 0u && ci.rows <= TEXT_MAX_ROWS) ? ci.rows
                                                               : (uint16_t)25u;
    const uint16_t cw = (ci.cell_w > 0u && ci.cell_w < 256u) ? ci.cell_w
                                                             : (uint16_t)8u;
    const uint16_t ch = (ci.cell_h > 0u && ci.cell_h < 256u) ? ci.cell_h
                                                             : (uint16_t)16u;

    if (s.cells != NULL && cols == s.cols && rows == s.rows) {
        s.cell_w = cw;
        s.cell_h = ch;
        return true;
    }

    ag_textcell_t *grid =
        (ag_textcell_t *)ag_malloc((size_t)cols * rows * sizeof(*grid));
    if (grid == NULL) {
        return s.cells != NULL; /* keep what there is rather than lose it */
    }
    for (uint32_t i = 0; i < (uint32_t)cols * rows; i++) {
        grid[i].ch = ' ';
        grid[i].attr = 0x07u;
    }

    lock();
    ag_textcell_t *old = s.cells;
    s.cells = grid;
    s.cols = cols;
    s.rows = rows;
    s.cell_w = cw;
    s.cell_h = ch;
    memset(s.row_dirty, 0, sizeof(s.row_dirty));
    unlock();

    ag_free(old);
    s.owe_everything = true;
    return true;
}

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

static wire_t send_info(void)
{
    uint8_t buf[WS_SLACK + 9];
    uint8_t *p = buf + WS_SLACK;
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
    return ws_send(OP_INFO, p, 9u, 10000u, 3000u);
}

static wire_t send_text_row(uint16_t row)
{
    uint8_t  buf[WS_SLACK + 2u + TEXT_MAX_COLS * 2u];
    uint8_t *p = buf + WS_SLACK;

    lock();
    const uint16_t count = s.cols;
    p[0] = (uint8_t)row;
    p[1] = (uint8_t)count;
    for (uint16_t x = 0; x < count; x++) {
        p[2u + x * 2u] = s.cells[(size_t)row * s.cols + x].ch;
        p[3u + x * 2u] = s.cells[(size_t)row * s.cols + x].attr;
    }
    unlock();

    const wire_t r =
        ws_send(OP_ROW, p, 2u + (uint32_t)count * 2u, 10000u, 3000u);
    /* Marked clean only once it has gone.  A row cleared before the send is a
     * row lost for good when the board was momentarily out of memory. */
    if (r == WIRE_OK) {
        lock();
        s.row_dirty[row] = 0u;
        unlock();
    }
    return r;
}

static wire_t send_cursor(void)
{
    uint8_t buf[WS_SLACK + 5];
    uint8_t *p = buf + WS_SLACK;

    lock();
    p[0] = (uint8_t)s.cur_col;
    p[1] = (uint8_t)s.cur_row;
    p[2] = s.cur_under.ch;
    p[3] = s.cur_under.attr;
    p[4] = s.cur_visible ? 1u : 0u;
    unlock();

    const wire_t r = ws_send(OP_CURSOR, p, 5u, 10000u, 3000u);
    if (r == WIRE_OK) {
        lock();
        s.cur_dirty = false;
        unlock();
    }
    return r;
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
/* Encode whatever is in s.band and put it on the wire.  Both paths end here. */
static wire_t encode_and_send(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                              uint32_t deadline_ms, uint32_t lock_ms)
{
    /* s.out is allocated with WS_SLACK bytes in front for ws_send's header. */
    uint8_t       *body = s.out + WS_SLACK;
    const uint32_t cap = s.band_cap - WS_SLACK;

    uint8_t op = 0;
    const uint32_t len = ag_pixband_encode(s.pix, s.band, w * h,
                                           body + BAND_HDR, cap - BAND_HDR,
                                           &op);
    if (len == 0u) {
        return WIRE_OK; /* refused rather than truncated: see ag_pixband.h */
    }
    put16(body + 0, x);
    put16(body + 2, y);
    put16(body + 4, w);
    put16(body + 6, h);
    return ws_send(op, body, BAND_HDR + len, deadline_ms, lock_ms);
}

static wire_t send_band(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    const uint32_t n = w * h;
    if (n == 0u || n > s.band_px || s.pix == NULL) {
        return WIRE_OK;
    }

    lock();
    if (s.frame == NULL || x + w > s.frame_w || y + h > s.frame_h) {
        unlock();
        return WIRE_OK;
    }
    /* Strided by the allocation, not by the surface: see cap_w above. */
    for (uint32_t r = 0; r < h; r++) {
        memcpy(s.band + r * w, s.frame + (size_t)(y + r) * s.cap_w + x,
               (size_t)w * sizeof(uint16_t));
    }
    unlock();

    return encode_and_send(x, y, w, h, 10000u, 3000u);
}

/*
 * The same rectangle, taken straight from the caller's pixels.
 *
 * For the board with no frame to copy into.  This runs on whichever task drew,
 * with the device registry held, and it blocks for as long as the network
 * needs - which is exactly the cost the frame exists to avoid and exactly what
 * REMDISP pays over its wire.  On a machine with 179 KB and no PSRAM that is
 * the trade that is on offer: a slower application, or no picture.
 */
static wire_t send_blit_direct(const ag_blit_t *b)
{
    if (s.pix == NULL || s.band == NULL || s.out == NULL) {
        return WIRE_OK;
    }
    const uint8_t *src = (const uint8_t *)b->px;

    /*
     * A budget for the whole rectangle, and a small one.
     *
     * The kernel calls blit_rect holding the device registry, and every other
     * driver in the machine is behind that lock - the console, the panel, the
     * touchscreen.  Whatever is spent here is spent by all of them.  The first
     * version gave each band ten seconds and gfxdemo froze the board solid; the
     * picture is worth some milliseconds and it is not worth the machine.
     */
    const uint64_t until = ag_micros() + 60000ull;

    const uint32_t down = (b->h + BAND_ROWS - 1u) / BAND_ROWS;
    const uint32_t across = (b->w + s.band_w - 1u) / s.band_w;
    const uint32_t total = down * across;
    if (total == 0u) {
        return WIRE_OK;
    }

    /*
     * Starting where the last one stopped, and wrapping.
     *
     * With a budget, an application that draws the same rectangle over and
     * over would send the top of it every time and the bottom never - the cut
     * always falls in the same place.  Carrying the position over makes the
     * whole picture arrive across a few frames instead of a third of it
     * arriving for ever.
     */
    uint32_t at = s.direct_at % total;
    uint32_t done = 0;

    while (done < total) {
        const uint64_t now = ag_micros();
        if (now >= until) {
            break; /* out of time, not out of connection */
        }
        uint32_t left_ms = (uint32_t)((until - now) / 1000ull);
        if (left_ms > 20u) {
            left_ms = 20u;
        }

        const uint32_t y = (at / across) * BAND_ROWS;
        const uint32_t x = (at % across) * s.band_w;
        uint32_t rows = b->h - y;
        if (rows > BAND_ROWS) {
            rows = BAND_ROWS;
        }
        uint32_t cols = b->w - x;
        if (cols > s.band_w) {
            cols = s.band_w;
        }

        for (uint32_t r = 0; r < rows; r++) {
            memcpy(s.band + r * cols,
                   src + (size_t)(y + r) * b->stride +
                       (size_t)x * sizeof(uint16_t),
                   (size_t)cols * sizeof(uint16_t));
        }
        /* Zero patience for the mutex: see ws_send.  The registry is held here
         * and everything else in the machine is behind it. */
        const wire_t r = encode_and_send((uint32_t)b->x + x, (uint32_t)b->y + y,
                                         cols, rows, left_ms, 0u);
        if (r == WIRE_GONE) {
            s.direct_at = at;
            return r;
        }
        if (r == WIRE_STALL) {
            break; /* nothing started; leave the rest for the next frame */
        }
        at = (at + 1u) % total;
        done++;
    }
    s.direct_at = at;
    return WIRE_OK;
}

/* The union of what has changed, in surface coordinates.  Caller holds the
 * lock; mark_damage takes it. */
static void mark_damage_locked(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (w == 0u || h == 0u) {
        return;
    }
    if (s.dx0 >= s.dx1 || s.dy0 >= s.dy1) {
        s.dx0 = x;
        s.dy0 = y;
        s.dx1 = x + w;
        s.dy1 = y + h;
        return;
    }
    if (x < s.dx0) {
        s.dx0 = x;
    }
    if (y < s.dy0) {
        s.dy0 = y;
    }
    if (x + w > s.dx1) {
        s.dx1 = x + w;
    }
    if (y + h > s.dy1) {
        s.dy1 = y + h;
    }
}

static void mark_damage(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    lock();
    mark_damage_locked(x, y, w, h);
    unlock();
}

/*
 * Everything the phone is owed, in one pass, and then out to wait for more.
 *
 * Returns false only when the connection is finished.  A board that is
 * momentarily out of memory - an application loading takes every byte for half
 * a second - is not a finished connection: what could not go out stays marked
 * and goes on the next pass.  Dropping the phone for that is a link that
 * visibly breaks every time the machine is busy.
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
            uint8_t buf[WS_SLACK + NONCE_LEN];
            memcpy(buf + WS_SLACK, s.nonce, NONCE_LEN);
            const wire_t r =
                ws_send(OP_AUTH, buf + WS_SLACK, NONCE_LEN, 10000u,
                        3000u);
            if (r == WIRE_GONE) {
                return false;
            }
            s.challenged = (r == WIRE_OK);
        }
        return true;
    }

    if (s.owe_info && !s.owe_everything) {
        const wire_t r = send_info();
        if (r == WIRE_GONE) {
            return false;
        }
        if (r == WIRE_OK) {
            s.owe_info = false;
        }
        return true;
    }

    if (s.owe_everything) {
        const wire_t r = send_info();
        if (r == WIRE_GONE) {
            return false;
        }
        if (r == WIRE_STALL) {
            return true; /* still owed; try again next pass */
        }
        s.owe_everything = false;
        s.owe_info = false;
        lock();
        for (uint32_t r2 = 0; r2 < s.rows; r2++) {
            s.row_dirty[r2] = 1u;
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
        if (!s.row_dirty[r]) {
            continue;
        }
        const wire_t w = send_text_row((uint16_t)r);
        if (w == WIRE_GONE) {
            return false;
        }
        if (w == WIRE_STALL) {
            return true; /* the row is still marked; come back to it */
        }
    }
    if (s.cur_dirty) {
        const wire_t w = send_cursor();
        if (w == WIRE_GONE) {
            return false;
        }
        if (w == WIRE_STALL) {
            return true;
        }
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
        for (uint32_t x = x0; x < x1; x += s.band_w) {
            uint32_t w = x1 - x;
            if (w > s.band_w) {
                w = s.band_w;
            }
            const wire_t r = send_band(x, y, w, h);
            if (r == WIRE_GONE) {
                return false;
            }
            if (r == WIRE_STALL) {
                /* Put back what has not gone, so the next pass sends it. */
                mark_damage(x, y, x1 - x, y1 - y);
                return true;
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
                if (send_all(s.conn, hdr, k, 500u) != WIRE_OK ||
                    (h.len > 0u &&
                     send_all(s.conn, body, h.len, 500u) != WIRE_OK)) {
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

/*
 * Wait for the other end to have it before hanging up.
 *
 * ag_net_send returns when lwIP has *queued* the bytes, not when they have
 * gone, and closing a socket throws the queue away.  So a page written in full
 * and closed immediately arrives cut off, with nothing said on either side: the
 * driver's log is clean because every send succeeded, and the client reports a
 * transfer that stopped.  Measured on the CYD fetching its own page: 14700
 * bytes served, 8042 and then 3946 received on two attempts - a different
 * number each time, because it is whatever had drained when close hit.
 *
 * This is the same lesson the UART layer in this tree already paid for once
 * (docs: "a write returns when queued not sent, close threw the queue away").
 *
 * The client has Content-Length, so it closes as soon as it has the body.
 * Reading until it does is therefore both the acknowledgement and the end of
 * the conversation; the deadline is for the client that leaves without saying
 * anything.
 */
/*
 * Handed over rather than closed, and closed later by the task's own loop.
 *
 * Waiting here would be the obvious thing and is the wrong one: this runs on
 * the task that also feeds the screen, so every page load would freeze the
 * picture for as long as the wait.  And the wait cannot be short: a client that
 * reads until end-of-stream rather than counting Content-Length - the shell's
 * own wget is one - will not close until we do.
 *
 * So the socket goes on a shelf with a deadline, the loop drains it a little on
 * every pass, and it is closed when the peer has gone or the deadline is up.
 * One at a time, because there is one of these at a time.
 */
static void defer_close(ag_handle_t h, uint32_t deadline_ms)
{
    if (s.closing >= 0) {
        (void)ag_net_close(s.closing);
    }
    s.closing = h;
    s.closing_until = ag_micros() + (uint64_t)deadline_ms * 1000ull;
}

static void pump_deferred(void)
{
    if (s.closing < 0) {
        return;
    }
    uint8_t sink[64];
    for (int i = 0; i < 8; i++) {
        const int32_t n = ag_net_recv(s.closing, sink, sizeof(sink));
        if (n == 0 || (n < 0 && n != -AG_EAGAIN)) {
            break; /* the peer has gone, which means it has the bytes */
        }
        if (n < 0) {
            if (ag_micros() < s.closing_until) {
                return; /* nothing to read yet, and there is still time */
            }
            break;
        }
    }
    (void)ag_net_close(s.closing);
    s.closing = -1;
}

static void serve_404(ag_handle_t h)
{
    static const char k_404[] = "HTTP/1.1 404 Not Found\r\n"
                                "Content-Length: 0\r\n"
                                "Connection: close\r\n\r\n";
    (void)send_all(h, k_404, (uint32_t)(sizeof(k_404) - 1u), 1000u);
}

/*
 * The page, streamed off the board's own disk.
 *
 * It used to be a C array in this driver, and on a board with memory to spare
 * that is the tidier thing: one file to ship.  On a board without, it is the
 * whole problem.  A .SYS keeps its data in RAM, so forty kilobytes of HTML -
 * fifteen after gzip - were fifteen kilobytes of a CYD that has about thirty
 * free with its radio up.  Measured there: thirty free at idle, and seven once
 * a phone had connected, which is the edge a board falls off.
 *
 * As a file it costs a one-kilobyte read buffer and lives in flash, which is
 * the resource this system has four megabytes of.  Nothing about "the tool
 * lives in the machine" is given up: the file ships in the C: image beside the
 * driver, and the phone still installs nothing.
 *
 * Content-Encoding: gzip, always, because that is the only form on the disk.
 * Every browser decompresses it and the board never has to.  A client that does
 * not speak gzip - curl without --compressed - gets bytes it cannot read, which
 * is a fair trade for a page written for browsers.
 */
static void serve_page(ag_handle_t h)
{
    ag_stat_t st;
    if (ag_stat(s.page, &st) != AG_OK || st.size == 0u) {
        char msg[192];
        const int k = snprintf(msg, sizeof(msg),
                               "HTTP/1.1 500 Internal Server Error\r\n"
                               "Content-Type: text/plain\r\n"
                               "Connection: close\r\n\r\n"
                               "%s is missing - put PHONE.GZ on this board "
                               "(tools/mkpage.py makes it).\n",
                               s.page);
        if (k > 0) {
            (void)send_all(h, msg, (uint32_t)k, 1000u);
        }
        return;
    }

    const ag_handle_t f = ag_open(s.page, AG_O_RDONLY);
    if (f < 0) {
        return;
    }

    char hdr[160];
    const int n = snprintf(hdr, sizeof(hdr),
                           "HTTP/1.1 200 OK\r\n"
                           "Content-Type: text/html; charset=utf-8\r\n"
                           "Content-Encoding: gzip\r\n"
                           "Content-Length: %u\r\n"
                           "Cache-Control: no-store\r\n"
                           "Connection: close\r\n\r\n",
                           (unsigned)st.size);
    ag_log(AG_LOG_INFO, "phone", "GET / -> %u bytes", (unsigned)st.size);
    if (n > 0 && send_all(h, hdr, (uint32_t)n, 2000u) == WIRE_OK) {
        /*
         * A kilobyte at a time, with ten seconds for each.  Generous on purpose:
         * this is going to a phone that may be three rooms away on a weak
         * signal, and the alternative to waiting is a page that arrives
         * truncated - which a browser renders as a blank screen and no error.
         */
        uint8_t  buf[1024];
        uint32_t sent = 0;
        for (;;) {
            const int32_t got = ag_read(f, buf, sizeof(buf));
            if (got <= 0) {
                break;
            }
            const wire_t w = send_all(h, buf, (uint32_t)got, 10000u);
            if (w != WIRE_OK) {
                /*
                 * Said, not swallowed.  A page that stops halfway is a blank
                 * screen in the browser and no error anywhere, and the only
                 * way to tell "the client stopped reading" from "the board ran
                 * out" is for this to name which one it was.
                 */
                ag_log(AG_LOG_WARN, "phone",
                       "page stopped after %u of %u bytes (%s)",
                       (unsigned)sent, (unsigned)st.size,
                       (w == WIRE_STALL) ? "the client stopped reading"
                                         : "the connection went");
                break;
            }
            sent += (uint32_t)got;
        }
        if (sent == st.size) {
            ag_log(AG_LOG_INFO, "phone", "page sent whole (%u bytes)",
                   (unsigned)sent);
        }
    }
    (void)ag_close(f);
}

/*
 * A fresh connection: read its request, then either answer it and close, or
 * turn it into the WebSocket.
 *
 * Returns true when the handle is no longer the caller's to close: either it
 * has become the live WebSocket, or it has been answered and handed to
 * defer_close, which hangs it up once the answer has actually gone.
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
            defer_close(h, 1500u);
            return true;
        }
        char resp[220];
        const int n = snprintf(resp, sizeof(resp),
                                  "HTTP/1.1 101 Switching Protocols\r\n"
                                  "Upgrade: websocket\r\n"
                                  "Connection: Upgrade\r\n"
                                  "Sec-WebSocket-Accept: %s\r\n\r\n",
                                  accept);
        if (n <= 0 || send_all(h, resp, (uint32_t)n, 2000u) != WIRE_OK) {
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
        defer_close(h, 1500u);
        return true;
    }
    if (strncmp(req + 4, "/favicon.ico", 12) == 0) {
        serve_404(h);
        defer_close(h, 1500u);
        return true;
    }
    serve_page(h);
    /* Not closed here: the bytes are only queued, and closing throws the queue
     * away.  See defer_close. */
    defer_close(h, 4000u);
    return true; /* the caller must not close it either */
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
    /*
     * The band, sized to the panel rather than to the widest panel anybody
     * might have.  On a board with PSRAM the difference is nothing; on the CYD
     * it is the difference between fitting and not - 512 columns of scratch is
     * twenty-five kilobytes of a machine with seventy free, for a screen that
     * is 320 across and never needs more.
     */
    if (s.band_w == 0u) {
        uint32_t w = s.want_w;
        if (w == 0u) {
            w = 320u;
        }
        if (w > BAND_MAX_W) {
            w = BAND_MAX_W;
        }
        s.band_w = w;
        s.band_px = BAND_ROWS * w;
        s.band_cap = WS_SLACK + BAND_HDR + s.band_px * 2u +
                     s.band_px / 64u + 64u;
    }

    if (s.pix == NULL) {
        const size_t need = ag_pixband_size(s.band_px);
        void *mem = ag_malloc(need);
        if (mem == NULL || !ag_pixband_init(mem, need, s.band_px)) {
            ag_free(mem);
            return false;
        }
        s.pix = (ag_pixband_ctx_t *)mem;
    }
    if (s.band == NULL) {
        s.band = (uint16_t *)ag_malloc(s.band_px * sizeof(uint16_t));
        if (s.band == NULL) {
            return false;
        }
    }
    if (s.out == NULL) {
        s.out = (uint8_t *)ag_malloc(s.band_cap);
        if (s.out == NULL) {
            return false;
        }
    }

    uint32_t w = s.want_w;
    uint32_t h = s.want_h;
    if (w == 0u || h == 0u) {
        return true; /* nothing has asked for more than what is already held */
    }
    if (!s.want_frame) {
        /*
         * No frame, but the size is still the answer to "how big is this
         * screen".  Leaving it at zero meant the geometry message said 0x0, the
         * page never sized its canvas, and bands landed in the 300x150 a
         * browser gives an unsized one - which is exactly the "the screen got
         * narrower" that came back from the board.
         */
        s.cap_w = w;
        s.cap_h = h;
        return true;
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
        /*
         * No frame, and that is a mode rather than a failure.
         *
         * The frame exists so that blit_rect can be a memcpy and the wire can
         * be the task's problem.  A board without the memory for one - the CYD
         * has no PSRAM, and 320x240 is 150 KB of its 179 - sends each rectangle
         * from the task that drew it instead, which is what REMDISP does over
         * its wire and costs the drawing application the wire time.  Slower for
         * that application, and the only alternative is no picture at all.
         */
        if (!s.moaned_frame) {
            s.moaned_frame = true;
            ag_log(AG_LOG_WARN, "phone",
                   "no memory for a %ux%u frame (%u KB); sending from the "
                   "drawing task instead",
                   (unsigned)w, (unsigned)h, (unsigned)((w * h * 2u) / 1024u));
        }
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

    uint64_t next_sync = 0;
    uint64_t next_beat = 0;
    uint32_t laps = 0;

    while (!s.stop) {
        /*
         * A heartbeat, because "the board stopped accepting" is a thing that
         * cannot be caught in the act.  Every five seconds this says where the
         * loop has been and how many times it has been round; a stage that
         * stops advancing names the call that is blocking, which is the one
         * thing an outside observer cannot see.  Add a counter rather than try
         * to catch the moment.
         */
        const uint64_t beat = (uint64_t)ag_micros();
        if (beat >= next_beat) {
            next_beat = beat + 5000000ull;
            ag_log(AG_LOG_INFO, "phone",
                   "alive: %u laps, stage %u, listen=%d conn=%d closing=%d",
                   (unsigned)laps, (unsigned)s.stage, (int)s.listen,
                   (int)s.conn, (int)s.closing);
        }
        laps++;

        s.stage = 1;
        /* The console's size, a few times a second.  See sync_console. */
        const uint64_t now = (uint64_t)ag_micros();
        if (now >= next_sync) {
            next_sync = now + 300000ull;
            s.stage = 8;
            if (!sync_console()) {
                TASK->sleep_ms(500u);
                continue;
            }
        }

        pump_deferred();

        s.stage = 2;
        if (!ensure_listen()) {
            TASK->sleep_ms(200u);
            continue;
        }
        s.stage = 3;
        if (!ensure_memory()) {
            TASK->sleep_ms(500u);
            continue;
        }
        s.stage = 4;

        /*
         * A new connection, whether or not one is already live.  A phone that
         * has locked its screen and come back does not close anything - it just
         * opens a second socket, and the old one sits there being written to
         * for as long as TCP takes to notice.  So the newest upgrade wins.
         */
        const ag_handle_t fresh = ag_tcp_accept(s.listen, 0u);
        /*
         * A listener can die, and when it does nothing says so: accept goes on
         * answering the same error for ever and the board simply stops letting
         * anybody in, with the log as quiet as if nobody had knocked.  EAGAIN
         * is the ordinary "nobody yet"; anything else means this socket is no
         * longer a door, so close it and open another.
         */
        if (fresh < 0 && fresh != -AG_EAGAIN && fresh != -AG_ETIMEDOUT) {
            ag_log(AG_LOG_WARN, "phone", "listener died (%d); reopening",
                   (int)fresh);
            (void)ag_net_close(s.listen);
            s.listen = -1;
            TASK->sleep_ms(200u);
            continue;
        }
        if (fresh >= 0) {
            /*
             * And turned away at the door when the board has nothing left.
             *
             * Not politeness: a connection accepted while lwIP or the Wi-Fi
             * driver is out of memory does not fail, it aborts the board.
             * HTTPD.AXE learned this and wrote the number down - sixteen
             * kilobytes, measured - and this had no floor at all.  On the CYD,
             * thirty kilobytes free at idle and seven with a phone attached,
             * that is the difference between a link and a board that stops
             * answering.
             */
            ag_meminfo_t mi;
            ag_meminfo(&mi);
            if (mi.system_free < MEM_FLOOR) {
                /*
                 * Said every time and not once.  This is the answer to "the
                 * page does not load" and it must be in the journal for the
                 * attempt being asked about, not for the first one after boot.
                 */
                ag_log(AG_LOG_WARN, "phone",
                       "refused a visitor: %u bytes free, floor is %u",
                       (unsigned)mi.system_free, (unsigned)MEM_FLOOR);
                (void)ag_net_close(fresh);
            } else {
                (void)ag_net_set_nonblock(fresh, true);
                if (!serve_request(fresh)) {
                    (void)ag_net_close(fresh);
                }
            }
        }

        if (s.conn < 0) {
            TASK->sleep_ms(50u);
            continue;
        }
        s.stage = 5;
        if (!pump_client()) {
            drop_conn("closed by peer");
            continue;
        }
        s.stage = 6;
        if (!service_client()) {
            drop_conn("write failed");
            continue;
        }
        s.stage = 7;

        /*
         * Ten milliseconds, which is the interval a screen is asked for rather
         * than the interval it changes.  Nothing here polls the picture: the
         * damage is marked by blit_rect and this only notices it, so a still
         * screen costs one wakeup a hundredth of a second and no bytes at all.
         */
        TASK->sleep_ms(10u);
    }

    if (s.closing >= 0) {
        (void)ag_net_close(s.closing);
        s.closing = -1;
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

    /*
     * No frame at all: this board could not hold one.  Then the rectangle goes
     * out from here, on the caller's task, and the caller waits for the wire.
     * Not a fallback bolted on - it is the only way a board with 179 KB and no
     * PSRAM can show a picture at all.
     */
    if (s.frame == NULL && s.pix != NULL) {
        /*
         * The surface's size, learned from the blit itself.  With no frame
         * there is nothing else that knows it, and the page needs it to size
         * the canvas these bands are placed on.
         */
        if (s.frame_w != b->surf_w || s.frame_h != b->surf_h) {
            s.frame_w = b->surf_w;
            s.frame_h = b->surf_h;
            s.owe_info = true;
        }
        if (s.conn >= 0 && (!needs_password() || s.authed)) {
            (void)send_blit_direct(b);
        }
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
        s.owe_info = true;
    }

    const uint8_t *src = (const uint8_t *)b->px;
    for (uint32_t r = 0; r < b->h; r++) {
        memcpy(s.frame + (size_t)(b->y + r) * s.cap_w + b->x,
               src + (size_t)r * b->stride, (size_t)b->w * sizeof(uint16_t));
    }

    mark_damage_locked(b->x, b->y, b->w, b->h);
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
        s.cells[(size_t)row * s.cols + x] = cells[x];
    }
    for (uint16_t x = count; x < s.cols; x++) {
        s.cells[(size_t)row * s.cols + x].ch = ' ';
        s.cells[(size_t)row * s.cols + x].attr = 0x07u;
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
    s.cells[(size_t)row * s.cols + col] = under;
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
    s.closing = -1;

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
    s.lock = TASK->mutex_create();
    s.wire = TASK->mutex_create();
    if (s.lock == NULL || s.wire == NULL) {
        return -AG_ENOMEM;
    }

    s.publish_text = want_text();

    /*
     * The console mirror BEFORE the device is published, and that ordering is
     * the whole of it.
     *
     * The kernel paints a text panel in full exactly once: the first tick after
     * it sees the driver.  Every tick after that it offers only the rows that
     * changed.  With the mirror allocated later, on the task, that one full
     * paint arrived while text_row still had nowhere to put it and was dropped
     * - so the phone opened on a blank screen and filled in a line at a time as
     * things happened to change.  Reported exactly that way: "the console is
     * empty; I started typing and the prompt appeared".
     */
    if (!sync_console()) {
        return -AG_ENOMEM;
    }

    ag_strlcpy(s.page, "C:\\PHONE.GZ", sizeof(s.page));
    if (api->cfg != NULL && AG_HAS(api->cfg, get_str)) {
        if (api->cfg->get_str("phone.password", s.password,
                              sizeof(s.password)) != AG_OK) {
            s.password[0] = '\0';
        }
        char p[sizeof(s.page)];
        if (api->cfg->get_str("phone.page", p, sizeof(p)) == AG_OK &&
            p[0] != '\0') {
            ag_strlcpy(s.page, p, sizeof(s.page));
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
        /*
         * `[phone] frame = no` on a board that must not even try.
         *
         * The failed allocation is harmless - it falls back to sending from the
         * drawing task - but the *successful* one on a tight board is not: a
         * hundred and fifty kilobytes taken here is a hundred and fifty the
         * application does not get, and DESKTOP.AXE wants seventy-one of them
         * in one piece.  A board whose owner knows it has no room says so
         * rather than finding out by having nothing else start.
         */
        s.want_frame = true;
        if (cfg != NULL && AG_HAS(cfg, get_bool)) {
            s.want_frame = cfg->get_bool("phone.frame", true);
        }
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
