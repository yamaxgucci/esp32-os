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
 *   run c:\remterm.axe [baud] [secs]   4000000 by default; any key stops it,
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
 * Build (LX6, for the ESP32 - the far end is an LX7 and needs its own image):
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32-elf-gcc \
 *       --include sdk/include -o build/apps/REMTERM.AXE apps/remterm/remterm.c
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_APP("REMTERM", "1.1", "argon", 0);

#define LINK_PORT 1
#define LINK_BAUD 4000000u

#define FR_MAGIC0 0xA5u
#define FR_MAGIC1 0x5Au
#define FR_ROW    'R'
#define FR_CUR    'C'
#define FR_BAND   'B' /* pixels as they are                                  */
#define FR_PACK   'P' /* PackBits over the pixels                            */
#define FR_IDX    'I' /* a palette and indices                               */

/* Both numbers have to match remdisp.c exactly. */
#define BAND_ROWS      8u
#define BAND_MAX_PX    (BAND_ROWS * 320u)
#define FR_MAX_PAYLOAD (12u + BAND_MAX_PX * 2u + BAND_MAX_PX / 64u + 16u)

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
#define RX_CHUNK 1024u

static uint8_t  s_rx[RX_CHUNK];
static uint32_t s_have;
static uint32_t s_at;

static int next_byte(uint32_t ms)
{
    if (s_at >= s_have) {
        const int32_t n = ag_uart_read(LINK_PORT, s_rx, sizeof(s_rx), ms);
        if (n <= 0) {
            return -1;
        }
        s_have = (uint32_t)n;
        s_at = 0;
    }
    return (int)s_rx[s_at++];
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

static bool want_pixels(uint32_t surf_w)
{
    if (surf_w == 0 || surf_w > MAX_SURF_W) {
        return false;
    }
    if (s_band == NULL || s_band_w != surf_w) {
        ag_free(s_band);
        s_band = (uint16_t *)ag_malloc((size_t)surf_w * BAND_ROWS *
                                       sizeof(uint16_t));
        if (s_band == NULL) {
            s_band_w = 0;
            return false;
        }
        s_band_w = surf_w;
    }
    if (!s_acquired) {
        ag_gfxinfo_t gi;
        if (ag_gfx_acquire(&gi) != AG_OK) {
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

int ag_main(int argc, char **argv)
{
    uint32_t baud = LINK_BAUD;
    if (argc >= 2 && (!parse_u32(argv[1], &baud) || baud == 0)) {
        ag_printf("usage: remterm [baud] [secs]\n");
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
        ag_printf("usage: remterm [baud] [secs]\n");
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

    ag_coninfo_t con;
    ag_coninfo(&con);

    ag_cls();
    ag_printf("remterm: %u baud, %ux%u cells, bands up to %ux%u\n",
              (unsigned)baud, (unsigned)con.cols, (unsigned)con.rows,
              (unsigned)MAX_SURF_W, (unsigned)BAND_ROWS);

    /*
     * Nothing below this line may print.  A printed line scrolls the console,
     * and the far end sends only the rows it thinks changed - so one scroll
     * here and the two screens disagree until its repair sweep catches up.
     */
    uint32_t rows = 0;   /* console rows applied                            */
    uint32_t bands = 0;  /* pixel rectangles presented                      */
    uint32_t px = 0;     /* pixels presented, in thousands                  */
    uint32_t bad = 0;    /* frames whose checksum or shape did not come out  */
    uint32_t stray = 0;  /* bytes dropped hunting for a header              */
    uint32_t pxlow = 0;
    /*
     * What the encodings actually bought, which is the only way to know: the
     * sender picks per rectangle, so the answer is a property of the picture and
     * not of the code.  `wire` is payload bytes accepted; against 2 * pixels it
     * is the compression, and the two counts say which encoding won.
     */
    uint32_t packedframes = 0;
    uint32_t idxframes = 0;
    uint32_t wire = 0;
    uint32_t wirekb = 0;

    const ag_time_t t0 = g_ag_api->time->us();
    const uint64_t  window = (uint64_t)secs * 1000000u;
    ag_time_t       first_px = 0;
    ag_time_t       last_px = 0;

    /*
     * The keyboard and the clock are looked at every so often rather than every
     * byte: both are calls through the ABI, and this loop runs once per byte off
     * a line that delivers four hundred thousand a second.  Two hundred and
     * fifty six bytes is well under a millisecond of wire.
     */
    uint32_t tick = 0;

    for (;;) {
        if ((tick++ & 0xffu) == 0u) {
            if (ag_kbhit()) {
                (void)ag_getch();
                break;
            }
            if (window != 0 &&
                (uint64_t)(g_ag_api->time->us() - t0) >= window) {
                break;
            }
            ag_heartbeat();
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
        const int claimed = next_byte(20);
        if (claimed < 0 || (uint8_t)claimed != sum) {
            bad++;
            continue;
        }

        if ((op == FR_BAND || op == FR_PACK || op == FR_IDX) && len >= 12u) {
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
                continue;
            }

            const uint8_t *body = payload + 12u;
            const uint32_t blen = len - 12u;
            bool           ok;
            if (op == FR_BAND) {
                if (blen < npx * 2u) {
                    bad++;
                    continue;
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
                continue;
            }
            if (op != FR_BAND) {
                packedframes++;
            }
            wire += len;

            if (!want_pixels(sw)) {
                continue;
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
            if (ag_gfx_present(&blit) == AG_OK) {
                bands++;
                pxlow += bw * bh;
                while (pxlow >= 1000u) {
                    pxlow -= 1000u;
                    px++;
                }
                if (first_px == 0) {
                    first_px = g_ag_api->time->us();
                }
                last_px = g_ag_api->time->us();
            }
        } else if (op == FR_ROW && len >= 2u) {
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

    uint32_t kpx_s = 0;
    uint32_t flow_ms = 0;
    if (first_px != 0 && last_px > first_px) {
        const uint64_t us = (uint64_t)(last_px - first_px);
        flow_ms = (uint32_t)(us / 1000u);
        kpx_s = (uint32_t)(((uint64_t)px * 1000000ull) / us);
    }

    wirekb = wire / 1024u;

    ag_printf("\nremterm: %u rows, %u rects, %u k pixels, %u bad, %u stray\n",
              (unsigned)rows, (unsigned)bands, (unsigned)px, (unsigned)bad,
              (unsigned)stray);
    ag_printf("encoded: %u packed, %u indexed, %u KB on the wire\n",
              (unsigned)packedframes, (unsigned)idxframes, (unsigned)wirekb);
    if (flow_ms != 0) {
        ag_printf("pixels flowed for %u ms, %u k pixels/s\n",
                  (unsigned)flow_ms, (unsigned)kpx_s);
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
            "rows=", " rects=", " kpixels=", " bad=", " stray=",
            " flow_ms=", " kpixels_per_s=", " packed=", " indexed=", " wirekb=",
        };
        const uint32_t vals[] = {rows,    bands,        px,
                                 bad,     stray,        flow_ms,
                                 kpx_s,   packedframes, idxframes,
                                 wirekb};

        char out[256];
        out[0] = '\0';
        for (uint32_t i = 0; i < 10u; i++) {
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
