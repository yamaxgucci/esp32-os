/*
 * ArgonOS - this board's glass, lent to the machine on the other end of the wire.
 *
 * The far side runs REMDISP.SYS, which publishes a display whose panel is a
 * serial port.  What comes down it is a screen: bands of pixels while an
 * application over there holds the display, rows of console cells when none
 * does.  This puts both on the glass.  It is a monitor at the end of a cable -
 * the same idea as VGA or HDMI, with a thinner cable and a protocol that admits
 * it.
 *
 *   run c:\remterm.axe [baud] [secs]   4000000 by default; Esc stops it,
 *                                      and so does `secs` if it is not zero
 *
 * The two kinds of traffic are drawn two different ways, and the difference is
 * not arbitrary:
 *
 *   Pixels go straight to the panel with ag_gfx_present - the caller's own
 *   memory, placed by the panel driver.  A band buffer of a few kilobytes is
 *   all that is held here, which matters because this board cannot hold a
 *   320x240 framebuffer at all: that is 150 KB and the largest block free is
 *   around ninety.  Not needing one is the whole reason the far end sends bands.
 *
 *   Characters are poked into this board's own console, which ILI9341.SYS is
 *   already rendering a row at a time.  Writing into a console that is already
 *   painted is shorter and safer than becoming a second owner of the panel, and
 *   the caret comes free because the local console blinks its own.
 *
 * Those two cannot both be on screen: while the display is acquired the kernel
 * suspends the console's text path, which is exactly right.  So the mode
 * follows the traffic - the first band acquires the display, the first console
 * row after that releases it - and the far end's own state is what drives it.
 *
 * And a third kind, which is not drawn at all: sound.  This board has a
 * converter with an amplifier behind it and the far end has neither, so it
 * sends samples and they come out here.  See "the sound" below - the hard part
 * is that a write to the converter blocks and this loop is the whole program.
 *
 * The encodings here are mirrored in host-tests/test_rempix.c, which
 * round-trips them against each other: a change here that is not made there
 * passes the tests and breaks the screen.
 *
 * Build (LX6, for the ESP32 - the far end is an LX7 and needs its own image):
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32-elf-gcc \
 *       --include sdk/include -o build/apps/REMTERM.AXE apps/remterm/remterm.c
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

/*
 * The heap is asked for, and that number is the whole of why whole frames
 * work here at all.
 *
 * A 320x240 frame is 150 KB and this board has 208 KB free, so it fits - but
 * an application that does not name a heap gets *a quarter of the free pool*,
 * which here is 52 KB.  That cap is right for its own reason (a default of a
 * megabyte would swallow a board without PSRAM whole), and it is why every
 * attempt to allocate the frame failed on a board with room for it: 150 KB
 * asked for out of a 52 KB heap, however many pieces it was cut into.
 *
 * Three wrong explanations came before this one - the shape of the heap, the
 * rounding of the pieces, the code arena - and each cost a build and a flash.
 * The board could have said so at any point; it took adding four numbers to
 * one printf to ask it.
 *
 * And 200 KB could not be had either, for a reason that settles the question:
 * a process heap is *one* allocation, and the largest free block on this board
 * is 108 KB.  So under this operating system a 150 KB frame cannot be
 * assembled here at all - not for want of memory (208 KB is free) but for want
 * of it in one piece, and cutting the frame into pieces does not help because
 * the pieces come out of the same heap.
 *
 * 100 KB is what fits, and it is enough for everything except the thing this
 * was for.  Whole frames on this board need the operating system out of the
 * way - a monitor firmware with no process model, where the whole 300 KB is
 * one pool.
 */
AG_APP_SIZED("REMTERM", "2.3", "argon", 0, 0, 100 * 1024);

#define LINK_PORT 1
#define LINK_BAUD 4000000u

#define FR_MAGIC0 0xA5u
#define FR_MAGIC1 0x5Au
#define FR_ROW    'R'
#define FR_CUR    'C'
#define FR_END    'E' /* that frame is whole: number, and how many bands     */
#define FR_RAW    'F' /* one slice of a whole frame, uncompressed            */
#define FR_RESEND 'R' /* ...which this end asks for again when one is missing */
#define FR_BAND   'B' /* pixels as they are                                  */
#define FR_PACK   'P' /* PackBits over the pixels                            */
#define FR_IDX    'I' /* a palette and indices                               */
#define FR_AUD    'A' /* a rate, then unsigned 8-bit mono samples            */
/*
 * And the four the sender learned after this program stopped being the one
 * that reads it: the picture as palette indices, the palette, the same indices
 * Huffman-coded, the coded blocks of a slice that moved, and the packet that
 * closes a frame.  All of them carry their sum inside the header like FR_RAW,
 * so they are counted and skipped here rather than mistaken for something with
 * a byte after the payload - which would cost the framing a packet each time.
 *
 * Reading them is not hard and is simply not done: the picture goes to the
 * standalone `monitor` firmware now, which can assemble a 150 KB frame whole
 * and this cannot (see monitor/CMakeLists.txt).  What REMTERM is still for is
 * the console and the sound, and those are unchanged.
 */
#define FR_IDX8   'X'
#define FR_PAL    'L'
#define FR_HUF    'H'
#define FR_HUFP   'P'
#define FR_DONE   'Z'

/*
 * Picture and sound are the same packet with a different body, and it is the
 * shape the sender's memory is already in - so a slice or a block of samples
 * leaves there in one DMA transfer with no processor in the path:
 *
 *   0   two magic bytes, the op, and a sixteen-bit length  (five bytes)
 *   5   twelve bytes of what-this-is: for a slice, frame / slice / slices /
 *       width / y / rows; for sound, rate / samples / sequence
 *   17  the sum, over those twelve and the body
 *   18  padding, to the cache line the sender's DMA wants
 *   32  the body
 *
 * The sum travels inside the header rather than after the payload because a
 * trailing byte would have to be written by somebody, and the only somebody
 * available after the body is the processor this arrangement exists to keep
 * out of the way.  Console rows and everything else still end with one.
 */
#define PKT_HDR   32u
#define PKT_GEOM  12u            /* summed bytes, from payload offset 0 */
#define PKT_SUM   12u            /* where the sum sits, in payload offsets */
#define PKT_BODY  (PKT_HDR - 5u) /* payload offset of the body */

/* Both numbers have to match remdisp.c exactly. */
#define BAND_ROWS      8u
#define BAND_MAX_PX    (BAND_ROWS * 320u)
#define FR_MAX_PAYLOAD (PKT_BODY + BAND_MAX_PX * 2u + BAND_MAX_PX / 64u + 16u)

/*
 * The widest picture this will take, and it is a memory decision.  The band
 * buffer has to hold whole rows of the far end's surface - ag_gfx_present wants
 * a stride that spans it - so the cost is surf_w * 8 * 2 bytes: five kilobytes
 * for 320, ten for 640.  A wider surface than this is refused rather than
 * drawn wrong.
 */
#define MAX_SURF_W 640u

/* ---- the codecs, each the mirror of one in remdisp.c ------------------- */

static uint16_t s_px[BAND_MAX_PX]; /* one rectangle, linear, before placing */

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

/*
 * PackBits over 16-bit pixels.  Returns false rather than writing past the end
 * when the stream disagrees with the pixel count it was promised - a frame that
 * survived its checksum can still have been built by a sender that does not
 * match this receiver, and a decoder that trusts a length is a decoder that
 * corrupts memory when it should refuse a frame.
 */
static bool unpack16(const uint8_t *in, uint32_t len, uint32_t npx)
{
    uint32_t at = 0;
    uint32_t out = 0;

    while (out < npx) {
        if (at >= len) {
            return false;
        }
        const uint8_t ctrl = in[at++];
        if (ctrl & 0x80u) {
            const uint32_t run = (uint32_t)(ctrl - 0x80u) + 2u;
            if (at + 2u > len || out + run > npx) {
                return false;
            }
            const uint16_t c = (uint16_t)rd16(in + at);
            at += 2u;
            for (uint32_t i = 0; i < run; i++) {
                s_px[out++] = c;
            }
        } else {
            const uint32_t lit = (uint32_t)ctrl + 1u;
            if (at + lit * 2u > len || out + lit > npx) {
                return false;
            }
            for (uint32_t i = 0; i < lit; i++) {
                s_px[out++] = (uint16_t)rd16(in + at);
                at += 2u;
            }
        }
    }
    return out == npx;
}

/* PackBits over bytes, into a caller's buffer. */
static bool unpack8(const uint8_t *in, uint32_t len, uint8_t *out, uint32_t n)
{
    uint32_t at = 0;
    uint32_t o = 0;

    while (o < n) {
        if (at >= len) {
            return false;
        }
        const uint8_t ctrl = in[at++];
        if (ctrl & 0x80u) {
            const uint32_t run = (uint32_t)(ctrl - 0x80u) + 2u;
            if (at >= len || o + run > n) {
                return false;
            }
            const uint8_t b = in[at++];
            for (uint32_t i = 0; i < run; i++) {
                out[o++] = b;
            }
        } else {
            const uint32_t lit = (uint32_t)ctrl + 1u;
            if (at + lit > len || o + lit > n) {
                return false;
            }
            for (uint32_t i = 0; i < lit; i++) {
                out[o++] = in[at++];
            }
        }
    }
    return o == n;
}

