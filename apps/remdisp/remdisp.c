/*
 * ArgonOS - this machine's screen, sent down a wire to the board that has glass.
 *
 * The S3 on the desk has no display.  The board next to it is little else but
 * one, and two soldered pins join them (docs/09-esp32-cyd.md, "Провод к S3").
 * So the screen becomes a device like any other: this publishes a display whose
 * panel is a serial port, and nothing above it can tell the difference.
 *
 * It carries three kinds of traffic - two because the kernel offers two and a
 * screen needs both, and a third because the board at the other end has a
 * speaker and this one does not:
 *
 *   Pixels (blit_rect, ABI 0.30) - the picture, in bands, sent from a task of
 *   this driver's own (ABI 0.43).  What the caller pays is a copy into the
 *   driver's frame; the wire is somebody else's problem after that, which is
 *   the whole point - an application drawing on a screen should not be able to
 *   tell that the screen is at the end of a cable.  This is the part
 *   that matters: it is a video link, the same idea as VGA or HDMI with a
 *   thinner cable, and the kernel hands it exactly what such a link wants.
 *   ag_blit_t is band-oriented by construction - the pixels belong to the
 *   caller, and the whole picture and the changed part are described
 *   separately - so a rectangle can be put on the wire without anybody holding
 *   a second copy of the frame.  Both ways in lead here: ag_gfx_flush of the
 *   system surface and ag_gfx_present of an application's own pixels, because
 *   the kernel sends a presented blit to every display device that has a
 *   blit_rect.  Applications need no change and do not know.
 *
 *   Characters (text_info / text_row / text_cursor, ABI 0.27) - the console,
 *   for when no application holds the display.  A forty-column row is eighty
 *   bytes and a whole screen is four kilobytes, so this costs nothing next to
 *   the pixels, and without it the far end would go dark at every prompt.
 *
 *   Sound (a second device, /dev/pcmrem) - eight-bit mono, 22 KB/s of the
 *   400 the wire carries.  The S3's own amplifier was unsoldered; the CYD has
 *   one on its converter pin.  See "sound" below for why it is in this file
 *   and not in a .SYS of its own, which is where it belongs by subject.
 *
 * What is NOT here, on purpose:
 *
 *   No printing from any of the callbacks.  The kernel calls them with the
 *   device registry held, while the console task takes the console first and
 *   the registry second - a driver that reaches for the console from inside one
 *   closes the ring and stops the machine.  Everything this driver has to say,
 *   it says from ag_driver_init.
 *
 *   No acknowledgement and no retry.  A lost band is a stale rectangle until
 *   something redraws it; a driver that blocked waiting for the far end to
 *   agree would trade a smear for a stopped machine.  The text path repairs
 *   itself instead (see the sweep below), and the pixel path is repaired by the
 *   next frame, which is what a video link does anyway.
 *
 * The encodings here are mirrored in host-tests/test_rempix.c, which
 * round-trips them against each other: a change here that is not made there
 * passes the tests and breaks the screen.
 *
 * Build (LX7, for the S3 - the far end is an LX6 and needs its own image):
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc \
 *       --include sdk/include -o build/apps/REMDISP.SYS apps/remdisp/remdisp.c
 *
 *   drv install a:\remdisp.sys
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_DRV("REMDISP", "1.2", "argon");

/* UART1: the console is UART0 on this board, and the numbering is the chip's. */
#define LINK_PORT 1

/*
 * Four megabaud, measured rather than chosen: 400 067 B/s over 4 MB with zero
 * corrupt and zero lost blocks, where five megabaud corrupts 0.12% (apps/link,
 * and the table in docs/09-esp32-cyd.md).  Both ends must agree, so the same
 * number appears in remterm.c.
 */
#ifndef LINK_BAUD
#define LINK_BAUD 4000000u
#endif

/*
 * The far end's console, which is not the same as the far end's glass: its
 * panel is 320x240 with an 8x8 font, so forty by thirty cells, but the console
 * drawn on it is forty by twenty-five (CONFIG_ARGON_CONSOLE_COLS/ROWS in
 * sdkconfig.defaults.esp32).  Cells past the console's edge would be sent for
 * nothing and dropped in silence over there.
 *
 * The kernel treats these as a clamp and not as a resize: a local console wider
 * than this arrives with its right-hand columns cut off rather than reflowed.
 * That is what happens today, because this board's console is eighty columns
 * and is sized at boot from its own Kconfig, before any module loads - so
 * making the two agree is a firmware decision, not something a driver can ask
 * for.  The pixels have no such problem; they are placed by size.
 */
#define REM_COLS 40u
#define REM_ROWS 25u

/* ---- wire format ------------------------------------------------------- */
/*
 *   A5 5A  op  len_lo len_hi   payload[len]   xor
 *
 * Two magic bytes rather than one because a receiver that has just lost
 * synchronisation is reading pixels as if they were a header, and pixels
 * contain every byte value there is.  The length is sixteen bits because a
 * band of pixels does not fit in eight; the xor is one byte, which is thin for
 * five kilobytes of payload, but its job is to catch a receiver reading at the
 * wrong offset, not to protect the picture - a wrong pixel is a wrong pixel for
 * one frame.
 */
#define FR_MAGIC0 0xA5u
#define FR_MAGIC1 0x5Au
#define FR_ROW    'R' /* one row of console cells                            */
#define FR_CUR    'C' /* the caret, and the cell under it                    */
#define FR_BAND   'B' /* a rectangle of pixels, RGB565 as they are           */
#define FR_PACK   'P' /* the same, PackBits over the pixels                  */
#define FR_IDX    'I' /* the same, as a palette and indices                  */
#define FR_AUD    'A' /* sound: a rate, then unsigned 8-bit mono samples     */

/*
 * A band is at most eight rows, and the reason is the far end's panel rather
 * than this end's memory: every band becomes one window-setting on the ILI9341,
 * three commands and a wait, and a band of one row would spend more of the
 * frame on setting up transfers than on pixels (the same lesson as commit
 * e37dbd2 on that board).  Eight rows of 320 is five kilobytes, which the far
 * end can hold without a framebuffer - and not having a framebuffer over there
 * is the whole reason this arrangement exists.
 */
#define BAND_ROWS 8u

/*
 * The largest band, in pixels, and the frame that has to hold one.
 *
 * Eight rows of 320 is the far end's panel width; anything wider is refused
 * rather than split, because a wider surface than the panel has nowhere to go.
 * The frame is sized for the *worst* case rather than the raw one: PackBits can
 * come out slightly larger than what it was given (one control byte per 128
 * pixels), and a codec whose output does not fit in the buffer it was handed is
 * a codec that corrupts the picture on exactly the frames that compress badly.
 */
#define BAND_MAX_PX    (BAND_ROWS * 320u)
#define FR_MAX_PAYLOAD (12u + BAND_MAX_PX * 2u + BAND_MAX_PX / 64u + 16u)

static const ag_io_api_t *io;
static bool               s_up;

/* One frame, built here and written in one call.  Static because the console
 * task's stack is not this driver's to spend, and the kernel serialises every
 * callback that uses it. */
static uint8_t s_frame[5u + FR_MAX_PAYLOAD + 1u];

/* What went and what did not.  Read back through the display's ioctl so the
 * thing above the driver can put it in its own readout - a driver cannot
 * print. */
/* Whether a rectangle may be trimmed against what the far end is believed to
 * have - see rem_ioctl.  On by default; the whole point of the switch is that
 * it can be turned off from outside. */
static bool s_trim = true;
/* Whether a band may be packed or palettised before it goes.  Lossless either
 * way, and stateless - turning it off buys nothing but certainty, which is
 * sometimes the thing wanted. */
static bool s_encode = true;

static uint64_t s_sent_bytes;
static uint32_t s_sent_frames;
static uint32_t s_dropped;

/*
 * True when every byte of the frame reached the port.
 *
 * It used to be void, on the grounds that there is nothing useful to do with a
 * short write from inside a kernel callback.  That is still true of the write
 * itself and false of the bookkeeping: the caller has just decided that the
 * far end now has these pixels, and if the frame did not go, that decision is
 * a lie that nothing corrects until the slow repair sweep comes round - up to
 * seven and a half seconds of a rectangle showing the frame before it.  So the
 * answer goes back and the hash is written only when it is true.
 */
/*
 * One frame atomic on the wire.  Picture and sound both leave this board over
 * the same UART and no longer share the device registry lock (the kernel drops
 * it before a blit so the wire cannot stall the console and the sound), so this
 * is what stops the two building the one shared frame buffer over each other.
 * Held only for one frame - a band, or a little block of samples - so the sound
 * waits at most one band for its turn and every frame comes off the wire whole,
 * which is all the receiver needs since each frame carries its own checksum.
 */
static ag_mutex_t s_wire_lock;

/*
 * Take, and give back only if it was taken.
 *
 * Every lock in this file used to be written as "take with a timeout, ignore
 * the answer, give it back at the end", and that is two faults in one line.
 * The give is from a task that does not hold it, which on FreeRTOS frees a
 * mutex its real owner still believes it holds - so two tasks proceed, and the
 * priority the first one inherited is handed back under it.  And what the two
 * then share is s_frame, the one buffer every frame is assembled in, so the
 * wire carries half of one frame followed by half of another.
 *
 * It could not happen while the only writer was whoever called blit_rect: the
 * kernel held the device registry across that call and the console needed the
 * same lock before it could print, so the two could never be inside here at
 * once.  Giving this driver a task of its own took that accidental
 * serialisation away, and both symptoms arrived together - a picture that was
 * torn and dirty, and a board that stopped answering at all.
 *
 * So the wire lock is waited for without a limit: the holder always finishes,
 * and there is nothing sensible to do with a refusal when what is being
 * guarded is a buffer.  The others keep their timeout and honour the answer.
 */
