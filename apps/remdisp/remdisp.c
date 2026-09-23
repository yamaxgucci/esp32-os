/*
 * ArgonOS - this machine's screen, sent down a wire to the board that has glass.
 *
 * The S3 on the desk has no display.  The board next to it is little else but
 * one, and two soldered pins join them (docs/09-esp32-cyd.md, "Провод к S3").
 * So the screen becomes a device like any other: this publishes a display whose
 * panel is a serial port, and nothing above it can tell the difference.
 *
 * It carries two kinds of traffic, because the kernel offers two and a screen
 * needs both:
 *
 *   Pixels (blit_rect, ABI 0.30) - the picture, in bands.  This is the part
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

AG_DRV("REMDISP", "1.1", "argon");

/* UART1: the console is UART0 on this board, and the numbering is the chip's. */
#define LINK_PORT 1

/*
 * Four megabaud, measured rather than chosen: 400 067 B/s over 4 MB with zero
 * corrupt and zero lost blocks, where five megabaud corrupts 0.12% (apps/link,
 * and the table in docs/09-esp32-cyd.md).  Both ends must agree, so the same
 * number appears in remterm.c.
 */
/*
 * THE DEFAULT IS NOT THE WIRE'S SPEED.  This cable has been measured at
 * 16 Mbaud - 1 599 966 B/s on the line, 1 132 730 B/s of payload after 24.7%
 * of blocks come back bad - and comparing anything against the 4 Mbaud below
 * compares it against 400 KB/s, which is that baud's ceiling and not the
 * wire's.  `[remdisp] baud` in SYSTEM.CFG moves it; the compiled-in number
 * stays where it was so a board that says nothing behaves as it did.
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
 * A driver still cannot ask for a resize - it loads long after the console is
 * built - but the size is no longer only a firmware decision either: whoever
 * sets this link up puts `[console] cols=40 rows=25` in SYSTEM.CFG, or types
 * `mode con cols=40 lines=25`, and then the two agree.  The pixels have no
 * such problem; they are placed by size.
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
#define FR_BULK   'B' /* anybody's bytes, in the shape that works            */

/*
 * A band is at most eight rows, and the reason is the far end's panel rather
 * than this end's memory: every band becomes one window-setting on the ILI9341,
 * three commands and a wait, and a band of one row would spend more of the
 * frame on setting up transfers than on pixels (the same lesson as commit
 * e37dbd2 on that board).  Eight rows of 320 is five kilobytes, which the far
 * end can hold without a framebuffer - and not having a framebuffer over there
 * is the whole reason this arrangement exists.
 */
#define BAND_ROWS 12u

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
/* ---------------------------------------------------------------------- */
/* The wire: a UART, or the SPI the same four pins can carry                */
/* ---------------------------------------------------------------------- */
/*
 * WHY THIS IS A SWITCH AND NOT A SECOND DRIVER.  Everything above and below
 * writes FRAMES - magic, length, payload, checksum - into a byte stream and
 * never asks what carries it.  That is the whole reason this path runs at the
 * wire's rate while the cluster link over the same copper manages a few
 * thousand bytes a second: there is no window, no acknowledgement and no
 * retransmission here, and a band that is damaged is a band redrawn on the
 * next frame.  Carrying the same stream over SPI is therefore a change of two
 * functions, not a change of design.
 *
 * AND SPI IS THE EASY CASE HERE, which is worth saying because it is the hard
 * case for the cluster link.  A link has to let the far end speak, so its
 * master clocks blocks whether or not it has anything to send - measured: 93%
 * of them empty.  A screen is ONE WAY.  This master clocks only when it has
 * pixels, so there are no empty blocks at all, and the reverse channel is one
 * byte that a poll ten times a second can collect.
 *
 * [remdisp] in SYSTEM.CFG:  wire = uart | spi, and for spi: bus, cs, khz,
 * block, gap_us.  The default is the UART, so a board that says nothing keeps
 * the behaviour it had.
 */