/* bpp, count-1, the palette, then PackBits over the packed indices. */
static bool unpack_indexed(const uint8_t *in, uint32_t len, uint32_t npx)
{
    static uint8_t  idx[BAND_MAX_PX];
    static uint16_t pal[256];

    if (len < 2u) {
        return false;
    }
    const uint32_t bpp = in[0];
    const uint32_t ncol = (uint32_t)in[1] + 1u;
    if ((bpp != 4u && bpp != 8u) || 2u + ncol * 2u > len) {
        return false;
    }
    for (uint32_t i = 0; i < ncol; i++) {
        pal[i] = (uint16_t)rd16(in + 2u + i * 2u);
    }

    const uint32_t at = 2u + ncol * 2u;
    const uint32_t bytes = (bpp == 4u) ? (npx + 1u) / 2u : npx;
    if (bytes > sizeof(idx)) {
        return false;
    }
    if (!unpack8(in + at, len - at, idx, bytes)) {
        return false;
    }

    if (bpp == 4u) {
        /* Backwards, so unpacking in place cannot overwrite a byte it still
         * needs: two pixels come out of every byte. */
        for (uint32_t i = npx; i-- > 0;) {
            const uint8_t byte = idx[i / 2u];
            const uint8_t v = (i & 1u) ? (uint8_t)(byte >> 4)
                                       : (uint8_t)(byte & 0x0fu);
            if (v >= ncol) {
                return false;
            }
            s_px[i] = pal[v];
        }
    } else {
        for (uint32_t i = 0; i < npx; i++) {
            if (idx[i] >= ncol) {
                return false;
            }
            s_px[i] = pal[idx[i]];
        }
    }
    return true;
}

/*
 * Bytes come off the port in blocks and are handed out one at a time.
 *
 * The obvious version - one ag_uart_read per byte - loses data, and the
 * arithmetic says why: a band is five kilobytes and 13 ms of wire at four
 * megabaud, while the kernel's receive buffer for a UART is one kilobyte.  A
 * read per byte is a call through the ABI and a lock each time and cannot drain
 * that fast; the first version of this lost 1070 bytes to exactly that.
 *
 * The timeout is short rather than generous on purpose: uart_read_bytes waits
 * for the length it was given or for the timeout, so a long timeout on a big
 * read would hold a keystroke back for as long as the line stayed quiet.
 */
/*
 * Eight kilobytes, so that a whole band fits in one bufferful and a frame that
 * turns out to be corrupt can be *un-read*.
 *
 * That is what the size is for now, and it is worth the eight kilobytes.  The
 * wire itself is good: a raw run of 4 MB at four megabaud gives 18 corrupt
 * blocks in 16114 - **one error in every 233 KB**.  But under the game the
 * receiver was throwing away 19% of everything that arrived, which is fifteen
 * frames lost per error, and the reason is that a stumble used to be
 * unrecoverable in one direction: the length byte of a corrupt frame is
 * garbage, so the reader consumed up to five kilobytes of whatever followed -
 * the *next* frames - and then went looking forward for a header it had
 * already swallowed.
 *
 * With the frame inside the buffer, a failed checksum rewinds to one byte past
 * the magic that started it and the scan carries on from there.  One error
 * costs one frame.
 */
#define RX_CHUNK 8192u

static uint8_t  s_rx[RX_CHUNK];
static uint32_t s_have;
static uint32_t s_at;
/* Where in s_rx the frame being read started, or RX_CHUNK if it began in a
 * bufferful that has since been replaced - in which case there is nothing to
 * rewind to and the old behaviour is all that is left. */
static uint32_t s_frame_at = RX_CHUNK;

static int next_byte(uint32_t ms)
{
    if (s_at >= s_have) {
        const int32_t n = ag_uart_read(LINK_PORT, s_rx, sizeof(s_rx), ms);
        if (n <= 0) {
            return -1;
        }
        s_have = (uint32_t)n;
        s_at = 0;
        s_frame_at = RX_CHUNK; /* the frame's start went with the old buffer */
    }
    return (int)s_rx[s_at++];
}

/* Put back everything after the magic bytes of the frame that just failed. */
/* Not more often than this, however bad the line gets - see the comment where
 * it is used. */
/* How often the receiver says what it has been getting - see the loop. */
#define REPORT_US 5000000ull

#define ASK_AGAIN_US 1000000ull
static ag_time_t s_asked_at;

/*
 * What this end sends back besides "send it again".
 *
 * The console of this board cannot be read while this program is showing
 * pixels: it holds the display, so a console line has nowhere to be drawn, and
 * taking the console back (Ctrl+backslash) takes the *focus* with it - after
 * which ag_gfx_acquire refuses and this program stops drawing for good, since
 * there is no way back to a user slot over a serial line.  So "it is drawing"
 * and "it is wedged" look identical from here: silence.
 *
 * Which meant that every look cost the thing being looked at, and a whole
 * evening went into telling those two states apart by restarting them.
 *
 * So the numbers go the other way, up the wire the picture came down.  The far
 * end prints them into its own console, which nobody is holding.  Sixteen
 * bytes every five seconds against a link that carries four hundred thousand.
 */
#define FR_STAT 'S'

static bool rewind_frame(void)
{
    if (s_frame_at >= s_have) {
        return false;
    }
    s_at = s_frame_at + 1u;
    s_frame_at = RX_CHUNK;
    return true;
}

static uint32_t get16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static bool parse_u32(const char *s, uint32_t *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    uint32_t v = 0;
    for (; *s != '\0'; ++s) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        const uint32_t d = (uint32_t)(*s - '0');
        if (v > (0xffffffffu - d) / 10u) {
            return false;
        }
        v = v * 10u + d;
    }
    *out = v;
    return true;
}

/* ---- the screen -------------------------------------------------------- */

static uint16_t *s_band;      /* surf_w * BAND_ROWS pixels, rows full width */
static uint32_t  s_band_w;    /* what it was sized for                      */
static bool      s_acquired;  /* the panel is ours; the console is suspended */

/* ---- one frame at a time ----------------------------------------------- */
/*
 * Why the bands of a frame are held back and put on the glass together.
 *
 * A band is a stripe, and this program used to put each one on the panel as it
 * arrived.  Watching that, a moving picture reaches the eye as a wipe down the
 * screen - the running bands - and worse, nobody can count anything: the
 * sender's own instrument reported "2.41 screens a second" while the person
 * watching the glass measured about one screen every three seconds, and both
 * were right.  The sender was adding up *pixels* and dividing by the area of a
 * screen, so a dialog box repainted forty times counted as several screens
 * while the screen as a whole was never once refreshed.  A number like that
 * cannot be argued with and cannot be used.
 *
 * So the far end now marks the end of a frame, and this end holds a frame's
 * bands until the marker arrives, then decodes and presents them in one go and
 * counts one frame.  That count is the frame rate a person sees, by
 * construction, because it is the number of times the picture was made whole.
 *
 * What is held is the *encoded* bands, not the pixels.  A screen of pixels is
 * 150 KB and this board has 125 KB free with 92 KB of it in one piece, so a
 * second framebuffer was never possible here - that is measured, not assumed.
 * Encoded, the same screen is a few kilobytes: measured on a Fallout map, a
 * frame is 6.4 KB, because the far end sends whichever of raw, PackBits or
 * palette-and-indices came out smallest.
 *
 * How much to ask for is therefore not a constant either, and getting it wrong
 * wedged the board.  Sixty-four kilobytes could not be had at all - this
 * program's own forty kilobytes of buffers come out of the same heap, and what
 * is left in one piece after it loads is about sixty-five, not the ninety-two
 * the shell reports before it.  Forty-eight *could* be had, which was worse:
 * it left sixteen kilobytes for everything else, the panel driver could not
 * get what it needs to paint, and the whole board went quiet with the display
 * held and the console suspended - no fault, no journal entry, nothing to read.
 *
 * So the ask starts where the need is, not where the heap is.  A frame is 6.4
 * KB measured on a Fallout map, so twenty-four kilobytes holds three of them
 * and leaves forty for the rest of the machine.  A frame that does not fit is
 * presented in two halves rather than dropped, and says so in frames_split.
 */
static const uint32_t k_pend_try[] = {24576u, 16384u, 8192u};

static uint8_t *s_pend;      /* [op][len32][payload] ... , or NULL if no room */
static uint32_t s_pend_cap;
static uint32_t s_pend_used;
static uint32_t s_pend_over; /* frames that had to be split; want this at 0   */

/* The counters.  At file scope because the band path is a function now, and
 * every one of them is only ever touched from the reader. */
static uint32_t rows;         /* console rows applied                        */
static uint32_t bands;        /* bands presented                             */
static uint32_t px;           /* pixels presented, in thousands              */
static uint32_t bad;          /* frames whose checksum or shape failed       */
static uint32_t s_newfmt;     /* packets this program is too old to read     */
static uint32_t stray;        /* bytes dropped hunting for a header          */
static uint32_t pxlow;
static uint32_t packedframes;
static uint32_t idxframes;
static uint32_t wire;         /* payload bytes accepted, all kinds           */
/*
 * And the pixel bytes on their own, because `wire` cannot answer the question
 * it looks like it answers.  It counts sound and end-markers too, so dividing
 * it by the pixels presented gives a compression ratio with 22 KB/s of audio
 * folded into it - which is how "the bands compress 7.4 times" and "the bands
 * compress 1.3 times" were both derived from the same run.  A ratio is only
 * worth quoting when its numerator and denominator describe the same bytes.
 */