static bool lock_take(ag_mutex_t m, uint32_t ms)
{
    return (m != NULL) && ag_mutex_lock(m, ms);
}

static void lock_give(ag_mutex_t m, bool held)
{
    if (m != NULL && held) {
        ag_mutex_unlock(m);
    }
}

static bool send_frame(uint8_t op, const uint8_t *payload, uint32_t len)
{
    if (!s_up || len > FR_MAX_PAYLOAD) {
        s_dropped++;
        return false;
    }
    const bool held = lock_take(s_wire_lock, UINT32_MAX);
    s_frame[0] = FR_MAGIC0;
    s_frame[1] = FR_MAGIC1;
    s_frame[2] = op;
    s_frame[3] = (uint8_t)len;
    s_frame[4] = (uint8_t)(len >> 8);

    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) {
        s_frame[5u + i] = payload[i];
        sum = (uint8_t)(sum ^ payload[i]);
    }
    s_frame[5u + len] = sum;

    /*
     * Not retried, and not printed - printing is forbidden here and stalling
     * to retry would stop the console or the application for the sake of one
     * rectangle.  Only counted, and reported to the caller so that what the
     * far end is believed to have stays true.
     */
    const uint32_t want = 5u + len + 1u;
    const int32_t  n = io->uart_write(LINK_PORT, s_frame, want);
    lock_give(s_wire_lock, held);
    if (n < 0 || (uint32_t)n != want) {
        s_dropped++;
        return false;
    }
    s_sent_bytes += want;
    s_sent_frames++;
    return true;
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* ---- pixels ------------------------------------------------------------ */

/*
 * What the far end already has, so that pixels it has cost nothing to keep
 * there - and so that what *is* sent is trimmed to the part that moved.
 *
 * This is the largest saving available and it is not compression: what a screen
 * does most of the time is not change.  A paused game, a menu, a status bar
 * along the bottom, the whole picture between two footsteps - bytes nobody
 * needs twice.  A flush of the whole surface is what an application does when
 * it does not track its own damage; this turns that back into damage tracking
 * without the application knowing.  Measured: a full-surface flush of a still
 * picture costs 12 ms instead of 190.
 *
 * It used to be an actual copy of the frame - 150 KB of RGB565, compared pixel
 * by pixel.  It is a hash per 8x8 block now, which is 4800 bytes, and the
 * reason is that on this board those two numbers decide whether the game on the
 * other side of the driver starts at all: 150 KB is a quarter of what Fallout 2
 * was short of when it was refused its arena.  The screen driver was holding
 * three per cent of the machine to answer a question that does not need the
 * pixels themselves.
 *
 * What is lost with it: the bounding box is now in units of eight pixels rather
 * than one, and within a band it is the whole eight rows.  A cursor that moved
 * costs a couple of hundred pixels instead of a hundred.  What is gained
 * besides the memory: the comparison reads one buffer instead of two.
 *
 * A hash can collide, and then a block that changed is not sent.  Thirty-two
 * bits over sixty-four pixels puts that at about one block in four thousand
 * million; at twelve hundred blocks and thirty frames a second it is once every
 * few weeks, and what it costs is one stale 8x8 square for one frame.  Against
 * a fixed 150 KB that is a good trade.
 *
 * Zero means "not known", and the hash is never allowed to be zero.  That is
 * how a cold start, a reconnect and a partial blit all say the same thing
 * without a flag: what this driver does not know about, it sends.  The previous
 * version left the buffer uninitialised and called that "disagrees with the far
 * end", which is wrong twice over - PSRAM survives a warm reset and can come
 * back holding the last run's picture, and a buffer of zeros is a claim that
 * the far end is showing black, which is exactly what a game fading in from
 * black is about to send.  The symptom was a black screen with the menu
 * appearing only under the mouse pointer, and every counter said the link was
 * working perfectly.
 *
 * What is deliberately NOT done: coding the difference against the last frame.
 * That is what a video codec would do and it would win by a mile, but undoing a
 * difference needs the previous frame at the *receiving* end, and the far end
 * cannot hold one - 150 KB against a largest free block of about ninety is the
 * whole reason this arrangement exists.
 */
#define BLK 8u /* the block the hash covers; the same eight rows as a band */

static uint32_t *s_hash;
static uint32_t  s_hash_cols;
static uint32_t  s_hash_rows;
static uint32_t  s_surf_w;
static uint32_t  s_surf_h;

static void hash_forget(void)
{
    if (s_hash != NULL) {
        for (uint32_t i = 0; i < s_hash_cols * s_hash_rows; i++) {
            s_hash[i] = 0;
        }
    }
}

static bool prev_ready(uint32_t w, uint32_t h)
{
    if (s_hash != NULL && s_surf_w == w && s_surf_h == h) {
        return true;
    }
    ag_free(s_hash);
    s_hash_cols = (w + BLK - 1u) / BLK;
    s_hash_rows = (h + BLK - 1u) / BLK;
    s_hash = (uint32_t *)ag_malloc((size_t)s_hash_cols * s_hash_rows *
                                   sizeof(uint32_t));
    if (s_hash == NULL) {
        s_hash_cols = 0;
        s_hash_rows = 0;
        s_surf_w = 0;
        s_surf_h = 0;
        return false;
    }
    s_surf_w = w;
    s_surf_h = h;
    hash_forget();
    return true;
}

/*
 * FNV-1a over the block's pixels, and never zero, because zero is the word for
 * "not known".  Sixteen bits at a time rather than eight: half the rounds for
 * the same coverage, and the pixels are sixteen bits anyway.
 */
static uint32_t block_hash(const uint8_t *src, uint32_t stride, uint32_t rows,
                           uint32_t cols)
{
    uint32_t h = 2166136261u;

    for (uint32_t r = 0; r < rows; r++) {
        const uint16_t *px = (const uint16_t *)(src + (size_t)r * stride);
        for (uint32_t i = 0; i < cols; i++) {
            h ^= px[i];
            h *= 16777619u;
        }
    }
    return h != 0u ? h : 1u;
}

/*
 * One band row forgotten every so often, so the picture repairs itself.
 *
 * A lost band is remembered as delivered - the grid records what was *sent*,
 * and the link has no acknowledgement - so anything that goes missing on the
 * wire stays wrong until that part of the picture changes again.  On a game
 * that is usually soon; on a status bar or a menu it is never.  And things do
 * go missing: measured with the game running, 123 KB of arriving bytes in forty
 * seconds were thrown away by the receiver hunting for a frame header, because
 * writing a band to the panel takes long enough for the UART's buffer to
 * overflow and this link has no flow control.
 *
 * So it is the same answer the console path already uses two hundred lines
 * below, for the same reason: a slow sweep that re-sends what is probably
 * already right.  A quarter of a second per band row is one extra band in every
 * fifty at the frame rates seen here, and it puts a ceiling of about eight
 * seconds on how long any part of the picture can stay wrong.
 *
 * Driven from the rectangle that is going out rather than from a clock of its
 * own: the point of forgetting a row is that it gets sent again, and a row
 * nothing is sending is not sent again by forgetting it.  So while nothing is
 * drawing nothing is repaired - which is the case that FR_HELLO covers.
 */
/*
 * A quarter of a second was the first number and it is too slow to live with.
 * Thirty-two rows of blocks at that rate is an eight second ceiling on how
 * long a wrong rectangle stays on the glass, and eight seconds of dirt behind
 * a mouse pointer is something a person reports as a fault however carefully
 * it was reasoned about.  Sixty milliseconds is about two seconds for the
 * whole screen and costs one band - two and a half kilobytes encoded - per
 * sixty, which is under a tenth of the wire.
 */
#define HEAL_BAND_US 60000ull

static uint32_t s_heal_band;
static uint64_t s_heal_band_at;

static void heal_a_band(uint32_t y, uint32_t h)
{
    if (s_hash == NULL || s_hash_rows == 0) {
        return;
    }
    const uint64_t now = (uint64_t)ag_api()->time->us();
    if (now < s_heal_band_at) {
        return;
    }
    s_heal_band_at = now + HEAL_BAND_US;

    /*
     * A row of blocks inside the rectangle that is being sent right now, and
     * not a row chosen by a counter of its own.
     *
     * The point of forgetting a row is that it gets sent again.  A row this
     * blit does not cover is not sent again by this blit - it waits for a
     * flush that reaches it, and once the application above only reports the
     * pixels it really changed, that may be a long wait.  Forgetting a row
     * inside the current rectangle makes the repair immediate: the blocks read
     * as changed a few lines below and go out with everything else.
     *
     * Which leaves who covers the rest of the screen, and the answer is
     * whoever is drawing: Fallout's port walks a strip of eight rows down the
     * panel four times a second for exactly this reason, and the console path
     * flushes full width anyway.  A row therefore comes round about every
     * seven and a half seconds, which is the ceiling on how long any part of
     * the picture can be wrong.
     */
    const uint32_t first = y / BLK;
    if (first >= s_hash_rows) {
        return;
    }
    uint32_t last = (y + (h ? h : 1u) - 1u) / BLK;
    if (last >= s_hash_rows) {
        last = s_hash_rows - 1u;
    }
    const uint32_t span = (last >= first) ? (last - first + 1u) : 1u;
    const uint32_t r = first + (s_heal_band++ % span);
    if (r >= s_hash_rows) {
        return;
    }
    for (uint32_t c = 0; c < s_hash_cols; c++) {
        s_hash[(size_t)r * s_hash_cols + c] = 0;
    }
}

