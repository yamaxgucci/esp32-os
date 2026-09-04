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

AG_APP("REMTERM", "1.2", "argon", 0);

#define LINK_PORT 1
#define LINK_BAUD 4000000u

#define FR_MAGIC0 0xA5u
#define FR_MAGIC1 0x5Au
#define FR_ROW    'R'
#define FR_CUR    'C'
#define FR_BAND   'B' /* pixels as they are                                  */
#define FR_PACK   'P' /* PackBits over the pixels                            */
#define FR_IDX    'I' /* a palette and indices                               */
#define FR_AUD    'A' /* a rate, then unsigned 8-bit mono samples            */

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
    /*
     * Where the time goes on this side, because the answer decided what to fix
     * next and could not be guessed.  The wire runs at 12% on average, so
     * nothing is short of bandwidth - what happens is that a whole frame
     * arrives in one burst of thirty bands and this end falls behind it.  These
     * two say by how much and at which of the two steps.
     */
    uint64_t us_decode = 0;
    uint64_t us_present = 0;

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

    bool stop = false;

    while (!stop) {
        if ((tick++ & 0xffu) == 0u) {
            /*
             * A key stops this.  A key, and not "something happened".
             *
             * ag_kbhit() answers whether any event is waiting, and the kernel
             * says so beside it: "an event carrying no character counts as
             * something, because deciding that here would mean taking the event
             * to look at it".  On this board the touchscreen is polled ten
             * times a second and its pen line runs beside the panel's own SPI,
             * so events arrive with nobody touching anything - and this loop
             * took every one of them as "stop".
             *
             * From the outside that was a screen showing the game which, at
             * some random minute, went back to a shell prompt with a blinking
             * cursor.  It cost most of a day: a receiver that has stopped and a
             * sender that has stopped look identical on the glass, and every
             * attempt to find out which sent a byte down the console - which is
             * also a key, and also stopped it.
             *
             * So the event is taken and looked at.  A key stops it; pointers,
             * focus changes and device notices are ignored.
             */
            ag_event_t ev;
            while (ag_poll_event(&ev, 0)) {
                if (ev.type == AG_EV_KEY_DOWN || ev.type == AG_EV_QUIT) {
                    stop = true;
                }
            }
            if (stop) {
                break;
            }
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
                char line[192];
                char num[24];
                line[0] = '\0';
                static const char *const k_rx_label[] = {
                    "[rx] ", " rects ", " KB, ", " rows, ",
                    " bad, ", " stray, sound ", " in ", " gaps ", " full",
                };
                const uint32_t k_rx_value[] = {
                    bands - last_bands,
                    (wire - last_wire) / 1024u,
                    rows - last_rows,
                    bad - last_bad,
                    stray - last_stray,
                    s_asamples - last_asamples,
                    s_agaps - last_agaps,
                    s_adropped - last_adropped,
                };
                for (uint32_t f = 0; f < 8u; f++) {
                    ag_strlcat(line, k_rx_label[f], sizeof(line));
                    ag_strlcat(line, ag_utoa(k_rx_value[f], num, sizeof(num),
                                             0, false),
                               sizeof(line));
                }
                ag_strlcat(line, k_rx_label[8], sizeof(line));
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

        if (op == FR_AUD && len >= 4u) {
            /*
             * Into the ring and straight back to reading.  Nothing here talks
             * to the converter - that is the player thread's job, and keeping
             * it out of this loop is the whole point (see "the sound").
             */
            const uint32_t rate = rd16(payload) | (rd16(payload + 2) << 16);
            if (rate >= 8000u && rate <= 48000u) {
                s_arate = rate;
                aud_push(payload + 4u, len - 4u);
            }
            wire += len;
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

            const ag_time_t t_dec = g_ag_api->time->us();
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
            us_decode += (uint64_t)(g_ag_api->time->us() - t_dec);
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
            const ag_time_t t_pres = g_ag_api->time->us();
            const ag_err_t  shown = ag_gfx_present(&blit);
            us_present += (uint64_t)(g_ag_api->time->us() - t_pres);
            if (shown == AG_OK) {
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

    ag_printf("\nremterm: %u rows, %u rects, %u k pixels, %u bad, %u stray\n",
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
            "rows=", " rects=", " kpixels=", " bad=", " stray=",
            " flow_ms=", " kpixels_per_s=", " packed=", " indexed=", " wirekb=",
            " snd_in=", " snd_played=", " snd_dropped=", " snd_hz=",
            " us_decode=", " us_present=",
        };
        const uint32_t vals[] = {rows,       bands,        px,
                                 bad,        stray,        flow_ms,
                                 kpx_s,      packedframes, idxframes,
                                 wirekb,     s_asamples,   s_aplayed,
                                 s_adropped, s_pcm_rate,
                                 (uint32_t)(bands ? us_decode / bands : 0),
                                 (uint32_t)(bands ? us_present / bands : 0)};

        char out[256];
        out[0] = '\0';
        for (uint32_t i = 0; i < 16u; i++) {
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