static uint32_t wire_px;      /* payload bytes of pixel bands only           */
static uint32_t frames;       /* whole frames put on the glass               */
static uint32_t framesmiss;   /* gaps in the numbering                       */
static uint32_t framespart;   /* frames with fewer bands than promised       */
static uint32_t framesresync; /* the sender began counting again             */
static uint32_t inframe;      /* bands presented since the last marker       */
static uint32_t nextframe;    /* the frame number expected next              */
static bool     numbered;
static uint64_t us_decode;
static uint64_t us_present;
static ag_time_t first_px;
static ag_time_t last_px;

/*
 * Three counters, because "no pixels reached the glass" has three causes with
 * one appearance, and a run that reported 509 frames closed and nought
 * rectangles presented could not say which.  Guessing cost an hour; a counter
 * costs three words each.
 */
static uint32_t s_no_width;  /* a surface this end cannot hold           */
static uint32_t s_no_mem;    /* no room for one band of it               */
static uint32_t s_no_acq;    /* the display would not be given to us     */
static uint32_t s_no_pres;   /* present() refused the band               */

static bool want_pixels(uint32_t surf_w)
{
    if (surf_w == 0 || surf_w > MAX_SURF_W) {
        s_no_width++;
        return false;
    }
    if (s_band == NULL || s_band_w != surf_w) {
        ag_free(s_band);
        s_band = (uint16_t *)ag_malloc((size_t)surf_w * BAND_ROWS *
                                       sizeof(uint16_t));
        if (s_band == NULL) {
            s_band_w = 0;
            s_no_mem++;
            return false;
        }
        s_band_w = surf_w;
    }
    if (!s_acquired) {
        ag_gfxinfo_t gi;
        if (ag_gfx_acquire(&gi) != AG_OK) {
            s_no_acq++;
            return false;
        }
        s_acquired = true;
    }
    return true;
}

static void want_text(void)
{
    if (s_acquired) {
        ag_gfx_release();
        s_acquired = false;
    }
}

/*
 * Decode one band and put it on the panel.  A function rather than a limb of
 * the reader because it is now called from two places: straight from the wire
 * when nothing is being held back, and from pend_flush when a whole frame is
 * being laid down at once.
 */
static void show_band(uint32_t op, const uint8_t *payload, uint32_t len)
{
    const uint32_t bx = get16(payload + 0);
    const uint32_t by = get16(payload + 2);
    const uint32_t bw = get16(payload + 4);
    const uint32_t bh = get16(payload + 6);
    const uint32_t sw = get16(payload + 8);
    const uint32_t sh = get16(payload + 10);
    const uint32_t npx = bw * bh;

    if (bw == 0 || bh == 0 || bh > BAND_ROWS || bx + bw > sw ||
        npx > BAND_MAX_PX) {
        bad++;
        return;
    }

    const ag_time_t t_dec = g_ag_api->time->us();
    const uint8_t *body = payload + 12u;
    const uint32_t blen = len - 12u;
    bool           ok;
    if (op == FR_BAND) {
        if (blen < npx * 2u) {
            bad++;
            return;
        }
        for (uint32_t i = 0; i < npx; i++) {
            s_px[i] = (uint16_t)rd16(body + i * 2u);
        }
        ok = true;
    } else if (op == FR_PACK) {
        ok = unpack16(body, blen, npx);
    } else {
        ok = unpack_indexed(body, blen, npx);
        if (ok) {
            idxframes++;
        }
    }
    if (!ok) {
        bad++;
        return;
    }
    if (op != FR_BAND) {
        packedframes++;
    }
    us_decode += (uint64_t)(g_ag_api->time->us() - t_dec);
    wire += len;
    wire_px += len;

    if (!want_pixels(sw)) {
        return;
    }

    /*
     * Into rows the width of the whole surface, at the rectangle's own
     * column: present() is given a stride that spans the surface and a
     * pointer to the rectangle's first pixel, and reads only bw of each
     * row.  What sits outside the rectangle is never looked at.
     */
    for (uint32_t r = 0; r < bh; r++) {
        uint16_t *dst = s_band + r * sw + bx;
        for (uint32_t i = 0; i < bw; i++) {
            dst[i] = s_px[r * bw + i];
        }
    }

    const ag_blit_t blit = {
        .px = s_band + bx,
        .stride = sw * sizeof(uint16_t),
        .surf_w = (uint16_t)sw,
        .surf_h = (uint16_t)sh,
        .x = (uint16_t)bx,
        .y = (uint16_t)by,
        .w = (uint16_t)bw,
        .h = (uint16_t)bh,
    };
    const ag_time_t t_pres = g_ag_api->time->us();
    const ag_err_t  shown = ag_gfx_present(&blit);
    us_present += (uint64_t)(g_ag_api->time->us() - t_pres);
    if (shown == AG_OK) {
        bands++;
        inframe++;
        pxlow += bw * bh;
        while (pxlow >= 1000u) {
            pxlow -= 1000u;
            px++;
        }
        if (first_px == 0) {
            first_px = g_ag_api->time->us();
        }
        last_px = g_ag_api->time->us();
    } else {
        s_no_pres++;
    }
}

/* Hold a band back.  False means it will not fit and the caller must flush. */
static bool pend_add(uint32_t op, const uint8_t *payload, uint32_t len)
{
    if (s_pend == NULL || len + 5u > s_pend_cap) {
        return false;
    }
    if (s_pend_used + 5u + len > s_pend_cap) {
        return false;
    }
    uint8_t *at = s_pend + s_pend_used;
    at[0] = (uint8_t)op;
    at[1] = (uint8_t)len;
    at[2] = (uint8_t)(len >> 8);
    at[3] = (uint8_t)(len >> 16);
    at[4] = (uint8_t)(len >> 24);
    memcpy(at + 5u, payload, len);
    s_pend_used += 5u + len;
    return true;
}

/* Put everything held on the glass, in the order it arrived. */
static void pend_flush(void)
{
    uint32_t at = 0;
    while (at + 5u <= s_pend_used) {
        const uint32_t op = s_pend[at];
        const uint32_t len = (uint32_t)s_pend[at + 1] |
                             ((uint32_t)s_pend[at + 2] << 8) |
                             ((uint32_t)s_pend[at + 3] << 16) |
                             ((uint32_t)s_pend[at + 4] << 24);
        if (at + 5u + len > s_pend_used) {
            break;
        }
        show_band(op, s_pend + at + 5u, len);
        at += 5u + len;
    }
    s_pend_used = 0;
}

/* ---- whole frames, assembled here -------------------------------------- */
/*
 * The far end sends the picture as it is: the whole frame, cut into numbered
 * slices of eight rows, no trimming and no compression.  This end puts it back
 * together and shows it only when it is whole, so what appears on the glass is
 * a frame and not a stripe.
 *
 * A frame is 150 KB and no board here will give 150 KB in one piece - with the
 * radios out of the image this one has 259 KB free and 108 in the largest
 * block.  That turns out not to matter, and the reason is worth writing down:
 * the frame does not need to be *contiguous*, it needs to reach the panel
 * without waiting for the wire in between.  So it is assembled in two or three
 * pieces and they are pushed to the panel one after another with nothing
 * between them but SPI - the whole screen in fifteen to twenty milliseconds,
 * which to an eye is a frame changing.
 *
 * What made the old picture crawl was never the size of the buffer.  It was
 * that each stripe went to the glass as it arrived, three hundred and seventy
 * milliseconds apart across a frame.
 *
 * A slice that does not arrive is asked for by number - 'R', frame, slice -
 * while the far end still has that frame.  A frame with a hole in it is not
 * shown; the next one carries the same pixels anyway.
 */
#define RAW_ROWS   8u
/*
 * How many pieces the frame may be assembled in.
 *
 * The heap here is 208 KB free with 108 in the largest block, so one piece of
 * 150 KB is out and the frame has to be split.  Splitting it further costs
 * nothing an eye can see: the pieces go to the panel one after another with no
 * wire wait between them, so the count changes the number of SPI transfers and
 * not what appears.
 *
 * What did cost something was splitting it *wrongly*.  The first version
 * rounded each piece up to a whole number of slices, so twelve pieces of a
 * 240-row frame came to 288 rows - 180 KB asked for to hold 150, and at four
 * pieces it was 164 KB.  Every rung of the ladder therefore asked for more
 * than the frame and failed on a board that had room for the frame, and the
 * receiver drew nothing at all while reporting no error anybody had thought to
 * count.
 *
 * So the pieces divide the frame exactly: thirty slices split into 1, 2, 3, 5,
 * 6, 10, 15 or 30 pieces, every one of them a whole number of slices and the
 * total always exactly the frame.
 */