/*
 * The far end asking to be told everything again.
 *
 * The receiver is a monitor, and a monitor gets switched off, unplugged and
 * plugged back in while the machine driving it keeps running.  Nothing breaks
 * when that happens - this link has no flow control, so a write completes
 * whether anybody is listening or not - but coming back is the problem: the
 * shadow says the far end already has this frame, so on a screen where nothing
 * is moving nothing would ever be sent again and the glass would stay dark.
 *
 * So the receiver says one byte when it starts, and this forgets every block
 * hash.  "Not known" is already the word for a block that has to be sent, so
 * the reconnect needs no state and no code path of its own: it is the same
 * road as a cold start.
 *
 * Polled rather than waited for, with a zero timeout, from the driver's own
 * task, which comes round every two milliseconds whether or not anything is
 * drawing.
 *
 * It used to be polled from the two callbacks the kernel makes often, because
 * a driver could not own a task, and that left a case out: while an
 * application holds the display the kernel stops calling text_cursor and calls
 * blit_rect only when the application draws, so an application holding a
 * picture that never changes never read this and a receiver that reconnected
 * under one stayed dark.  Measured, not assumed - gfxdemo paints once and
 * waits, and a receiver started after it got nothing.  ABI 0.43 gave the
 * module a task, and the case went with it.
 */
/* Where the repair sweep has got to, and how many rows it still owes.
 * poll_far_end sends it back to the top.  See sweep_step, far below. */
static uint32_t s_sweep_row;
static uint64_t s_sweep_at;
static uint32_t s_sweep_owed = 0xffffffffu; /* everything, until told otherwise */

#define FR_HELLO 'H'

static void poll_far_end(void)
{
    uint8_t buf[16];
    for (;;) {
        const int32_t n = io->uart_read(LINK_PORT, buf, sizeof(buf), 0);
        if (n <= 0) {
            return;
        }
        for (int32_t i = 0; i < n; i++) {
            if (buf[i] == (uint8_t)FR_HELLO) {
                /* Not freed - only forgotten.  The table is fine; it is what
                 * it claims about the far end that has stopped being true.
                 * And the sweep starts from the top, so a receiver that has
                 * just appeared is given a whole picture instead of whatever
                 * the game happens to redraw next. */
                hash_forget();
                s_sweep_row = 0;
                s_sweep_at = 0;
                s_sweep_owed = 0xffffffffu;
            }
        }
        if ((uint32_t)n < sizeof(buf)) {
            return;
        }
    }
}

/*
 * Work space.  Static rather than automatic because these are kilobytes, and
 * the task that used to run this belonged to whoever flushed - an
 * application's stack is not the driver's to spend.  It is the driver's own
 * task now, whose stack is the driver's, but the size is the reason either way.
 *
 * Two callers can reach this: the wire task with a rectangle out of the
 * driver's frame, and blit_rect directly for a picture that frame cannot hold
 * (see the note above rem_blit_rect).  So it is locked - one rectangle at a
 * time, for as long as the rectangle takes.  It used to be the device registry
 * that provided that, held across blit_rect by the kernel, and relying on
 * somebody else's lock for one's own invariant is how this would have become a
 * bug the day either side changed.
 */
static ag_mutex_t s_send_lock;

static uint8_t  s_raw[FR_MAX_PAYLOAD];  /* geometry + pixels, uncompressed   */
static uint8_t  s_cand[FR_MAX_PAYLOAD]; /* geometry + whatever encoded best  */
static uint8_t  s_idx[BAND_MAX_PX];     /* one index per pixel, before packing */

/* The hashes of the blocks in the band being built, held until the band goes.
 * Wide enough for the widest surface this can carry - a band is at most 320
 * pixels, so forty blocks, and the margin is free. */
#define REM_MAX_COLS 64u
static uint8_t  s_fresh_col[REM_MAX_COLS];
static uint32_t s_fresh_hash[REM_MAX_COLS];
static uint16_t s_pal[256];

/*
 * A hash from colour to palette slot, so building a palette is one pass rather
 * than a search per pixel.  A linear scan would be up to 256 comparisons for
 * each of 2560 pixels, which is most of a millisecond per band - more than the
 * wire time it saves.  Open addressing, power-of-two table, no deletion: the
 * table is rebuilt for every band, and clearing it is what the generation
 * counter avoids.
 */
#define PAL_HASH 1024u
static uint16_t s_hcol[PAL_HASH];
static uint16_t s_hgen[PAL_HASH];
static uint8_t  s_hidx[PAL_HASH];
static uint16_t s_gen;

/*
 * PackBits over 16-bit pixels: a control byte, then either that many literal
 * pixels or that many copies of one.
 *
 *   0x00..0x7F   (n + 1) literals    1..128
 *   0x80..0xFF   (n - 0x80 + 2) copies of the next pixel   2..129
 *
 * One pass, no memory, and good at what game art is made of: flat panels, black
 * borders, skies.  Worst case one byte per 128 pixels, under half a percent -
 * and the caller sends something else if this does not come out smaller.
 */
static uint32_t pack16(const uint16_t *px, uint32_t n, uint8_t *out,
                       uint32_t cap)
{
    uint32_t at = 0;
    uint32_t i = 0;

    while (i < n) {
        uint32_t run = 1;
        while (run < 129u && i + run < n && px[i + run] == px[i]) {
            run++;
        }
        if (run >= 2u) {
            if (at + 3u > cap) {
                return 0;
            }
            out[at++] = (uint8_t)(0x80u + (run - 2u));
            put16(out + at, px[i]);
            at += 2u;
            i += run;
            continue;
        }
        uint32_t lit = 1;
        while (lit < 128u && i + lit + 1u < n &&
               px[i + lit] != px[i + lit + 1u]) {
            lit++;
        }
        if (i + lit > n) {
            lit = n - i;
        }
        if (at + 1u + lit * 2u > cap) {
            return 0;
        }
        out[at++] = (uint8_t)(lit - 1u);
        for (uint32_t k = 0; k < lit; k++) {
            put16(out + at, px[i + k]);
            at += 2u;
        }
        i += lit;
    }
    return at;
}

/* The same shape over bytes, for the index stream. */
static uint32_t pack8(const uint8_t *b, uint32_t n, uint8_t *out, uint32_t cap)
{
    uint32_t at = 0;
    uint32_t i = 0;

    while (i < n) {
        uint32_t run = 1;
        while (run < 129u && i + run < n && b[i + run] == b[i]) {
            run++;
        }
        if (run >= 3u) { /* below three a literal is no worse and often better */
            if (at + 2u > cap) {
                return 0;
            }
            out[at++] = (uint8_t)(0x80u + (run - 2u));
            out[at++] = b[i];
            i += run;
            continue;
        }
        uint32_t lit = 1;
        while (lit < 128u && i + lit + 2u < n &&
               !(b[i + lit] == b[i + lit + 1u] &&
                 b[i + lit] == b[i + lit + 2u])) {
            lit++;
        }
        if (i + lit > n) {
            lit = n - i;
        }
        if (at + 1u + lit > cap) {
            return 0;
        }
        out[at++] = (uint8_t)(lit - 1u);
        for (uint32_t k = 0; k < lit; k++) {
            out[at++] = b[i + k];
        }
        i += lit;
    }
    return at;
}

/*
 * Palette the band, and this is the encoding that fits the content rather than
 * the wire.  Fallout is an eight-bit game: the whole frame has at most 256
 * colours and a strip 320 by 8 has far fewer, because it is one part of one
 * scene.  Sixteen colours or fewer is four bits a pixel - a straight four to
 * one before any run is looked at - and the runs in an index stream are longer
 * than the runs in a stream of colour pairs, so the two multiply.
 *
 * Returns the encoded length after the geometry, or 0 when the band has more
 * than 256 colours (a photograph, a gradient) and this is the wrong tool.
 *
 * Layout: bpp, count-1, the palette, then pack8 over the packed indices.
 */
static uint32_t encode_indexed(const uint16_t *px, uint32_t n, uint8_t *out,
                               uint32_t cap)
{
    if (++s_gen == 0) {
        /* Wrapped: every stale mark now reads as current, so start over. */
        for (uint32_t i = 0; i < PAL_HASH; i++) {
            s_hgen[i] = 0;
        }
        s_gen = 1;
    }

    /*
     * One-entry cache in front of the hash, and it is not a micro-optimisation.
     * The whole reason this encoding wins on this content is that the same
     * colour repeats - and a repeat used to cost a multiply, a mask and a probe
     * each time.  On a measured full frame the encoder was thirteen
     * milliseconds against thirty-seven of wire, so it was the second largest
     * cost in the picture.
     */
    uint16_t last_c = px[0];
    uint8_t  last_i = 0;
    bool     have_last = false;

    uint32_t ncol = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint16_t c = px[i];
        if (have_last && c == last_c) {
            s_idx[i] = last_i;
            continue;
        }
        uint32_t slot = ((uint32_t)c * 2654435761u) >> 20 & (PAL_HASH - 1u);
        for (;;) {
            if (s_hgen[slot] != s_gen) {
                if (ncol == 256u) {
                    return 0; /* too many colours for a byte of index */
                }
                s_hgen[slot] = s_gen;
                s_hcol[slot] = c;
                s_hidx[slot] = (uint8_t)ncol;
                s_pal[ncol] = c;
                s_idx[i] = (uint8_t)ncol;
                last_c = c;
                last_i = (uint8_t)ncol;
                have_last = true;
                ncol++;
                break;
            }
            if (s_hcol[slot] == c) {
                s_idx[i] = s_hidx[slot];
                last_c = c;
                last_i = s_hidx[slot];
                have_last = true;
                break;
            }
            slot = (slot + 1u) & (PAL_HASH - 1u);
        }
    }

    const uint32_t bpp = (ncol <= 16u) ? 4u : 8u;
    uint32_t       at = 0;

    if (2u + ncol * 2u > cap) {
        return 0;
    }
    out[at++] = (uint8_t)bpp;
    out[at++] = (uint8_t)(ncol - 1u);
    for (uint32_t i = 0; i < ncol; i++) {
        put16(out + at, s_pal[i]);
        at += 2u;
    }

    /* Pack the indices down to bpp, in place in s_idx. */
    uint32_t bytes = n;
    if (bpp == 4u) {
        bytes = (n + 1u) / 2u;
        for (uint32_t i = 0; i < bytes; i++) {
            const uint8_t lo = s_idx[i * 2u];
            const uint8_t hi = (i * 2u + 1u < n) ? s_idx[i * 2u + 1u] : 0u;
            s_idx[i] = (uint8_t)((hi << 4) | (lo & 0x0fu));
        }
    }

    const uint32_t packed = pack8(s_idx, bytes, out + at, cap - at);
    if (packed == 0) {
        return 0;
    }
    return at + packed;
}