#define RD_WIRE_UART 0
#define RD_WIRE_SPI  1
/*
 * A wire that throws everything away, and it is an instrument rather than a
 * joke: it is the only way to weigh what this driver costs against what the
 * transport costs.  With `wire = none` the frames are built exactly as usual
 * and then dropped, so whatever the sender still manages is its own ceiling.
 * The difference against the same run over SPI is the wire.
 */
#define RD_WIRE_NONE 2

static int      s_wire = RD_WIRE_UART;
static uint64_t s_polled;   /* when the reverse channel was last read */
static int      s_bus  = 2;
static int      s_cs   = 10;
static uint32_t s_khz  = 20000u;
static uint32_t s_baud = LINK_BAUD;
/*
 * THE BLOCK IS WHAT THE FAR END ARMS, so a write longer than one is truncated
 * at the slave and the rest is clocked into a chip whose transaction has
 * ended.  A frame here is up to 5194 bytes, so the write is CUT INTO BLOCKS
 * and the far end puts the stream back together the same way it does over a
 * UART - which it already knows how to do, because a UART has no blocks
 * either.
 *
 * 512 rather than the 8192 one transfer could carry: the kernel's slave
 * service was measured falling over at 4096 (123 stalls and 123 rebuilds of
 * the interface in one run), and this is not the place to find out why.
 */
static uint32_t s_blk  = 512u;
/*
 * Dead time between two blocks, because the far end has to have the next
 * transaction armed when the select falls and a master clocking back to back
 * leaves it no room.  The cluster link measured this as load-bearing on the
 * same pair; 0 is legal and worth trying, which is what the key is for.
 */
static uint32_t s_gap  = 100u;

static void wire_gap(void)
{
    if (s_gap == 0u) {
        return;
    }
    const uint64_t t0 = ag_api()->time->us();
    while ((uint32_t)(ag_api()->time->us() - t0) < s_gap) {
        /* bounded by the number above: deliberate dead time, not a stall */
    }
}

static void wire_write(const uint8_t *p, uint32_t n)
{
    if (s_wire == RD_WIRE_NONE) {
        return;
    }
    if (s_wire == RD_WIRE_UART) {
        (void)io->uart_write(LINK_PORT, p, n);
        return;
    }
    while (n > 0u) {
        const uint32_t k = (n > s_blk) ? s_blk : n;
        (void)io->spi_xfer(s_bus, s_cs, p, NULL, k);
        p += k;
        n -= k;
        if (n > 0u) {
            wire_gap();
        }
    }
    /*
     * AND AFTER THE LAST PIECE TOO, which used to be left out because the gap
     * was thought of as something between the parts of one frame.  It is not:
     * the far end has to ARM a block between transactions, and with a block
     * big enough to hold a whole frame there is only ever one piece, so
     * consecutive frames arrived back to back with no room at all.  The faster
     * the clock the sooner the next one starts, which is why doubling the rate
     * made it worse rather than better.
     */
    wire_gap();
}

/*
 * What the far end has said, which is one byte and rarely.  On SPI a receive
 * needs a clock, so this one clocks a short empty block; the zeros it reads
 * when nothing was queued are skipped by the caller, which is scanning for a
 * single character anyway.
 */
static int32_t wire_read(uint8_t *p, uint32_t n)
{
    if (s_wire == RD_WIRE_NONE) {
        return 0;
    }
    if (s_wire == RD_WIRE_UART) {
        return io->uart_read(LINK_PORT, p, n, 0);
    }
    if (io->spi_xfer(s_bus, s_cs, NULL, p, n) != AG_OK) {
        return 0;
    }
    return (int32_t)n;
}