#define RAW_PIECES 30u
static const uint32_t k_split[] = {1u, 2u, 3u, 5u, 6u, 10u, 15u, 30u};
#define RAW_W      320u
#define RAW_H      240u

static uint8_t *s_piece[RAW_PIECES];
static uint32_t s_pieces;        /* how many were needed                     */
static uint32_t s_piece_rows;    /* rows each one holds                      */
static uint32_t s_asm_bytes;     /* what was taken, for the banner           */
static uint32_t s_asm_best;      /* most pieces any attempt managed          */
static uint32_t s_asm_best_of;   /* ...out of how many it needed             */
static uint32_t s_asm_bytes_each;

static uint32_t s_cur_frame = 0xffffffffu;
static uint32_t s_have_mask;     /* slices of this frame that arrived        */
static uint32_t s_want_mask;     /* slices this frame should have            */
static uint32_t s_ask_mask;      /* ...and which have already been asked for */
static uint32_t s_frame_w = RAW_W;
static uint32_t s_frame_h = RAW_H;

static uint32_t s_raw_slices;    /* slices taken in                          */
static uint32_t s_raw_frames;    /* frames put on the glass whole            */
static uint32_t s_raw_partial;   /* frames dropped with a hole in them       */
static uint32_t s_raw_asked;     /* resend requests sent                     */
static uint64_t s_raw_present;   /* microseconds spent pushing to the panel  */

/*
 * Take the frame in as few pieces as the heap allows.  One is best and has
 * never happened; two or three is the ordinary answer and costs nothing.
 */
static void asm_alloc(void)
{
    for (uint32_t k = 0; k < sizeof(k_split) / sizeof(k_split[0]); k++) {
        const uint32_t n = k_split[k];
        /* Exactly, with nothing over: RAW_H is thirty slices and every n here
         * divides it, so n pieces hold the frame and not a row more. */
        const uint32_t rows = RAW_H / n;
        const uint32_t bytes = rows * RAW_W * 2u;
        /*
         * Out of the process arena first, and out of the system's internal
         * memory when that runs out.
         *
         * Those are two different pools and only the second one makes this
         * possible.  A process heap is a single allocation - the largest free
         * block on this board is 108 KB - so a 150 KB frame cannot come from
         * the arena however it is cut.  But ag_malloc_caps(AG_MEM_FAST) does
         * not go to the arena at all: the kernel serves it from the system
         * heap, where another 80 KB sits.  Ninety-eight plus eighty is a
         * hundred and seventy-eight, and the frame is a hundred and fifty.
         */
        uint32_t got = 0;
        for (uint32_t i = 0; i < n; i++) {
            s_piece[i] = (uint8_t *)ag_malloc(bytes);
            if (s_piece[i] == NULL) {
                s_piece[i] = (uint8_t *)ag_malloc_caps(bytes, AG_MEM_FAST);
            }
            if (s_piece[i] == NULL) {
                break;
            }
            got++;
        }
        if (got > s_asm_best) {
            s_asm_best = got;
            s_asm_best_of = n;
            s_asm_bytes_each = bytes;
        }
        if (got == n) {
            s_pieces = n;
            s_piece_rows = rows;
            s_asm_bytes = bytes * n;
            return;
        }
        for (uint32_t i = 0; i < got; i++) {
            ag_free(s_piece[i]);
            s_piece[i] = NULL;
        }
    }
}

static void ask_again(uint32_t frame, uint32_t slice)
{
    uint8_t req[5];
    req[0] = (uint8_t)FR_RESEND;
    req[1] = (uint8_t)frame;
    req[2] = (uint8_t)(frame >> 8);
    req[3] = (uint8_t)slice;
    req[4] = (uint8_t)(slice >> 8);
    (void)ag_uart_write(LINK_PORT, req, sizeof(req));
    s_raw_asked++;
}

/* The frame is whole: push every piece, back to back, nothing in between. */
static void frame_show(void)
{
    if (!want_pixels(s_frame_w)) {
        return;
    }
    const ag_time_t t0 = g_ag_api->time->us();
    for (uint32_t i = 0; i < s_pieces; i++) {
        const uint32_t y0 = i * s_piece_rows;
        if (y0 >= s_frame_h) {
            break;
        }
        uint32_t rows = s_piece_rows;
        if (y0 + rows > s_frame_h) {
            rows = s_frame_h - y0;
        }
        const ag_blit_t blit = {
            .px = (const uint16_t *)s_piece[i],
            .stride = s_frame_w * sizeof(uint16_t),
            .surf_w = (uint16_t)s_frame_w,
            .surf_h = (uint16_t)s_frame_h,
            .x = 0,
            .y = (uint16_t)y0,
            .w = (uint16_t)s_frame_w,
            .h = (uint16_t)rows,
        };
        if (ag_gfx_present(&blit) != AG_OK) {
            s_no_pres++;
        }
    }
    s_raw_present += (uint64_t)(g_ag_api->time->us() - t0);
    s_raw_frames++;
    if (first_px == 0) {
        first_px = t0;
    }
    last_px = g_ag_api->time->us();
}

static void raw_slice(const uint8_t *p, uint32_t len)
{
    const uint32_t frame = get16(p + 0);
    const uint32_t slice = get16(p + 2);
    const uint32_t slices = get16(p + 4);
    const uint32_t w = get16(p + 6);
    const uint32_t y0 = get16(p + 8);
    const uint32_t rows = get16(p + 10);

    if (w == 0u || w > RAW_W || rows == 0u || rows > RAW_ROWS ||
        slices == 0u || slices > 32u || slice >= slices ||
        y0 + rows > RAW_H || len < PKT_BODY + rows * w * 2u) {
        bad++;
        return;
    }
    s_raw_slices++;
    wire_px += len;

    /*
     * With no buffer, show the slice where it stands.
     *
     * Assembling the frame is the better picture and it is not worth a blank
     * screen: when the allocation fails - which it did, on every layout from
     * one piece to twelve - the old behaviour is still a picture, a stripe at
     * a time, and a stripe at a time is what this link did for weeks.  The
     * first version returned here instead, so a receiver that could not get
     * its buffer took every slice in and drew nothing at all, for ever,
     * without saying so.
     */
    if (s_pieces == 0u) {
        if (!want_pixels(w)) {
            return;
        }
        const ag_blit_t one = {
            .px = (const uint16_t *)(p + PKT_BODY),
            .stride = w * sizeof(uint16_t),
            .surf_w = (uint16_t)w,
            .surf_h = (uint16_t)(slices * RAW_ROWS),
            .x = 0,
            .y = (uint16_t)y0,
            .w = (uint16_t)w,
            .h = (uint16_t)rows,
        };
        const ag_time_t t0 = g_ag_api->time->us();
        if (ag_gfx_present(&one) == AG_OK) {
            if (first_px == 0) {
                first_px = t0;
            }
            last_px = g_ag_api->time->us();
        } else {
            s_no_pres++;
        }
        s_raw_present += (uint64_t)(g_ag_api->time->us() - t0);
        return;
    }

    if (frame != s_cur_frame) {
        /* The one before did not finish: say what was missing, then let go. */
        if (s_cur_frame != 0xffffffffu && s_have_mask != s_want_mask) {
            s_raw_partial++;
        }
        s_cur_frame = frame;
        s_frame_w = w;
        s_frame_h = slices * RAW_ROWS;
        if (s_frame_h > RAW_H) {
            s_frame_h = RAW_H;
        }
        s_have_mask = 0;
        s_ask_mask = 0;
        s_want_mask = (slices >= 32u) ? 0xffffffffu : ((1u << slices) - 1u);
    }

    /*
     * Out of order means one went missing: ask for it, once, while the far end
     * still holds this frame.
     *
     * Once is the whole of it.  The first version asked for every absent
     * earlier slice on every slice that arrived, which on a frame that was
     * never going to complete came to a hundred and fourteen thousand requests
     * in twenty seconds - five bytes each, against the picture, on the same
     * wire.  A request that has been made is not information until it has been
     * answered or the frame has gone.
     */
    for (uint32_t i = 0; i < slice; i++) {
        const uint32_t bit = 1u << i;
        if ((s_have_mask & bit) == 0u && (s_ask_mask & bit) == 0u) {
            s_ask_mask |= bit;
            ask_again(frame, i);
        }
    }

    const uint32_t piece = y0 / s_piece_rows;
    if (piece >= s_pieces) {
        bad++;
        return;
    }
    const uint32_t off = (y0 % s_piece_rows) * w * 2u;
    memcpy(s_piece[piece] + off, p + PKT_BODY, (size_t)rows * w * 2u);
    s_have_mask |= (1u << slice);

    if (s_have_mask == s_want_mask) {
        frame_show();
        s_cur_frame = 0xffffffffu; /* shown; the next slice starts a new one */
    }
}