/*
 * A rectangle of the picture, in bands, trimmed and encoded.
 *
 * Called on the driver's own task, from the damage rem_blit_rect left behind -
 * or, for a picture the driver's frame cannot hold, on whoever presented it.
 * It blocks for as long as the wire needs, which is why it is not the game's
 * thread any more.  What happens here is the wire being asked to carry less, in
 * three steps that multiply:
 *
 *   1. The band is compared with the shadow and reduced to the bounding box of
 *      what actually moved.  A caret, a line of text, a walking figure - each
 *      becomes a narrow rectangle instead of the full width, and a band that did
 *      not move at all is not sent.  A box rather than a per-pixel list because
 *      the far end draws by setting a window, and one window that is slightly
 *      too big beats four that are exact.
 *   2. Whichever of three encodings comes out smallest is used: raw, PackBits
 *      over the pixels, or palette-and-indices.
 *   3. Bands stay the transport unit, eight rows at a time, because on the far
 *      end every band is one window-setting on the panel.
 */
/* Defined with the sound, below; the picture path pumps it. */
static void aud_drain(void);

static void send_rect_locked(const ag_blit_t *b);

static void send_rect(const ag_blit_t *b)
{
    if (b == NULL || b->px == NULL || b->w == 0 || b->h == 0) {
        return;
    }
    /* What will not fit in a frame is refused rather than truncated: a band
     * drawn from half its pixels is worse than a band not drawn. */
    if ((uint32_t)b->w * BAND_ROWS > BAND_MAX_PX) {
        return;
    }
    if (s_send_lock != NULL && !ag_mutex_lock(s_send_lock, 2000u)) {
        return;
    }
    send_rect_locked(b);
    if (s_send_lock != NULL) {
        ag_mutex_unlock(s_send_lock);
    }
}

static void send_rect_locked(const ag_blit_t *b)
{

    const uint8_t *src = (const uint8_t *)b->px;
    /*
     * The hash grid is in the surface's own coordinates and its blocks are
     * eight pixels square, so a band can be compared against it only when it
     * lines up - on a block boundary in both directions.  It does not have to
     * span the surface: a rectangle that starts on a block boundary covers a
     * run of grid columns and those are the ones it updates.
     *
     * It used to demand the full width, and that was fine while the only
     * things driving this wire handed over the whole picture.  Then Fallout's
     * blit learned to report the pixels it really changed, and every flush
     * became a narrow rectangle - which took this branch, was sent whole, and
     * made the rows it touched forget what the far end had.  Sixteen kilobytes
     * a frame went down the wire where the grid had been sending almost
     * nothing, and the frame rate fell from 21 to 14 for a change that was
     * supposed to cut the traffic by ninety per cent.  It did cut it; it also
     * turned the trimming off.
     *
     * A blit that still does not line up is sent whole and made to forget the
     * rows it touched.  That is the conservative direction: sending too much
     * is slow, and trimming against a grid that was only partly filled is a
     * screen with holes in it that nothing ever repairs.
     */
    const uint32_t bx = (uint32_t)b->x;
    const uint32_t gx0 = bx / BLK;
    const uint32_t gcols = ((uint32_t)b->w + BLK - 1u) / BLK;
    const bool aligned = s_trim && prev_ready(b->surf_w, b->surf_h) &&
                         (bx % BLK) == 0u && ((uint32_t)b->y % BLK) == 0u &&
                         gx0 + gcols <= s_hash_cols;

    for (uint32_t band = 0; band < b->h; band += BAND_ROWS) {
        uint32_t rows = b->h - band;
        if (rows > BAND_ROWS) {
            rows = BAND_ROWS;
        }

        /*
         * A short band cannot be hashed, and this is not a nicety.
         *
         * The hash of a block row is stored as if it described all eight rows,
         * so a band with fewer - the last one of a blit whose height is not a
         * multiple of eight - would record a hash taken over four rows and then
         * hide any later change in the other four.  What that looks like is a
         * rectangle of the previous frame that never goes away, and the obvious
         * place to see one is behind a moving mouse pointer, because a pointer
         * is exactly the thing that makes small blits of arbitrary height.
         */
        const bool track = aligned && rows == BAND_ROWS;

        uint32_t  x0 = 0, x1 = (uint32_t)b->w - 1u;
        const uint32_t y0 = 0, y1 = rows - 1u;
        uint32_t *row = NULL;
        uint32_t  nfresh = 0;

        if (track) {
            /*
             * Which blocks of this band differ from what the far end has, as a
             * bounding box in the band's own coordinates.  Start inverted so
             * that "nothing moved" is detectable.
             *
             * Vertically there is nothing to trim: a band is exactly one row of
             * blocks, so any change in it costs all eight rows.  That is the
             * price of the hash and it is a couple of hundred pixels.
             */
            row = s_hash + (size_t)(((uint32_t)b->y + band) / BLK) *
                               s_hash_cols + gx0;
            x0 = (uint32_t)b->w;
            x1 = 0;
            nfresh = 0;
            for (uint32_t c = 0; c < gcols; c++) {
                uint32_t cols = (uint32_t)b->w - c * BLK;
                if (cols > BLK) {
                    cols = BLK;
                }
                const uint32_t h =
                    block_hash(src + (size_t)band * b->stride + c * BLK * 2u,
                               b->stride, rows, cols);
                if (h != row[c]) {
                    /* Remembered, not written.  It is written when the band it
                     * belongs to has actually gone down the wire - see
                     * send_frame. */
                    if (nfresh < REM_MAX_COLS) {
                        s_fresh_col[nfresh] = (uint8_t)c;
                        s_fresh_hash[nfresh] = h;
                        nfresh++;
                    }
                    if (c * BLK < x0) {
                        x0 = c * BLK;
                    }
                    if (c * BLK + cols - 1u > x1) {
                        x1 = c * BLK + cols - 1u;
                    }
                }
            }
            if (x0 > x1) {
                continue; /* the far end already has this band */
            }
        } else if (s_hash != NULL && (uint32_t)b->y + band < s_surf_h) {
            /* Sent whole, and the grid told so: these rows are no longer
             * described by what is in it. */
            const uint32_t first = ((uint32_t)b->y + band) / BLK;
            const uint32_t last = ((uint32_t)b->y + band + rows - 1u) / BLK;
            for (uint32_t r = first; r <= last && r < s_hash_rows; r++) {
                for (uint32_t c = 0; c < s_hash_cols; c++) {
                    s_hash[(size_t)r * s_hash_cols + c] = 0;
                }
            }
        }

        const uint32_t rw = x1 - x0 + 1u;
        const uint32_t rh = y1 - y0 + 1u;
        const uint32_t npx = rw * rh;

        /* Gather the rectangle. */
        uint16_t *out = (uint16_t *)(s_raw + 12u);
        for (uint32_t r = 0; r < rh; r++) {
            const uint16_t *want =
                (const uint16_t *)(src + (size_t)(band + y0 + r) * b->stride) +
                x0;
            for (uint32_t i = 0; i < rw; i++) {
                out[r * rw + i] = want[i];
            }
        }

        uint8_t geom[12];
        put16(geom + 0, (uint32_t)b->x + x0);
        put16(geom + 2, (uint32_t)b->y + band + y0);
        put16(geom + 4, rw);
        put16(geom + 6, rh);
        put16(geom + 8, b->surf_w);
        put16(geom + 10, b->surf_h);
        for (uint32_t i = 0; i < 12u; i++) {
            s_raw[i] = geom[i];
        }

        const uint32_t raw = npx * 2u;
        uint32_t       best = raw;
        uint8_t        best_op = FR_BAND;
        uint32_t       best_len = 0;

        const uint32_t p =
            s_encode ? pack16(out, npx, s_cand + 12u, FR_MAX_PAYLOAD - 12u) : 0u;
        if (p != 0 && p < best) {
            best = p;
            best_op = FR_PACK;
            best_len = p;
        }
        const uint32_t q =
            s_encode ? encode_indexed(out, npx,
                                      s_cand + 12u + (best_op == FR_PACK ? p : 0u),
                                      FR_MAX_PAYLOAD - 12u -
                                          (best_op == FR_PACK ? p : 0u))
                     : 0u;
        if (q != 0 && q < best) {
            /* It won; move it to the front of the candidate buffer. */
            uint8_t *from = s_cand + 12u + p;
            for (uint32_t i = 0; i < q; i++) {
                s_cand[12u + i] = from[i];
            }
            best = q;
            best_op = FR_IDX;
            best_len = q;
        }

        bool went;
        if (best_op == FR_BAND) {
            went = send_frame(FR_BAND, s_raw, 12u + raw);
        } else {
            for (uint32_t i = 0; i < 12u; i++) {
                s_cand[i] = geom[i];
            }
            went = send_frame(best_op, s_cand, 12u + best_len);
        }

        /* Only now is it true that the far end has these blocks.  A band that
         * did not go leaves its hashes as they were, so the next flush that
         * reaches these rows sends it again. */
        if (went && row != NULL) {
            for (uint32_t i = 0; i < nfresh; i++) {
                row[s_fresh_col[i]] = s_fresh_hash[i];
            }
        }
    }

}

