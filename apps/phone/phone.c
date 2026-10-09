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

AG_DRV("PHONE", "1.1", "argon");

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

/*
 * How often the repair sweep sends one row it was not asked for.
 *
 * Two hundred milliseconds: a forty-row console is completely refreshed in
 * eight seconds, which is fast enough that a lost line is a glitch rather than
 * a wrong screen, and slow enough to cost about four hundred bytes a second on
 * a link that carries pictures.
 */
#define REPAIR_EVERY_US 200000ull

/* Events waiting to be read out of /dev/pkbd0 by an application that wants them. */
#define KEY_RING 64u

/*
 * Below this much free memory, the next visitor is turned away at the door.
 *
 * The number and the reason are HTTPD.AXE's, which found them first: a
 * connection accepted when lwIP or the Wi-Fi driver is out of memory does not
 * fail, it aborts the board.  Sixteen kilobytes is measured, not guessed.
 *
 * Overridable at build time for the emulator only: its image has 117 KB of
 * internal memory and 8 KB of it free, so a scripted check of the picture path
 * (tools/phoneshot.py) is turned away before it can look at anything.  QEMU's
 * network is not the Wi-Fi driver this floor protects.
 */
#ifndef MEM_FLOOR
#define MEM_FLOOR (16u * 1024u)
#endif

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
 *   'S'  u8 lines - the console moved up by that many, the rest is unchanged
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
 *
 * The same messages cross Bluetooth, for a phone with no network in common
 * with the board (ArgonOS's Android app; see tools/phoneapp).  The board is a
 * GATT peripheral - service 0xfff0 - and:
 *
 *   phone to board: one write to 0xfff2 is one message, exactly as above.
 *   board to phone: notifications on 0xfff1, each a flag byte and a piece of
 *                   one message.  0x40 = the first piece, 0x80 = the last; a
 *                   short message is one piece with both.  A piece that says
 *                   "first" throws away whatever half message came before, so
 *                   a message the board gave up on part way cannot corrupt the
 *                   next one.
 *   'Q'  board to phone, BLE only: "you no longer have the screen", then the
 *        reason as text - what a WebSocket says with its close frame.
 *
 * No pictures over Bluetooth: text and keys only.  A band is kilobytes, BLE
 * carries tens of them a second at best, and the console has to share it.
 */
#define OP_INFO 'M'
#define OP_ROW 'R'
#define OP_CURSOR 'C'
#define OP_SCROLL 'S'
#define OP_AUTH 'A'
#define OP_QUIT 'Q'

#define IN_HELLO 'H'
#define IN_KEY 'K'
#define IN_PTR 'P'
#define IN_TEXT 'T'
#define IN_AUTH 'A'

#define INFO_HAS_TEXT 0x01u
/*
 * Which screen is in front, said by the board (ABI 0.68, display held):
 * INFO_SAYS_MODE means the board knows, and INFO_PICTURE is then the answer -
 * an application holds the screen.  Without INFO_SAYS_MODE the page guesses
 * from traffic, as it always did.
 */
#define INFO_PICTURE   0x02u
#define INFO_SAYS_MODE 0x04u

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
    /*
     * Rows the client has yet to be told to scroll.
     *
     * A scroll used to arrive here as text_row for every row on the screen -
     * thirty rows of Wi-Fi, about five kilobytes, for one printed line.  Now
     * the console says it moved and this passes that on in two bytes.  Owed
     * rather than sent at once because the send can stall, and the rows that
     * follow a scroll are only correct after it.
     */
    uint8_t       scroll_owed;

    /*
     * What the picture path has done since boot.  `blits` is the kernel
     * handing this driver a rectangle - the half that happens whether or not
     * anybody is connected - and `bands` is what went out on the wire.  Blits
     * with no bands is a driver that is being given a picture and cannot send
     * it; no blits at all is nothing drawing.
     */
    uint32_t      blits;
    uint32_t      bands;
    uint32_t      bands_lite; /* of those, sent with no picture memory   */
    uint32_t      band_bytes;
    uint64_t      mem_retry_at; /* when to ask for picture memory again  */
    /*
     * And the four ways one does not.  A picture that does not appear looks
     * the same from a chair whichever of them it was, and they want different
     * answers: no buffers is a memory budget, hushed is this driver's own
     * pacing, busy is the wire, refused is the encoder being given less room
     * than the band needs.
     */
    uint32_t      band_nomem;
    uint32_t      band_hushed;
    uint32_t      band_busy;
    uint32_t      band_refused;
    uint32_t      band_nostack;

    /*
     * Complaints owed from inside blit_rect.
     *
     * That call runs with the kernel's device registry held, and the console
     * is a device: on the S3 a single ag_log from in there took the whole
     * machine down, first flush, no output, no watchdog.  So the pixel path
     * records what it would have said and the driver's task says it.
     */
    bool          owe_nomem;
    char          owe_nomem_what[24];
    uint32_t      owe_nomem_bytes;
    bool          owe_noframe;
    uint32_t      owe_frame_w, owe_frame_h;
    bool          owe_nostack;
    uint32_t      owe_stack_left;
    uint32_t      stall_band;
    uint32_t      stall_text;
    bool          moaned_stack;

    /* The wire. */
    ag_handle_t listen;
    ag_handle_t conn;    /* an upgraded WebSocket, or -1                    */
    /*
     * Or the client is on Bluetooth instead - never both, there is one screen.
     * See pump_ble: a phone that connects over BLE is only a client once it has
     * said hello, and whichever of the two arrived last has the screen.
     */
    bool        ble_client;
    bool        ble_adv;       /* this driver has the board advertising        */
    uint64_t    ble_retry_at;  /* when to try advertising again after a refusal */
    ag_handle_t closing; /* answered, waiting to be hung up - see defer_close */
    uint64_t    closing_until;
    /*
     * When this client last proved it was there - a byte read from it,
     * or a message it actually took.  See LINK_SILENT_US.
     */
    uint64_t    last_life_us;
    volatile bool     up;
    volatile bool     owe_everything;
    /*
     * The row the repair sweep will send next, whether or not it changed.
     *
     * Rows go out when they change, which is right and cheap and has one
     * failure mode: a row that is lost - a frame dropped while the phone's
     * screen was off, a message that went out during a stall - is never sent
     * again, because nothing on the board thinks it is owed.  The screen then
     * has a wrong line in it until something happens to rewrite that line, and
     * the only cure a person has is to reload the page.  REMDISP has the same
     * problem and answers it the same way.
     *
     * One row every repair interval, round and round: a forty-row console is
     * whole again within a few seconds, and the cost is one row's worth of
     * bytes - about eighty - per interval.
     */
    uint16_t          repair_row;
    uint64_t          repair_at;
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
    /*
     * When it is worth offering the client another message.
     *
     * A client whose window has shut does not open it again because we asked.
     * Measured on the CYD with a phone that had stopped reading: every lap
     * spent its 200 ms finding that out, which is five laps a second instead
     * of a hundred - and a driver at five laps a second reads the keyboard
     * five times a second, so slots would not switch and Esc did nothing.  The
     * board looked wedged; it was queuing politely.
     *
     * So a stall buys silence: nothing is offered until this time, and the lap
     * goes back to reading input, which costs nothing and is what the person
     * at the other end is waiting for.
     */
    uint64_t          hush_until;
    volatile uint8_t  stage;     /* where the task's loop last was            */
    /*
     * Whether anything has sent pixels since the client arrived.
     *
     * The band, the packed output and the codec's context are about thirteen
     * kilobytes on this panel, and a text application needs none of them.  On
     * a board with 208 KB that thirteen is the difference between the driver
     * admitting a visitor and refusing one: with the file manager up, free
     * memory sat at 11868 bytes against a floor of 16384, and the page simply
     * would not load - the board was holding a picture buffer for a screen
     * made of characters.
     */
    bool              want_pixels;
    bool              moaned_frame;
    bool              moaned_mem;
    bool              want_frame;
    bool              codec_light; /* [phone] codec = light               */
    /*
     * Whether an application holds the screen, as the kernel says it (held,
     * ABI 0.68), and whether this kernel says it at all.
     */
    volatile bool     picture_live;
    bool              knows_held;

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

/*
 * How long to keep trying before the first byte of a message has gone.
 *
 * Thirty milliseconds, because a shut window does not open within one: this
 * only has to be long enough to ride out a momentarily busy socket.  What
 * follows a stall is the hush (see s.hush_until), and that is where the real
 * waiting is done - in laps that read the keyboard instead of laps that block.
 */
#define SEND_START_MS 30u

/*
 * And how long a message already begun may go with nothing moving.
 *
 * Measured on the board, in the journal of the evening this was found:
 * "send stalled at 47 of 85 after 10000 ms", and beside it "worst stage 6 took
 * 10002121 us".  Ten seconds inside one write - and the accept that lets a
 * phone load the page is in the same loop, so for those ten seconds the board
 * had a network, a listener, and no way to answer anybody.
 *
 * The window is idle time, not total time: every byte the peer takes puts it
 * back.  So a slow phone on a weak signal is never cut off however long its
 * screen takes to arrive, and a phone that has stopped taking bytes at all is
 * let go after two seconds instead of ten.  Two seconds of a peer accepting
 * nothing, on a link where the two ends are in the same room, is a peer that
 * has gone; it will open another socket when it comes back, and the newest
 * upgrade wins anyway.
 */
#define SEND_IDLE_MS 2000u

/*
 * How much of the caller's stack this driver insists on before it sends a
 * picture from inside blit_rect.  See the note there: the path is an encoder
 * plus lwIP, and eight kilobytes of application stack was measured to be too
 * little for it.
 */
#define BLIT_STACK_FLOOR 6144u

/*
 * Sending straight from the drawing task (no frame): how long a tile may wait
 * for a client that is not reading before it is given up, and how long for the
 * wire's mutex while the task finishes a message of its own.  See the note in
 * send_blit_direct.
 */
#define DIRECT_WAIT_MS 1000u
#define DIRECT_LOCK_MS 500u

/*
 * And how long a client may prove nothing at all before it is let go.
 *
 * A phone that is merely idle is not silent: the repair sweep sends it
 * a row several times a second and the row goes, so the clock below is
 * reset by the ordinary working of the link.  What resets nothing is a
 * peer whose window shut and never opened - the program behind it gone,
 * the socket still open on its board.  Then every send stalls at its
 * first byte, a stall is not a failure, and the connection is held for
 * as long as the board runs.  Measured: 830 seconds and counting, with
 * the beat cheerfully reporting conn=1895825409 the whole time.
 *
 * It matters more than one stuck socket, because one client is all
 * there is: while that connection is held nobody else is served, and
 * the memory floor then refuses the new visitor who would have replaced
 * it.  The board keeps its network, keeps its listener, and can never
 * be reached again - which is exactly how this was found.
 *
 * Thirty seconds: long enough that a phone in a pocket, a lift or a
 * screen lock comes back to the same session, short enough that nobody
 * waits for a board twice.
 */
#define LINK_SILENT_US 30000000ull

static wire_t send_all(ag_handle_t h, const void *buf, uint32_t len,
                       uint32_t idle_ms)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t       at = 0;
    uint64_t       until = ag_micros() + (uint64_t)idle_ms * 1000ull;
    /*
     * Which "try again" it was, kept for the line below.  EAGAIN and ENOMEM
     * clear the same way and so are handled the same, but they mean opposite
     * things about the machine: a full window is a peer that is not reading,
     * and no buffer is this board being out of them.  Guessing between the two
     * costs an afternoon; the counter costs two integers.
     */
    unsigned again = 0, nomem = 0;
    /*
     * Two deadlines, because the two halves of a stuck send are not one
     * problem.
     *
     * Once a byte of a WebSocket message has gone, the rest must follow: the
     * client is mid-frame, and anything else arriving on that socket is a
     * stream it can no longer parse.  That half is worth the full deadline.
     *
     * Before the first byte, nothing is owed to anybody, and waiting is pure
     * harm.  Measured on the CYD against a phone whose window had filled: five
     * thousand EAGAINs and ten seconds per attempt, with the driver's entire
     * loop inside the wait - so keys went unread, slots would not switch and
     * commands looked ignored, ten seconds at a time.  The client that is not
     * reading gains nothing from it either: it will read when it reads, and
     * the message can go then.
     */
    const uint64_t start_by = ag_micros() + (uint64_t)SEND_START_MS * 1000ull;

    while (at < len) {
        const int32_t n = ag_net_send(h, p + at, len - at);
        if (n > 0) {
            at += (uint32_t)n;
            /* Progress, so the clock starts again: see SEND_IDLE_MS. */
            until = ag_micros() + (uint64_t)idle_ms * 1000ull;
            continue;
        }
        if (n == -AG_EAGAIN) {
            again++;
        } else if (n == -AG_ENOMEM) {
            nomem++;
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
        if (at == 0u && ag_micros() > start_by) {
            /*
             * Nothing sent, nothing owed: let the loop get on with reading
             * input, and offer this message again next time round.
             *
             * Said once, because this is the stall that matters and nothing
             * ever printed it.  A hundred and ten bytes that cannot start on
             * an idle link is not a shut window, it is the stack having no
             * buffer - and the two counters tell them apart.  Two frames of a
             * two-kilobyte picture in thirty seconds was traced this far and
             * no further for want of this line.
             */
            static bool moaned_start;
            if (!moaned_start) {
                moaned_start = true;
                ag_log(AG_LOG_WARN, "phone",
                       "a %u byte send could not start in %u ms: "
                       "%u window, %u no-buffer",
                       (unsigned)len, (unsigned)SEND_START_MS, again, nomem);
            }
            return WIRE_STALL;
        }
        if (ag_micros() > until) {
            ag_log(AG_LOG_WARN, "phone",
                   "send stalled at %u of %u, nothing moved for %u ms: "
                   "%u window, %u no-buffer",
                   (unsigned)at, (unsigned)len, (unsigned)idle_ms, again,
                   nomem);
            if (at == 0u) {
                return WIRE_STALL; /* nothing started; nothing broken */
            }
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

/* ---- the same message, over Bluetooth ----------------------------------- */

#define BLE_FIRST 0x40u
#define BLE_LAST  0x80u

/*
 * One message as notifications: a flag byte and as much of the message as the
 * link's MTU leaves room for, piece after piece.
 *
 * The deadlines mean what send_all's do.  Before the first piece nothing is
 * owed, so a radio with no buffer is a stall after SEND_START_MS and the loop
 * goes back to reading keys.  After it, the rest is worth waiting for up to
 * `idle_ms` - and unlike a WebSocket, giving up half way is not fatal: the
 * next message starts with a "first" piece and the phone drops the half it
 * had.  So a message abandoned part way is a stall, not a lost client.
 */
static wire_t ble_send(const uint8_t *msg, uint32_t len, uint32_t idle_ms)
{
    uint8_t        pkt[256];
    uint32_t       at = 0;
    uint64_t       until = (uint64_t)ag_micros() + (uint64_t)idle_ms * 1000ull;
    const uint64_t start_by =
        (uint64_t)ag_micros() + (uint64_t)SEND_START_MS * 1000ull;

    while (at < len) {
        uint32_t room = ag_ble_adv_mtu();
        if (room < 2u) {
            return WIRE_GONE; /* nobody connected and listening */
        }
        if (room > sizeof(pkt)) {
            room = sizeof(pkt);
        }
        uint32_t take = len - at;
        if (take > room - 1u) {
            take = room - 1u;
        }
        pkt[0] = (uint8_t)(((at == 0u) ? BLE_FIRST : 0u) |
                           ((at + take == len) ? BLE_LAST : 0u));
        memcpy(pkt + 1, msg + at, take);

        const ag_err_t e = ag_ble_adv_notify(pkt, take + 1u);
        if (e == AG_OK) {
            at += take;
            until = (uint64_t)ag_micros() + (uint64_t)idle_ms * 1000ull;
            continue;
        }
        if (e == -AG_ERANGE) {
            continue; /* the MTU changed between asking and sending */
        }
        if (e != -AG_EAGAIN) {
            return WIRE_GONE;
        }
        const uint64_t now = (uint64_t)ag_micros();
        if ((at == 0u && now > start_by) || now > until) {
            return WIRE_STALL;
        }
        TASK->sleep_ms(2u);
    }
    return WIRE_OK;
}

static wire_t ws_send(uint8_t op, void *payload, uint32_t len,
                      uint32_t idle_ms, uint32_t lock_ms)
{
    if (s.conn < 0 && !s.ble_client) {
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
    const bool is_band = (op != OP_ROW && op != OP_CURSOR && op != OP_INFO &&
                          op != OP_SCROLL && op != OP_AUTH && op != OP_QUIT);
    if (is_band && s.ble_client) {
        return WIRE_STALL; /* no pictures over Bluetooth: see the protocol */
    }
    /*
     * The hush covers the picture too, and that was tried both ways.
     *
     * It refuses most bands - measured, 211 of 258 - and exempting them does
     * not buy a picture: the same two frames arrive and the console's rows
     * drop from two hundred to fifty-five, because the two are competing for
     * one wire and the quiet was what kept the text moving.  So the picture
     * goes at the speed the console leaves it, which on this board is a frame
     * or two every thirty seconds, and the counters below say so rather than
     * leaving it to be guessed.
     */
    if (s.hush_until != 0u && (uint64_t)ag_micros() < s.hush_until) {
        if (is_band) {
            s.band_hushed++;
        }
        return WIRE_STALL;
    }
    if (s.wire != NULL && !TASK->mutex_lock(s.wire, lock_ms)) {
        if (is_band) {
            s.band_busy++;
        }
        return WIRE_STALL;
    }
    /* Over Bluetooth the op and the payload go as they are: the pieces carry
     * their own framing, and the WebSocket header in the slack is unused. */
    const wire_t r = s.ble_client
                         ? ble_send(p - 1, 1u + len, idle_ms)
                         : send_all(s.conn, p - (n + 1u), n + 1u + len, idle_ms);
    if (s.wire != NULL) {
        TASK->mutex_unlock(s.wire);
    }
    /*
     * A closed window is worth a hundred milliseconds of quiet; anything that
     * went through means the client is reading again and the quiet is over.
     * Short on purpose - long enough that the lap costs nothing, short enough
     * that a screen still feels live once the phone catches up.
     */
    if (r == WIRE_STALL) {
        /* Who lights the quiet that refuses everybody else. */
        if (is_band) {
            s.stall_band++;
        } else {
            s.stall_text++;
        }
        s.hush_until = (uint64_t)ag_micros() + 100000ull;
    } else if (r == WIRE_OK) {
        s.hush_until = 0u;
        s.last_life_us = (uint64_t)ag_micros();
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
    if (reason == NULL) {
        return;
    }
    const uint32_t n = (uint32_t)strlen(reason);
    if (n > 100u) {
        return;
    }
    if (s.ble_client) {
        /* Bluetooth has no close frame to carry a reason, so it is a message:
         * 'Q' and the text.  The link itself stays up - the phone may want the
         * screen back, and that is one hello away. */
        uint8_t m[1u + 100u];
        m[0] = (uint8_t)OP_QUIT;
        memcpy(m + 1, reason, n);
        (void)ble_send(m, 1u + n, 200u);
        return;
    }
    if (s.conn < 0) {
        return;
    }
    uint8_t buf[128];
    const uint32_t k = ag_ws_hdr_build(buf, AG_WS_CLOSE, n + 2u, true);
    buf[k] = 0x03u; /* 1000, a normal closure */
    buf[k + 1u] = 0xe8u;
    memcpy(buf + k + 2u, reason, n);
    (void)send_all(s.conn, buf, k + 2u + n, 200u);
}

/*
 * Let a client go, and say whether TCP should still be trying to reach it.
 *
 * `gone` is for a peer this driver has already decided is not listening: a
 * send that stalled past its deadline, half a minute of silence, a write that
 * failed.  A polite close there is a promise to deliver what is queued to
 * somebody who is taking nothing, and TCP keeps that promise indefinitely -
 * measured as 11520 bytes queued and 32 KB of heap held until the access point
 * was taken down.  A reset ends it now.
 *
 * Everything else closes: a client replaced by a newer one, or turned away for
 * the wrong password, is a peer that is still there and still reading.
 */
static void drop_conn_reason(const char *why, bool gone);

static void drop_conn(const char *why)
{
    drop_conn_reason(why, false);
}

static void drop_conn_reason(const char *why, bool gone)
{
    /* A new client is not the old one's shut window. */
    s.hush_until = 0u;
    /*
     * The picture buffers are NOT freed here, and that is a fix, not an
     * oversight.
     *
     * They used to be, on the argument that a client gone should give its
     * thirteen kilobytes back.  But this runs on the driver's task, and the
     * same buffers are allocated and read on the DRAWING application's task
     * inside blit_rect - two tasks, no shared lock over the pointers.  A
     * disconnect during a picture (a slot switch, a second browser taking the
     * link) therefore freed s.pix/s.band/s.out from under a blit_rect that was
     * still using or allocating them: a double-free that comprehensive heap
     * poisoning caught as `CORRUPT HEAP: Bad head ... got 0xfefefefe` under
     * multi_heap_free, and that otherwise showed up as the board freezing or
     * resetting on the first slot switch.
     *
     * They are small (about thirteen kilobytes, from PSRAM on a board that has
     * it) and they are freed once, on the driver's own task, when the task
     * exits - the one place nothing else can be touching them.  Held for the
     * life of the driver rather than freed across a task boundary.
     */
    s.last_life_us = 0u;
    if (s.ble_client) {
        s.ble_client = false;
        ag_log(AG_LOG_INFO, "phone", "bluetooth client gone (%s)",
               why ? why : "?");
    }
    if (s.conn >= 0) {
        if (!gone || ag_net_reset(s.conn) != AG_OK) {
            (void)ag_net_close(s.conn);
        }
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

static wire_t send_info(uint32_t lock_ms)
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
    p[8] = (uint8_t)((s.publish_text ? INFO_HAS_TEXT : 0u) |
                     (s.knows_held ? INFO_SAYS_MODE : 0u) |
                     (s.picture_live ? INFO_PICTURE : 0u));
    return ws_send(OP_INFO, p, 9u, SEND_IDLE_MS, lock_ms);
}

/* Two bytes that stand for a screenful.  See OP_SCROLL. */
static wire_t send_scroll(uint8_t lines)
{
    uint8_t  buf[WS_SLACK + 1u];
    uint8_t *p = buf + WS_SLACK;
    p[0] = lines;
    return ws_send(OP_SCROLL, p, 1u, SEND_IDLE_MS, 3000u);
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
        ws_send(OP_ROW, p, 2u + (uint32_t)count * 2u, SEND_IDLE_MS, 3000u);
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

    const wire_t r = ws_send(OP_CURSOR, p, 5u, SEND_IDLE_MS, 3000u);
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
/*
 * An encoded band, `len` bytes of it at body + BAND_HDR, put on the wire.
 * `body` has WS_SLACK writable bytes in front of it for ws_send's header.
 */
static wire_t send_encoded(uint8_t op, uint8_t *body, uint32_t len, uint32_t x,
                           uint32_t y, uint32_t w, uint32_t h, uint32_t idle_ms,
                           uint32_t lock_ms)
{
    if (len == 0u) {
        /*
         * Refused rather than truncated (ag_pixband.h) - and counted, because
         * returning success for a band that was never sent is how a picture
         * that never appeared came to look like a picture nobody drew.
         */
        s.band_refused++;
        return WIRE_OK;
    }
    put16(body + 0, x);
    put16(body + 2, y);
    put16(body + 4, w);
    put16(body + 6, h);
    const wire_t r = ws_send(op, body, BAND_HDR + len, idle_ms, lock_ms);
    if (r == WIRE_OK) {
        s.bands++;
        s.band_bytes += BAND_HDR + len;
    }
    return r;
}

/* Encode whatever is in s.band and put it on the wire.  Both paths end here. */
static wire_t encode_and_send(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                              uint32_t idle_ms, uint32_t lock_ms)
{
    /* s.out is allocated with WS_SLACK bytes in front for ws_send's header. */
    uint8_t       *body = s.out + WS_SLACK;
    const uint32_t cap = s.band_cap - WS_SLACK;

    uint8_t op = 0;
    const uint32_t len = ag_pixband_encode(s.pix, s.band, w * h,
                                           body + BAND_HDR, cap - BAND_HDR,
                                           &op);
    return send_encoded(op, body, len, x, y, w, h, idle_ms, lock_ms);
}

/*
 * The picture with no picture memory at all.
 *
 * The band, the packed output and the codec's context are thirteen kilobytes
 * on a 320-wide panel, and a board that cannot spare them used to show no
 * picture: every rectangle was dropped and counted as "no memory".  But the
 * pixels already exist - in the drawing application's own buffer, for as long
 * as blit_rect runs - and the screen at the other end keeps whatever it is
 * sent: the page paints each band onto a canvas that holds it.  So the only
 * memory this needs is one message, and this is it: a few rows, encoded
 * straight out of the caller's surface by the context-free encoder
 * (ag_pixband_encode_rows: PackBits or raw, no palette).
 *
 * Static, so it is part of the driver as loaded and can never fail to be had.
 * Two kilobytes holds three rows of a 320-wide screen raw, more when they pack.
 * Used only by the direct path, from blit_rect.
 */
#define LITE_CAP 2048u
#define LITE_PAYLOAD (LITE_CAP - WS_SLACK - BAND_HDR)
static uint8_t s_lite[LITE_CAP];

/*
 * The light path's send, done by the driver's own task.
 *
 * The drawing application encodes - PackBits over a few rows is a loop with no
 * stack to speak of - and the driver's task puts the message on the wire,
 * because the wire is lwIP and the Wi-Fi driver, and those want stack the
 * application may not have.  Measured on the Audio Kit: the amplifier draws
 * with 4.3 KB of its stack left, the floor for sending from there is 6 KB, and
 * every band of its panel was refused for want of the difference.  Handed
 * over, it needs none of it.
 *
 * One message at a time, through s_lite: the application fills it, wakes the
 * task (s_hand_go, which is also what the task naps on, so it wakes at once)
 * and waits for s_hand_done.  The task answers whatever happens - a client
 * that has gone answers WIRE_GONE - so the wait is long only as a backstop.
 */
static ag_sem_t          s_hand_go;
static ag_sem_t          s_hand_done;
static volatile bool     s_hand_pending;
static volatile wire_t   s_hand_result;
static uint8_t           s_hand_op;
static uint32_t          s_hand_len, s_hand_x, s_hand_y, s_hand_w, s_hand_h;

#define HAND_WAIT_MS 10000u

/* On the task: send what the application handed over, if anything. */
static void serve_handoff(void)
{
    if (!s_hand_pending) {
        return;
    }
    s_hand_result =
        send_encoded(s_hand_op, s_lite + WS_SLACK, s_hand_len, s_hand_x,
                     s_hand_y, s_hand_w, s_hand_h, SEND_IDLE_MS, 3000u);
    s_hand_pending = false;
    TASK->sem_give(s_hand_done);
}

/* The task's nap, which a hand-over cuts short. */
static void nap(uint32_t ms)
{
    if (s_hand_go != NULL) {
        (void)TASK->sem_take(s_hand_go, ms);
    } else {
        TASK->sleep_ms(ms);
    }
    serve_handoff();
}

static wire_t encode_rows_and_send(const uint8_t *src, uint32_t stride,
                                   uint32_t x, uint32_t y, uint32_t w,
                                   uint32_t h)
{
    uint8_t *body = s_lite + WS_SLACK;
    uint8_t  op = 0;
    const uint32_t len = ag_pixband_encode_rows(src, stride, w, h,
                                                body + BAND_HDR, LITE_PAYLOAD,
                                                &op);
    if (len == 0u) {
        s.band_refused++;
        return WIRE_OK;
    }
    if (s_hand_go == NULL || s_hand_done == NULL) {
        return WIRE_GONE; /* no task to hand it to: the driver is going away */
    }
    s_hand_op = op;
    s_hand_len = len;
    s_hand_x = x;
    s_hand_y = y;
    s_hand_w = w;
    s_hand_h = h;
    s_hand_pending = true;
    TASK->sem_give(s_hand_go);
    if (!TASK->sem_take(s_hand_done, HAND_WAIT_MS)) {
        /* The task never got to it.  Take the message back before s_lite is
         * written again; if the task is sending it right now, this frame is
         * the one that tears, and it is counted. */
        s_hand_pending = false;
        s.band_busy++;
        return WIRE_STALL;
    }
    const wire_t r = s_hand_result;
    if (r == WIRE_OK) {
        s.bands_lite++;
    }
    return r;
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

    return encode_and_send(x, y, w, h, SEND_IDLE_MS, 3000u);
}

/*
 * The same rectangle, taken straight from the caller's pixels.
 *
 * For the board with no frame to copy into.  This runs on whichever task drew,
 * under the kernel's blit lock (not the registry - display.c lets that go
 * first), and it blocks for as long as the network needs - which is exactly
 * the cost the frame exists to avoid and exactly what REMDISP pays over its
 * wire.  On a machine with no room for a frame that is the trade on offer: a
 * slower application, or no picture.
 */
static wire_t send_blit_direct(const ag_blit_t *b, bool full)
{
    const uint8_t *src = (const uint8_t *)b->px;

    /*
     * The tile: a band of the full codec (sent from this, the drawing task),
     * or as many whole rows as one light message carries (s_lite, sent by the
     * driver's task - see encode_rows_and_send).
     */
    uint32_t   tw = full ? s.band_w : b->w;
    uint32_t   th = BAND_ROWS;
    if (!full) {
        const uint32_t px = LITE_PAYLOAD / sizeof(uint16_t);
        if (tw > px) {
            tw = px;
        }
        th = px / tw;
        if (th > BAND_ROWS) {
            th = BAND_ROWS;
        }
    }

    /*
     * The surface's size before any of its pixels.
     *
     * The task sends it when it next looks (owe_info), but these bands go now,
     * from here - so the first bands of a new surface used to reach the page
     * ahead of the size they belong to.  The page then drew them on a canvas
     * of the old size, and sizing the canvas when the size did arrive cleared
     * it: the first picture an application drew was the one that was lost.
     * For a program that draws once and waits, that was its whole picture.
     */
    const uint64_t give_up = ag_micros() + (uint64_t)DIRECT_WAIT_MS * 1000ull;
    while (s.owe_info) {
        if (!full) {
            /* The task sends it (service_client), on its own stack. */
            if (s.conn < 0) {
                return WIRE_GONE;
            }
            if (ag_micros() > give_up) {
                s.band_hushed++;
                return WIRE_STALL;
            }
            if (s_hand_go != NULL) {
                TASK->sem_give(s_hand_go);
            }
            TASK->sleep_ms(2u);
            continue;
        }
        const wire_t r = send_info(DIRECT_LOCK_MS);
        if (r == WIRE_OK) {
            s.owe_info = false;
        } else if (r == WIRE_GONE || ag_micros() > give_up) {
            s.band_hushed++;
            return r; /* the size could not go, so neither can its pixels */
        } else {
            TASK->sleep_ms(5u);
        }
    }

    const uint32_t down = (b->h + th - 1u) / th;
    const uint32_t across = (b->w + tw - 1u) / tw;
    const uint32_t total = down * across;

    /*
     * Every tile, in order, and the drawing application waits for the wire.
     *
     * This used to be a share of time - a pass at most every 150 ms, 25 ms
     * inside it, everything else dropped - because blit_rect then ran with the
     * device registry held and the whole machine sat behind it.  It no longer
     * does (display.c lets the registry go and holds only the blit lock, which
     * nothing but another blit takes), and with no frame on this board a
     * dropped tile is not a screen that updates later: there is nothing left
     * to send it from, so it is a hole in the picture until the application
     * happens to draw that place again.  The kernel's band renderer hands a
     * whole flush over as many strips in quick succession, and the gate let
     * through the first of them and threw the rest away.
     *
     * So the application is paced by the wire instead, which is the cost of
     * having no frame and the one REMDISP has always paid.  A tile is given
     * up only when the client has stopped reading for DIRECT_WAIT_MS - then it
     * is counted as hushed, and the client is the one that is gone.
     */
    for (uint32_t at = 0; at < total; at++) {
        const uint32_t y = (at / across) * th;
        const uint32_t x = (at % across) * tw;
        uint32_t rows = b->h - y;
        if (rows > th) {
            rows = th;
        }
        uint32_t cols = b->w - x;
        if (cols > tw) {
            cols = tw;
        }
        const uint8_t *corner =
            src + (size_t)y * b->stride + (size_t)x * sizeof(uint16_t);

        if (full) {
            for (uint32_t k = 0; k < rows; k++) {
                memcpy(s.band + k * cols, corner + (size_t)k * b->stride,
                       (size_t)cols * sizeof(uint16_t));
            }
        }
        const uint64_t tile_by =
            ag_micros() + (uint64_t)DIRECT_WAIT_MS * 1000ull;
        for (;;) {
            const wire_t r =
                full ? encode_and_send((uint32_t)b->x + x, (uint32_t)b->y + y,
                                       cols, rows, SEND_IDLE_MS, DIRECT_LOCK_MS)
                     : encode_rows_and_send(corner, b->stride,
                                            (uint32_t)b->x + x,
                                            (uint32_t)b->y + y, cols, rows);
            if (r == WIRE_OK) {
                break;
            }
            if (r == WIRE_GONE) {
                return r;
            }
            if (ag_micros() > tile_by) {
                /* The client has stopped reading; the rest of this
                 * rectangle is lost to it, and counted. */
                s.band_hushed += total - at;
                return WIRE_STALL;
            }
            TASK->sleep_ms(5u);
        }
    }
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
                ws_send(OP_AUTH, buf + WS_SLACK, NONCE_LEN, SEND_IDLE_MS,
                        3000u);
            if (r == WIRE_GONE) {
                return false;
            }
            s.challenged = (r == WIRE_OK);
        }
        return true;
    }

    if (s.owe_info && !s.owe_everything) {
        const wire_t r = send_info(3000u);
        if (r == WIRE_GONE) {
            return false;
        }
        if (r == WIRE_OK) {
            s.owe_info = false;
        }
        return true;
    }

    if (s.owe_everything) {
        const wire_t r = send_info(3000u);
        if (r == WIRE_GONE) {
            return false;
        }
        if (r == WIRE_STALL) {
            return true; /* still owed; try again next pass */
        }
        s.owe_everything = false;
        s.owe_info = false;
        lock();
        /* Everything is going; a scroll the client never saw means nothing. */
        s.scroll_owed = 0u;
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

    /*
     * The scroll first, and nothing else until it has gone.
     *
     * A row sent before it would land on the client's old screen and be
     * scrolled out from under itself a moment later, which is a line of text
     * in the wrong place until something happens to that row again.
     */
    if (s.scroll_owed != 0u) {
        const wire_t w = send_scroll(s.scroll_owed);
        if (w == WIRE_GONE) {
            return false;
        }
        if (w != WIRE_OK) {
            return true; /* still owed; the rows wait with it */
        }
        s.scroll_owed = 0u;
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

    /*
     * And one row that nobody asked for.  See s.repair_row: this is the only
     * thing that repairs a line the client never received, and it costs one
     * row per interval whether or not anything is wrong.
     */
    /* Not under a picture: the console is behind it, nobody is looking at
     * these rows, and every one is Wi-Fi traffic beside the application. */
    if (s.rows != 0u && !s.picture_live) {
        const uint64_t now_us = (uint64_t)ag_micros();
        if (now_us >= s.repair_at) {
            s.repair_at = now_us + REPAIR_EVERY_US;
            if (s.repair_row >= s.rows) {
                s.repair_row = 0;
            }
            const wire_t w = send_text_row(s.repair_row);
            if (w == WIRE_GONE) {
                return false;
            }
            if (w == WIRE_OK) {
                s.repair_row++;
            }
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

    /* Taken and dropped over Bluetooth, which carries no pictures: the damage
     * is still collected, so a WebSocket client that takes over later gets the
     * whole screen from owe_everything and nothing is lost by this. */
    if (s.ble_client || x0 >= x1 || y0 >= y1) {
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
     * Ctrl+C and F12 used to be dropped here, on the grounds that a phone which
     * can stop the foreground process is a phone that stops it by accident.
     *
     * That was the wrong trade, and using the thing showed it: a phone could
     * start a program and then had no way at all to leave one.  A file manager
     * that would not take Escape - because it had a one-kilobyte arena and was
     * barely running - left the board occupied with nothing to be done about it
     * from the only screen in the room.  Accidents are also less likely than
     * they look: these arrive as deliberate chords, latched modifier then key,
     * or from the bar's own Stop key.  A stuck board is worse than a stopped
     * program.
     */

    /*
     * Chords, written down as they arrive.
     *
     * "Switching slots does not work" has three possible homes - the page not
     * sending the modifier, this driver dropping it, or the supervisor not
     * acting on it - and they look identical from a phone.  One line at the
     * boundary tells them apart.  Only keys with a modifier, and only presses,
     * so an ordinary session adds nothing to the journal.
     */
    if (kind == 1u && mods != 0u) {
        ag_log(AG_LOG_INFO, "phone", "chord: hid %u, mods %u", (unsigned)hid,
               (unsigned)mods);
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

/* ---- the input write-ahead log ----------------------------------------- */
/*
 * Every key and tap from the page is written to C:\PHONELOG.TXT and flushed to
 * flash BEFORE it is acted on.  A driver's ag_log reaches only the journal in
 * RAM, and a watchdog reset wipes that - so the very input that hung the board
 * would vanish with it.  Synced ahead of the injection, the last line in this
 * file is the one that did it, and the whole file replays the session.  Asked
 * for on 19 September: "record my presses so they can be replayed and the hang
 * caught."
 *
 * Opened, written, synced and closed per event: slow by flash standards and
 * nothing by human ones, and the point is durability, not throughput.  Pointer
 * moves are not logged - they arrive many times a second and never carry the
 * fault - only key up/down, typed text, taps and the wheel.
 */
#define INLOG_PATH "C:\\PHONELOG.TXT"
#define INLOG_CAP  (64u * 1024u)
static uint32_t s_inlog_bytes;

static void input_log(const char *line)
{
    if (line == NULL || !AG_HAS(ag_api()->fs, write)) {
        return;
    }
    const uint32_t n = (uint32_t)strlen(line);
    uint32_t flags = AG_O_WRONLY | AG_O_CREATE | AG_O_APPEND;
    if (s_inlog_bytes + n > INLOG_CAP) {
        /* Wrap rather than grow without bound: a long session keeps its most
         * recent trail, which is the part a hang is at the end of. */
        flags = AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC;
        s_inlog_bytes = 0u;
    }
    const ag_handle_t f = ag_open(INLOG_PATH, flags);
    if (f < 0) {
        return;
    }
    if (ag_write(f, line, n) == (int32_t)n) {
        s_inlog_bytes += n;
    }
    /* Sync, so the line is on flash before the injection below runs - the whole
     * point is that a line survives the input that hangs the board. */
    (void)ag_sync(f);
    (void)ag_close(f);
}

static void handle_message(const uint8_t *p, uint32_t len)
{
    if (len == 0u) {
        return;
    }
    const uint8_t op = p[0];
    const uint8_t *body = p + 1;
    const uint32_t n = len - 1u;
    const unsigned ms = (unsigned)(ag_micros() / 1000ull);

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
            {
                char b[48];
                (void)snprintf(b, sizeof(b), "%u ---- bad password ----\n", ms);
                input_log(b);
            }
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
        {
            char b[48];
            (void)snprintf(b, sizeof(b), "%u ==== authenticated ====\n", ms);
            input_log(b);
        }
        ag_log(AG_LOG_INFO, "phone", "client authenticated");
        break;
    }

    case IN_HELLO: {
        char b[40];
        (void)snprintf(b, sizeof(b), "%u hello\n", ms);
        input_log(b);
        s.owe_everything = true;
        break;
    }

    case IN_KEY:
        if (n >= 7u) {
            /* Write-ahead: on flash before it is injected, so a key that hangs
             * the board is still the last line in the log after the reset. */
            char b[80];
            (void)snprintf(b, sizeof(b),
                           "%u key %s mods=%u hid=%u uni=%u rpt=%u\n", ms,
                           (body[0] == 1u) ? "down"
                                           : (body[0] == 2u) ? "up" : "?",
                           (unsigned)body[1], (unsigned)rd16(body + 2),
                           (unsigned)rd16(body + 4), (unsigned)body[6]);
            input_log(b);
            inject_key(body[0], body[1], rd16(body + 2), rd16(body + 4),
                       body[6] != 0u);
        }
        break;

    case IN_PTR:
        if (n >= 7u) {
            ag_event_t ev;
            /* Write-ahead, but not for moves: a tap (down/up) or the wheel can
             * hang the board through a redraw; a move never has, and there are
             * hundreds a second of them. */
            if (body[0] == 1u || body[0] == 2u || body[0] == 4u) {
                char b[80];
                (void)snprintf(b, sizeof(b),
                               "%u ptr %s btn=%u x=%d y=%d w=%d\n", ms,
                               (body[0] == 1u) ? "down"
                                               : (body[0] == 2u) ? "up"
                                                                 : "wheel",
                               (unsigned)body[1],
                               (int)(int16_t)rd16(body + 2),
                               (int)(int16_t)rd16(body + 4),
                               (int)(int8_t)body[6]);
                input_log(b);
            }
            /*
             * The second button, written down when it arrives.
             *
             * A touch screen has one button and the desktop's context menu
             * wants the other, so the page arms it with a latch - and from the
             * outside a tap that arrives as the wrong button is
             * indistinguishable from a tap that did not arrive.  Only the
             * press, and only the button that is unusual, so an ordinary
             * session of tapping adds nothing to the journal.
             */
            if (body[0] == 1u && (body[1] & 2u) != 0u) {
                ag_log(AG_LOG_INFO, "phone", "right button at %d,%d",
                       (int)(int16_t)rd16(body + 2),
                       (int)(int16_t)rd16(body + 4));
            }
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
        {
            /* The typed text itself, capped so one paste cannot fill the log. */
            char b[80];
            const uint32_t take = (n < 48u) ? n : 48u;
            (void)snprintf(b, sizeof(b), "%u text \"%.*s\"\n", ms, (int)take,
                           (const char *)body);
            input_log(b);
        }
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
        s.last_life_us = (uint64_t)ag_micros();

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

/* ---- Bluetooth: the same client, another way in ------------------------ */

/*
 * The name the board advertises: `[phone] name`, else `[net] hostname`, else
 * "argon" - the name a person already calls the board by on the network.
 * Twenty characters at most, because the advertisement is 31 bytes and the
 * service UUID and the flags take nine of them.
 */
static void ble_name(char *out, size_t cap)
{
    const ag_cfg_api_t *cfg = ag_api()->cfg;
    out[0] = '\0';
    if (cfg != NULL && AG_HAS(cfg, get_str)) {
        if (cfg->get_str("phone.name", out, cap) != AG_OK || out[0] == '\0') {
            if (cfg->get_str("net.hostname", out, cap) != AG_OK) {
                out[0] = '\0';
            }
        }
    }
    if (out[0] == '\0') {
        ag_strlcpy(out, "argon", cap);
    }
    if (strlen(out) > 20u) {
        out[20] = '\0';
    }
}

/*
 * Everything Bluetooth, once a lap: advertise while the system has the radio
 * up, notice a phone leaving, and read what the phone wrote.
 *
 * The radio is never raised here.  `bt on` or `[bt] on = yes` is the system's
 * decision, exactly as joining a Wi-Fi network is, and this only uses a radio
 * that is already there - the same standing the WebSocket side has with
 * ensure_listen, which waits for the network rather than bringing it up.
 *
 * A phone becomes the client on its hello, not on connecting.  Connecting is
 * the radio's business and happens before the phone has even subscribed to
 * anything it could be told; the hello is the page saying it is ready to be
 * shown a screen - which is also exactly when the WebSocket side sends
 * everything.  Only a hello (or the answer to a challenge) takes the screen,
 * so a key still in flight from a phone that was just told "replaced" cannot
 * take it back behind the other client's back.
 */
static void pump_ble(void)
{
    const ag_ble_api_t *ble = ag_api()->ble;
    if (!AG_HAS(ble, adv_recv) || !AG_HAS(ble, radio_on)) {
        return; /* an older kernel, or a build with no BLE peripheral */
    }

    if (!ble->radio_on()) {
        if (s.ble_adv) {
            s.ble_adv = false;
            ag_log(AG_LOG_INFO, "phone", "bluetooth is off; not advertising");
        }
        if (s.ble_client) {
            drop_conn("bluetooth off");
        }
        return;
    }

    ag_ble_adv_status_t st;
    memset(&st, 0, sizeof(st));
    if (ag_ble_adv_status(&st) != AG_OK) {
        return;
    }
    if (!st.advertising) {
        s.ble_adv = false;
        const uint64_t now = (uint64_t)ag_micros();
        if (now >= s.ble_retry_at) {
            char name[24];
            ble_name(name, sizeof(name));
            const ag_err_t e = ag_ble_adv_start(name);
            if (e == AG_OK) {
                s.ble_adv = true;
                ag_log(AG_LOG_INFO, "phone", "bluetooth: advertising as \"%s\"",
                       name);
            } else {
                /* Once every few seconds, not every lap: a refusal now is a
                 * refusal a millisecond from now. */
                s.ble_retry_at = now + 5000000ull;
                ag_log(AG_LOG_WARN, "phone", "bluetooth: cannot advertise (%d)",
                       (int)e);
            }
        }
        return;
    }
    s.ble_adv = true;

    if (s.ble_client && (!st.connected || ag_ble_adv_mtu() == 0u)) {
        drop_conn("bluetooth link gone");
    }

    uint8_t msg[256];
    for (;;) {
        const int32_t n = ag_ble_adv_recv(msg, sizeof(msg));
        if (n <= 0) {
            break;
        }
        if (!s.ble_client) {
            if (msg[0] != IN_HELLO) {
                continue; /* not a client yet, and this is not the knock */
            }
            /* Whoever arrived last has the screen - the WebSocket side's rule,
             * and the one who had it is told why rather than just going dark. */
            if (s.conn >= 0) {
                ws_close("replaced");
                drop_conn("replaced");
            }
            s.ble_client = true;
            s.scroll_owed = 0u;
            s.hush_until = 0u;
            s.authed = false;
            s.challenged = false;
            s.owe_everything = true;
            make_nonce();
            ag_log(AG_LOG_INFO, "phone", "client connected over bluetooth "
                                         "(%u bytes a piece)",
                   (unsigned)ag_ble_adv_mtu());
        }
        handle_message(msg, (uint32_t)n);
        if (!s.ble_client) {
            break; /* turned away (wrong password): the rest is not ours */
        }
    }
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
            const wire_t w = send_all(h, buf, (uint32_t)got, SEND_IDLE_MS);
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
        s.scroll_owed = 0u;
        s.last_life_us = (uint64_t)ag_micros();
        s.authed = false;
        s.challenged = false;
        s.owe_everything = true;
        /* A fresh challenge per client.  Nothing called this before, so every
         * connection was asked the same all-zero nonce and a digest seen once
         * on the wire opened the board for ever after. */
        make_nonce();
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
/*
 * Which buffer could not be had, and what the machine had at the time.
 *
 * Said once: the caller comes straight back and a line an attempt is two a
 * second of the same sentence.  Both figures, because they answer different
 * questions - the arena is whoever is being charged, and inside blit_rect that
 * is the process that happened to draw rather than this driver, so a refusal
 * with the system half free is that and not a shortage.
 */
/*
 * Reachable from blit_rect, so it does not speak - see `owe_nomem`.  It writes
 * down what it would have said; the task says it, with the memory figures read
 * there rather than here (ag_meminfo is an ABI call, and this is no place to
 * make one either).
 */
static bool no_pixel_memory(const char *what, size_t bytes)
{
    if (!s.owe_nomem) {
        size_t i = 0;
        for (; what[i] != '\0' && i + 1u < sizeof(s.owe_nomem_what); i++) {
            s.owe_nomem_what[i] = what[i];
        }
        s.owe_nomem_what[i] = '\0';
        s.owe_nomem_bytes = (uint32_t)bytes;
        s.owe_nomem = true;
    }
    return false;
}

/*
 * The complaints owed by the pixel path, said from the task, which holds
 * nothing.  Once each: they are about how the board is built, not about this
 * frame.
 */
static void say_what_is_owed(void)
{
    if (s.owe_nomem) {
        s.owe_nomem = false;
        ag_meminfo_t mi;
        ag_meminfo(&mi);
        ag_log(AG_LOG_WARN, "phone",
               "no memory for %s (%u bytes): arena %u free, %u largest; "
               "system %u free; the picture goes the light way (no palette, "
               "straight from the drawing task)",
               s.owe_nomem_what, (unsigned)s.owe_nomem_bytes,
               (unsigned)mi.arena_free, (unsigned)mi.arena_largest,
               (unsigned)mi.system_free);
    }
    if (s.owe_noframe) {
        s.owe_noframe = false;
        ag_log(AG_LOG_WARN, "phone",
               "no memory for a %ux%u frame (%u KB); sending from the drawing "
               "task instead",
               (unsigned)s.owe_frame_w, (unsigned)s.owe_frame_h,
               (unsigned)((s.owe_frame_w * s.owe_frame_h * 2u) / 1024u));
    }
    if (s.owe_nostack) {
        s.owe_nostack = false;
        ag_log(AG_LOG_WARN, "phone",
               "picture sent the light way: %u bytes of the drawing task's "
               "stack left, %u wanted for the full codec (AG_APP_SIZED)",
               (unsigned)s.owe_stack_left, (unsigned)BLIT_STACK_FLOOR);
    }
}

/*
 * The frame is given up, for this driver's life, when the board has not got
 * the memory for it - and given up means blit_rect stops waiting for it.
 *
 * It did not, and the comment where the frame fails said it did: "sending from
 * the drawing task instead" was printed, while blit_rect went on seeing
 * want_frame, dropping every rectangle and asking for the frame again.  A
 * board with too little memory for a frame showed no picture at all.
 */
static void give_up_frame(void)
{
    if (s.want_frame && s.frame == NULL) {
        s.want_frame = false;
    }
}

static bool ensure_memory(void)
{
    if (s.codec_light) {
        /* Nothing to allocate; the size is still the screen's (see below). */
        if (s.want_w != 0u && s.want_h != 0u) {
            s.cap_w = s.want_w;
            s.cap_h = s.want_h;
            s.want_w = 0;
            s.want_h = 0;
        }
        return true;
    }
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

    /*
     * A board that cannot find these thirteen kilobytes will not find a frame
     * ten times their size either, so a refusal here gives the frame up too:
     * the picture then goes straight from the drawing task, the light way.
     */
    if (s.pix == NULL) {
        const size_t need = ag_pixband_size(s.band_px);
        void *mem = ag_malloc(need);
        if (mem == NULL || !ag_pixband_init(mem, need, s.band_px)) {
            ag_free(mem);
            give_up_frame();
            return no_pixel_memory("the encoder", need);
        }
        s.pix = (ag_pixband_ctx_t *)mem;
    }
    if (s.band == NULL) {
        s.band = (uint16_t *)ag_malloc(s.band_px * sizeof(uint16_t));
        if (s.band == NULL) {
            give_up_frame();
            return no_pixel_memory("a band", s.band_px * sizeof(uint16_t));
        }
    }
    if (s.out == NULL) {
        s.out = (uint8_t *)ag_malloc(s.band_cap);
        if (s.out == NULL) {
            give_up_frame();
            return no_pixel_memory("the encoded band", s.band_cap);
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
            s.owe_frame_w = w;
            s.owe_frame_h = h;
            s.owe_noframe = true; /* said by the task: see say_what_is_owed */
        }
        give_up_frame();
        /* The size stays the answer to "how big is this screen": see the
         * !want_frame branch above. */
        s.cap_w = w;
        s.cap_h = h;
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
    /* Freed inside the lock: send_band reads s.frame under this same lock, so
     * the old buffer cannot be handed to it and freed at the same time.  This
     * grow runs on whichever task drew (blit_rect calls ensure_memory), which
     * is not the task that reads the frame - the same cross-task hazard the
     * buffer frees above were removed for. */
    ag_free(old);
    unlock();

    ag_log(AG_LOG_INFO, "phone", "frame %ux%u (%u KB)", (unsigned)w,
           (unsigned)h, (unsigned)((w * h * 2u) / 1024u));
    s.owe_everything = true;
    return true;
}

/*
 * Which part of a lap the time went into.
 *
 * The heartbeat is printed at the top of the loop, so it can only ever report
 * a lap that finished: when a lap took ten seconds instead of ten
 * milliseconds, the stage it printed was the end of the *previous* one and the
 * call actually holding things up was invisible.  A stopwatch per stage has no
 * such blind spot - whatever ran long is still the largest number when the
 * next beat comes round, whether or not the loop was in it at the time.
 *
 * Kept as a maximum rather than a total: one ten-second call and a thousand
 * ten-millisecond ones add up alike, and it is the single long call that is
 * the fault being looked for.
 */
#define STAGE_COUNT 10u
static uint32_t stage_max_us[STAGE_COUNT];
static uint8_t  stage_at;
static uint64_t stage_since;

static void stage_enter(uint8_t n)
{
    const uint64_t now = (uint64_t)ag_micros();
    if (stage_since != 0u && stage_at < STAGE_COUNT) {
        const uint32_t spent = (uint32_t)(now - stage_since);
        if (spent > stage_max_us[stage_at]) {
            stage_max_us[stage_at] = spent;
        }
    }
    stage_at = n;
    stage_since = now;
    s.stage = n;
}

static void phone_task(void *arg)
{
    (void)arg;

    uint64_t next_sync = 0;
    uint64_t next_beat = 0;
    uint32_t laps = 0;

    while (!s.stop) {
        /* A light band an application is waiting on, whatever else this lap
         * is about to do. */
        serve_handoff();
        /* Anything the pixel path could not say for itself - see the note
         * above say_what_is_owed. */
        say_what_is_owed();

        /*
         * A heartbeat, because "the board stopped accepting" is a thing that
         * cannot be caught in the act.  Every thirty seconds this says where the
         * loop has been and how many times it has been round; a stage that
         * stops advancing names the call that is blocking, which is the one
         * thing an outside observer cannot see.  Add a counter rather than try
         * to catch the moment.
         */
        const uint64_t beat = (uint64_t)ag_micros();
        if (beat >= next_beat) {
            next_beat = beat + 30000000ull;
            unsigned worst = 0, worst_us = 0;
            for (unsigned i = 0; i < STAGE_COUNT; i++) {
                if (stage_max_us[i] > worst_us) {
                    worst_us = stage_max_us[i];
                    worst = i;
                }
                stage_max_us[i] = 0;
            }
            ag_log(AG_LOG_INFO, "phone",
                   "alive: %u laps, in stage %u, worst stage %u took %u us, "
                   "listen=%d conn=%d closing=%d ble=%s",
                   (unsigned)laps, (unsigned)s.stage, worst, worst_us,
                   (int)s.listen, (int)s.conn, (int)s.closing,
                   s.ble_client ? "client"
                                : s.ble_adv ? "advertising" : "off");
            /*
             * And the picture, on its own line so a search for it finds one
             * thing.  Silent when nothing has ever drawn, which is the common
             * case on a board showing a shell.
             */
            if (s.blits != 0u || s.bands != 0u) {
                ag_log(AG_LOG_INFO, "phone",
                       "picture: %u blits in, %u bands out (%u light), "
                       "%u bytes; "
                       "not sent: %u no memory, %u hushed, %u wire busy, "
                       "%u refused; %u light for want of stack; "
                       "stalls: %u band, %u text; "
                       "surface %ux%u, frame %s",
                       (unsigned)s.blits, (unsigned)s.bands,
                       (unsigned)s.bands_lite,
                       (unsigned)s.band_bytes, (unsigned)s.band_nomem,
                       (unsigned)s.band_hushed, (unsigned)s.band_busy,
                       (unsigned)s.band_refused, (unsigned)s.band_nostack,
                       (unsigned)s.stall_band, (unsigned)s.stall_text,
                       (unsigned)s.frame_w,
                       (unsigned)s.frame_h,
                       (s.frame != NULL) ? "held" : "none (sent from the "
                                                    "drawing task)");
            }
        }
        laps++;

        stage_enter(1);
        /* The console's size, a few times a second.  See sync_console. */
        const uint64_t now = (uint64_t)ag_micros();
        if (now >= next_sync) {
            next_sync = now + 300000ull;
            stage_enter(8);
            if (!sync_console()) {
                TASK->sleep_ms(500u);
                continue;
            }
        }

        /*
         * Either way in, or both.  The network is waited for rather than
         * required: a board with Bluetooth and no Wi-Fi is still a board a
         * phone can reach, and one with neither yet is one that may get
         * either later - `wifi connect`, `bt on` - without this driver being
         * reloaded.
         */
        const bool have_net = ag_api()->net != NULL;
        if (have_net) {
            pump_deferred();
        }

        stage_enter(2);
        const bool listening = have_net && ensure_listen();
        stage_enter(9);
        pump_ble();
        if (!listening && !s.ble_client) {
            TASK->sleep_ms(have_net ? 200u : 50u);
            continue;
        }
        stage_enter(3);
        /*
         * Allocate on THIS task, and on a frame board do it before anything has
         * drawn - not only after the first blit set want_pixels.
         *
         * Two things were wrong when the buffers were taken inside blit_rect.
         * First, blit_rect runs on the drawing application's task, so the
         * allocation was charged to that application's arena - small internal
         * SRAM on a .AXE - and a 5 KB band failed there while the driver's own
         * eight megabytes of PSRAM sat free, which is the "no memory for a band:
         * arena 8 MB free" that made no sense.  Second, the failure dropped the
         * application's FIRST blit, and for a program that paints a whole scene
         * once and then only animates a strip (gfxpix), that first blit IS the
         * scene: the phone then showed black everywhere the moving dot had not
         * yet been.  Taken here, on a task that belongs to no process, the frame
         * comes from the driver's PSRAM and is ready before the scene is drawn,
         * so the scene lands in it and every client - even one that connects
         * later - gets the whole picture from owe_everything.
         */
        /*
         * And a refusal stops nothing.  This used to sleep half a second and
         * start the lap again, which skipped the accept, the console and the
         * keyboard below: a board short of thirteen kilobytes for a PICTURE
         * stopped answering the page and stopped sending text.  Now the picture
         * goes the light way (s_lite) while there is no memory, and the memory
         * is asked for again every two seconds rather than every lap.
         */
        if ((s.want_frame || s.want_pixels) && now >= s.mem_retry_at &&
            !ensure_memory()) {
            s.mem_retry_at = now + 2000000ull;
        }
        stage_enter(4);

        /*
         * A new connection, whether or not one is already live.  A phone that
         * has locked its screen and come back does not close anything - it just
         * opens a second socket, and the old one sits there being written to
         * for as long as TCP takes to notice.  So the newest upgrade wins.
         */
        const ag_handle_t fresh =
            listening ? ag_tcp_accept(s.listen, 0u) : -AG_EAGAIN;
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

        if (s.conn < 0 && !s.ble_client) {
            nap(50u); /* answers a hand-over too: WIRE_GONE, nobody to send to */
            continue;
        }
        /*
         * A client that has proved nothing for half a minute.  Not an
         * error anywhere - every call returned what it should - which
         * is why nothing else in this loop can see it.  WebSocket only:
         * a Bluetooth link that has gone is said so by the radio itself
         * (pump_ble), and a phone showing a quiet shell writes nothing.
         */
        if (s.conn >= 0 && s.last_life_us != 0u &&
            (uint64_t)ag_micros() - s.last_life_us > LINK_SILENT_US) {
            drop_conn_reason("silent for 30 s", true);
            continue;
        }
        stage_enter(5);
        if (s.conn >= 0 && !pump_client()) {
            drop_conn("closed by peer");
            continue;
        }
        stage_enter(6);
        if (!service_client()) {
            drop_conn_reason("write failed", true);
            continue;
        }
        stage_enter(7);

        /*
         * Ten milliseconds, which is the interval a screen is asked for rather
         * than the interval it changes.  Nothing here polls the picture: the
         * damage is marked by blit_rect and this only notices it, so a still
         * screen costs one wakeup a hundredth of a second and no bytes at all.
         * A light band handed over by a drawing application cuts it short.
         */
        nap(10u);
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
    /* The advertising this driver started goes with it: a board that says
     * "connect to me" with nothing left to answer is a trap for the phone. */
    if (s.ble_adv && AG_HAS(ag_api()->ble, adv_stop)) {
        (void)ag_api()->ble->adv_stop();
        s.ble_adv = false;
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
    s.blits++;
    (void)h;
    if (b == NULL || b->px == NULL || b->w == 0u || b->h == 0u) {
        return;
    }

    /*
     * The picture buffers are NOT allocated here, and that is the whole of the
     * crash the board kept hitting.
     *
     * blit_rect runs on the DRAWING application's task, so an ag_malloc here
     * comes from that application's arena - and when the application exits, its
     * arena is freed whole, taking the driver's band, encoder and frame with it
     * while the driver still points at them.  The driver then went on writing
     * pixels into freed memory: comprehensive heap poisoning caught it as
     * "CORRUPT HEAP: ... got 0xf800..." - an RGB565 red in a block that should
     * have held the free pattern - and without poisoning it was the board
     * freezing or resetting a moment after a program that drew was stopped.
     *
     * The buffers therefore belong to the driver's own task, which has no
     * process and takes them from the driver's heap (the ensure_memory call in
     * phone_task).  Here we only USE them.  With no frame the direct path below
     * needs none of them (it has s_lite); with a frame, a rectangle that
     * arrives before the task has made them is dropped - it allocates within a
     * lap and the next one lands.
     */
    s.want_pixels = true;

    /*
     * No frame at all: this board could not hold one.  Then the rectangle goes
     * out from here, on the caller's task, and the caller waits for the wire.
     * Not a fallback bolted on - it is the only way a board with 179 KB and no
     * PSRAM can show a picture at all.
     */
    if (s.frame == NULL) {
        /*
         * A frame board whose task has not made the frame yet: drop this
         * rectangle and tell the task the size to make.  Do NOT fall into the
         * direct path below - it encodes and sends from this, the drawing
         * application's, stack, which was its own source of resets.  On a frame
         * board the frame is ready within a lap of boot, so this is only ever
         * the first blit or two.
         */
        if (s.want_frame) {
            s.want_w = b->surf_w;
            s.want_h = b->surf_h;
            return;
        }
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
            /*
             * On the caller's stack, so ask whether there is one to spend.
             *
             * What follows is the band encoder, the WebSocket framing and the
             * whole of lwIP, and an application that declared eight kilobytes
             * for its own drawing has not got it: measured, three board resets
             * in seventeen runs, each a stack overflow in the application that
             * drew and each followed by the allocator asserting on a heap the
             * overflow had already crossed into.
             *
             * Zero means the kernel cannot say (ABI older than 0.51), and then
             * this behaves as it always did.
             */
            const uint32_t left = ag_stack_left();
            bool full = (s.pix != NULL && s.band != NULL && s.out != NULL);
            if (full && left != 0u && left < BLIT_STACK_FLOOR) {
                /*
                 * Too little stack for the full codec and lwIP here - so not
                 * refused any more, but sent the light way, which encodes on
                 * this stack in a few hundred bytes and sends on the driver's.
                 * Counted, and said once, because it costs the palette.
                 */
                full = false;
                s.band_nostack++;
                if (!s.moaned_stack) {
                    s.moaned_stack = true;
                    s.owe_stack_left = left;
                    s.owe_nostack = true; /* said by the task, not from here */
                }
            }
            (void)send_blit_direct(b, full);
        }
        return;
    }

    if (s.pix == NULL || s.band == NULL || s.out == NULL) {
        s.band_nomem++;
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

/*
 * The console moved up, and this mirror moves with it.
 *
 * Called on the console task with the registry held, like text_row, and under
 * the same rule: it must not print and it must not wait.  All it does is a
 * memmove and a note; the wire is the task's.
 */
static void phone_text_scroll(ag_handle_t h, uint16_t lines)
{
    (void)h;
    if (lines == 0u || s.cells == NULL || lines >= s.rows) {
        return;
    }
    lock();
    const uint16_t keep = (uint16_t)(s.rows - lines);
    memmove(s.cells, s.cells + (size_t)lines * s.cols,
            (size_t)keep * s.cols * sizeof(s.cells[0]));
    for (uint16_t y = keep; y < s.rows; y++) {
        for (uint16_t x = 0; x < s.cols; x++) {
            s.cells[(size_t)y * s.cols + x].ch = ' ';
            s.cells[(size_t)y * s.cols + x].attr = 0x07u;
        }
    }
    /* A row that was waiting to be sent is the same row of text, higher up. */
    for (uint16_t y = 0; y < keep; y++) {
        s.row_dirty[y] = s.row_dirty[y + lines];
    }
    for (uint16_t y = keep; y < s.rows; y++) {
        s.row_dirty[y] = 1u;
    }
    /*
     * More than the screen is tall, or more than the client has kept up with:
     * cheaper to send the screen than to explain it, and owe_everything is
     * exactly that.
     */
    const uint32_t owed = (uint32_t)s.scroll_owed + lines;
    if (owed >= s.rows || owed > 255u) {
        s.scroll_owed = 0u;
        s.owe_everything = true;
    } else {
        s.scroll_owed = (uint8_t)owed;
    }
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

/*
 * An application took the screen or let it go (ABI 0.68).  The page is told
 * with the next geometry message, and shows the picture or the console because
 * the board said so - not because two seconds passed without a band, which is
 * how a still picture used to turn into the console under a running program.
 */
static void phone_held(ag_handle_t h, bool held)
{
    (void)h;
    s.knows_held = true;
    if (s.picture_live != held) {
        s.picture_live = held;
        s.owe_info = true;
    }
}

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
    .text_scroll = phone_text_scroll,
    .held = phone_held,
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
    .text_scroll = NULL,
    .held = phone_held,
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

    if (api->net == NULL && !AG_HAS(api->ble, adv_recv)) {
        ag_printf("PHONE: this build has neither a network nor a BLE "
                  "peripheral\n");
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
    /* The light path's hand-over to the task (encode_rows_and_send).  Without
     * them the light path answers WIRE_GONE and nothing else changes. */
    s_hand_go = TASK->sem_create(0u, 1u);
    s_hand_done = TASK->sem_create(0u, 1u);
    /* A kernel at 0.68 or later says who holds the screen (phone_held). */
    s.knows_held = (ag_api()->abi_major > 0u || ag_api()->abi_minor >= 68u);

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
        /*
         * `[phone] codec = light`: never ask for the thirteen kilobytes of
         * band, packed output and palette context either, and send every
         * picture the light way (s_lite) - for the board where those thirteen
         * are what keeps the page loading (argon-phone-memory-floor).  No
         * frame then either: the frame path encodes through those buffers.
         */
        if (cfg != NULL && AG_HAS(cfg, get_str)) {
            char codec[8];
            if (cfg->get_str("phone.codec", codec, sizeof(codec)) == AG_OK &&
                strcmp(codec, "light") == 0) {
                s.codec_light = true;
                s.want_frame = false;
            }
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
        /*
         * Sized, not allocated.  Taking the picture buffers at load time means
         * holding thirteen kilobytes from boot for a screen that may only ever
         * show text - and on this board that is exactly the memory an
         * application needs to leave the driver, so the page stopped loading
         * whenever anything ran.  They are taken when the first rectangle
         * arrives and given back when the client goes.
         */
    }

    /*
     * The input log's running size, and a line that marks this boot.
     *
     * The size is read from the file so append lands past what a previous boot
     * wrote and the cap still bounds it.  The boot marker is the divider a hang
     * is found by: the last input line before a "boot" line is the one the
     * board reset on.
     */
    if (AG_HAS(api->fs, write)) {
        ag_stat_t lst;
        s_inlog_bytes = (ag_stat(INLOG_PATH, &lst) == AG_OK)
                            ? (uint32_t)lst.size
                            : 0u;
        char b[64];
        (void)snprintf(b, sizeof(b), "==== boot %s ====\n", "PHONE " "1.1");
        input_log(b);
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

    ag_printf("PHONE: open http://<board>:%u/%s - %ux%u cells%s, %s\n",
              (unsigned)s.port,
              AG_HAS(api->ble, adv_recv) ? " or the app over bluetooth" : "",
              (unsigned)s.cols, (unsigned)s.rows,
              s.publish_text ? "" : " (pixels only)",
              needs_password() ? "password set"
                               : "NO PASSWORD ([phone] password to set one)");
    return AG_OK;
}