/* ---- the sound --------------------------------------------------------- */
/*
 * The other half of being a monitor: this board has a converter on GPIO26 with
 * an amplifier behind it, and the board at the far end has neither.  So the
 * wire carries samples as well as pixels, and they come out here.
 *
 * The hard part is not the bytes - 22 KB/s of the 400 the link carries - it is
 * that **a write to the converter blocks and this wire cannot wait.**
 *
 * /dev/pcm0 gathers samples until it has 93 ms of them and then hands the lot
 * to the DAC, which takes 93 ms to play them; so in steady state every write
 * that completes an accumulator sits there for most of a tenth of a second
 * waiting for room.  That is not a fault - it is what pacing to real time means
 * - but the link has no flow control, so during that tenth of a second the
 * UART's buffer overflows and everything in it is lost.
 *
 * That was measured before it was reasoned about, and the numbers are worth
 * keeping: in forty seconds of a static main menu the receiver played 711424
 * samples and dropped **none**, while the picture managed 35 rectangles and
 * 163 KB of arriving bytes were thrown away as garbage looking for a frame
 * header.  Perfect sound, no picture: the audio was blocking the reader, and
 * because the sender never learns any of this, everything looked healthy at
 * the other end.
 *
 * So the converter gets a thread of its own.  The reader stays in its loop
 * doing nothing but pulling bytes off the wire; the writer blocks all it likes
 * on its own stack.  The ring between them is 186 ms, which is longer than any
 * single band of pixels can hold the line and short enough that a sound let go
 * is not still playing.
 *
 * Nothing here opens the converter until the first sample arrives.  A monitor
 * that is only showing a picture should not be holding an amplifier open,
 * humming.
 */
/*
 * 743 ms of sound, and the size is set by how long the *sender* can go without
 * sending any.
 *
 * On the S3 the kernel holds its device registry for the whole of a blit, and
 * one blit is every band of a frame - so while a full screen is going out, the
 * task that produces sound cannot reach the wire at all.  A frame that takes
 * two hundred milliseconds is two hundred milliseconds of nothing arriving
 * here, and at 186 ms of ring that is silence: "a quarter of a second plays, a
 * fifth does not", which is how it was reported and what it measured.
 *
 * Sixteen kilobytes of a board that has ninety free, against a stutter that
 * makes music unrecognisable.
 */
#define AUD_RING  17640u /* samples: 0.8 s at 22050, 17 KB                   */
#define AUD_BITE    256u /* samples handed over at once: 11.6 ms            */

static ag_handle_t s_pcm = -1;
static uint32_t    s_pcm_rate;
static uint8_t     s_aring[AUD_RING];
static uint32_t    s_aring_r;
static uint32_t    s_aring_w;
static volatile uint32_t s_aring_used;
static uint32_t    s_asamples;  /* accepted off the wire                    */
static uint32_t    s_aplayed;   /* handed to the converter                  */
static uint32_t    s_adropped;  /* the ring was full                        */
static uint32_t    s_agaps;     /* the ring ran dry while playing: a stutter */
static bool        s_astarved;  /* the ring is empty right now                */
static ag_time_t   s_astarved_at;
static bool        s_agap_counted;

/*
 * The lead built before playback starts or resumes, in samples.
 *
 * This is the whole answer to the choppy sound, and the size is chosen against
 * a measured enemy.  Sound and picture share one wire, and a whole screen sent
 * at once holds that wire - and so keeps sound off it - for up to three
 * hundred milliseconds.  A cushion smaller than that empties during the gap
 * and the speaker plays silence; two hundred milliseconds, which is what this
 * was, could not cover a three hundred millisecond hole, so it still stuttered.
 *
 * Five hundred milliseconds covers the worst hole with two hundred to spare.
 * The cost is half a second of delay between a sound happening in the game and
 * being heard, which for music and ambience nobody notices, and the ring is a
 * second and a half so there is room for the bursts that refill it between
 * holes.  The only real limit is that if the holes came closer together than
 * the buffer refills the buffer would still lose - but on average the wire
 * delivers audio as fast as it is played, so between two holes it refills.
 */
#define AUD_CUSHION 11025u /* 500 ms */
#define AUD_DAC_US  90000ull
static volatile uint32_t s_arate;  /* what the far end says it is sending    */
static volatile int      s_astop;
static ag_thread_t s_athread;
static ag_mutex_t  s_alock;

/*
 * One producer (the reader) and one consumer (the thread), so the ring itself
 * needs no lock for its data - but the two indices and the count do, because
 * dropping the oldest moves the *reader's* index from the producer's side.
 * The lock is held for a few instructions and never across a device call.
 */
static void aud_push(const uint8_t *p, uint32_t n)
{
    if (!ag_mutex_lock(s_alock, 20u)) {
        return; /* the player is busy; this much sound is lost, not the link */
    }
    for (uint32_t i = 0; i < n; i++) {
        if (s_aring_used == AUD_RING) {
            /*
             * Drop the oldest, not the newest.  What is oldest is what the
             * converter is furthest from wanting, and dropping the newest would
             * hold a stale ring for ever once it filled.
             */
            s_aring_r = (s_aring_r + 1u) % AUD_RING;
            s_aring_used--;
            s_adropped++;
        }
        s_aring[s_aring_w] = p[i];
        s_aring_w = (s_aring_w + 1u) % AUD_RING;
        s_aring_used++;
    }
    s_asamples += n;
    ag_mutex_unlock(s_alock);
}

/* The converter, opened at the rate the far end says it is sending. */
static bool aud_open(uint32_t rate)
{
    if (s_pcm >= 0 && s_pcm_rate == rate) {
        return true;
    }
    if (s_pcm >= 0) {
        (void)ag_dev_close(s_pcm);
        s_pcm = -1;
    }
    if (rate < 8000u || rate > 48000u) {
        return false;
    }
    s_pcm = ag_dev_open("/dev/pcm0");
    if (s_pcm < 0) {
        return false;
    }
    const ag_audio_fmt_t fmt = {.rate = rate, .channels = 1u, .bits = 16u};
    (void)ag_dev_ioctl(s_pcm, AG_IOC_AUDIO_SETFMT, (void *)&fmt, sizeof(fmt));
    s_pcm_rate = rate;
    return true;
}

/*
 * The player.  Everything that may block lives here and nowhere else.
 *
 * It does not ask how much room the converter has: it just writes, and the
 * write returns when the sound has somewhere to go.  That blocking *is* the
 * clock - it is what keeps this thread from spinning and what makes the ring
 * fill and empty at the rate the far end is producing.
 */
static void aud_player(void *arg)
{
    (void)arg;
    static int16_t block[AUD_BITE];

    /*
     * A cushion, because a ring that is drained the moment anything lands in
     * it is a ring that is empty at every jitter.
     *
     * The far end sends sound in 256-sample frames, in bursts, between bands
     * of picture that take tens of milliseconds each; the converter plays
     * continuously.  Play what arrives as it arrives and the ring hovers at
     * zero, so every late burst is a hole - the receiver counted 75-97 of them
     * every five seconds at the main menu, fifteen to twenty a second, and the
     * music sounded exactly like that.  So playback does not start, or
     * restart after a hole, until AUD_CUSHION samples are waiting: about two
     * hundred milliseconds of lead, out of a ring that holds seven hundred.
     * Fewer, longer pauses instead of a stutter, and only when the wire has
     * really stopped delivering.
     */
    bool primed = false;

    while (!s_astop) {
        const uint32_t rate = s_arate;
        if (rate != 0u && !primed) {
            if (s_aring_used < AUD_CUSHION) {
                ag_delay(5);
                continue;
            }
            primed = true;
        }
        if (rate == 0u || s_aring_used == 0u) {
            primed = false; /* build the cushion again before resuming */
            /*
             * An empty ring while the sound is supposed to be playing is a
             * gap the speaker plays as silence.  Counted as one gap per run of
             * empty polls, not one per poll: "how many times did it stutter"
             * is the question, and a 300 ms hole is one hole.
             *
             * This is the number the first version did not have.  It counted
             * the ring overflowing - samples that arrived with nowhere to go -
             * and reported "0 lost" while the music was audibly breaking up,
             * because the loss was the other way round: the picture had the
             * wire, the samples arrived late, and the ring ran dry.  A counter
             * that cannot see the fault being reported is worse than none,
             * since it argues with the person.
             */
            /*
             * Counted only once the converter's own buffer must also be dry:
             * it holds about ninety milliseconds, so a ring that is empty for
             * less than that has not yet been heard.  A gap is what the ear
             * would call one.
             */
            if (rate != 0u && s_aplayed != 0u) {
                const ag_time_t now = g_ag_api->time->us();
                if (!s_astarved) {
                    s_astarved = true;
                    s_astarved_at = now;
                } else if (!s_agap_counted &&
                           (uint64_t)(now - s_astarved_at) >= AUD_DAC_US) {
                    s_agap_counted = true;
                    s_agaps++;
                }
            }
            ag_delay(5);
            continue;
        }
        s_astarved = false;
        s_agap_counted = false;
        if (!aud_open(rate)) {
            ag_delay(50);
            continue;
        }

        uint32_t n = 0;
        if (ag_mutex_lock(s_alock, 100u)) {
            n = s_aring_used;
            if (n > AUD_BITE) {
                n = AUD_BITE;
            }
            for (uint32_t i = 0; i < n; i++) {
                /* Unsigned eight-bit back to signed sixteen: the inverse of
                 * what the far end did to it, and the only shape pcm0 takes. */
                const int32_t v = (int32_t)s_aring[s_aring_r] - 128;
                block[i] = (int16_t)(v << 8);
                s_aring_r = (s_aring_r + 1u) % AUD_RING;
                s_aring_used--;
            }
            ag_mutex_unlock(s_alock);
        }
        if (n == 0u) {
            ag_delay(5);
            continue;
        }
        const int32_t wrote = ag_dev_write(s_pcm, block, n * sizeof(int16_t));
        if (wrote > 0) {
            s_aplayed += (uint32_t)wrote / sizeof(int16_t);
        }
    }

    if (s_pcm >= 0) {
        (void)ag_dev_close(s_pcm);
        s_pcm = -1;
    }
}