/* ---- the wire is not the caller's to wait for -------------------------- */
/*
 * Everything above is what has to happen to a rectangle before it can go down a
 * serial line, and it is not fast: hashing, trimming, three encodings tried
 * against each other, and then the write itself - a band is two and a half
 * kilobytes at four megabaud, six milliseconds, and a full screen is thirty
 * bands.  Until now all of it happened on whichever task called blit_rect,
 * which is the task that drew the frame.  A game therefore ran at the speed of
 * the wire: measured, three hundred milliseconds a frame with the whole picture
 * moving, and the game had no idea it was talking to a wire at all.
 *
 * It should not have to know.  A screen at the end of a cable is still a
 * screen, and what an application does with one is put pixels in it.
 *
 * So blit_rect now does the one thing that has to happen while the caller's
 * pixels are still there - copy them - and marks the rectangle.  A task of the
 * driver's own (ABI 0.43) picks the damage up and does the rest.  The copy is
 * the price: a quarter of the screen is 38 KB and about two milliseconds of
 * PSRAM, against six milliseconds for one band of the thirty that a frame used
 * to cost the caller.
 *
 * The frame belongs to the driver for a second reason, which is correctness.
 * ag_blit_t's pixels belong to the caller, and when they come from
 * gfx->present they are an application's own memory, which the application is
 * free to reuse the instant blit_rect returns.  Reading them later from another
 * task would be reading whatever is there by then.
 *
 * No lock around the copy, deliberately.  The sender may read a rectangle that
 * is being written under it and send half of one frame and half of the next -
 * which is what a video link does when it is not synchronised to the source,
 * and it repairs itself: the hash it records is the hash of what it read, so
 * the next change to those blocks reads as a change and goes out again.  A lock
 * would put the caller back to waiting for the wire, which is the whole thing
 * being fixed.
 */
static uint16_t  *s_fb;      /* the driver's own copy of the picture       */
static uint32_t   s_fb_w, s_fb_h;
static ag_mutex_t s_dmg_lock;
static uint32_t   s_dmg_x0, s_dmg_y0, s_dmg_x1, s_dmg_y1; /* x1/y1 exclusive */
static bool       s_dmg;
static volatile bool s_stop;
static uint32_t   s_late;    /* blits that went the old way, for `dev`      */

/* Waited for without a limit, for the same reason as the wire lock: what it
 * guards is a buffer, and half of one is not a picture. */
static bool dmg_lock(void) { return lock_take(s_dmg_lock, UINT32_MAX); }

static void dmg_unlock(bool held) { lock_give(s_dmg_lock, held); }

/*
 * Snapped outward to the block grid, because the trimming above only runs on a
 * rectangle that lines up with it - see `aligned` in send_rect.  A rectangle
 * that does not is sent whole and makes those rows forget, which costs far more
 * than the few pixels of rounding.
 */
static void dmg_add_locked(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t x0 = (x / BLK) * BLK;
    uint32_t y0 = (y / BLK) * BLK;
    uint32_t x1 = ((x + w + BLK - 1u) / BLK) * BLK;
    uint32_t y1 = ((y + h + BLK - 1u) / BLK) * BLK;

    if (x1 > s_fb_w) {
        x1 = s_fb_w;
    }
    if (y1 > s_fb_h) {
        y1 = s_fb_h;
    }
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    if (!s_dmg) {
        s_dmg_x0 = x0;
        s_dmg_y0 = y0;
        s_dmg_x1 = x1;
        s_dmg_y1 = y1;
        s_dmg = true;
    } else {
        if (x0 < s_dmg_x0) {
            s_dmg_x0 = x0;
        }
        if (y0 < s_dmg_y0) {
            s_dmg_y0 = y0;
        }
        if (x1 > s_dmg_x1) {
            s_dmg_x1 = x1;
        }
        if (y1 > s_dmg_y1) {
            s_dmg_y1 = y1;
        }
    }
}

/* True and the rectangle, or false and nothing to do. */
static bool dmg_take(uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h)
{
    bool got = false;

    const bool held = dmg_lock();
    if (s_dmg) {
        *x = s_dmg_x0;
        *y = s_dmg_y0;
        *w = s_dmg_x1 - s_dmg_x0;
        *h = s_dmg_y1 - s_dmg_y0;
        s_dmg = false;
        got = true;
    }
    dmg_unlock(held);
    return got;
}

static void rem_blit_rect(ag_handle_t h, const ag_blit_t *b)
{
    (void)h;
    if (b == NULL || b->px == NULL || b->w == 0 || b->h == 0) {
        return;
    }

    /*
     * The damage lock covers the copy as well as the rectangle, and it is not
     * the wire it is protecting against - the sender holds it for the few
     * instructions it takes to pick the rectangle up, so nothing here ever
     * waits on it.  What it settles is who may free the frame: the driver can
     * be unloaded while an application is drawing, and a memcpy into memory
     * that has just been handed back is the kind of fault that shows up
     * somewhere else entirely.  See rem_stop.
     */
    const bool held = dmg_lock();

    /*
     * The old way, for the two cases the frame cannot serve: no frame at all
     * (the allocation failed at boot, or the driver is going away), or a
     * picture of a different size - gfx->present hands over an application's
     * own surface, which is whatever shape that application wanted.  Slower for
     * the caller, and correct.
     */
    if (s_fb == NULL || b->surf_w != s_fb_w || b->surf_h != s_fb_h ||
        (uint32_t)b->x + b->w > s_fb_w || (uint32_t)b->y + b->h > s_fb_h) {
        dmg_unlock(held);
        s_late++;
        poll_far_end();
        heal_a_band((uint32_t)b->y, b->h);
        send_rect(b);
        aud_drain();
        return;
    }

    const uint8_t *src = (const uint8_t *)b->px;
    const size_t   row_bytes = (size_t)b->w * sizeof(uint16_t);
    for (uint32_t r = 0; r < b->h; r++) {
        memcpy(s_fb + (size_t)((uint32_t)b->y + r) * s_fb_w + b->x,
               src + (size_t)r * b->stride, row_bytes);
    }

    dmg_add_locked((uint32_t)b->x, (uint32_t)b->y, b->w, b->h);
    dmg_unlock(held);
}

/*
 * The task.  One rectangle at a time, and the sound between them.
 *
 * Two milliseconds of sleep when there is nothing, which is a fortieth of the
 * frame rate this link can carry and cheap enough not to matter; the wire is
 * the bottleneck, not the polling.  Nothing is notified because nothing needs
 * to be: a screen is a thing that is looked at, not waited on.
 */
static volatile bool s_wire_running;

/*
 * A sweep that sends what nobody asked for.
 *
 * Everything above only ever puts on the wire what the application redrew.
 * That is right for traffic and wrong for a screen: the parts of the picture
 * the game does not touch are never sent, so whatever the far end had before
 * stays on the glass - and on a map where only a lamp and a critter animate,
 * that is most of it.  What Maxim saw was exactly this: an old frozen picture
 * with a few rectangles moving inside it, filling in over ten seconds as the
 * repair crept through.
 *
 * The old repair could not fix it, because it forgot a row of blocks *inside
 * the rectangle being sent* - a row nothing is sending is not sent again by
 * forgetting it.  That was the best a driver with no task of its own could do.
 * With a task and a frame of its own, the honest thing is available: walk the
 * screen, one row of blocks at a time, and send what is there.
 *
 * One row per turn, and only when nothing is waiting - real damage always goes
 * first, so this costs nothing while the picture is moving.  Thirty rows on
 * this panel, so a screen nobody is drawing on is made right in well under a
 * second, and a row is two and a half kilobytes encoded against a wire that
 * carries four hundred.
 *
 * The hashes for the row are forgotten before it is sent, or the band would be
 * compared against what the far end is *believed* to have and skipped - which
 * is the belief being repaired.
 */
/*
 * Two speeds, because the sweep is answering two different questions.
 *
 * "What has this receiver got?" - nothing, when it has just said hello or the
 * driver has just loaded.  Then the whole screen is owed and it should go as
 * fast as it can: thirty rows at one every twenty milliseconds is six tenths of
 * a second, and until it has been paid the far end is showing somebody else's
 * picture.
 *
 * "Has anything been lost since?" - almost never, and asking costs the same as
 * answering, because a row can only be checked by sending it.  So once the debt
 * is paid this drops to a row every half second: a whole screen every fifteen,
 * a twentieth of the wire, and nothing anybody notices.
 *
 * Running it fast all the time was measured and is not free: fifty rows a
 * second is seven milliseconds of encoding each, and the game fell from 45 fps
 * to 25 while the far end gained a refresh nobody needed.
 */
#define SWEEP_FAST_US 20000ull
#define SWEEP_SLOW_US 500000ull

static bool sweep_step(uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h)
{
    if (s_hash == NULL || s_hash_rows == 0u || s_fb_w == 0u) {
        return false;
    }
    const uint64_t now = (uint64_t)ag_api()->time->us();
    if (now < s_sweep_at) {
        return false;
    }
    s_sweep_at = now + ((s_sweep_owed != 0u) ? SWEEP_FAST_US : SWEEP_SLOW_US);

    const uint32_t r = s_sweep_row % s_hash_rows;
    s_sweep_row++;
    if (s_sweep_owed == 0xffffffffu) {
        s_sweep_owed = s_hash_rows; /* the whole screen, now that its size is known */
    }
    if (s_sweep_owed != 0u) {
        s_sweep_owed--;
    }
    for (uint32_t c = 0; c < s_hash_cols; c++) {
        s_hash[(size_t)r * s_hash_cols + c] = 0;
    }
    *x = 0;
    *y = r * BLK;
    *w = s_fb_w;
    *h = BLK;
    if (*y + *h > s_fb_h) {
        *h = s_fb_h - *y;
    }
    return *h != 0u;
}