/*
 * THE FRAME BUFFER IS DMA-CAPABLE WHEN THE WIRE IS SPI, and that is not a
 * micro-optimisation - it is the difference between a picture and nothing.
 *
 * The port splits any transfer longer than AG_PORT_SPI_BOUNCE (1024) into
 * separate transactions when it has to copy through its bounce buffer, and a
 * separate transaction means the chip select RISES between the pieces.  The
 * far end frames on exactly that, so a 5194-byte band arrived as six
 * fragments and was destroyed, while an 80-byte console row - one transaction
 * either way - arrived perfectly.  Measured: 24 rows, 0 rectangles, 2 511 644
 * bytes counted stray.
 *
 * io_hw.c takes a fast path for a write-only transfer FROM DMA-CAPABLE MEMORY:
 * no copy and no cap, so the whole frame crosses as one transaction.  A static
 * array in a loadable driver is not that memory, so the buffer is allocated.
 */
static uint8_t *s_frame;

static void send_frame(uint8_t op, const uint8_t *payload, uint32_t len)
{
    if (!s_up || len > FR_MAX_PAYLOAD) {
        return;
    }
    s_frame[0] = FR_MAGIC0;
    s_frame[1] = FR_MAGIC1;
    s_frame[2] = op;
    s_frame[3] = (uint8_t)len;
    s_frame[4] = (uint8_t)(len >> 8);

    /*
     * A WORD AT A TIME, because this is every byte of every band.  It used to
     * copy and checksum byte by byte, which at the rates this now runs at is
     * a whole pass over five kilobytes per band spent on a loop the compiler
     * cannot widen: the XOR of the bytes is the XOR of the words folded down,
     * so the two can be done together on aligned words and the ends by hand.
     */
    memcpy(s_frame + 5u, payload, len);

    uint32_t       acc = 0;
    const uint8_t *q = payload;
    uint32_t       n = len;
    while (n != 0u && (((uintptr_t)q & 3u) != 0u)) {
        acc ^= (uint32_t)*q++;
        n--;
    }
    while (n >= 4u) {
        uint32_t w;
        memcpy(&w, q, sizeof(w));
        acc ^= (w & 0xFFu) ^ ((w >> 8) & 0xFFu) ^ ((w >> 16) & 0xFFu) ^
               (w >> 24);
        q += 4;
        n -= 4u;
    }
    while (n != 0u) {
        acc ^= (uint32_t)*q++;
        n--;
    }
    s_frame[5u + len] = (uint8_t)acc;

    /*
     * The result is ignored deliberately.  There is nothing useful to do with a
     * short write from inside a kernel callback: printing is forbidden here, and
     * stalling to retry would stop the console or the application for the sake
     * of one rectangle.
     */
    wire_write(s_frame, 5u + len + 1u);
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* ---- pixels ------------------------------------------------------------ */

/*
 * The last frame the far end was given, so that pixels it already has cost
 * nothing but the comparison - and so that what *is* sent can be trimmed to the
 * part that moved.
 *
 * This is the largest saving available and it is not compression: what a screen
 * does most of the time is not change.  A paused game, a menu, a status bar
 * along the bottom, the whole picture between two footsteps - bytes nobody
 * needs twice.  A flush of the whole surface is what an application does when
 * it does not track its own damage; this turns that back into damage tracking
 * without the application knowing.
 *
 * 150 KB for 320x240, from the module's heap and so from PSRAM.  If it cannot
 * be had, everything is sent - slower, not broken.
 *
 * What is deliberately NOT done with it: coding the difference against it.
 * That is what a video codec would do and it would win by a mile, but undoing
 * a difference needs the previous frame at the *receiving* end, and the far end
 * cannot hold one - 150 KB against a largest free block of about ninety is the
 * whole reason this arrangement exists.  Reading it back out of the panel's own
 * memory over SPI is slow and, on this controller, not dependable.  So the
 * shadow is used to decide what to send, never to encode it.
 */
static uint16_t *s_prev;
static uint32_t  s_prev_w;
static uint32_t  s_prev_h;

/*
 * WHEN THE SHADOW STOPS PAYING FOR ITSELF.
 *
 * Trimming a band to what actually moved is the whole reason a still screen
 * costs nothing, and on a picture that changes EVERYWHERE it is two passes
 * over the band - a comparison and an update - that never once save a byte.
 * That is the video case, and it is the case where the frame rate matters.
 *
 * So after enough bands in a row come back "all of it moved", the shadow is
 * dropped and the bands go whole; it is allowed back after a while, and the
 * first frame after that goes out whole anyway (the buffer is deliberately not
 * cleared), so nothing can be missed by letting go of it.
 */
#define PREV_GIVE_UP   16u
#define PREV_TRY_AGAIN 600u

static uint32_t s_prev_full;  /* bands in a row where everything moved */
static uint32_t s_prev_rest;  /* bands to wait before tracking again   */

static bool prev_ready(uint32_t w, uint32_t h)
{
    if (s_prev_rest != 0u) {
        s_prev_rest--;
        if (s_prev != NULL) {
            ag_free(s_prev);
            s_prev = NULL;
            s_prev_w = 0;
            s_prev_h = 0;
        }
        return false;
    }
    if (s_prev != NULL && s_prev_w == w && s_prev_h == h) {
        return true;
    }
    ag_free(s_prev);
    s_prev = (uint16_t *)ag_malloc((size_t)w * h * sizeof(uint16_t));
    if (s_prev == NULL) {
        s_prev_w = 0;
        s_prev_h = 0;
        return false;
    }
    s_prev_w = w;
    s_prev_h = h;
    /*
     * Deliberately not cleared.  Whatever is in it disagrees with the far end,
     * so the first frame goes out whole - which is exactly what a far end that
     * has just started listening needs.
     */
    return true;
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
 * So the receiver says one byte when it starts, and this drops the shadow.
 * prev_ready then allocates a fresh one that is deliberately not cleared,
 * which is exactly "disagrees with the far end" - the next frame goes out
 * whole.  No new state, no new code path: the reconnect uses the same road as
 * the first connection.
 *
 * Polled rather than waited for, with a zero timeout, from the two callbacks
 * the kernel already makes often.  A driver cannot own a task.
 *
 * Which leaves one case out, and it is worth naming rather than discovering:
 * while an application holds the display the kernel stops calling text_cursor,
 * and it calls blit_rect only when that application draws.  An application
 * holding a picture that never changes therefore never reads this, and a
 * receiver that reconnects under one stays dark.  Measured, not assumed:
 * gfxdemo paints once and waits, and a receiver started after it got nothing.
 *
 * It does not matter for what this is for - a game redraws, and the console
 * repairs itself through the sweep below - so the fix is not made here.  What
 * it would be: register a second device of class AG_DEV_INPUT whose poll()
 * returns no events and services this instead.  The kernel calls that ten
 * times a second regardless of who owns the screen, and it exists precisely
 * because a loadable driver cannot own a task.
 */
#define FR_HELLO 'H'

static void poll_far_end(void)
{
    uint8_t buf[16];

    /*
     * TEN TIMES A SECOND AND NOT ONCE A BAND.
     *
     * This sits at the top of blit_rect, which is called for every band, and a
     * read is the one thing that cannot be queued: it has to wait for the
     * write already on the wire, so every poll turned an overlapped write back
     * into a blocking one.  Measured at 40 MHz: with the poll per band the
     * sender offered 2885 KB/s and the engine was idle half the time.  What
     * comes back this way is a single byte saying a new screen has appeared,
     * so a tenth of a second is far more often than it needs.
     */
    const uint64_t now = ag_api()->time->us();
    if (s_polled != 0u && (uint32_t)(now - s_polled) < 100000u) {
        return;
    }
    s_polled = now;
    for (;;) {
        const int32_t n = wire_read(buf, sizeof(buf));
        if (n <= 0) {
            return;
        }
        for (int32_t i = 0; i < n; i++) {
            if (buf[i] == (uint8_t)FR_HELLO) {
                ag_free(s_prev);
                s_prev = NULL;
                s_prev_w = 0;
                s_prev_h = 0;
            }
        }
        /*
         * ONE PASS ON SPI, AND THIS IS NOT A TIDY-UP.  The loop drains a UART
         * buffer and ends when the buffer is empty, which a UART says by
         * handing back less than was asked for.  SPI has no buffer and cannot
         * say it: a read CLOCKS its bytes, so it always returns the full
         * length, and this loop never ended.  The driver's ten-a-second hook
         * stayed inside it for ever - measured on the boards at 43651
         * transactions in twenty seconds, ONE of them carrying real bytes, and
         * not a single band ever sent, because the hook never came back to
         * send one.
         */
        if (s_wire == RD_WIRE_SPI || (uint32_t)n < sizeof(buf)) {
            return;
        }
    }
}

/*
 * Work space.  Static rather than automatic because these are kilobytes and the
 * task this runs on belongs to whoever flushed - an application's stack is not
 * the driver's to spend.  Safe as static because the kernel holds the device
 * registry across blit_rect, so two flushes cannot be inside here at once.
 */
static uint8_t  s_raw[FR_MAX_PAYLOAD];  /* geometry + pixels, uncompressed   */
/* How many bands in a row refused to compress, and a counter to
 * re-test on.  See the note where they are used. */
static uint32_t s_nocomp;
static uint32_t s_bandseq;
static uint8_t  s_cand[FR_MAX_PAYLOAD]; /* geometry + whatever encoded best  */
static uint8_t  s_idx[BAND_MAX_PX];     /* one index per pixel, before packing */
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
 * Called on whichever task flushed or presented, with the device registry held,
 * and it blocks for as long as the wire needs.  That is deliberate: an
 * application that asks for more than the wire can carry runs at the speed of
 * the wire, which is honest and measurable.  What happens here is the wire being
 * asked to carry less, in three steps that multiply:
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
static void rem_blit_rect(ag_handle_t h, const ag_blit_t *b)
{
    (void)h;
    if (b == NULL || b->px == NULL || b->w == 0 || b->h == 0) {
        return;
    }
    /* What will not fit in a frame is refused rather than truncated: a band
     * drawn from half its pixels is worse than a band not drawn. */
    if ((uint32_t)b->w * BAND_ROWS > BAND_MAX_PX) {
        return;
    }

    poll_far_end();

    const uint8_t *src = (const uint8_t *)b->px;
    bool           track = prev_ready(b->surf_w, b->surf_h);

    for (uint32_t band = 0; band < b->h; band += BAND_ROWS) {
        uint32_t rows = b->h - band;
        if (rows > BAND_ROWS) {
            rows = BAND_ROWS;
        }

        uint32_t x0 = 0, x1 = (uint32_t)b->w - 1u;
        uint32_t y0 = 0, y1 = rows - 1u;

        if (track) {
            /* The bounding box of the difference, in the band's own
             * coordinates.  Start inverted so "nothing moved" is detectable. */
            x0 = (uint32_t)b->w;
            x1 = 0;
            y0 = rows;
            y1 = 0;
            for (uint32_t r = 0; r < rows; r++) {
                const uint16_t *want =
                    (const uint16_t *)(src + (size_t)(band + r) * b->stride);
                const uint16_t *have = s_prev +
                                       (size_t)((uint32_t)b->y + band + r) *
                                           s_prev_w + b->x;
                for (uint32_t i = 0; i < (uint32_t)b->w; i++) {
                    if (want[i] != have[i]) {
                        if (i < x0) {
                            x0 = i;
                        }
                        if (i > x1) {
                            x1 = i;
                        }
                        if (r < y0) {
                            y0 = r;
                        }
                        if (r > y1) {
                            y1 = r;
                        }
                    }
                }
            }
            if (x0 > x1) {
                s_prev_full = 0;
                continue; /* the far end already has this band */
            }
            if (x0 == 0u && y0 == 0u && x1 == (uint32_t)b->w - 1u &&
                y1 == rows - 1u) {
                if (++s_prev_full >= PREV_GIVE_UP) {
                    /*
                     * Arm the rest and stop tracking for the rest of THIS
                     * call.  The buffer itself is let go in prev_ready, which
                     * is the only place that is not inside this loop: `track`
                     * is decided once per blit, and freeing here left it true
                     * with nothing behind it - a fault, found on the board the
                     * first time this ran.
                     */
                    s_prev_full = 0;
                    s_prev_rest = PREV_TRY_AGAIN;
                    track = false;
                }
            } else {
                s_prev_full = 0;
            }
        }

        const uint32_t rw = x1 - x0 + 1u;
        const uint32_t rh = y1 - y0 + 1u;
        const uint32_t npx = rw * rh;

        /* Gather the rectangle, and bring the shadow up to date with it. */
        uint16_t *out = (uint16_t *)(s_raw + 12u);
        for (uint32_t r = 0; r < rh; r++) {
            const uint16_t *want =
                (const uint16_t *)(src + (size_t)(band + y0 + r) * b->stride) +
                x0;
            /* A row at a time.  This used to be two per-pixel loops, which
             * is the same work the compiler cannot widen - on the full-band
             * case that is every pixel of every frame. */
            memcpy(out + (size_t)r * rw, want, (size_t)rw * sizeof(uint16_t));
            if (track) {
                uint16_t *have = s_prev +
                                 (size_t)((uint32_t)b->y + band + y0 + r) *
                                     s_prev_w + b->x + x0;
                memcpy(have, want, (size_t)rw * sizeof(uint16_t));
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

        /*
         * THE CAP IS THE CURRENT BEST, NOT THE BUFFER.  An encoding that has
         * already grown past the raw pixels cannot win, and pack16 answers 0
         * the moment it passes its cap - so on a picture that does not
         * compress it now gives up after a few hundred bytes instead of
         * walking all 5120 twice.  On a picture that does compress nothing
         * changes: it never reaches the cap.
         */
        const uint32_t room = FR_MAX_PAYLOAD - 12u;
        /*
         * A PICTURE THAT DID NOT COMPRESS LAST TIME PROBABLY WILL NOT NOW.
         *
         * Both encoders walk the whole band before they can lose, and on
         * moving photographic or noisy content they lose every time - two
         * passes over five kilobytes per band, on the path that decides the
         * frame rate.  So after enough failures in a row they are skipped, and
         * one band in sixty-four still tries, which is what notices a picture
         * that has become flat again.  A picture that compresses never reaches
         * the threshold, so nothing changes for it.
         */
        const uint32_t p = (s_nocomp < 8u || (s_bandseq & 63u) == 0u)
                               ? pack16(out, npx, s_cand + 12u,
                                        (best < room) ? best : room)
                               : 0u;
        const bool tried = (s_nocomp < 8u || (s_bandseq & 63u) == 0u);
        s_bandseq++;
        if (p != 0 && p < best) {
            best = p;
            best_op = FR_PACK;
            best_len = p;
        }
        const uint32_t used = (best_op == FR_PACK) ? p : 0u;
        const uint32_t left = room - used;
        const uint32_t q = tried ? encode_indexed(out, npx, s_cand + 12u + used,
                                                  (best < left) ? best : left)
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

        if (tried) {
            if (best_op == FR_BAND) {
                if (s_nocomp < 255u) {
                    s_nocomp++;
                }
            } else {
                s_nocomp = 0;
            }
        }

        if (best_op == FR_BAND) {
            send_frame(FR_BAND, s_raw, 12u + raw);
        } else {
            for (uint32_t i = 0; i < 12u; i++) {
                s_cand[i] = geom[i];
            }
            send_frame(best_op, s_cand, 12u + best_len);
        }
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
}

/* ---- the device -------------------------------------------------------- */

/*
 * Geometry without taking the display.  There is no surface here and nothing to
 * hand out, so out->fb stays NULL and acquire is absent - which is what tells
 * the kernel this panel is fed rather than drawn into.  The size reported is
 * the far end's glass, because that is what a caller placing a picture needs to
 * know, and it is not the same as the console size above.
 */
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

static const ag_dev_ops_t k_dev_ops = {
    .ioctl = NULL,
};

/*
 * THE WIRE AS A PIPE, and this is the whole point of the driver rather than a
 * sideline.
 *
 * What this driver does for a screen - build a frame as big as the transport
 * will take, hand it over in ONE transaction, and check it at the far end with
 * a checksum - is not about pixels.  Measured on this pair at 40 MHz it moves
 * 2.68 MB/s with one bad frame in 6043.  The cluster link over the same four
 * wires moves about 20 KB/s, and the difference is not the data and not the
 * copper: it cuts the stream into 248-byte frames and waits for an
 * acknowledgement of each, so the bytes in flight are a window of 5.9 KB
 * against a measured round trip of 25 to 45 ms.
 *
 * So the shape is offered to everybody else as a character device.  Write to
 * /dev/wire0 and the bytes cross in frames of FR_MAX_PAYLOAD, one transaction
 * each.  What it does NOT do, and the caller must know: there is no
 * acknowledgement and nothing is sent again.  A frame that fails its checksum
 * at the far end is counted and dropped.  That is the right trade for pixels
 * and for anything idempotent or checkpointed above; it is the wrong one for a
 * file transfer that must not lose a byte, and the answer for those is a
 * batched acknowledgement on top of this, not a return to 248-byte frames.
 */
static int32_t pipe_write(ag_device_t *dev, const void *buf, size_t len,
                          uint64_t off)
{
    (void)dev;
    (void)off;
    if (!s_up || buf == NULL) {
        return -AG_ENODEV;
    }

    const uint8_t *p = (const uint8_t *)buf;
    size_t         left = len;
    while (left > 0u) {
        const uint32_t n =
            (left > FR_MAX_PAYLOAD) ? FR_MAX_PAYLOAD : (uint32_t)left;
        send_frame(FR_BULK, p, n);
        p += n;
        left -= n;
    }
    return (int32_t)len;
}

static const ag_dev_ops_t k_pipe_ops = {
    .write = pipe_write,
};

ag_err_t ag_driver_init(void)
{
    io = ag_api()->io;
    if (io == NULL || !AG_HAS(io, uart_write)) {
        return -AG_ENOTSUP;
    }

    /*
     * Open the port here, where printing is still allowed, rather than lazily
     * from a callback: the io layer logs a warning when a UART has no pins in
     * BOARD.CFG, and logging from inside the text path is the deadlock the ABI
     * header warns about.  Failing now also means the fault arrives as a driver
     * that did not load rather than as a screen that stays dark for no reason.
     */
    /* Which wire, before anything is opened.  A board that says nothing gets
     * the UART, which is what every board that has one has had. */
    char wire[8];
    ag_strlcpy(wire, "uart", sizeof(wire));
    if (ag_api()->cfg != NULL && AG_HAS(ag_api()->cfg, get_str)) {
        (void)ag_api()->cfg->get_str("remdisp.wire", wire, sizeof(wire));
    }
    if (wire[0] == 's' || wire[0] == 'S') {
        s_wire = RD_WIRE_SPI;
    } else if (wire[0] == 'n' || wire[0] == 'N') {
        s_wire = RD_WIRE_NONE;
    }
    if (ag_api()->cfg != NULL && AG_HAS(ag_api()->cfg, get_int)) {
        s_bus = ag_api()->cfg->get_int("remdisp.bus", s_bus);
        s_cs = ag_api()->cfg->get_int("remdisp.cs", s_cs);
        s_khz = (uint32_t)ag_api()->cfg->get_int("remdisp.khz", (int32_t)s_khz);
        s_baud = (uint32_t)ag_api()->cfg->get_int("remdisp.baud",
                                                  (int32_t)s_baud);
        s_blk = (uint32_t)ag_api()->cfg->get_int("remdisp.block",
                                                 (int32_t)s_blk);
        const int32_t g = ag_api()->cfg->get_int("remdisp.gap_us",
                                                 (int32_t)s_gap);
        s_gap = (g > 0) ? (uint32_t)g : 0u;
    }
    if (s_blk == 0u || s_blk > 8192u) {
        s_blk = 512u;
    }

    s_frame = (uint8_t *)ag_malloc_caps(5u + FR_MAX_PAYLOAD + 1u,
                                        (s_wire == RD_WIRE_SPI) ? AG_MEM_DMA
                                                                : AG_MEM_DEFAULT);
    if (s_frame == NULL) {
        ag_printf("REMDISP: no room for a %u byte frame buffer\n",
                  (unsigned)(5u + FR_MAX_PAYLOAD + 1u));
        return -AG_ENOMEM;
    }

    ag_err_t err;
    if (s_wire == RD_WIRE_SPI) {
        if (!AG_HAS(io, spi_xfer) || !AG_HAS(io, spi_config)) {
            ag_printf("REMDISP: this kernel has no io->spi_xfer\n");
            return -AG_ENOTSUP;
        }
        err = io->spi_config(s_bus, s_cs, s_khz);
        if (err != AG_OK) {
            ag_printf("REMDISP: spi%d cs%d at %u kHz: %s\n", s_bus, s_cs,
                      (unsigned)s_khz, ag_strerror(err));
            ag_printf("REMDISP: set [spi%d] sck/mosi/miso in C:\\BOARD.CFG\n",
                      s_bus);
            return err;
        }
    } else {
        err = io->uart_config(LINK_PORT, s_baud, 8, 0, 1);
        if (err != AG_OK) {
            ag_printf("REMDISP: uart%d at %u baud: %s\n", LINK_PORT,
                      (unsigned)s_baud, ag_strerror(err));
            ag_printf("REMDISP: set [uart%d] tx/rx in C:\\BOARD.CFG\n",
                      LINK_PORT);
            return err;
        }
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
     * Which wire, said out loud.  It used to say `uart` whichever one was
     * open, and a banner that names the wrong transport is worse than none:
     * a run over SPI printed `uart1 at 4000000 baud` and cost an hour of
     * looking for a fault in the wrong half of the system.
     */
    if (s_wire == RD_WIRE_NONE) {
        ag_printf("REMDISP: no wire - frames are built and dropped; "
                  "%ux%u cells, pixels in %u-row bands\n",
                  (unsigned)REM_COLS, (unsigned)REM_ROWS,
                  (unsigned)BAND_ROWS);
    } else if (s_wire == RD_WIRE_SPI) {
        ag_printf("REMDISP: spi%d cs%d at %u kHz, blocks of %u; %ux%u cells, "
                  "pixels in %u-row bands\n",
                  s_bus, s_cs, (unsigned)s_khz, (unsigned)s_blk,
                  (unsigned)REM_COLS, (unsigned)REM_ROWS,
                  (unsigned)BAND_ROWS);
    } else {
        ag_printf("REMDISP: uart%d at %u baud; %ux%u cells, "
                  "pixels in %u-row bands\n",
                  LINK_PORT, (unsigned)s_baud, (unsigned)REM_COLS,
                  (unsigned)REM_ROWS, (unsigned)BAND_ROWS);
    }
    /*
     * And the same wire as a pipe.  A failure to publish it is not a failure
     * to be a screen, so it is noted and the driver stays up.
     */
    const ag_dev_add_t pipe_desc = {
        .name = "wire0",
        .driver = "REMDISP",
        .cls = AG_DEV_CHAR,
        .flags = 0,
        .ops = &k_pipe_ops,
        .priv = NULL,
    };
    if (ag_dev_add(&pipe_desc) != AG_OK) {
        ag_printf("REMDISP: /dev/wire0 could not be published\n");
    }

    return AG_OK;
}