int ag_main(int argc, char **argv)
{
    uint32_t baud = LINK_BAUD;
    if (argc >= 2 && (!parse_u32(argv[1], &baud) || baud == 0)) {
        ag_printf("usage: remterm [baud] [secs] [whole]\n");
        return 1;
    }
    /*
     * Zero means "until a key", which is what a person wants.  A number is for
     * the harness: a run that ends on its own can be scripted, and a terminal
     * that only ends on a keystroke cannot be, because the keystroke would have
     * to travel down the same console the harness is reading.
     */
    uint32_t secs = 0;
    if (argc >= 3 && !parse_u32(argv[2], &secs)) {
        ag_printf("usage: remterm [baud] [secs] [whole]\n");
        return 1;
    }
    /*
     * Holding a frame's bands and presenting them together is the default now,
     * and it was not when it arrived: the first version of it put nothing on
     * the glass at all, so it went in behind an argument until it was
     * understood.  What it needed was not the buffer but a sender whose frames
     * contain more than one band - the repair sweep was emitting a frame per
     * stripe, so there was nothing to hold and holding it changed nothing.
     * Pass 0 to go back to a band at a time.
     */
    uint32_t whole = 1;
    if (argc >= 4 && !parse_u32(argv[3], &whole)) {
        ag_printf("usage: remterm [baud] [secs] [whole]\n");
        return 1;
    }

    if (!AG_HAS(g_ag_api->gfx, present)) {
        ag_printf("remterm: this kernel has no gfx->present (ABI 0.31)\n");
        return 1;
    }

    const ag_err_t err = g_ag_api->io->uart_config(LINK_PORT, baud, 8, 0, 1);
    if (err != AG_OK) {
        ag_printf("uart%d at %u baud: %s\n", LINK_PORT, (unsigned)baud,
                  ag_strerror(err));
        ag_printf("is [uart%d] tx/rx set in C:\\BOARD.CFG?\n", LINK_PORT);
        return 1;
    }

    /*
     * One byte to say "I have just started; tell me everything".  Without it a
     * monitor that was switched off and back on stays dark until something on
     * the far end happens to change, because over there a shadow of the last
     * frame says this end already has it.  It also removes any order of
     * starting: the receiver may come up long after the sender.
     */
    {
        const uint8_t hello = (uint8_t)'H';
        (void)ag_uart_write(LINK_PORT, &hello, 1u);
    }

    /*
     * The player, started before anything can arrive rather than on the first
     * sample: creating a thread takes a stack out of internal memory, and doing
     * that from inside the reading loop would be a pause in the reading loop at
     * the exact moment sound started - which is the moment there is most to
     * lose.  It sleeps until s_arate says the far end is sending.
     */
    s_alock = ag_mutex_create();
    if (s_alock != NULL) {
        s_athread = ag_thread_create(aud_player, NULL, "remsnd", 4096, 2, 0);
    }

    /*
     * The buffer for a frame's worth of encoded bands, as big as the heap will
     * give - and only when asked for, which is why there is a third argument.
     *
     * Holding frames whole is the right thing and it is not yet the safe
     * thing: the first version of it put nothing on the glass at all - 509
     * frames closed, nought rectangles presented - and a person watching had a
     * blank screen while the counters looked busy.  A change that can do that
     * does not belong on by default while it is still being understood.  So:
     * `remterm <baud> <secs> 1` to hold frames, nothing to behave as before.
     */
    /*
     * Nothing is allocated for the band path any more.  The far end sends
     * whole frames now, so the twenty-four kilobytes that used to hold a
     * frame's worth of encoded bands were being taken away from the buffer
     * that assembles the frame - which needs every byte it can get.
     */
    (void)whole;

    asm_alloc();

    ag_coninfo_t con;
    ag_coninfo(&con);

    ag_cls();
    ag_printf("remterm: %u baud, %ux%u cells, bands up to %ux%u\n",
              (unsigned)baud, (unsigned)con.cols, (unsigned)con.rows,
              (unsigned)MAX_SURF_W, (unsigned)BAND_ROWS);
    if (s_pieces != 0u) {
        ag_printf("frame assembled in %u piece(s), %u KB, %u rows each\n",
                  (unsigned)s_pieces, (unsigned)(s_asm_bytes / 1024u),
                  (unsigned)s_piece_rows);
    } else {
        /*
         * And say what was available, because "no memory" is not a finding.
         * Every guess about this failure so far has been wrong - the shape of
         * the heap, the size of the pieces, the code arena - and each one cost
         * a build and a flash to disprove.  The board knows the answer.
         */
        ag_meminfo_t mi;
        ag_meminfo(&mi);
        ag_printf("no frame buffer: got %u of %u x %u B\n",
                  (unsigned)s_asm_best, (unsigned)s_asm_best_of,
                  (unsigned)s_asm_bytes_each);
        ag_printf("arena %u free %u big, fast %u\n",
                  (unsigned)mi.arena_free, (unsigned)mi.arena_largest,
                  (unsigned)mi.fast_free);
    }

    /*
     * Nothing below this line may print.  A printed line scrolls the console,
     * and the far end sends only the rows it thinks changed - so one scroll
     * here and the two screens disagree until its repair sweep catches up.
     */
    /* The counters live at file scope now; see "one frame at a time". */
    /*
     * What the encodings actually bought, which is the only way to know: the
     * sender picks per rectangle, so the answer is a property of the picture and
     * not of the code.  `wire` is payload bytes accepted; against 2 * pixels it
     * is the compression, and the two counts say which encoding won.
     */
    uint32_t wirekb = 0;
    /*
     * Frames, which is what a person watching counts, and until the sender
     * started marking the end of one there was no way to count them here: the
     * wire carried stripes, and thirty stripes might be one frame or thirty.
     *
     * `frames` is whole frames the marker closed.  `framesmiss` comes from
     * gaps in the numbering - the only way loss is visible at all on this link,
     * since a band that never arrives leaves nothing behind to notice.  And
     * `framespart` counts frames that arrived with fewer bands than they said
     * they had, which is a stripe lost inside a frame that did arrive.
     */

    /*
     * Where the time goes on this side, because the answer decided what to fix
     * next and could not be guessed.  The wire runs at 12% on average, so
     * nothing is short of bandwidth - what happens is that a whole frame
     * arrives in one burst of thirty bands and this end falls behind it.  These
     * two say by how much and at which of the two steps.
     */


    const ag_time_t t0 = g_ag_api->time->us();
    const uint64_t  window = (uint64_t)secs * 1000000u;


    /*
     * The keyboard and the clock are looked at every so often rather than every
     * byte: both are calls through the ABI, and this loop runs once per byte off
     * a line that delivers four hundred thousand a second.  Two hundred and
     * fifty six bytes is well under a millisecond of wire.
     */
    uint32_t tick = 0;

    /*
     * The receiver's half of the story, once every few seconds.
     *
     * Everything this program counts used to be printed when it stopped, and
     * that is the wrong time: the questions are about a moment during play -
     * was the picture that stayed wrong a frame this end rejected, or one the
     * other end never sent?  A pointer that leaves dirt behind it and a link
     * that is simply full look the same on the glass and are different faults,
     * and only this end can tell them apart, because only this end knows how
     * many frames arrived broken.
     *
     * It prints into this board's own console, which is the same console the
     * far end's text lands in, so it comes out of the serial port next to
     * everything else and can be read afterwards.
     */
    uint64_t report_at = (uint64_t)g_ag_api->time->us() + REPORT_US;
    uint32_t last_rows = 0, last_bands = 0, last_bad = 0, last_stray = 0;
    uint32_t last_wire = 0, last_asamples = 0, last_adropped = 0;
    uint32_t last_agaps = 0;
    uint32_t last_frames = 0, last_framesmiss = 0, last_framespart = 0;

    while (true) {
        if ((tick++ & 0xffu) == 0u) {
            /*
             * No key stops this, and that is the fix rather than the omission
             * it looks like.
             *
             * It used to be `if (ag_kbhit()) { ag_getch(); break; }`.  Both
             * halves are traps.  ag_kbhit() answers whether *any* event is
             * waiting - the kernel says so beside it: "an event carrying no
             * character counts as something" - and ag_getch() blocks until a
             * character arrives, forever, because that is what the application
             * API offers.  So one touch event, and this program went into
             * getch and stayed there: holding the display, drawing nothing,
             * and swallowing every byte sent at the console afterwards.
             *
             * On the glass that was a frozen screen with no cursor, and from
             * the other end a board that answered nothing at all.  It is why
             * "the picture froze" and "the board hung" were the same report
             * for most of a day, and why every attempt to ask which one it was
             * made it worse: the question is a keystroke, and the keystroke
             * went into the same hole.
             *
             * The CYD polls a touchscreen ten times a second whose pen line
             * runs beside the panel's SPI, so those events arrive with nobody
             * touching anything.
             *
             * A receiver does not need a stop key.  Ctrl+backslash takes the
             * display back, and `remterm <baud> <secs>` still stops on its own
             * clock.  Nothing here reads the keyboard now, so nothing here can
             * be stopped by something that is not one.
             */
            if (window != 0 &&
                (uint64_t)(g_ag_api->time->us() - t0) >= window) {
                break;
            }
            ag_heartbeat();

            const uint64_t now = (uint64_t)g_ag_api->time->us();
            if (now >= report_at) {
                report_at = now + REPORT_US;
                /*
                 * Out of the console port directly, and built without printf.
                 *
                 * The application SDK has no snprintf on purpose - a printf
                 * is twenty kilobytes of an arena that holds sixty-four - so
                 * the line is assembled from ag_utoa.
                 *
                 * It goes to the console, and there is a caveat worth writing
                 * down rather than rediscovering: while this program is
                 * showing pixels it owns the display, and a console line then
                 * has nowhere to be drawn.  So these appear while the far end
                 * is at a shell prompt and stop while a game is running, which
                 * is the half of the time they are most wanted.  The totals at
                 * the end of the run always arrive, and they answer the same
                 * questions less precisely.  Writing to UART0 directly instead
                 * does not work: the console owns those pins and the io layer
                 * refuses to reserve them twice.
                 */
                char line[256];
                char num[24];
                line[0] = '\0';
                /*
                 * The four "why is nothing on the glass" counters are in this
                 * line, not only in the report at the end, because the report
                 * only appears when the run is over and the interesting state
                 * is one that does not end: a receiver taking bands perfectly
                 * and presenting none of them will happily do that for ever.
                 * Reading it required stopping it, which changed it.
                 */
                static const char *const k_rx_label[] = {
                    "[rx] ", " frames (", " lost, ", " partial), ",
                    " rects ", " KB, ", " rows, ", " bad, ",
                    " stray; nowidth ", " nomem ", " noacq ", " nopres ",
                    "; raw ", " slices ", " frames ", " partial ", " asked",
                    "; sound ", " in ", " gaps ", " full",
                    "; too new to read ",
                };
                const uint32_t k_rx_value[] = {
                    frames - last_frames,
                    framesmiss - last_framesmiss,
                    framespart - last_framespart,
                    bands - last_bands,
                    (wire - last_wire) / 1024u,
                    rows - last_rows,
                    bad - last_bad,
                    stray - last_stray,
                    s_no_width,
                    s_no_mem,
                    s_no_acq,
                    s_no_pres,
                    s_raw_slices,
                    s_raw_frames,
                    s_raw_partial,
                    s_raw_asked,
                    s_asamples - last_asamples,
                    s_agaps - last_agaps,
                    s_adropped - last_adropped,
                    s_newfmt,
                };
                for (uint32_t f = 0; f < 20u; f++) {
                    ag_strlcat(line, k_rx_label[f], sizeof(line));
                    ag_strlcat(line, ag_utoa(k_rx_value[f], num, sizeof(num),
                                             0, false),
                               sizeof(line));
                }
                ag_strlcat(line, k_rx_label[19], sizeof(line));
                ag_strlcat(line, "\r\n", sizeof(line));
                ag_printf("%s", line);
                last_rows = rows;
                last_bands = bands;
                last_bad = bad;
                last_stray = stray;
                last_wire = wire;
                last_asamples = s_asamples;
                last_adropped = s_adropped;
                last_agaps = s_agaps;
                last_frames = frames;
                last_framesmiss = framesmiss;
                last_framespart = framespart;

                /*
                 * And the same figures back up the wire, where they can be
                 * read without taking the display away from this program.
                 * Little-endian, no checksum: the far end only logs it, and a
                 * mangled counter is a wrong line in a log, not a wrong
                 * picture.
                 */
                uint8_t st[17];
                const uint32_t stv[4] = {
                    s_raw_frames, s_raw_slices, s_pieces, s_raw_asked,
                };
                st[0] = (uint8_t)FR_STAT;
                for (uint32_t k = 0; k < 4u; k++) {
                    st[1u + k * 4u] = (uint8_t)stv[k];
                    st[2u + k * 4u] = (uint8_t)(stv[k] >> 8);
                    st[3u + k * 4u] = (uint8_t)(stv[k] >> 16);
                    st[4u + k * 4u] = (uint8_t)(stv[k] >> 24);
                }
                (void)ag_uart_write(LINK_PORT, st, sizeof(st));
            }
        }

        int c = next_byte(10);
        if (c < 0) {
            tick = 0; /* an idle line is a good moment to look at the keys */
            continue;
        }
        if ((uint8_t)c != FR_MAGIC0) {
            stray++;
            continue;
        }
        /* Where this frame began, for rewind_frame if it turns out to be
         * rubbish.  s_at has already stepped past the byte just read. */
        s_frame_at = (s_at > 0u) ? (s_at - 1u) : RX_CHUNK;
        c = next_byte(20);
        if (c < 0 || (uint8_t)c != FR_MAGIC1) {
            stray++;
            continue;
        }

        const int op = next_byte(20);
        const int l0 = next_byte(20);
        const int l1 = next_byte(20);
        if (op < 0 || l0 < 0 || l1 < 0) {
            stray++;
            continue;
        }
        const uint32_t len = (uint32_t)l0 | ((uint32_t)l1 << 8);
        if (len > FR_MAX_PAYLOAD) {
            stray++;
            continue;
        }

        static uint8_t payload[FR_MAX_PAYLOAD];
        uint8_t        sum = 0;
        bool           short_read = false;
        for (uint32_t i = 0; i < len; i++) {
            const int p = next_byte(20);
            if (p < 0) {
                short_read = true;
                break;
            }
            payload[i] = (uint8_t)p;
            sum = (uint8_t)(sum ^ (uint8_t)p);
        }
        if (short_read) {
            stray++;
            continue;
        }
        /*
         * Picture and sound check themselves and stop here.  Their sum is in
         * the header, over the twelve bytes of it and the body as it arrived -
         * padding included, which is the run the sender computed it over - and
         * there is no byte after the payload to read.
         *
         * A packet that does not add up is dropped rather than rewound: five
         * kilobytes put back through the framer to look for a header inside
         * them costs more than the slice is worth, and the gap logic below
         * asks for a missing slice anyway.
         */
        if (op == FR_IDX8 || op == FR_PAL || op == FR_HUF || op == FR_HUFP ||
            op == FR_DONE) {
            s_newfmt++;
            wire += len;
            continue; /* sum is in the header; nothing follows the payload */
        }

        if (op == FR_RAW || op == FR_AUD) {
            uint8_t got = 0;
            if (len >= PKT_BODY) {
                for (uint32_t i = 0; i < PKT_GEOM; i++) {
                    got = (uint8_t)(got ^ payload[i]);
                }
                for (uint32_t i = PKT_BODY; i < len; i++) {
                    got = (uint8_t)(got ^ payload[i]);
                }
            }
            if (len < PKT_BODY || got != payload[PKT_SUM]) {
                bad++;
                continue;
            }
            if (op == FR_RAW) {
                raw_slice(payload, len);
            } else {
                /*
                 * Rate, then how many of the samples are real: the sender
                 * rounds the count on the wire up to a cache line because its
                 * DMA reads whole ones, so the tail of a short block is
                 * padding and the header says how much to keep.
                 */
                const uint32_t rate = (uint32_t)payload[0] |
                                      ((uint32_t)payload[1] << 8) |
                                      ((uint32_t)payload[2] << 16) |
                                      ((uint32_t)payload[3] << 24);
                uint32_t want = (uint32_t)payload[4] |
                                ((uint32_t)payload[5] << 8);
                if (want > len - PKT_BODY) {
                    want = len - PKT_BODY;
                }
                if (rate >= 8000u && rate <= 48000u && want != 0u) {
                    s_arate = rate;
                    aud_push(payload + PKT_BODY, want);
                }
            }
            wire += len;
            continue;
        }

        const int claimed = next_byte(20);
        if (claimed < 0 || (uint8_t)claimed != sum) {
            bad++;
            /*
             * And say so, because a lost band does not come back on its own.
             *
             * The far end keeps a hash of every eight-by-eight block and sends
             * only what differs from it, which means it believes a band it has
             * written to the wire has arrived.  A band the checksum rejects
             * therefore leaves that part of the glass showing the frame before
             * it, for as long as those pixels do not change again - the stale
             * rectangle behind a mouse pointer that a person reports as "the
             * codec is not seeing the change".  There is a slow sweep over
             * there that forgets one row of blocks every quarter second, so it
             * does come right, in up to eight seconds.
             *
             * The same byte the receiver sends when it starts makes the far end
             * forget everything at once, so it comes right on the next frame
             * instead.  Rate limited, and that is not a nicety: a full repaint
             * is about a hundred kilobytes on a wire that loses a frame every
             * two hundred, so a repaint per corrupt frame is a repaint that
             * causes the next one.  One a second, at most; while the line is
             * healthy this costs nothing at all.
             */
            const ag_time_t now = g_ag_api->time->us();
            if (now - s_asked_at >= ASK_AGAIN_US) {
                s_asked_at = now;
                const uint8_t hello = (uint8_t)'H';
                (void)ag_uart_write(LINK_PORT, &hello, 1u);
            }
            /* Give back everything this frame swallowed: its length came out
             * of the corruption and the bytes after it are the next frames. */
            (void)rewind_frame();
            continue;
        }

        if (op == FR_END && len == 4u) {
            const uint32_t no = get16(payload);
            const uint32_t promised = get16(payload + 2);
            /* The whole point: one change of the glass per frame. */
            pend_flush();
            if (numbered && no != nextframe) {
                /*
                 * How far ahead the number jumped is how many frames went
                 * missing - but only for a jump small enough to be one.
                 *
                 * The first version of this line was `(no - nextframe) &
                 * 0xffff` and nothing else, and it reported 720 950 frames
                 * lost out of 1326 received.  That is eleven jumps *backwards*
                 * - a sender whose counter started again - each of which the
                 * wrapping subtraction turned into sixty-five thousand losses.
                 * A number that large is not a measurement of anything, and it
                 * discredits the numbers next to it.
                 *
                 * So: a jump of less than a quarter of the range is loss, and
                 * anything else is a sender that restarted.  Counted, and
                 * counted separately, because the two mean opposite things.
                 */
                const uint32_t ahead = (uint32_t)((no - nextframe) & 0xffffu);
                if (ahead < 16384u) {
                    framesmiss += ahead;
                    /*
                     * And say so, because this is the only way a frame that
                     * never arrived can be noticed at all.  A checksum sees a
                     * frame that came out wrong; nothing sees a frame that did
                     * not come.  The far end repairs the screen when asked and
                     * does nothing when not, so an unreported gap is a patch of
                     * stale picture that stays until something redraws it.
                     */
                    const ag_time_t now = g_ag_api->time->us();
                    if (now - s_asked_at >= ASK_AGAIN_US) {
                        s_asked_at = now;
                        const uint8_t hello = (uint8_t)'H';
                        (void)ag_uart_write(LINK_PORT, &hello, 1u);
                    }
                } else {
                    framesresync++;
                }
            }
            nextframe = (no + 1u) & 0xffffu;
            numbered = true;
            if (inframe < promised) {
                framespart++;
            }
            inframe = 0;
            frames++;
            wire += len;
            continue;
        }

        /*
         * Picture and sound never reach here: they are read above, where their
         * own sum is checked.  Into the ring and straight back to the wire is
         * the rule for the sound - nothing on this path talks to the
         * converter, which is the player thread's job (see "the sound").
         */

        if ((op == FR_BAND || op == FR_PACK || op == FR_IDX) && len >= 12u) {
            /*
             * Held back until the frame is closed, so the glass changes once
             * per frame instead of once per stripe.  A frame too big for the
             * buffer is laid down in two halves rather than dropped, and says
             * so in pend_over.
             */
            if (!pend_add(op, payload, len)) {
                if (s_pend != NULL && s_pend_used != 0u) {
                    s_pend_over++;
                    pend_flush();
                }
                if (!pend_add(op, payload, len)) {
                    show_band(op, payload, len);
                }
            }
        } else if (op == FR_ROW && len >= 2u) {
            /* Whatever is held belongs under the text, not over it. */
            pend_flush();
            want_text();
            const uint32_t y = payload[0];
            uint32_t       count = payload[1];
            if (len < 2u + count * 2u) {
                bad++;
                continue;
            }
            if (y >= con.rows) {
                continue;
            }
            if (count > con.cols) {
                count = con.cols;
            }
            for (uint32_t x = 0; x < count; x++) {
                ag_poke((uint16_t)x, (uint16_t)y, (char)payload[2u + x * 2u],
                        payload[3u + x * 2u]);
            }
            rows++;
        } else if (op == FR_CUR && len == 5u) {
            if (!s_acquired) {
                const uint32_t cx = payload[0];
                const uint32_t cy = payload[1];
                if (cx < con.cols && cy < con.rows) {
                    ag_poke((uint16_t)cx, (uint16_t)cy, (char)payload[2],
                            payload[3]);
                    ag_gotoxy((uint16_t)cx, (uint16_t)cy);
                }
                ag_cursor(payload[4] != 0);
            }
        }
    }

    want_text();
    ag_free(s_band);
    s_band = NULL;
    ag_cursor(true);

    /* The player closes the converter on its way out - which matters, because
     * closing it is what walks the pin down to ground instead of dropping it,
     * and that difference is a thump in the speaker. */
    s_astop = 1;
    if (s_athread != NULL) {
        (void)ag_thread_join(s_athread, 1000u);
        s_athread = NULL;
    }
    if (s_alock != NULL) {
        ag_mutex_delete(s_alock);
        s_alock = NULL;
    }

    uint32_t kpx_s = 0;
    uint32_t flow_ms = 0;
    if (first_px != 0 && last_px > first_px) {
        const uint64_t us = (uint64_t)(last_px - first_px);
        flow_ms = (uint32_t)(us / 1000u);
        kpx_s = (uint32_t)(((uint64_t)px * 1000000ull) / us);
    }

    wirekb = wire / 1024u;

    ag_printf("\nremterm: %u frames (%u lost, %u partial, %u resync, "
              "%u split), %u fps\n",
              (unsigned)frames, (unsigned)framesmiss, (unsigned)framespart,
              (unsigned)framesresync, (unsigned)s_pend_over,
              (unsigned)((flow_ms != 0u) ? (frames * 1000u / flow_ms) : 0u));
    ag_printf("remterm: %u rows, %u rects, %u k pixels, %u bad, %u stray\n",
              (unsigned)rows, (unsigned)bands, (unsigned)px, (unsigned)bad,
              (unsigned)stray);
    ag_printf("encoded: %u packed, %u indexed, %u KB on the wire\n",
              (unsigned)packedframes, (unsigned)idxframes, (unsigned)wirekb);
    if (flow_ms != 0) {
        ag_printf("pixels flowed for %u ms, %u k pixels/s\n",
                  (unsigned)flow_ms, (unsigned)kpx_s);
    }
    if (s_asamples != 0u) {
        ag_printf("sound: %u samples in, %u played, %u gaps (ring ran dry), "
                  "%u dropped (ring full), %u Hz\n",
                  (unsigned)s_asamples, (unsigned)s_aplayed, (unsigned)s_agaps,
                  (unsigned)s_adropped, (unsigned)s_pcm_rate);
    }

    /*
     * And the same to a file, because the console cannot be trusted to carry
     * it.  While the far end's console is on this screen, its prompt is poked
     * into this one and goes out of this board's serial port along with
     * everything else - where a test harness watching for a prompt sees the
     * *other* machine's and decides the command has finished.  The remote
     * screen fools the thing that reads it.  A monitor that can be asked what
     * it saw does not care who was watching.
     */
    const ag_handle_t f = ag_open("c:\\remterm.txt",
                                  AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (f >= 0) {
        static const char *const k_names[] = {
            "frames=", " frames_lost=", " frames_partial=",
            " frames_resync=", " frames_split=",
            " no_width=", " no_mem=", " no_acquire=", " no_present=",
            " fps=", " bytes_per_frame=",
            " rows=", " rects=", " kpixels=", " bad=", " stray=",
            " flow_ms=", " kpixels_per_s=", " packed=", " indexed=", " wirekb=",
            " wirekb_px=", " raw_over_wire_pct=",
            " snd_in=", " snd_played=", " snd_dropped=", " snd_hz=",
            " us_decode=", " us_present=",
        };
        const uint32_t vals[] = {frames,     framesmiss,   framespart,
                                 framesresync, s_pend_over,
                                 s_no_width, s_no_mem, s_no_acq, s_no_pres,
                                 (uint32_t)((flow_ms != 0u)
                                                ? (frames * 1000u / flow_ms)
                                                : 0u),
                                 (uint32_t)(frames ? wire / frames : 0),
                                 rows,       bands,        px,
                                 bad,        stray,        flow_ms,
                                 kpx_s,      packedframes, idxframes,
                                 wirekb,     wire_px / 1024u,
                                 /* raw pixel bytes as a percentage of the
                                  * bytes that carried them: 100 is no
                                  * compression at all, 200 is two to one. */
                                 (uint32_t)((wire_px != 0u)
                                     ? ((uint64_t)px * 1000ull * 2ull * 100ull /
                                        wire_px)
                                     : 0u),
                                 s_asamples,   s_aplayed,
                                 s_adropped, s_pcm_rate,
                                 (uint32_t)(bands ? us_decode / bands : 0),
                                 (uint32_t)(bands ? us_present / bands : 0)};

        char out[256];
        out[0] = '\0';
        for (uint32_t i = 0; i < 29u; i++) {
            char num[24];
            (void)ag_strlcat(out, k_names[i], sizeof(out));
            (void)ag_strlcat(out, ag_utoa(vals[i], num, sizeof(num), 0, false),
                             sizeof(out));
        }
        (void)ag_strlcat(out, "\n", sizeof(out));
        (void)ag_write(f, out, strlen(out));
        (void)ag_close(f);
    }
    return 0;
}