/*
 * What the wire did with the last five seconds, printed from the task itself.
 *
 * Not from a callback - printing from one is the deadlock the header warns
 * about - and not while any lock is held.  It exists because "the picture is
 * slow" is four different faults with one appearance: too much asked for, too
 * little compressed away, a wire that cannot carry it, or a sender that never
 * catches up.  One line separates them, and without it the argument is about
 * impressions.
 *
 * Silent when nothing is drawing, so an idle board stays quiet.
 */
#define STAT_US 5000000ull

static uint32_t s_win_rects;
static uint64_t s_win_px;
static uint64_t s_win_wire;
static uint32_t s_win_worst_us;
static uint32_t s_win_behind;

static void wire_stats(void)
{
    static uint64_t at;
    const uint64_t  now = (uint64_t)ag_api()->time->us();

    if (at == 0ull) {
        at = now + STAT_US;
        return;
    }
    if (now < at) {
        return;
    }
    at = now + STAT_US;
    if (s_win_rects == 0u) {
        return;
    }

    const uint64_t secs = STAT_US / 1000000ull;
    const uint32_t px_kb = (uint32_t)(s_win_px * 2ull / 1024ull / secs);
    const uint32_t wire_kb = (uint32_t)(s_win_wire / 1024ull / secs);

    /*
     * Screens a second, not rectangles a second.  A rectangle is whatever part
     * of the picture moved, so "thirteen a second" says nothing at all about
     * how often the screen is right - which is the only question the person
     * looking at the glass is asking.  Pixels sent over pixels there are.
     */
    const uint32_t full = ((uint32_t)s_fb_w * s_fb_h != 0u)
                              ? (uint32_t)(s_win_px * 100ull /
                                           ((uint64_t)s_fb_w * s_fb_h) / secs)
                              : 0u;

    ag_printf("REMDISP: %u.%02u screens/s (%u rect/s), %u KB/s of pixels -> "
              "%u KB/s on the wire (%u%%), worst rect %u ms, behind %u\n",
              (unsigned)(full / 100u), (unsigned)(full % 100u),
              (unsigned)(s_win_rects / (uint32_t)secs), (unsigned)px_kb,
              (unsigned)wire_kb,
              (unsigned)((px_kb != 0u) ? (wire_kb * 100u / px_kb) : 0u),
              (unsigned)(s_win_worst_us / 1000u), (unsigned)s_win_behind);

    s_win_rects = 0;
    s_win_px = 0;
    s_win_wire = 0;
    s_win_worst_us = 0;
    s_win_behind = 0;
}

static void wire_task(void *arg)
{
    (void)arg;
    s_wire_running = true;
    while (!s_stop) {
        poll_far_end();
        wire_stats();

        uint32_t x = 0, y = 0, w = 0, h = 0;
        bool     repair = false;
        if (!dmg_take(&x, &y, &w, &h)) {
            /* Nothing was asked for; put a row of the picture on the wire
             * anyway.  See sweep_step. */
            repair = sweep_step(&x, &y, &w, &h);
            if (!repair) {
                aud_drain();
                ag_delay(2);
                continue;
            }
        }

        /* Read once: rem_stop takes the pointer away while this task may be
         * between the two, and it does not give the memory back until this
         * task has said it has finished the round it is in. */
        uint16_t *fb = s_fb;
        if (fb == NULL) {
            continue;
        }

        if (!repair) {
            heal_a_band(y, h);
        }

        /*
         * Bands, and not the whole rectangle in one call, because send_rect
         * walks it in bands anyway - but the damage is taken in one piece so
         * that a frame drawn in twenty small pieces goes out as one sweep down
         * the screen rather than twenty.
         */
        const ag_blit_t b = {
            .px = fb + (size_t)y * s_fb_w + x,
            .stride = s_fb_w * sizeof(uint16_t),
            .surf_w = (uint16_t)s_fb_w,
            .surf_h = (uint16_t)s_fb_h,
            .x = (uint16_t)x,
            .y = (uint16_t)y,
            .w = (uint16_t)w,
            .h = (uint16_t)h,
        };
        const uint64_t t0 = (uint64_t)ag_api()->time->us();
        const uint64_t wire0 = s_sent_bytes;
        send_rect(&b);
        const uint32_t took = (uint32_t)((uint64_t)ag_api()->time->us() - t0);

        s_win_rects++;
        s_win_px += (uint64_t)w * h;
        s_win_wire += s_sent_bytes - wire0;
        if (took > s_win_worst_us) {
            s_win_worst_us = took;
        }
        /* Damage that arrived while this one was going out: the sender did not
         * catch up, and what is on the glass is already old. */
        if (s_dmg) {
            s_win_behind++;
        }

        aud_drain();
    }
    s_wire_running = false;
}

/*
 * Unload, in the one order that is safe.
 *
 * The kernel calls this while the image is still mapped and then waits for the
 * task; what has to be settled before it frees the frame is that nobody is
 * inside it.  Two users: the sender, which stops when it sees the flag, and
 * blit_rect, which copies into the frame under the damage lock.  So the pointer
 * is taken away under that lock - after which every blit takes the old path -
 * and only then is the memory given back, once the sender has said it has
 * gone.
 *
 * Not freeing it would be the other choice, and it is worse than it looks: a
 * driver replaced twice would be holding three hundred kilobytes of a board
 * that has eight megabytes and a game that wants six.
 */
static void rem_stop(void)
{
    s_stop = true;

    const bool held = dmg_lock();
    uint16_t *fb = s_fb;
    s_fb = NULL;
    s_fb_w = 0;
    s_fb_h = 0;
    s_dmg = false;
    dmg_unlock(held);

    for (int i = 0; i < 200 && s_wire_running; i++) {
        ag_delay(5);
    }
    if (!s_wire_running) {
        ag_free(fb);

        /*
         * And the grid, which nothing has ever given back: it is five
         * kilobytes, and a driver loaded and replaced a dozen times over an
         * afternoon of `drv install` had leaked sixty by the end.  Same
         * ordering as the frame - after the sender has gone, because the
         * sender is what reads it.
         */
        ag_free(s_hash);
        s_hash = NULL;
        s_hash_cols = 0;
        s_hash_rows = 0;
        s_surf_w = 0;
        s_surf_h = 0;
    }
}

/* ---- characters -------------------------------------------------------- */

/*
 * A copy of what was last sent, and a slow sweep that sends it again.
 *
 * The kernel only offers the rows it thinks changed, and it repaints in full
 * exactly once - the first time it sees a panel.  Down a wire that is two
 * assumptions too many.  The far end may have started listening after that one
 * repaint; a frame may be lost; and the far end's own console is a real console
 * that prints its own log lines over the picture.  Each of those leaves the two
 * screens disagreeing about a row that, from the kernel's side, has not changed
 * - so nothing would ever correct it.
 *
 * Two kilobytes of shadow and about a kilobyte a second of a four-hundred-
 * kilobyte wire buys a console that repairs itself within a couple of seconds
 * of anything going wrong, and it removes the "start the receiver first" rule
 * the first version needed.
 */
#define HEAL_ROWS_PER_SWEEP 5u
#define HEAL_INTERVAL_US    250000ull

static ag_textcell_t s_shadow[REM_ROWS][REM_COLS];
static uint8_t       s_shadow_cols[REM_ROWS];
static uint32_t      s_heal_next;
static uint64_t      s_heal_at;

static ag_err_t rem_text_info(ag_handle_t h, uint16_t *cols, uint16_t *rows)
{
    (void)h;
    if (cols != NULL) {
        *cols = (uint16_t)REM_COLS;
    }
    if (rows != NULL) {
        *rows = (uint16_t)REM_ROWS;
    }
    return AG_OK;
}

static void send_row(uint16_t row)
{
    const uint32_t count = s_shadow_cols[row];
    if (count == 0) {
        return;
    }
    uint8_t payload[2u + REM_COLS * 2u];
    payload[0] = (uint8_t)row;
    payload[1] = (uint8_t)count;
    for (uint32_t x = 0; x < count; x++) {
        payload[2u + x * 2u] = s_shadow[row][x].ch;
        payload[3u + x * 2u] = s_shadow[row][x].attr;
    }
    send_frame(FR_ROW, payload, 2u + count * 2u);
}

static void rem_text_row(ag_handle_t h, uint16_t row,
                         const ag_textcell_t *cells, uint16_t count)
{
    (void)h;
    if (cells == NULL || row >= REM_ROWS) {
        return;
    }
    if (count > REM_COLS) {
        count = (uint16_t)REM_COLS;
    }
    for (uint16_t x = 0; x < count; x++) {
        s_shadow[row][x] = cells[x];
    }
    s_shadow_cols[row] = (uint8_t)count;
    send_row(row);
}

/*
 * The repair sweep, driven from text_cursor because that is the one callback
 * the kernel makes when nothing has changed: the caret blinks about twice a
 * second for as long as the machine is alive, which is the heartbeat this needs
 * and exactly when there is spare wire to use.  It stops while an application
 * holds the display, because then the kernel stops calling text_cursor at all -
 * which is correct: repainting console rows over somebody's picture is the last
 * thing wanted there.
 */
static void heal_a_few(void)
{
    const uint64_t now = (uint64_t)ag_api()->time->us();
    if (now < s_heal_at) {
        return;
    }
    s_heal_at = now + HEAL_INTERVAL_US;
    poll_far_end();

    for (uint32_t i = 0; i < HEAL_ROWS_PER_SWEEP; i++) {
        send_row((uint16_t)s_heal_next);
        s_heal_next = (s_heal_next + 1u) % REM_ROWS;
    }
}

static void rem_text_cursor(ag_handle_t h, uint16_t col, uint16_t row,
                            ag_textcell_t under, bool visible)
{
    (void)h;
    if (col >= REM_COLS || row >= REM_ROWS) {
        return;
    }
    const uint8_t payload[5] = {
        (uint8_t)col, (uint8_t)row, under.ch, under.attr,
        (uint8_t)(visible ? 1u : 0u),
    };
    send_frame(FR_CUR, payload, sizeof(payload));

    /* The cell under the caret is now what the far end has there, and the
     * shadow must agree or the next sweep would put the old character back. */
    if (col < s_shadow_cols[row]) {
        s_shadow[row][col] = under;
    }
    heal_a_few();
    /* Sound gets its turn on the console path too, so it keeps flowing when the
     * far end is showing text rather than a picture. */
    aud_drain();
}

/* ---- the device -------------------------------------------------------- */

/*
 * Geometry without taking the display.  There is no surface here and nothing to
 * hand out, so out->fb stays NULL and acquire is absent - which is what tells
 * the kernel this panel is fed rather than drawn into.  The size reported is
 * the far end's glass, because that is what a caller placing a picture needs to
 * know, and it is not the same as the console size above.
 */
/* ---- sound -------------------------------------------------------------- */
/*
 * The same wire, one more opcode, and the far end becomes a sound card as well
 * as a screen.  It has to be the same wire: the CYD breaks out three pins and
 * two of them are already this link.
 *
 * Why this lives inside the display driver rather than in a .SYS of its own,
 * which is where it belongs by subject: two modules writing one UART interleave
 * their frames and neither arrives.  One module owning the port is the whole of
 * the arrangement, and the kernel makes it free - blit_rect and a device write
 * are both called with the device registry held, so the two paths cannot be
 * inside send_frame at once and the shared frame buffer needs no lock.  That is
 * also the cost: a band of pixels is up to 13 ms of blocking write, and sound
 * queued behind one waits that long.  The far end's converter holds 93 ms, which
 * is what covers it.
 *
 * Eight-bit mono on the wire, converted here rather than there.  Not a
 * compromise for bandwidth - 88 KB/s of 22 kHz stereo would fit in 400 - but
 * because that is what the far end plays: its output is one DAC pin into a
 * small speaker, and sending sixteen-bit stereo would be sending three bytes
 * out of four to be thrown away by the receiver's own conversion.  22 KB/s is
 * five percent of the link, which is what leaves the picture alone.
 *
 * No pacing here, deliberately.  A sink that accepts everything instantly makes
 * the program above it run its mixer as fast as the processor allows (the
 * lesson written up over pcmnull's clock in dev/audio.c) - but that clock is
 * upstream of this device, in api->audio, and it governs whatever reaches
 * pcmmix.  A second one here would only fight it.  What this does instead is
 * drop: if the wire cannot take the samples, they are counted and gone, because
 * a sound device that blocks stops the game.
 */
#define AUD_RATE_DEFAULT 22050u
/*
 * 256 samples, which is 11.6 ms and exactly what pcmmix hands down in one mix
 * period, so the common case is one write in and one frame out.  Small matters:
 * the frame is written with the registry held, so this is how long the picture
 * and the console can be made to wait for the sound.
 */
#define AUD_CHUNK 256u

static ag_audio_fmt_t s_afmt;
static bool           s_afmt_set;
static uint64_t       s_abytes_in;
static uint64_t       s_abytes_sent;
static uint8_t        s_amono[AUD_CHUNK + 4u];

/*
 * Sound waits here, and is put on the wire by whoever is already writing to it
 * - never by the task that produced it.
 *
 * This is the whole cure for the freeze.  Picture and sound share one UART, and
 * the sound used to be written to it from the audio task, which held the device
 * registry lock while it waited for the wire.  With the picture on its own
 * thread that was a lock held across a blocking write on one core while the
 * game waited for it on the other - the board alive, everything stopped, the
 * sound already buffered still playing.  Now rem_pcm_write only drops samples
 * in this ring and returns at once, so it holds no lock waiting for anything;
 * the samples reach the wire from aud_drain(), called from the same paths that
 * already write it (a blit, the console sweep).  One writer of the wire, and
 * the producer never blocks.
 *
 * Eight kilobytes is about 370 ms at 22 kHz, more than the gap between two of
 * those draws, and the far end holds half a second of its own besides.
 */
#define AUD_WIRE_RING 8192u
static uint8_t   s_awire[AUD_WIRE_RING];
static uint32_t  s_awire_r, s_awire_w, s_awire_used;
static uint32_t  s_awire_rate;
static ag_mutex_t s_aud_lock;

/* Put up to a chunk of buffered sound on the wire.  Called from the paths that
 * are already writing the wire, so the audio never rides the producer's stack
 * or its locks. */
static void aud_drain(void)
{
    if (s_aud_lock == NULL || s_awire_used == 0u || s_awire_rate == 0u) {
        return;
    }
    for (int batch = 0; batch < 8; batch++) {
        uint32_t n = 0;
        const bool aheld = lock_take(s_aud_lock, 100u);
        put16(s_amono + 0, s_awire_rate & 0xffffu);
        put16(s_amono + 2, s_awire_rate >> 16);
        while (n < AUD_CHUNK && s_awire_used > 0u) {
            s_amono[4u + n] = s_awire[s_awire_r];
            s_awire_r = (s_awire_r + 1u) % AUD_WIRE_RING;
            s_awire_used--;
            n++;
        }
        lock_give(s_aud_lock, aheld);
        if (n == 0u) {
            break;
        }
        send_frame(FR_AUD, s_amono, 4u + n);
        s_abytes_sent += (uint64_t)n;
    }
}

static void aud_default_fmt(ag_audio_fmt_t *out)
{
    out->rate = AUD_RATE_DEFAULT;
    out->channels = 2;
    out->bits = 16;
}

static int aud_fmt_ok(const ag_audio_fmt_t *fmt)
{
    return fmt != NULL && fmt->rate >= 8000u && fmt->rate <= 48000u &&
           fmt->bits == 16u && (fmt->channels == 1u || fmt->channels == 2u);
}

static ag_err_t rem_pcm_open(ag_device_t *dev, uint32_t flags)
{
    (void)dev;
    (void)flags;
    if (!s_afmt_set) {
        aud_default_fmt(&s_afmt);
        s_afmt_set = true;
    }
    return s_up ? AG_OK : -AG_ENODEV;
}

static ag_err_t rem_pcm_close(ag_device_t *dev)
{
    (void)dev;
    return AG_OK;
}

/*
 * Signed 16-bit to unsigned 8-bit is the top byte with the sign flipped, and
 * two channels become one by averaging rather than by dropping the right - the
 * same arithmetic the far end's DAC driver does, moved to this side of the
 * wire.  An effect that lives in one channel would otherwise vanish, and there
 * is no stereo at the other end to lose it in.
 */
static int32_t rem_pcm_write(ag_device_t *dev, const void *buf, size_t len,
                             uint64_t off)
{
    (void)dev;
    (void)off;

    if (buf == NULL || len < 2u) {
        return -AG_EINVAL;
    }
    if (!s_afmt_set) {
        aud_default_fmt(&s_afmt);
        s_afmt_set = true;
    }

    const uint32_t chans = s_afmt.channels ? s_afmt.channels : 2u;
    const size_t   frame_bytes = (size_t)chans * sizeof(int16_t);
    const size_t   frames = len / frame_bytes;
    if (frames == 0u) {
        return -AG_EINVAL;
    }
    s_abytes_in += (uint64_t)(frames * frame_bytes);

    /*
     * Only buffered here, not sent.  This returns at once, holding no lock
     * across the wire - which is the whole point (see s_awire).  The samples
     * go out from aud_drain(), on the thread that already owns the wire.
     */
    const int16_t *src = (const int16_t *)buf;
    const bool wheld = lock_take(s_aud_lock, 100u);
    s_awire_rate = s_afmt.rate;
    for (size_t i = 0; i < frames; i++) {
        uint8_t m;
        if (chans == 2u) {
            const int32_t mono = ((int32_t)src[i * 2u] + src[i * 2u + 1u]) / 2;
            m = (uint8_t)((mono + 32768) >> 8);
        } else {
            m = (uint8_t)(((int32_t)src[i] + 32768) >> 8);
        }
        if (s_awire_used == AUD_WIRE_RING) {
            /* Full: drop the oldest, so a stall loses the past and not the
             * present - a gap the far end's cushion is there to swallow. */
            s_awire_r = (s_awire_r + 1u) % AUD_WIRE_RING;
            s_awire_used--;
        }
        s_awire[s_awire_w] = m;
        s_awire_w = (s_awire_w + 1u) % AUD_WIRE_RING;
        s_awire_used++;
    }
    lock_give(s_aud_lock, wheld);

    /*
     * Always a full accept.  What did not reach the glass is in the counters,
     * and a short return here would only make the mixer above offer it again.
     */
    return (int32_t)(frames * frame_bytes);
}

static ag_err_t rem_pcm_ioctl(ag_device_t *dev, uint32_t cmd, void *arg,
                              size_t arglen)
{
    (void)dev;

    if (cmd == AG_IOC_AUDIO_GETFMT) {
        if (arg == NULL || arglen < sizeof(ag_audio_fmt_t)) {
            return -AG_EINVAL;
        }
        if (!s_afmt_set) {
            aud_default_fmt(&s_afmt);
            s_afmt_set = true;
        }
        *(ag_audio_fmt_t *)arg = s_afmt;
        return AG_OK;
    }
    if (cmd == AG_IOC_AUDIO_SETFMT) {
        if (arg == NULL || arglen < sizeof(ag_audio_fmt_t)) {
            return -AG_EINVAL;
        }
        if (!aud_fmt_ok((const ag_audio_fmt_t *)arg)) {
            return -AG_EINVAL;
        }
        s_afmt = *(const ag_audio_fmt_t *)arg;
        s_afmt_set = true;
        return AG_OK;
    }
    if (cmd == AG_IOC_AUDIO_GETSTATS) {
        if (arg == NULL || arglen < sizeof(ag_audio_stats_t)) {
            return -AG_EINVAL;
        }
        ag_audio_stats_t *st = (ag_audio_stats_t *)arg;
        memset(st, 0, sizeof(*st));
        st->bytes_in = s_abytes_in;
        st->bytes_sent = s_abytes_sent;
        return AG_OK;
    }
    if (cmd == AG_IOC_FLUSH || cmd == AG_IOC_RESET) {
        return AG_OK;
    }
    return -AG_ENOTSUP;
}

static const ag_dev_ops_t k_pcm_ops = {
    .open = rem_pcm_open,
    .close = rem_pcm_close,
    .write = rem_pcm_write,
    .ioctl = rem_pcm_ioctl,
};

static ag_err_t rem_info(ag_handle_t h, ag_gfxinfo_t *out)
{
    (void)h;
    if (out == NULL) {
        return -AG_EINVAL;
    }
    *out = (ag_gfxinfo_t){0};
    out->width = 320;
    out->height = 240;
    out->fmt = AG_PIX_RGB565;
    return AG_OK;
}

static const ag_display_ops_t k_display_ops = {
    .size = sizeof(ag_display_ops_t),
    .info = rem_info,
    .acquire = NULL, /* no surface: the picture is somebody else's memory */
    .release = NULL,
    .flush = NULL,
    .swap = NULL,
    .text_info = rem_text_info,
    .text_row = rem_text_row,
    .text_cursor = rem_text_cursor,
    .blit_rect = rem_blit_rect,
};

/*
 * The only switch this driver has, and it turns the cleverness off.
 *
 * Everything else here is arithmetic that cannot disagree with itself.  The
 * hash grid is not: it is a claim about a machine at the other end of a wire,
 * and when the claim is wrong - a frame the line lost, a receiver that missed
 * a byte - the picture stays wrong until the slow repair sweep reaches those
 * rows.  With the trimming off every rectangle is sent whole, which is a
 * quarter of a second for a full screen and cannot be wrong at all.
 *
 * It is the floor to measure against, and the first thing to try when a
 * picture is misbehaving and nobody knows whose fault it is.
 */
static ag_err_t rem_ioctl(ag_device_t *dev, uint32_t cmd, void *arg,
                          size_t arglen)
{
    (void)dev;
    if (cmd != AG_IOC_DISPLAY_TRIM) {
        return -AG_ENOTSUP;
    }
    if (arg == NULL || arglen < sizeof(int)) {
        return -AG_EINVAL;
    }
    /*
     * A level, not a flag: 0 is raw bands and no memory of the far end, 1 is
     * lossless packing with no memory, 2 is everything.  The bottom level is
     * the one the whole switch exists for - what arrives is exactly what was
     * in the buffer, band by band, and there is nothing left in this driver
     * that could disagree with the screen.
     */
    const int level = *(const int *)arg;
    s_encode = level >= 1;
    s_trim = level >= 2;
    if (!s_trim) {
        hash_forget();
    }
    return AG_OK;
}

static const ag_dev_ops_t k_dev_ops = {
    .ioctl = rem_ioctl,
};

ag_err_t ag_driver_init(void)
{
    io = ag_api()->io;
    if (io == NULL || !AG_HAS(io, uart_write)) {
        return -AG_ENOTSUP;
    }
    if (ag_api()->task != NULL && ag_api()->task->mutex_create != NULL) {
        s_wire_lock = ag_api()->task->mutex_create();
        s_aud_lock = ag_api()->task->mutex_create();
        s_send_lock = ag_api()->task->mutex_create();
    }

    /*
     * Open the port here, where printing is still allowed, rather than lazily
     * from a callback: the io layer logs a warning when a UART has no pins in
     * BOARD.CFG, and logging from inside the text path is the deadlock the ABI
     * header warns about.  Failing now also means the fault arrives as a driver
     * that did not load rather than as a screen that stays dark for no reason.
     */
    const ag_err_t err = io->uart_config(LINK_PORT, LINK_BAUD, 8, 0, 1);
    if (err != AG_OK) {
        ag_printf("REMDISP: uart%d at %u baud: %s\n", LINK_PORT,
                  (unsigned)LINK_BAUD, ag_strerror(err));
        ag_printf("REMDISP: set [uart%d] tx/rx in C:\\BOARD.CFG\n", LINK_PORT);
        return err;
    }
    s_up = true;

    const ag_dev_add_t desc = {
        .name = "rem0",
        .driver = "REMDISP",
        .cls = AG_DEV_DISPLAY,
        .ops = &k_dev_ops,
        .class_ops = &k_display_ops,
        .priv = NULL,
    };
    const ag_err_t added = ag_dev_add(&desc);
    if (added != AG_OK) {
        s_up = false;
        return added;
    }

    /*
     * The sound card that shares the wire.  A separate device rather than a
     * second face of rem0, because that is what the mixer looks for: pcmmix
     * picks its sink by name, and `pcmrem` is in that list ahead of pcm0 -
     * which on a board whose amplifier was unsoldered is a converter playing to
     * nothing.  Registered second and not fatal if it fails: a screen with no
     * sound is worth having, the other way round is not.
     */
    {
        const ag_dev_add_t pcm = {
            .name = "pcmrem",
            .driver = "REMDISP",
            .cls = AG_DEV_AUDIO,
            .flags = AG_DEVF_EXCLUSIVE,
            .ops = &k_pcm_ops,
            .priv = NULL,
        };
        const ag_err_t pcm_added = ag_dev_add(&pcm);
        if (pcm_added != AG_OK) {
            ag_printf("REMDISP: no pcmrem (%s); the screen still works\n",
                      ag_strerror(pcm_added));
        }
    }

    /*
     * Take the hash grid now, at boot, rather than on the first band.
     *
     * It is only 4800 bytes since it stopped being a copy of the frame, so this
     * is no longer about the memory - it is about *when*.  Asking lazily means
     * asking after whatever application is now on the screen has taken its
     * arena, and on this board that is a game holding six megabytes of the
     * eight; the screen would then quietly stop trimming at the one moment
     * there was finally something to look at, and nothing would say why.  A
     * display driver owns its frame memory and does not queue behind the
     * programs it is showing.
     *
     * 320x240 is what rem_info advertises, so it is what the kernel's own
     * surface will be.  An application that presents something bigger still
     * works: prev_ready reallocates, and if that fails the picture is sent
     * whole - slower, not broken.
     */
    if (!prev_ready(320u, 240u)) {
        ag_printf("REMDISP: no damage grid; every band will be sent whole\n");
    }

    /*
     * The frame, and the task that sends it - both taken here, at boot, and for
     * the same two reasons the grid above is.
     *
     * When: a hundred and fifty kilobytes asked for lazily is a hundred and
     * fifty kilobytes asked for after the game has taken six megabytes of the
     * eight, and the answer would be no at exactly the moment there was
     * finally something worth showing.
     *
     * Whose: memory asked for from inside blit_rect is charged to whoever
     * called in, because that is what a process arena is - so a display driver
     * allocating there would be spending the game's memory to show the game.
     * ag_driver_init runs on no process, and what is asked for here comes from
     * the system heap.
     */
    s_fb_w = 320u;
    s_fb_h = 240u;
    s_fb = (uint16_t *)ag_malloc((size_t)s_fb_w * s_fb_h * sizeof(uint16_t));
    if (s_fb == NULL) {
        s_fb_w = 0;
        s_fb_h = 0;
        ag_printf("REMDISP: no frame of my own (%u KB); the wire will be fed "
                  "by whoever draws, and they will wait for it\n",
                  (unsigned)(320u * 240u * 2u / 1024u));
    } else {
        memset(s_fb, 0, (size_t)s_fb_w * s_fb_h * sizeof(uint16_t));
        if (ag_api()->task != NULL && ag_api()->task->mutex_create != NULL) {
            s_dmg_lock = ag_api()->task->mutex_create();
        }
        /*
         * Six kilobytes of stack: send_rect's work space is static (see the
         * note above it), so what is on the stack here is the band loop's own
         * arithmetic and a twelve byte header.  Priority above the default so
         * the wire keeps being fed while a game holds both cores; it spends its
         * life blocked on a UART, so preferring it costs nothing.
         */
        s_stop = false;
        if (!ag_module_task(wire_task, NULL, "remwire", 6144u, 6)) {
            ag_free(s_fb);
            s_fb = NULL;
            s_fb_w = 0;
            s_fb_h = 0;
            ag_printf("REMDISP: no task of my own; sending from the caller\n");
        } else {
            ag_module_on_unload(rem_stop);
        }
    }

    ag_printf("REMDISP: uart%d at %u baud; %ux%u cells, pixels in %u-row "
              "bands%s\n",
              LINK_PORT, (unsigned)LINK_BAUD, (unsigned)REM_COLS,
              (unsigned)REM_ROWS, (unsigned)BAND_ROWS,
              (s_fb != NULL) ? ", sent from a task of its own" : "");
    return AG_OK;
}
