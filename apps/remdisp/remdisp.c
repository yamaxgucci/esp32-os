/*
 * ArgonOS - this machine's screen, sent down a wire to the board that has glass.
 *
 * The S3 on the desk has no display.  The board next to it is little else but
 * one, and two soldered pins join them (docs/09-esp32-cyd.md, "РџСЂРѕРІРѕРґ Рє S3").
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
 *   one on its converter pin.  It is a stream in its own right: its own
 *   buffers, laid out as wire packets exactly as the picture is, handed to the
 *   same DMA and interleaved with the slices a block at a time.  See "sound"
 *   below for why it is in this file and not in a .SYS of its own, which is
 *   where it belongs by subject.
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

AG_DRV("REMDISP", "2.0", "argon");

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
#define FR_END    'E' /* that frame is whole: its number, and how many bands */
#define FR_RAW    'F' /* one slice of a whole frame, uncompressed           */

/*
 * The plain way, and for now the only way.
 *
 * Everything else in this file - the hash grid, the trimming to changed
 * blocks, three encodings raced against each other, the byte budget - was
 * built to make a slow wire carry a picture, and each piece of it works.
 * Together they produced half a screen a second and a set of numbers nobody
 * could reconcile with what was on the glass, because every one of them
 * measured a different thing and two of them were wrong.
 *
 * So this is the floor to build back up from: copy the whole frame, cut it
 * into numbered slices, send them all, and let the far end ask again for what
 * it did not get.  A 320x240 frame is 150 KB and the wire carries 400 KB/s, so
 * the ceiling is 2.7 frames a second - three times what all the cleverness was
 * delivering, and every one of them a whole frame rather than a stripe.
 *
 * A slice is eight rows because that is what fits a frame on this wire, not
 * for any reason to do with the picture.
 */
/*
 * Above the applications again, and this time it is earned.
 *
 * It ran at 6 by habit and that was the bug: applications run at 5, and a task
 * above them is promising to hand the core straight back, which a task writing
 * a hundred and fifty kilobytes through a two-kilobyte ring cannot do.  It was
 * dropped to 3 and the game came back - 40 fps instead of 1.5 - at the cost of
 * the picture, which fell to one frame a second because the sender then had to
 * queue behind everything.
 *
 * What changed is that the picture is carried by DMA now.  This task hands a
 * slice to the hardware and sleeps; it needs about three milliseconds a frame
 * for the sums and a handful of small writes, so the promise the priority
 * makes is one it can keep.  And if it ever stops keeping it, the kernel says
 * so and drops it below the applications until the window turns - see
 * argon/budget.h.  Measured with all three in place: the game at 36-42 fps,
 * the screen at 1.8-2.0 whole frames a second, and not one budget warning.
 */
#ifndef WIRE_PRIO
#define WIRE_PRIO 6
#endif

#ifndef NO_AUDIO
#define NO_AUDIO 0
#endif

#ifndef SEND_ALWAYS
#define SEND_ALWAYS 0
#endif

#ifndef TICK_PRIO
#define TICK_PRIO 5
#endif

#ifndef CORE_METER
#define CORE_METER 0
#endif

#ifndef PORT_ON_SYS_CORE
#define PORT_ON_SYS_CORE 0
#endif

#ifndef PORT_WRITE_MAX
#define PORT_WRITE_MAX 0
#endif

#ifndef RAW_SILENT
#define RAW_SILENT 0
#endif

#define RAW_ROWS 8u

/*
 * Why a frame needs an end.
 *
 * Until now the wire carried bands and nothing else, and a band is a stripe of
 * the screen.  The far end put each one on the glass as it arrived, so a moving
 * picture reached the eye as a wipe from the top - the stripes somebody
 * watching calls running bands - and neither end could say how many *frames* a
 * second the glass was showing, because neither end knew where one frame ended
 * and the next began.  Every number about the picture was therefore about
 * rectangles, which is not what a person looks at.
 *
 * So: one two-byte number per frame and the count of bands it was made of,
 * four bytes at the end of every rectangle the driver sends.  It buys three
 * things that could not be had before.  The far end can count frames.  A gap
 * in the numbering names a frame that did not arrive, which no checksum can -
 * loss used to be invisible here, there being nothing to miss.  And a frame
 * that arrives with fewer bands than it promised is a frame that lost one,
 * which is the difference between "the wire dropped a stripe" and "the wire
 * dropped everything".
 */

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

/* Which frame is being sent, and how many bands have gone into it. */
static uint16_t s_frame_no;
static uint16_t s_frame_bands;

/*
 * The handshake that keeps the driver from ever seeing half a frame.
 *
 * The game draws into the system surface and ends its frame with a flush, and
 * that flush is the only moment anybody knows the picture is whole.  So the
 * copy happens there - but only when this driver has asked for one.  The rest
 * of the time the flush returns at once and the game waits for nothing, which
 * is the point: the game is not paying for the wire, it is paying for one
 * memcpy whenever the wire is ready for another frame.
 */
static volatile bool s_want_copy;   /* the driver would like a frame        */
static volatile bool s_have_copy;   /* ...and here it is                    */

/* What each stage costs, measured rather than reasoned about. */
static uint32_t s_t_copy_us;     /* 2: the game held for the copy            */
/*
 * And every step after it, separately, because the sum said nothing.
 *
 * "Send" was 406-486 ms a frame and the wire alone accounts for 383 of it, so
 * the interesting question - what the driver spends that is not the wire - was
 * hidden inside one number.  Four timers instead: the gather out of the frame,
 * the byte-at-a-time fill of the ring, the byte-at-a-time drain of it, and the
 * write to the port.
 */
static uint32_t s_t_gather_us;   /* 3: frame -> the slice being sent          */
static uint32_t s_t_ring_us;     /* 4: slice -> ring, a byte at a time        */
static uint32_t s_t_out_us;      /* 5: ring -> out buffer, a byte at a time   */
static uint32_t s_t_port_us;     /* 6: out buffer -> the wire                 */
static uint32_t s_t_send_us;     /* the frame put on the wire                */
static uint32_t s_t_wait_us;     /* the driver waiting for a frame boundary  */
static uint32_t s_raw_frames;    /* frames sent whole                        */
static uint32_t s_raw_resent;    /* slices the far end had to ask for again  */
static uint64_t s_raw_bytes;

/* One frame, built here and written in one call.  Static because the console
 * task's stack is not this driver's to spend, and the kernel serialises every
 * callback that uses it. */

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
static uint32_t s_dropped; /* refused: the ring was full                     */
/*
 * Two counters, one per core, at the lowest priority there is.  They do
 * nothing but add one, so the rate each of them manages is how much of that
 * core nobody else wanted.  Read them in three states - the driver silent, the
 * driver asleep instead of writing, the driver really sending - and the
 * question "is a core being eaten, and which" stops being a matter of opinion.
 *
 * -DCORE_METER=1 builds them; they are off otherwise, because an idle-priority
 * spinner is exactly the sort of thing that should not ship.
 */
static volatile uint32_t s_spin[2];

#if CORE_METER
static volatile uint32_t s_laps[2]; /* laps nobody else wanted, per core */
#endif
static uint32_t s_in_blit_us, s_in_text_us, s_in_pcm_us;
/*
 * Whether the picture is carried by the UART's own DMA.  Decided once, when
 * the wire task starts, and given up for good if the port ever refuses a
 * transfer - see ABI 0.44 and the note there about which chips have it.
 */
static bool s_dma;
static bool s_asked_recently; /* the far end wanted something last frame */

/*
 * Step four, taken apart.  Three hundred milliseconds a frame were going
 * somewhere that neither the wire nor the processor could account for, and a
 * total is no use for finding out which of five things it is.
 */
static uint32_t s_t_hdr_us, s_t_wait_hand_us, s_t_dma_us, s_t_sum_us,
    s_t_rest_us;

static uint32_t s_rx_bytes;   /* everything the far end sent us */
static uint32_t s_t_fill_us;  /* step 3: PSRAM -> the internal buffer        */
static uint32_t s_chunks;     /* how many buffer-fulls a frame took          */
static uint32_t s_lost;    /* accepted, then the port would not take it       */

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

/*
 * One task writes the port.  Everybody else leaves the frame here.
 *
 * The wire lock used to be held across the write itself, and the write blocks:
 * uart_write_bytes waits for the peripheral to make room, and a port carrying
 * a picture is full most of the time.  So whoever held it - the sender, the
 * console drawing a row of text, the sound - held it for as long as the wire
 * took, and everything else queued behind.  Twice that ended with a board that
 * answered nothing at all: the console cannot print without this lock, and a
 * machine that cannot print is a machine with no way to say what is wrong.
 *
 * The io layer learned this lesson before this file did, and wrote it down:
 * "a thread blocked in a write to the picture link held the lock the console
 * needed to print... the whole board went quiet at once".  It stopped holding
 * its lock across the write.  This is the same answer one layer up.
 *
 * So a frame is assembled straight into a ring and the caller returns.  The
 * ring is drained by the one task that owns the port, and nothing else ever
 * touches it.  What is held while the wire is blocking is nothing at all.
 *
 * A frame that does not fit is dropped rather than waited for, and the caller
 * is told so its idea of what the far end has stays true.  That is the right
 * failure for a video link: a dropped band is a stale rectangle until the
 * repair sweep comes round, where waiting is a stopped machine.
 */
#define TX_RING 24576u

static uint8_t   *s_tx;
static uint32_t   s_tx_r, s_tx_w, s_tx_used;
static ag_mutex_t s_tx_lock;   /* the ring's own bookkeeping, held for bytes  */
static ag_mutex_t s_drain_lock; /* the port itself, held across the write     */
/*
 * The buffer the port reads from, and the ring behind it, both want to be in
 * internal SRAM rather than PSRAM.  Not for their own speed - they are small -
 * but because the game is walking PSRAM at the same time on the other core,
 * and every byte uart_write dribbles out of PSRAM is a byte the game does not
 * get.  Measured: with the port write skipped the game ran at 20 fps, with it
 * at 1.7, while the driver only ever occupied the other core.  So take as much
 * internal memory as the system will part with, and fall back to the static
 * two kilobytes if it will part with none.
 */
#define TX_OUT_STATIC 2048u
static uint8_t    s_tx_out_fallback[TX_OUT_STATIC];
static uint8_t   *s_tx_out = s_tx_out_fallback;
static uint32_t   s_tx_out_sz = TX_OUT_STATIC;
static bool       s_task_ok;   /* false: no task, so the caller drains        */

static void tx_put(uint8_t b)
{
    s_tx[s_tx_w] = b;
    s_tx_w = (s_tx_w + 1u) % TX_RING;
    s_tx_used++;
}

/* Defined below: the one place that writes the port. */
static uint32_t tx_drain(void);

static bool send_frame(uint8_t op, const uint8_t *payload, uint32_t len)
{
    if (!s_up || s_tx == NULL || len > FR_MAX_PAYLOAD) {
        s_dropped++;
        return false;
    }
    const uint32_t want = 5u + len + 1u;

    const bool held = lock_take(s_tx_lock, UINT32_MAX);
    if (TX_RING - s_tx_used < want) {
        /*
         * The wire is behind.  Dropped, counted, and reported: the caller is
         * about to write down that the far end has these pixels, and that has
         * to stay true or the picture is wrong until the sweep repairs it.
         */
        lock_give(s_tx_lock, held);
        s_dropped++;
        return false;
    }

    tx_put(FR_MAGIC0);
    tx_put(FR_MAGIC1);
    tx_put(op);
    tx_put((uint8_t)len);
    tx_put((uint8_t)(len >> 8));

    uint8_t sum = 0;
    const uint64_t r0 = (uint64_t)ag_api()->time->us();
    for (uint32_t i = 0; i < len; i++) {
        tx_put(payload[i]);
        sum = (uint8_t)(sum ^ payload[i]);
    }
    tx_put(sum);
    s_t_ring_us += (uint32_t)((uint64_t)ag_api()->time->us() - r0);
    lock_give(s_tx_lock, held);

    s_sent_bytes += want;
    s_sent_frames++;

    /* With no task of its own this driver is what it always was: the caller
     * carries the frame to the port and waits for it. */
    if (!s_task_ok) {
        (void)tx_drain();
    }
    return true;
}

/*
 * Take what is waiting and put it on the wire.  Only the wire task calls this,
 * and it holds no lock while the port blocks - the bytes are copied out first.
 */
static uint32_t tx_drain(void)
{
    uint32_t total = 0;

    /*
     * Only the wire task gets here while there is one, so this is uncontended
     * and costs a take and a give.  It matters in the degraded case: with no
     * task the callers drain for themselves, and two of them writing the port
     * at once is the interleaved rubbish this whole arrangement exists to
     * prevent.
     */
    const bool dheld = lock_take(s_drain_lock, UINT32_MAX);

    for (;;) {
        const bool held = lock_take(s_tx_lock, UINT32_MAX);
        uint32_t   n = s_tx_used;
        if (n > s_tx_out_sz) {
            n = s_tx_out_sz;
        }
        const uint64_t o0 = (uint64_t)ag_api()->time->us();
        for (uint32_t i = 0; i < n; i++) {
            s_tx_out[i] = s_tx[s_tx_r];
            s_tx_r = (s_tx_r + 1u) % TX_RING;
            s_tx_used--;
        }
        s_t_out_us += (uint32_t)((uint64_t)ag_api()->time->us() - o0);
        lock_give(s_tx_lock, held);

        if (n == 0u) {
            break;
        }

        /*
         * Until every byte of what was taken is out, or the port refuses.
         *
         * These bytes have already left the ring, so a short write that is not
         * finished is not a slow frame - it is a frame with a hole in it, and
         * the receiver spends the rest of the second hunting for a header.
         * That is exactly what it reported: seventeen bad frames and forty-five
         * kilobytes of stray in a ten second window, where the same link had
         * been carrying zero of each.
         */
        uint32_t off = 0;
        const uint64_t p0 = (uint64_t)ag_api()->time->us();
        /*
         * RAW_SILENT=3: everything happens except the wire.  The frame is
         * copied, gathered, put through the ring a byte at a time and taken
         * out of it again - and then thrown away.  If the game is fast in this
         * build the cost is the port; if it is slow, the cost is ours.
         */
#if RAW_SILENT == 3
        off = n;
#endif
        while (off < n) {
            const int32_t w = io->uart_write(LINK_PORT, s_tx_out + off, n - off);
            if (w <= 0) {
                s_lost += n - off; /* the hole, named rather than hidden */
                break;
            }
            off += (uint32_t)w;
        }
        s_t_port_us += (uint32_t)((uint64_t)ag_api()->time->us() - p0);
        total += off;
        if (off != n) {
            break;
        }
    }

    lock_give(s_drain_lock, dheld);
    return total;
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
/*
 * Who painted last owns the glass.
 *
 * This driver carries two kinds of picture down one wire - an application's
 * pixels and the console's characters - and the far end draws whichever
 * arrives.  That was harmless while pixels only ever moved because an
 * application drew them: when the console had the screen, nothing sent pixels.
 *
 * The repair sweep broke that.  It sends rows of the framebuffer on a clock of
 * its own, so after an application exited - or was killed - it went on painting
 * that application's last frame over the console's text.  What Maxim saw was a
 * band of stale graphics crawling up the screen, one row of blocks every half
 * second, thirty rows, fifteen seconds a lap, with the text winning where the
 * two met.  A very accurate description of the bug, as it turned out.
 *
 * So the sweep asks who drew last.  Pixels newer than text: an application owns
 * the screen and the sweep is repairing its picture.  Text newer than pixels:
 * the console owns it, and there is nothing here to repair - the text path has
 * a repair sweep of its own, two hundred lines below.
 */
static uint64_t s_last_blit_us;
static uint64_t s_last_text_us;

static uint64_t s_sweep_at;
static uint32_t s_sweep_owed = 0xffffffffu; /* everything, until told otherwise */
static uint64_t s_sweep_asked_at;           /* when the far end last complained */

#define FR_HELLO 'H'
/*
 * And what the far end says about itself: 'S' then four little-endian
 * counters - frames closed, bands presented, times the display was refused,
 * times there was no memory for a band.
 *
 * It exists because the far end's own console is unreadable while it is
 * drawing (it holds the display) and taking that console back stops it drawing
 * for good.  So "it is drawing" and "it is wedged" were the same observation
 * from over there, and the only way to tell them apart was to restart it.  Now
 * it says so, here, into a console nobody is holding.
 */
#define FR_STAT  'S'
/*
 * And 'R' - "send slice N of frame M again".  Four bytes after it, little
 * endian.  This is the whole of the recovery scheme for whole-frame mode: the
 * far end knows which slices it has because they are numbered, so it can name
 * what is missing instead of asking for the screen back.
 */
#define FR_RESEND 'R'
static void raw_resend(uint32_t frame, uint32_t slice);

/*
 * What the far end says back, collected a byte at a time: a status frame of
 * sixteen bytes, and a four-byte "send slice N of frame M again".
 */
static uint8_t  s_stat[16];
static uint32_t s_stat_have = 0xffffffffu; /* not collecting */
static uint8_t  s_req[4];
static uint32_t s_req_have = 0xffffffffu;

static uint32_t stat32(uint32_t at)
{
    return (uint32_t)s_stat[at] | ((uint32_t)s_stat[at + 1u] << 8) |
           ((uint32_t)s_stat[at + 2u] << 16) |
           ((uint32_t)s_stat[at + 3u] << 24);
}

static void poll_far_end(void)
{
    uint8_t buf[16];
    for (;;) {
        const int32_t n = io->uart_read(LINK_PORT, buf, sizeof(buf), 0);
        if (n <= 0) {
            return;
        }
        /*
         * Counted, because the far end says very little and anything else
         * arriving is the wire talking to itself.  Two hand-soldered lines
         * side by side, one of them swinging at four megabaud: if the receiver
         * is picking that up, it is taking an interrupt for every few bytes of
         * nothing, and that is a cost nobody has looked for.
         */
        s_rx_bytes += (uint32_t)n;
        for (int32_t i = 0; i < n; i++) {
            if (s_stat_have != 0xffffffffu) {
                s_stat[s_stat_have++] = buf[i];
                if (s_stat_have < sizeof(s_stat)) {
                    continue;
                }
                s_stat_have = 0xffffffffu;
                ag_printf("REMTERM: %u frames whole, %u slices in, "
                          "%u buffer pieces, %u resends asked\n",
                          (unsigned)stat32(0), (unsigned)stat32(4),
                          (unsigned)stat32(8), (unsigned)stat32(12));
                continue;
            }
            if (s_req_have != 0xffffffffu) {
                s_req[s_req_have++] = buf[i];
                if (s_req_have < sizeof(s_req)) {
                    continue;
                }
                s_req_have = 0xffffffffu;
                raw_resend((uint32_t)s_req[0] | ((uint32_t)s_req[1] << 8),
                           (uint32_t)s_req[2] | ((uint32_t)s_req[3] << 8));
                continue;
            }
            if (buf[i] == (uint8_t)FR_STAT) {
                s_stat_have = 0;
                continue;
            }
            if (buf[i] == (uint8_t)FR_RESEND) {
                s_req_have = 0;
                continue;
            }
            if (buf[i] == (uint8_t)FR_HELLO) {
                /*
                 * "Send it again."  The far end says this when it starts, on
                 * every frame it could not decode, and now also on every frame
                 * that never arrived at all - which it can see because the
                 * frames are numbered.
                 *
                 * What it triggers is one repair frame covering the screen.
                 * Not hash_forget() on its own, which was too much and too
                 * little at once: too much because forgetting the grid on every
                 * complaint means the trimming never works - measured, the wire
                 * went from 30 KB/s to 380 and the game from 45 fps to 2.4 -
                 * and too little because forgetting only repairs the parts the
                 * game happens to redraw next.  And not a row at a time either,
                 * which is what replaced it: thirty rows is thirty frames and a
                 * stripe crawling down the glass.
                 *
                 * Rate limited, and that is not a nicety: under load the link
                 * loses a frame or two a second, and a full repaint per lost
                 * frame is a repaint that loses the next one.  Two seconds is
                 * the shortest interval at which a repaint (about 20 KB) costs
                 * less than a tenth of the budget.
                 */
                const uint64_t now = (uint64_t)ag_api()->time->us();
                if (s_sweep_asked_at == 0ull ||
                    now - s_sweep_asked_at >= 2000000ull) {
                    s_sweep_asked_at = now;
                    s_sweep_at = 0;
                    s_sweep_owed = 1u;
                }
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

/*
 * Where a band's time goes, because "seven milliseconds of encoding" was an
 * inference from a frame rate and not a measurement, and the four candidates -
 * comparing, gathering, packing, palettising - are all plausible and only one
 * of them can be first.
 */
static uint32_t s_t_hash, s_t_gather, s_t_pack, s_t_idx, s_t_wire;
static uint32_t s_bands;
/* Pixels actually put on the wire, counted band by band as they go.  Declared
 * here rather than beside the other window counters because the band loop is
 * above them and it is the only place that may add to it. */
static uint64_t s_win_px;

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
/* Defined with the sound, below.  The wire task is the only caller of any of
 * them: sound leaves this driver the way the picture does, and both are handed
 * over from the one thread that owns the port. */
static uint32_t aud_pump(uint32_t max);
static void     aud_settle(void);
static void     aud_close_idle(void);
static void     aud_stats(void);
static uint8_t *aud_take_blocks(void);

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
    s_frame_bands = 0;
    send_rect_locked(b);

    /*
     * The end marker goes out under the same lock as the bands it closes.
     * Outside it, the fallback path in blit_rect could put a band of the next
     * frame on the wire between the last band of this one and its end, and the
     * far end would present a frame it had not finished receiving - which is
     * the exact fault this marker exists to remove.
     */
    if (s_frame_bands != 0u) {
        uint8_t end[4];
        end[0] = (uint8_t)s_frame_no;
        end[1] = (uint8_t)(s_frame_no >> 8);
        end[2] = (uint8_t)s_frame_bands;
        end[3] = (uint8_t)(s_frame_bands >> 8);
        (void)send_frame(FR_END, end, sizeof(end));
        s_frame_no++;
    }
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
            const uint32_t th0 = (uint32_t)ag_api()->time->us();
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
            s_t_hash += (uint32_t)ag_api()->time->us() - th0;
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

        /* Gather the rectangle.  A row at a time and not a pixel at a time:
         * the rows are contiguous at both ends and this is on the wire task's
         * critical path. */
        const uint32_t tg0 = (uint32_t)ag_api()->time->us();
        uint16_t *out = (uint16_t *)(s_raw + 12u);
        for (uint32_t r = 0; r < rh; r++) {
            const uint16_t *want =
                (const uint16_t *)(src + (size_t)(band + y0 + r) * b->stride) +
                x0;
            memcpy(out + (size_t)r * rw, want, (size_t)rw * sizeof(uint16_t));
        }
        s_t_gather += (uint32_t)ag_api()->time->us() - tg0;
        s_bands++;

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

        const uint32_t tp0 = (uint32_t)ag_api()->time->us();
        const uint32_t p =
            s_encode ? pack16(out, npx, s_cand + 12u, FR_MAX_PAYLOAD - 12u) : 0u;
        s_t_pack += (uint32_t)ag_api()->time->us() - tp0;
        if (p != 0 && p < best) {
            best = p;
            best_op = FR_PACK;
            best_len = p;
        }
        const uint32_t ti0 = (uint32_t)ag_api()->time->us();
        const uint32_t q =
            s_encode ? encode_indexed(out, npx,
                                      s_cand + 12u + (best_op == FR_PACK ? p : 0u),
                                      FR_MAX_PAYLOAD - 12u -
                                          (best_op == FR_PACK ? p : 0u))
                     : 0u;
        s_t_idx += (uint32_t)ag_api()->time->us() - ti0;
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

        const uint32_t tw0 = (uint32_t)ag_api()->time->us();
        /*
         * Put what is already queued on the wire before adding to it.
         *
         * A rectangle is up to thirty bands and each is a kilobyte or two, so a
         * full screen is more than the ring holds - and the ring only used to
         * be drained between rectangles.  The far end therefore lost the tail
         * of every big rectangle: six thousand frames refused in a ten minute
         * game, near enough a third of the picture, while the wire itself sat
         * two thirds idle.
         *
         * Draining here paces the sender against the wire, which is what a
         * sender should be paced against.  It costs nothing when the ring is
         * empty - one lock, one look - and it is only ever this task and the
         * fallback caller, which serialise on the drain lock.
         */
        (void)tx_drain();

        bool went;
        if (best_op == FR_BAND) {
            went = send_frame(FR_BAND, s_raw, 12u + raw);
        } else {
            for (uint32_t i = 0; i < 12u; i++) {
                s_cand[i] = geom[i];
            }
            went = send_frame(best_op, s_cand, 12u + best_len);
        }
        s_t_wire += (uint32_t)ag_api()->time->us() - tw0;

        /* Only now is it true that the far end has these blocks.  A band that
         * did not go leaves its hashes as they were, so the next flush that
         * reaches these rows sends it again. */
        if (went && row != NULL) {
            for (uint32_t i = 0; i < nfresh; i++) {
                row[s_fresh_col[i]] = s_fresh_hash[i];
            }
        }
        if (went) {
            s_frame_bands++;
            s_win_px += npx;
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
 * What it costs, and what would remove it: a whole screen is 150 KB and about
 * ten milliseconds of PSRAM, which is a third of a frame while a movie is
 * playing, and the 150 KB is why the game's arena had to come down.  Both would
 * go if the kernel said which blits point into a surface that outlives the call
 * - the system framebuffer is allocated once at boot and never moves, so the
 * driver could read it in place and keep no frame at all.  That is one flag in
 * ag_blit_t and one line in display.c.
 *
 * Not done, and the reason is worth writing down rather than rediscovering: it
 * turns a self-contained copy into a pointer this driver holds across time, and
 * a pointer held across time is only as good as the promise that nothing frees
 * it.  There is an unexplained intermittent wedge on this board (the console
 * stops answering mid-game, with and without this driver loaded), and adding a
 * lifetime assumption while that is outstanding is how a second unexplained
 * fault gets built on top of the first.
 *
 * No lock around the copy, deliberately.  The sender may read a rectangle that
 * is being written under it and send half of one frame and half of the next -
 * which is what a video link does when it is not synchronised to the source,
 * and it repairs itself: the hash it records is the hash of what it read, so
 * the next change to those blocks reads as a change and goes out again.  A lock
 * would put the caller back to waiting for the wire, which is the whole thing
 * being fixed.
 */
/*
 * The driver's copy of the picture, laid out as the wire wants it.
 *
 * Not a plain 320x240 array: thirty blocks, each of them a thirty-two byte
 * header followed by eight rows of pixels.  The header is empty space as far
 * as the game is concerned and the whole point as far as the wire is: a slice
 * is then one unbroken run of memory, and one unbroken run can be handed to
 * the DMA in a single transfer with no processor in the path at all.
 *
 * Thirty-two because that is this chip's data cache line, which is what the
 * DMA demands of an address and a length in external memory.  A block is
 * 32 + 5120 = 5152 bytes, a multiple of it, so every block after the first is
 * aligned for free.  The cost is 32 bytes of padding on the wire per slice -
 * six tenths of one per cent - and what it buys is the handover between the
 * processor and the hardware, thirty times a frame, gone.
 */
#define PKT_HDR 32u

static uint8_t   *s_pkt;     /* aligned; thirty header-and-pixels blocks     */
static uint8_t   *s_pkt_raw; /* what was actually allocated, for freeing     */
static uint32_t   s_pkt_blk; /* PKT_HDR + one slice of pixels                */

/* Where slice c's block starts, and where its pixels start inside it. */
static uint8_t *pkt_block(uint32_t c) { return s_pkt + (size_t)c * s_pkt_blk; }
static uint16_t *pkt_pixels(uint32_t c)
{
    return (uint16_t *)(void *)(pkt_block(c) + PKT_HDR);
}
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

static void rem_blit_rect_inner(ag_handle_t h, const ag_blit_t *b)
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
     * Anything that is not the system surface is ignored, and that is a change
     * with a bruise on it.
     *
     * There used to be a fallback here: a blit of a different shape - which is
     * what gfx->present hands over, an application's own surface - was encoded
     * and sent from this thread, the caller's.  In whole-frame mode that path
     * did something much worse than being slow.  It sent bands *and it
     * advanced the frame number*, in the middle of a frame the wire task was
     * halfway through sending.  The far end saw the number change on every
     * slice, decided each one began a new frame, threw away what it had and
     * asked for it all again: two thousand slices, no frames, and a hundred
     * and fourteen thousand requests to resend in twenty seconds.
     *
     * The game calls both - flush for the screen and present for its own
     * surfaces - so both arrive here, and only one of them is the picture.
     */
    if (s_pkt == NULL || b->surf_w != s_fb_w || b->surf_h != s_fb_h) {
        dmg_unlock(held);
        s_late++;
        return;
    }

    s_last_blit_us = (uint64_t)ag_api()->time->us();

    /*
     * This call is the end of the game's frame, and the only place anybody
     * knows the picture is whole.  So the copy happens here - and only when
     * the driver asked for one.
     *
     * Asked-for, rather than every frame, is the whole difference.  Before,
     * every flush copied its rectangle into this driver's frame whether the
     * wire wanted it or not: the game paid three to six milliseconds of PSRAM
     * on every frame it drew, to feed a wire that could take two frames a
     * second.  Now it pays that once per frame *sent*, and the rest of its
     * flushes return in nanoseconds.
     *
     * The whole surface is copied, not the dirty rectangle, because the whole
     * frame is what goes out.  b->px points into the middle of the surface -
     * at the rectangle's corner - so the base is found by walking back.
     */
    if (!s_want_copy || s_have_copy) {
        dmg_unlock(held);
        return;
    }
    const uint64_t c0 = (uint64_t)ag_api()->time->us();
    const uint8_t *base = (const uint8_t *)b->px -
                          (size_t)b->y * b->stride -
                          (size_t)b->x * sizeof(uint16_t);
    const size_t row_bytes = (size_t)s_fb_w * sizeof(uint16_t);
    /*
     * Row by row into the blocks, which is what it was already doing - only
     * the destination is now the pixels of slice r/8 rather than a flat frame.
     * Same bytes, same count, and the headers between them are left alone.
     */
    for (uint32_t r = 0; r < s_fb_h; r++) {
        uint8_t *dst = (uint8_t *)pkt_pixels(r / RAW_ROWS) +
                       (size_t)(r % RAW_ROWS) * row_bytes;
        memcpy(dst, base + (size_t)r * b->stride, row_bytes);
    }
    s_t_copy_us = (uint32_t)((uint64_t)ag_api()->time->us() - c0);
    s_want_copy = false;
    s_have_copy = true;
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
 * How much of the machine this driver may spend, and why it needs a limit.
 *
 * The wire carries four hundred kilobytes a second and the encoder can fill it.
 * Neither of them is the scarce thing: both ends of every band are in PSRAM,
 * and so is the game's picture, and so is the game's arena.  One bus, two
 * cores.  Measured during the opening movie, where the whole screen changes and
 * the driver was free to send 2.45 screens a second: the game's own blit - a
 * palette conversion, pure memory - went from 2 ms to 354, its frame rate to
 * 2.2, and in the end a frame took longer than the fifteen seconds its watchdog
 * allows and the game was killed.  The driver had throttled the thing it exists
 * to show.
 *
 * So: a budget in bytes a second, spent on whatever asks first - real damage
 * before repair, as everywhere else here.
 *
 * Not a frame rate limit: what costs the machine is bytes, and a screen that
 * changed in one corner is not the same as one that changed everywhere.
 *
 * The number was a hundred kilobytes because that is three times what a game
 * standing on a map asks for (33 KB/s measured).  That yardstick was wrong,
 * and it took somebody watching the glass to say so: a game standing still is
 * not what anyone looks at.  When the picture *moves*, every frame is a whole
 * screen - 150 KB of pixels, about 20 KB once encoded - so a hundred kilobytes
 * a second is five whole screens a second and no more.  That is the lag on the
 * glass, and it is this constant, not the wire: four megabaud would carry
 * fifteen screens a second and eight would carry twenty-nine.
 *
 * So it is a build-time number now, to be swept rather than argued about, and
 * what it should be is whatever the game can afford to give up.
 */
#ifndef WIRE_BUDGET_BPS
#define WIRE_BUDGET_BPS 100000u
#endif

static uint64_t s_budget_at;
static uint32_t s_budget_left = WIRE_BUDGET_BPS;
static uint32_t s_budget_waits;

static bool budget_ok(void)
{
    const uint64_t now = (uint64_t)ag_api()->time->us();

    if (now >= s_budget_at) {
        s_budget_at = now + 1000000ull;
        s_budget_left = WIRE_BUDGET_BPS;
    }
    return s_budget_left != 0u;
}

static void budget_spend(uint32_t bytes)
{
    s_budget_left = (bytes >= s_budget_left) ? 0u : (s_budget_left - bytes);
}

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
 * It used to walk one row of blocks per turn, and that was the wrong shape.
 * Thirty rows meant thirty *frames*, each one row tall, twenty milliseconds
 * apart - and what a person sees when a screen is repaired a row at a time is
 * a stripe crawling down the glass for six hundred milliseconds, stopping
 * wherever the budget runs out.  "Everything changes in stripes, and the
 * stripes hang" is the exact report, and it was not a wire fault or a codec
 * fault: it was this loop, drawing a stripe at a time because that was cheap
 * to write.
 *
 * It also poisoned every measurement of the frame rate.  A repair row is one
 * band, so most of what the far end counted as frames were repair rows: 354
 * frames in twenty seconds carrying 296 bands between them, less than one band
 * per frame.  Holding a frame's bands back to present them together - which is
 * the right thing and was asked for - therefore changed nothing at all,
 * because there was nothing to hold.
 *
 * So a repair is now one frame covering the whole screen, and it happens
 * rarely instead of constantly.  The hash trimming means the bands that have
 * not changed cost nothing to skip, so what actually goes out is only what the
 * far end is missing; and it goes out as one frame, which is what a screen
 * changing looks like.
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
 * "Has anything been lost since?" - and the answer is that it depends entirely
 * on how hard this is being driven, which makes sweeping fast the wrong answer
 * twice over.  Measured over five runs of the game, changing one thing at a
 * time:
 *
 *   sweep 500 ms   game 46 fps   1.2 screens/s   31 KB/s   0 bad, 0 stray
 *   sweep  60 ms   game 35 fps   1.7 screens/s   88 KB/s   ~2 bad frames/s
 *
 * Eleven frames a second of the game bought half a screen a second - and the
 * link, which had been carrying thirty kilobytes without losing a byte, started
 * losing two frames a second at eighty-eight.  This wire has no flow control,
 * and the far end drops what arrives while it is writing a band to its panel.
 * Sending less is not only cheaper, it is also more reliable, and the two
 * compound: fewer bytes, fewer losses, fewer repairs needed.
 *
 * So the steady rate is a row every half second, and the fast rate is kept for
 * the two moments that need it - this driver starting, and the far end saying
 * it could not read something.  The second is rate limited to one full sweep in
 * ten seconds, because the far end says that on every corrupt frame and the
 * loop closes otherwise: a repaint per corrupt frame is a repaint that causes
 * the next one, which the receiver's own source predicted and this driver then
 * demonstrated at 2.4 fps.
 */
/*
 * And it only happens when the far end says something is missing.
 *
 * A repair that runs on a clock has to forget the hashes to have any effect,
 * and forgetting them means sending the whole screen again - so a timed sweep
 * costs a full screen every time it fires, forever, on a picture that has not
 * changed at all.  The old one hid that cost by spreading it over thirty turns;
 * spreading it is what made the stripes.
 *
 * There is no need to guess any more.  Every frame is numbered now, so the far
 * end can tell that one never arrived - a gap in the numbering - as well as
 * that one arrived broken, and it says so.  Unasked, nothing is sent: a screen
 * with nothing missing needs no repair, which is the whole point.
 */
static bool sweep_step(uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h)
{
    if (s_hash == NULL || s_hash_rows == 0u || s_fb_w == 0u) {
        return false;
    }
    /* Nobody has drawn a picture, or the console has the screen back. */
    if (s_last_blit_us == 0ull || s_last_text_us > s_last_blit_us) {
        return false;
    }
    if (s_sweep_owed == 0u) {
        return false; /* nothing is missing */
    }
    const uint64_t now = (uint64_t)ag_api()->time->us();
    if (now < s_sweep_at) {
        return false;
    }
    s_sweep_at = now;
    s_sweep_owed = 0u;

    /*
     * The whole screen, and every hash forgotten with it.  Forgetting is what
     * makes the trimming send a block again, so this is the only way to
     * recover a band the far end never got - and doing it for the whole screen
     * at once is what makes the recovery look like a frame rather than a
     * stripe.  What it costs is only the blocks that actually differ from what
     * the far end has, because the trimming still runs: on a still picture the
     * first repair sends everything and the next one sends nothing.
     */
    hash_forget();
    *x = 0;
    *y = 0;
    *w = s_fb_w;
    *h = s_fb_h;
    return *w != 0u && *h != 0u;
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
    /*
     * And not while the console owns the glass, which is the case the guard
     * above does not cover: repair frames keep s_win_rects moving after an
     * application exits, so these lines went on printing - into the console,
     * which at that moment *is* the far end's screen.  Four lines every five
     * seconds, scrolling the very picture they describe.  What a person saw
     * was a screen full of the driver shouting about itself.
     */
    if (s_last_text_us > s_last_blit_us) {
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

    if (s_budget_waits != 0u) {
        ag_printf("REMDISP: %u turns waiting for the budget\n",
                  (unsigned)s_budget_waits);
        s_budget_waits = 0;
    }
    if (s_dropped != 0u || s_lost != 0u || s_late != 0u) {
        ag_printf("REMDISP: %u frames refused (ring full), %u bytes lost in "
                  "the port, %u blits the old way\n",
                  (unsigned)s_dropped, (unsigned)s_lost, (unsigned)s_late);
    }

    if (s_bands != 0u) {
        ag_printf("REMDISP: %u bands, us each: compare %u, gather %u, pack %u, "
                  "palette %u, wire %u\n",
                  (unsigned)s_bands, (unsigned)(s_t_hash / s_bands),
                  (unsigned)(s_t_gather / s_bands), (unsigned)(s_t_pack / s_bands),
                  (unsigned)(s_t_idx / s_bands), (unsigned)(s_t_wire / s_bands));
    }
    s_bands = 0;
    s_t_hash = 0;
    s_t_gather = 0;
    s_t_pack = 0;
    s_t_idx = 0;
    s_t_wire = 0;

    s_win_rects = 0;
    s_win_px = 0;
    s_win_wire = 0;
    s_win_worst_us = 0;
    s_win_behind = 0;
}


/*
 * There used to be a raw_send_slice here: it copied a slice into a staging
 * buffer, framed it, and pushed it through the ring a byte at a time.  The
 * frame is laid out as wire packets now - see pkt_block - so a slice needs
 * no staging and no framing at send time, and the only user this had left
 * was the resend, which hands the same block over again.
 */

static uint32_t raw_slices(void)
{
    return (s_fb_h + RAW_ROWS - 1u) / RAW_ROWS;
}

/* ---- steps 3 and 4: PSRAM to internal memory, internal memory to the wire -
 *
 * Maxim's shape, and it exists to settle one question and no other: does
 * reading and writing PSRAM slow the game down?  So PSRAM is touched exactly
 * once beyond the copy of step 2, and touched the way hardware likes it -
 * straight through, largest run we can hold.  Fill as much of an internal
 * buffer as it will take with finished frames, hand the port that buffer, and
 * go round again until the picture is out.  No slice staged in PSRAM, no ring.
 *
 * If the game is still slow in this shape, PSRAM is not what is slowing it,
 * and the hypothesis is dead rather than wounded.
 */
/*
 * Fill in one slice's header, in place, and return the sum it carries.
 *
 * The header lives in the block, ahead of the pixels it describes, so filling
 * it in is the last thing that happens before the whole block is handed over
 * as one transfer.  The sum travels inside the header rather than after the
 * payload for exactly that reason: a byte at the end would have to be written
 * by somebody, and the only somebody available after the pixels is the
 * processor, which is what this shape exists to keep out of the way.
 */
static void pkt_head(uint32_t c, uint32_t slices, uint32_t y0, uint32_t rows)
{
    uint8_t *const q   = pkt_block(c);
    const uint32_t px  = rows * s_fb_w;
    const uint32_t len = s_pkt_blk - 5u; /* everything after the five */

    q[0] = FR_MAGIC0;
    q[1] = FR_MAGIC1;
    q[2] = FR_RAW;
    q[3] = (uint8_t)len;
    q[4] = (uint8_t)(len >> 8);
    put16(q + 5, s_frame_no);
    put16(q + 7, c);
    put16(q + 9, slices);
    put16(q + 11, s_fb_w);
    put16(q + 13, y0);
    put16(q + 15, rows);
    q[17] = 0;
    memset(q + 18, 0, PKT_HDR - 18u); /* the padding is sent, so it is known */

    /*
     * A word at a time over the geometry and the pixels together - they are
     * contiguous now, which is the other thing this layout buys.  The four
     * lanes fold to the same answer a byte-wise xor would give, because both
     * runs are multiples of four.
     */
    uint32_t acc = 0;
    for (uint32_t i = 5; i < 17u; i++) {
        acc ^= (uint32_t)q[i] << (((i - 5u) & 3u) * 8u);
    }
    const uint32_t *w = (const uint32_t *)(const void *)(q + PKT_HDR);
    const uint32_t  words = (px * sizeof(uint16_t)) / 4u;
    for (uint32_t i = 0; i < words; i++) {
        acc ^= w[i];
    }
    q[17] = (uint8_t)(acc ^ (acc >> 8) ^ (acc >> 16) ^ (acc >> 24));
}

static void raw_resend(uint32_t frame, uint32_t slice)
{
    if (frame != s_frame_no || s_pkt == NULL) {
        return; /* that frame is gone; the next whole one will do instead */
    }
    const uint32_t slices = raw_slices();
    if (slice >= slices) {
        return;
    }
    const uint32_t y0 = slice * RAW_ROWS;
    uint32_t rows = RAW_ROWS;
    if (y0 + rows > s_fb_h) {
        rows = s_fb_h - y0;
    }

    /*
     * The block is still sitting there with the pixels the far end missed, so
     * a resend is the same transfer again - fill the header in (the frame
     * number has not moved yet) and hand it over.
     */
    pkt_head(slice, slices, y0, rows);
    if (s_dma) {
        (void)io->uart_tx_wait(LINK_PORT, 1000u);
        if (io->uart_dma_write(LINK_PORT, pkt_block(slice), s_pkt_blk) >= 0) {
            s_raw_resent++;
            return;
        }
    }
    if (io->uart_write(LINK_PORT, pkt_block(slice), s_pkt_blk) > 0) {
        s_raw_resent++;
    }
}

static void raw_send_direct(void)
{
    const uint32_t slices = raw_slices();

    /*
     * The console goes first, all of it, and then the port is the hardware's
     * until the picture is out.
     *
     * Console text travels by the ring and the ring is written by the
     * processor, which is the other master of this transmitter; handing over
     * between the two costs a wait for the port to empty.  Done once at the
     * top of a frame that is one wait; done between slices, as it used to be,
     * it was thirty, and they cost more than the bytes they carried.
     *
     * Sound is not in this list any more, and that is the point of the change:
     * it leaves by DMA now, so it needs no handover at all and can go out
     * between the slices where it belongs.
     */
    const uint64_t t0 = (uint64_t)ag_api()->time->us();
    aud_close_idle();
    (void)tx_drain();
    if (s_dma) {
        (void)io->uart_tx_wait(LINK_PORT, 1000u);
    }
    s_t_rest_us += (uint32_t)((uint64_t)ag_api()->time->us() - t0);

    uint32_t fill_us = 0;
    uint32_t port_us = 0;
    uint32_t sent = 0;

    for (uint32_t c = 0; c < slices && !s_stop; c++) {
        const uint32_t y0 = c * RAW_ROWS;
        uint32_t rows = RAW_ROWS;
        if (y0 + rows > s_fb_h) {
            rows = s_fb_h - y0;
        }

        const uint64_t f0 = (uint64_t)ag_api()->time->us();
        pkt_head(c, slices, y0, rows);
        fill_us += (uint32_t)((uint64_t)ag_api()->time->us() - f0);

        const uint64_t p0 = (uint64_t)ag_api()->time->us();
#if RAW_SILENT == 4
        ag_delay((uint32_t)(((uint64_t)s_pkt_blk * 10ull * 1000ull) /
                            (uint64_t)LINK_BAUD));
#else
        if (s_dma) {
            /*
             * Handed over and left with the hardware: the queue is what keeps
             * the transmitter fed while this task is being scheduled again.
             * The block belongs to the DMA until the drain at the end of the
             * frame, which is fine - nothing writes it until the next copy,
             * and the next copy cannot happen until this frame is out.
             */
            const int32_t w =
                io->uart_dma_submit(LINK_PORT, pkt_block(c), s_pkt_blk);
            if (w < 0) {
                s_dma = false;
                ag_printf("REMDISP: the port will not take a DMA transfer "
                          "(%d); sending by hand\n", (int)w);
                (void)io->uart_write(LINK_PORT, pkt_block(c), s_pkt_blk);
            }
        } else {
            (void)io->uart_write(LINK_PORT, pkt_block(c), s_pkt_blk);
        }
#endif
        port_us += (uint32_t)((uint64_t)ag_api()->time->us() - p0);

        sent += s_pkt_blk;
        s_sent_frames++;
        s_chunks++;

        /*
         * And a block of sound, into the same queue, if one is ready.
         *
         * This is where the evenness comes from.  Seventeen blocks of sound
         * fit in the 386 ms a picture takes, and there are thirty slices to
         * put them between - so each one is 544 bytes slipped in behind 5152,
         * and the far end's converter is fed steadily right through the frame
         * instead of being handed the lot at the top of it.  Two, not one, so
         * that a burst after a pause is cleared inside a frame rather than
         * trailing the picture for several.
         */
        (void)aud_pump(2u);

        /*
         * Stand aside between slices, but do not touch the port: the kernel
         * work the game leans on is served on this core, and a yield costs
         * nothing when there is nobody waiting for it.
         */
        ag_yield();
    }

    /*
     * And wait for what is still in the air.  Until this returns the blocks
     * are the hardware's, so nothing may draw into them - which is exactly the
     * order the frame handshake already keeps.
     */
    if (s_dma) {
        const uint64_t w0 = (uint64_t)ag_api()->time->us();
        (void)io->uart_dma_drain(LINK_PORT);
        port_us += (uint32_t)((uint64_t)ag_api()->time->us() - w0);
    }
    aud_settle();

    s_t_fill_us += fill_us;
    s_t_port_us += port_us;
    s_t_dma_us += port_us;
    s_sent_bytes += sent;
}


static void raw_stats(void)
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
    if (s_raw_frames == 0u) {
        return;
    }
#if RAW_SILENT == 0 && !SEND_ALWAYS
    if (s_last_text_us > s_last_blit_us) {
        return; /* the console has the glass; do not scroll it */
    }
#endif
    /* In a bisection build nothing reaches the monitor, so the console always
     * owns the glass and the report above would never be printed - and silence
     * there is indistinguishable from a driver that did no work at all. */

    const uint32_t secs = (uint32_t)(STAT_US / 1000000ull);
    const uint32_t fps100 = s_raw_frames * 100u / secs;
    const uint32_t kbs = (uint32_t)(s_raw_bytes / 1024ull / secs);
    /* What the copy costs in megabytes a second, which is the number that
     * says whether PSRAM or the code is the problem. */
    const uint32_t copy_kbs =
        (s_t_copy_us != 0u)
            ? (uint32_t)((uint64_t)s_fb_w * s_fb_h * 2ull * 1000000ull /
                         s_t_copy_us / 1024ull)
            : 0u;

    ag_printf("RAW: %u.%02u frames/s, %u KB/s; copy %u us (%u KB/s), "
              "send %u us, wait %u us, %u resent\n",
              (unsigned)(fps100 / 100u), (unsigned)(fps100 % 100u),
              (unsigned)kbs, (unsigned)s_t_copy_us, (unsigned)copy_kbs,
              (unsigned)s_t_send_us, (unsigned)s_t_wait_us,
              (unsigned)s_raw_resent);
    /* Per frame, so it can be read against the copy and the wire. */
    ag_printf("RAW: per frame us: step2 copy %u, step3 fill %u, step4 port %u"
              " (%u chunks of %u B); ring %u, out %u\n",
              (unsigned)(s_t_copy_us),
              (unsigned)(s_t_fill_us / s_raw_frames),
              (unsigned)(s_t_port_us / s_raw_frames),
              (unsigned)(s_chunks / s_raw_frames),
              (unsigned)s_tx_out_sz,
              (unsigned)(s_t_ring_us / s_raw_frames),
              (unsigned)(s_t_out_us / s_raw_frames));
#if CORE_METER
    {
        static uint32_t was[2];
        const uint32_t  a = s_laps[0], b = s_laps[1];
        ag_printf("RAW: free laps per second: sys %u, app %u\n",
                  (unsigned)((a - was[0]) / (unsigned)(STAT_US / 1000000ull)),
                  (unsigned)((b - was[1]) / (unsigned)(STAT_US / 1000000ull)));
        was[0] = a;
        was[1] = b;
    }
#endif
    ag_printf("RAW: worst us the caller spent inside: blit %u, text %u, "
              "pcm %u\n",
              (unsigned)s_in_blit_us, (unsigned)s_in_text_us,
              (unsigned)s_in_pcm_us);
    s_in_blit_us = 0;
    s_in_text_us = 0;
    s_in_pcm_us = 0;
    aud_stats();
    ag_printf("RAW: bytes in from the far end: %u\n", (unsigned)s_rx_bytes);
    ag_printf("RAW: step4 apart, us per frame: header %u, handover %u, dma %u, "
              "sum %u, between %u\n",
              (unsigned)(s_t_hdr_us / s_raw_frames),
              (unsigned)(s_t_wait_hand_us / s_raw_frames),
              (unsigned)(s_t_dma_us / s_raw_frames),
              (unsigned)(s_t_sum_us / s_raw_frames),
              (unsigned)(s_t_rest_us / s_raw_frames));
    s_t_hdr_us = 0;
    s_t_wait_hand_us = 0;
    s_t_dma_us = 0;
    s_t_sum_us = 0;
    s_t_rest_us = 0;
    s_rx_bytes = 0;
    s_t_gather_us = 0;
    s_t_fill_us = 0;
    s_chunks = 0;
    s_t_ring_us = 0;
    s_t_out_us = 0;
    s_t_port_us = 0;
    if (s_dropped != 0u || s_lost != 0u) {
        ag_printf("RAW: %u slices refused (ring full), %u bytes lost in the "
                  "port\n", (unsigned)s_dropped, (unsigned)s_lost);
    }
    s_raw_frames = 0;
    s_raw_bytes = 0;
    s_raw_resent = 0;
}

#if CORE_METER
/*
 * How much of each core nobody wanted: one counter per core, below every
 * application, counting laps.  The number means nothing on its own - only
 * against the same build in the other state, which is why it is always run
 * twice: driver asleep, driver sending.
 *
 * It stands aside every few thousand laps.  The first version did not, and two
 * tasks that never yield silenced the whole board.
 */
static void spin_task(void *arg)
{
    const uint32_t which = (uint32_t)(uintptr_t)arg;
    uint32_t       n = 0;
    while (!s_stop) {
        s_laps[which]++;
        if (++n >= 4096u) {
            n = 0;
            ag_yield();
        }
    }
}
#endif

#if CORE_METER
/*
 * A clock with no hands, on the game's own core.
 *
 * It asks to be woken every hundred milliseconds and reports how late it was,
 * measured against when the tick was due rather than against the previous one,
 * so a stall is reported once at its true size.  It waits on nothing but the
 * clock: no wire, no screen, no card.  If this is late while a frame is going
 * out, the system is not running the application core's threads, which is
 * Maxim's reading; if it is punctual while the game crawls, then nothing is
 * being starved and the game is waiting on something of its own.
 */
static void tick_task(void *arg)
{
    (void)arg;
    ag_printf("RAW: tick task alive\n");
    const uint64_t us0 = (uint64_t)ag_api()->time->us();
    uint64_t due = us0;
    uint32_t n = 0, worst = 0;
    while (!s_stop) {
        due += 100000ull;
        const uint64_t now = (uint64_t)ag_api()->time->us();
        if (due > now) {
            ag_delay((uint32_t)((due - now) / 1000ull));
        }
        const uint64_t woke = (uint64_t)ag_api()->time->us();
        const uint32_t late =
            (woke > due) ? (uint32_t)((woke - due) / 1000ull) : 0u;
        if (late > worst) {
            worst = late;
        }
        if (++n >= 10u) {
            ag_printf("RAW: app-core tick: worst late %u ms of 10\n",
                      (unsigned)worst);
            worst = 0;
            n = 0;
        }
    }
}
#endif

/*
 * The gaps: whatever is waiting, while there is no picture to send.
 *
 * Sound goes by DMA like the picture, so it goes first and the hardware is
 * given back before the console is written - the console still travels by
 * hand, and the processor must not be at the port while a transfer is in the
 * air.
 */
static void wire_idle_traffic(void)
{
    aud_close_idle();
    if (aud_pump(4u) != 0u) {
        if (s_dma) {
            (void)io->uart_dma_drain(LINK_PORT);
        }
        aud_settle();
    }
    if (s_tx_used != 0u) {
        (void)tx_drain();
    }
}

static void wire_task(void *arg)
{
    (void)arg;
    s_wire_running = true;

    /*
     * Asked here rather than at load: the port is open by now, and on a chip
     * without UHCI - the original ESP32, the S2 - this is simply false and the
     * ordinary write carries the picture as it always did.
     */
    s_dma = (io->uart_dma_ready != NULL) && io->uart_dma_ready(LINK_PORT) &&
            (io->uart_dma_submit != NULL) && (io->uart_dma_drain != NULL) &&
            (io->uart_tx_wait != NULL);
    ag_printf("REMDISP: picture by %s\n",
              s_dma ? "DMA" : "the processor");
#if PORT_ON_SYS_CORE
    {
        const ag_err_t e = io->uart_config(LINK_PORT, LINK_BAUD, 8, 0, 1);
        ag_printf("REMDISP: port opened from the wire task: %s\n",
                  ag_strerror(e));
        s_up = (e == AG_OK);
    }
#endif
    while (!s_stop) {
        poll_far_end();
        (void)tx_drain();
        raw_stats();

        /*
         * Ask for a frame, and wait at the door rather than reaching in.  The
         * game copies it at the end of its own frame - see rem_blit_rect - so
         * what arrives here is never half drawn.
         */
        /*
         * Built with -DRAW_SILENT=1 this driver publishes its devices, takes
         * its task and its frame, and sends nothing at all - no copy, no
         * slices, no port.  It exists to answer one question that three
         * arithmetically impossible explanations failed to: is what costs the
         * game the *sending*, or merely this driver being here?  One variable,
         * and the game's own eng says which.
         */
#if RAW_SILENT == 1
        ag_delay(20);
        continue;
#endif
        /*
         * -DSEND_ALWAYS=1: do not wait for anybody to draw.  The buffer is
         * whatever it happens to hold, and it goes out again and again, which
         * is all that is wanted from a test whose question is what a busy wire
         * does to everything else.  With this the game need not be running at
         * all, so nothing of the game's own can be blamed or credited.
         */
        if (!s_have_copy && !SEND_ALWAYS) {
            const uint64_t w0 = (uint64_t)ag_api()->time->us();
            s_want_copy = true;
            wire_idle_traffic();
            ag_delay(2);
            s_t_wait_us += (uint32_t)((uint64_t)ag_api()->time->us() - w0);
            continue;
        }
        s_t_wait_us = 0;

        if (s_pkt == NULL) {
            s_have_copy = false;
            continue;
        }

        const uint64_t t0 = (uint64_t)ag_api()->time->us();
        const uint64_t b0 = s_sent_bytes;
        raw_send_direct();
        s_t_send_us = (uint32_t)((uint64_t)ag_api()->time->us() - t0);
        s_raw_bytes += s_sent_bytes - b0;
        s_raw_frames++;

        /*
         * A moment for the far end to say what it missed, while the frame is
         * still here to resend.  Once the next copy happens these pixels are
         * gone, and a slice asked for after that is answered by the next whole
         * frame instead - which is not worse, only later.
         */
        /*
         * A moment for the far end to say what it missed, and a shorter one
         * than it was.  Thirty milliseconds of every frame is seven per cent
         * of the wire spent on insurance; ten is enough for a request that is
         * already sitting in the receive buffer, and the far end has asked for
         * nothing at all since the frames started arriving whole.
         */
        /*
         * Only for a far end that has been asking.  The window is dead time on
         * the wire - ten milliseconds of every four hundred - and it is there
         * to catch a request for a slice that is about to be overwritten.  A
         * receiver that has asked for nothing in the last few frames is not
         * about to start between two of them, and if it does the next whole
         * frame answers it anyway, one frame later.
         */
        const uint32_t window_ms = (s_raw_resent != 0u || s_asked_recently) ? 10u : 0u;
        s_asked_recently = (s_raw_resent != 0u);
        const uint64_t until =
            (uint64_t)ag_api()->time->us() + (uint64_t)window_ms * 1000ull;
        while ((uint64_t)ag_api()->time->us() < until && !s_stop) {
            poll_far_end();
            wire_idle_traffic();
            ag_delay(2);
        }

        s_frame_no++;
        s_have_copy = false;
    }

    /* Whatever is still queued, before the port stops being ours - and the
     * transfers in the air first, because rem_stop is about to give their
     * memory back. */
    if (s_dma) {
        (void)io->uart_dma_drain(LINK_PORT);
    }
    aud_settle();
    (void)tx_drain();
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
    uint8_t *fb = s_pkt_raw;
    s_pkt = NULL;
    s_pkt_raw = NULL;
    s_up = false; /* nothing more goes into the ring once it may be freed */
    s_fb_w = 0;
    s_fb_h = 0;
    s_dmg = false;
    dmg_unlock(held);

    /*
     * The sound's blocks the same way and for the same reason: taken away
     * under the lock its producer holds, so a write already inside finishes
     * into memory that is still there and every write after it finds NULL and
     * counts a drop.  Freed below, once the sender has gone - the sender is
     * what hands them to the DMA.
     */
    uint8_t *const ab = aud_take_blocks();

    for (int i = 0; i < 200 && s_wire_running; i++) {
        ag_delay(5);
    }
    if (!s_wire_running) {
        ag_free(fb);
        ag_free(ab);
        ag_free(s_tx);
        s_tx = NULL;

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

/*
 * How long a picture owns the glass after the last band of it went out.
 *
 * "Whoever painted last owns the glass" was already the rule here, and it was
 * only ever applied to the repair sweep.  The console rows went out whatever
 * was happening - so a game that prints to its own console, which is every
 * game with a log or a frame counter, sent five rows a second down the wire
 * beside its picture, and the far end switched to text for each of them.  On
 * the glass that is not a picture with text over it: it is text, running,
 * with the picture appearing between the lines.  Which is exactly the report -
 * "no picture, only text running" - while this end's counters showed 26
 * rectangles a second going out perfectly well.
 *
 * Two seconds, because it has to outlast the gap between two frames of a game
 * that has slowed down, and be short enough that a game which exits gives the
 * console its screen back without a wait anybody notices.
 */
#define TEXT_YIELDS_US 2000000ull

/* True while the picture has the glass and the console has been going into the
 * shadow unsent.  What it buys is the repaint below: rows are only sent when
 * they change, so a console that changed while nobody was showing it would
 * come back a line at a time over the top of the old picture. */
static bool s_text_held;

static void send_row_now(uint16_t row);

static void send_row(uint16_t row)
{
    /*
     * Not while an application is painting.  The text still goes into the
     * shadow, so the whole console can be put out at once when the picture
     * stops - which is the next thing that happens here.
     */
    const uint64_t now = (uint64_t)ag_api()->time->us();
    if (s_last_blit_us != 0ull && now - s_last_blit_us < TEXT_YIELDS_US) {
        s_text_held = true;
        return;
    }
    if (s_text_held) {
        s_text_held = false;
        s_last_text_us = now;
        for (uint32_t r = 0; r < REM_ROWS; r++) {
            send_row_now((uint16_t)r);
        }
        return; /* this row went with the rest of them */
    }
    s_last_text_us = now;
    send_row_now(row);
}

static void send_row_now(uint16_t row)
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

static void rem_text_row_inner(ag_handle_t h, uint16_t row,
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
/* ---- sound -------------------------------------------------------------- */
/*
 * The same wire, one more opcode, and the far end becomes a sound card as well
 * as a screen.  It has to be the same wire: the CYD breaks out three pins and
 * two of them are already this link.
 *
 * Why this lives inside the display driver rather than in a .SYS of its own,
 * which is where it belongs by subject: two modules writing one UART interleave
 * their frames and neither arrives.  One module owning the port is the whole of
 * the arrangement.
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
 *
 * ---- sound is the picture with a different payload -----------------------
 *
 * It used to be the odd one out, and the odd one out is what costs.  A write
 * from the mixer was converted into a ring; a drain copied a chunk of that ring
 * into a staging buffer; send_frame copied the staging buffer into the transmit
 * ring a byte at a time, computing an xor as it went; tx_drain copied the
 * transmit ring into the port's own buffer; and uart_write carried that to the
 * hardware with the processor.  Four copies and a byte-wise checksum for five
 * per cent of the traffic.
 *
 * The copies were not the worst of it.  The processor writing the port is a
 * second master of a transmitter whose other master is the DMA, and the two
 * cannot be interleaved - so every handover between them costs a wait for the
 * port to go empty.  Sound between the slices meant thirty of those a frame,
 * and they were measured to cost more than the bytes they carried; the cure at
 * the time was to send all of the sound at the top of a frame and none of it
 * during, which is 386 ms of jitter on a stream that has to arrive evenly.
 *
 * So sound is laid out exactly as the picture is: blocks of [thirty-two byte
 * header][samples], aligned, contiguous, each of them a finished packet.  The
 * mixer's write converts straight into a block, nothing copies it again, and
 * the wire task hands it to the same DMA queue the slices go into.  Packets
 * from the two streams interleave on the wire in the order they were queued,
 * which is what a packet header is for - and a block of sound now costs the
 * wire its own 544 bytes and nothing else whatever.
 */
#define AUD_RATE_DEFAULT 22050u

/*
 * 512 samples is 23 ms at 22 kHz, and 32 + 512 = 544 bytes is seventeen cache
 * lines - so every block after the first lands aligned for free, which is the
 * same arithmetic that makes the picture's blocks work.
 *
 * Why 23 ms and not the 11.6 the mixer hands down: a block is one transfer and
 * one header, and at this size the sound is seventeen transfers in the time the
 * picture takes thirty.  Halving it would double the queue traffic to buy an
 * evenness the far end's own buffer already provides.
 *
 * Thirty-two blocks is 745 ms of cushion and seventeen kilobytes.  It has to be
 * more than one picture's worth: a block handed to the DMA is not the
 * producer's again until the frame's drain, so a whole frame of sound - all
 * seventeen blocks of it - can be in the air at once, and the ring has to hold
 * that much again to keep filling while it is.
 */
#define AUD_PKT_SAMPLES 512u
#define AUD_PKT_BLK     (PKT_HDR + AUD_PKT_SAMPLES)
#define AUD_PKTS        32u

static ag_audio_fmt_t s_afmt;
static bool           s_afmt_set;
static uint64_t       s_abytes_in;
static uint64_t       s_abytes_sent;
static uint32_t       s_adropped;   /* samples the wire had no room for       */

static uint8_t   *s_apkt;      /* aligned; AUD_PKTS header-and-samples blocks */
static uint8_t   *s_apkt_raw;  /* what was actually allocated, for freeing    */
static uint16_t   s_alen[AUD_PKTS]; /* real samples in each finished block    */
static uint32_t   s_arate;
static uint32_t   s_afill;     /* samples written into the block in hand      */
static uint64_t   s_awrote_at; /* when the mixer last put a sample here       */
/*
 * Three counters, all of them counting blocks since the driver loaded and none
 * of them ever reset: what matters is the difference between two, and unsigned
 * arithmetic wraps the right way round.
 *
 *   below s_a_done          finished, the producer's again
 *   s_a_done .. s_a_sent    handed to the hardware, still in the air
 *   s_a_sent .. s_a_filled  finished packets waiting their turn
 *   s_a_filled              the one being written into
 *
 * The producer only ever touches the block at s_a_filled, and only while
 * s_a_filled - s_a_done is under AUD_PKTS; everything else is either the
 * hardware's or queued behind it.  That is the whole of the concurrency here,
 * and it is why no sample is ever copied twice.
 */
static uint32_t   s_a_filled, s_a_sent, s_a_done;
static ag_mutex_t s_aud_lock;

static uint8_t *apkt_block(uint32_t i)
{
    return s_apkt + (size_t)(i % AUD_PKTS) * AUD_PKT_BLK;
}

/*
 * The header, filled in the moment before the block leaves - the same shape as
 * the picture's, so the far end reads both with one piece of code: five bytes
 * of framing, twelve of what-this-is, the sum at seventeen, zeroes to
 * thirty-two.  Here the twelve say a rate, a count of samples and a sequence
 * number, so a receiver can tell a gap from a reorder.
 *
 * Returns how many bytes to hand over.  That is rounded up to a cache line,
 * because a transfer out of external memory has to be a whole number of them;
 * the padding is whatever the block held last time round the ring and is not
 * played, since the header says how many samples are real.  Thirty-one samples
 * is a millisecond and a half, and only the last block of a sound is ever
 * short.
 */
static uint32_t apkt_head(uint32_t idx, uint32_t samples)
{
    uint8_t *const q   = apkt_block(idx);
    const uint32_t pad = (samples + (PKT_HDR - 1u)) & ~(PKT_HDR - 1u);
    const uint32_t len = (PKT_HDR - 5u) + pad;

    q[0] = FR_MAGIC0;
    q[1] = FR_MAGIC1;
    q[2] = FR_AUD;
    q[3] = (uint8_t)len;
    q[4] = (uint8_t)(len >> 8);
    put16(q + 5, s_arate & 0xffffu);
    put16(q + 7, s_arate >> 16);
    put16(q + 9, samples);
    put16(q + 11, idx & 0xffffu);
    put16(q + 13, 0);
    put16(q + 15, 0);
    q[17] = 0;
    memset(q + 18, 0, PKT_HDR - 18u);

    uint32_t acc = 0;
    for (uint32_t i = 5; i < 17u; i++) {
        acc ^= (uint32_t)q[i] << (((i - 5u) & 3u) * 8u);
    }
    const uint32_t *w = (const uint32_t *)(const void *)(q + PKT_HDR);
    for (uint32_t i = 0; i < pad / 4u; i++) {
        acc ^= w[i];
    }
    q[17] = (uint8_t)(acc ^ (acc >> 8) ^ (acc >> 16) ^ (acc >> 24));
    return PKT_HDR + pad;
}

/* Everything handed over has left, so the blocks behind it are the producer's
 * again.  Only ever called where the transfers are known to be finished. */
static void aud_settle(void)
{
    if (s_a_done == s_a_sent) {
        return;
    }
    const bool held = lock_take(s_aud_lock, 100u);
    s_a_done = s_a_sent;
    lock_give(s_aud_lock, held);
}

/*
 * Hand over what is ready, up to a limit, and say how many went.
 *
 * The block is taken out of the queue under the lock and only then filled in
 * and submitted, so a block the producer can still see is never one the
 * hardware is about to read.  Called from the wire task and nowhere else -
 * one writer of the port, which is the rule this whole driver is built on.
 */
static uint32_t aud_pump(uint32_t max)
{
    if (s_apkt == NULL) {
        return 0u;
    }
    uint32_t went = 0;
    while (went < max) {
        const bool     held = lock_take(s_aud_lock, 100u);
        const bool     any  = (s_a_filled - s_a_sent) != 0u;
        const uint32_t idx  = s_a_sent;
        const uint32_t n    = any ? s_alen[idx % AUD_PKTS] : 0u;
        if (any) {
            s_a_sent++;
        }
        lock_give(s_aud_lock, held);
        if (!any) {
            break;
        }

        const uint32_t bytes = apkt_head(idx, n);
        bool sent = false;
        if (s_dma) {
            const int32_t w =
                io->uart_dma_submit(LINK_PORT, apkt_block(idx), bytes);
            if (w < 0) {
                s_dma = false;
                ag_printf("REMDISP: the port will not take a DMA transfer "
                          "(%d); sound by hand\n", (int)w);
            } else {
                sent = true;
            }
        }
        if (!sent) {
            /* No DMA on this chip, or it has just refused one.  The block is
             * a finished packet either way, so there is nothing to do here but
             * carry it: no staging, no framing, no checksum. */
            (void)io->uart_write(LINK_PORT, apkt_block(idx), bytes);
            aud_settle();
        }
        s_abytes_sent += (uint64_t)n;
        s_sent_frames++;
        went++;
    }
    return went;
}

/*
 * A block that has stopped filling.
 *
 * A sound ending part-way through one would otherwise sit here until the next
 * sound started - the tail of a line of speech arriving behind the silence
 * after it.  A tenth of a second with nothing written is the end of something,
 * so close the block and let it go short.
 */
static void aud_close_idle(void)
{
    if (s_apkt == NULL || s_afill == 0u) {
        return;
    }
    if ((uint64_t)ag_api()->time->us() - s_awrote_at < 100000ull) {
        return;
    }
    const bool held = lock_take(s_aud_lock, 100u);
    if (s_afill != 0u && (s_a_filled - s_a_done) < AUD_PKTS) {
        s_alen[s_a_filled % AUD_PKTS] = (uint16_t)s_afill;
        s_afill = 0;
        s_a_filled++;
    }
    lock_give(s_aud_lock, held);
}

/*
 * One line in the five-second report, and only when there is something to say.
 * What matters here is whether the sound is keeping up: bytes a second says
 * whether the mixer above is producing at its rate, the queue depth says
 * whether the wire is taking them at that rate, and anything dropped says it
 * stopped being true.
 */
static void aud_stats(void)
{
    static uint64_t was;
    const uint32_t  n = (uint32_t)(s_abytes_sent - was);
    was = s_abytes_sent;
    if (n == 0u && s_adropped == 0u) {
        return;
    }
    const uint32_t secs = (uint32_t)(STAT_US / 1000000ull);
    ag_printf("RAW: sound %u B/s, %u blocks, %u waiting, %u dropped\n",
              (unsigned)(n / secs),
              (unsigned)((n + AUD_PKT_SAMPLES - 1u) / AUD_PKT_SAMPLES),
              (unsigned)(s_a_filled - s_a_sent), (unsigned)s_adropped);
    s_adropped = 0;
}

/*
 * Unload: take the blocks away from the producer and say what to free.
 *
 * Under the lock, so a write already inside finishes into memory that is still
 * there; every write after this finds NULL and counts a drop.  The memory is
 * not freed here - the sender may still have blocks in the air, and rem_stop
 * gives them back once it has gone.
 */
static uint8_t *aud_take_blocks(void)
{
    const bool held = lock_take(s_aud_lock, 1000u);
    uint8_t *const raw = s_apkt_raw;
    s_apkt = NULL;
    s_apkt_raw = NULL;
    lock_give(s_aud_lock, held);
    return raw;
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
static int32_t rem_pcm_write_inner(ag_device_t *dev, const void *buf, size_t len,
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
     * Converted straight into the block that will leave by DMA - there is no
     * second buffer between this and the wire.  It returns at once and holds
     * nothing across the port, which is the whole point: a sound device that
     * waits stops the game above it.
     */
    const int16_t *src   = (const int16_t *)buf;
    const bool     wheld = lock_take(s_aud_lock, 100u);
    s_arate = s_afmt.rate;
    if (s_apkt == NULL) {
        s_adropped += (uint32_t)frames;
    } else {
        for (size_t i = 0; i < frames; i++) {
            uint8_t m;
            if (chans == 2u) {
                const int32_t mono =
                    ((int32_t)src[i * 2u] + src[i * 2u + 1u]) / 2;
                m = (uint8_t)((mono + 32768) >> 8);
            } else {
                m = (uint8_t)(((int32_t)src[i] + 32768) >> 8);
            }
            if (s_afill == 0u && (s_a_filled - s_a_done) >= AUD_PKTS) {
                if (s_a_filled == s_a_sent) {
                    /* Every block is in the air at once, which takes a frame
                     * of picture far longer than one should ever be.  There is
                     * nowhere to put this sample. */
                    s_adropped++;
                    continue;
                }
                /*
                 * Otherwise: three quarters of a second is queued and the wire
                 * is not going to catch up.  Playing the backlog out in order
                 * would only keep the sound that far behind the picture for
                 * ever, so the queue is thrown away and the sound starts again
                 * from now.  What is in the air is left alone - it belongs to
                 * the hardware until the frame's drain.
                 */
                s_adropped += (s_a_filled - s_a_sent) * AUD_PKT_SAMPLES;
                s_a_filled = s_a_sent;
            }
            apkt_block(s_a_filled)[PKT_HDR + s_afill] = m;
            s_afill++;
            if (s_afill == AUD_PKT_SAMPLES) {
                s_alen[s_a_filled % AUD_PKTS] = (uint16_t)AUD_PKT_SAMPLES;
                s_afill = 0;
                s_a_filled++;
            }
        }
        s_awrote_at = (uint64_t)ag_api()->time->us();
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

/* ---- how long the caller spends in here ---------------------------------
 *
 * Everything the game asks of this driver goes through one of these three, so
 * timing them from the outside answers the question the whole investigation
 * came down to: while a frame is on the wire the game's engine step grows from
 * 20 ms to 580, its own core is provably free, and a thread of priority twelve
 * on that core wakes on time.  So the game is waiting somewhere, and if it is
 * waiting in here, this is where it shows.  Worst case per window, because an
 * average would hide exactly the stall being hunted.
 */
static void rem_blit_rect(ag_handle_t h, const ag_blit_t *b)
{
    const uint64_t t0 = (uint64_t)ag_api()->time->us();
    rem_blit_rect_inner(h, b);
    const uint32_t d = (uint32_t)((uint64_t)ag_api()->time->us() - t0);
    if (d > s_in_blit_us) {
        s_in_blit_us = d;
    }
}

static void rem_text_row(ag_handle_t h, uint16_t row,
                         const ag_textcell_t *cells, uint16_t count)
{
    const uint64_t t0 = (uint64_t)ag_api()->time->us();
    rem_text_row_inner(h, row, cells, count);
    const uint32_t d = (uint32_t)((uint64_t)ag_api()->time->us() - t0);
    if (d > s_in_text_us) {
        s_in_text_us = d;
    }
}

static int32_t rem_pcm_write(ag_device_t *dev, const void *buf, size_t len,
                             uint64_t off)
{
    const uint64_t t0 = (uint64_t)ag_api()->time->us();
    const int32_t  r = rem_pcm_write_inner(dev, buf, len, off);
    const uint32_t d = (uint32_t)((uint64_t)ag_api()->time->us() - t0);
    if (d > s_in_pcm_us) {
        s_in_pcm_us = d;
    }
    return r;
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
        s_tx_lock = ag_api()->task->mutex_create();
        s_drain_lock = ag_api()->task->mutex_create();
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
    /*
     * Where this call is made decides which core the UART's interrupt lands
     * on: uart_driver_install allocates it on the calling core, and this
     * function runs on the console's - which is the game's.  At four megabaud
     * the transmitter empties its FIFO every 320 us, so that is three thousand
     * interrupts a second into whichever core is unlucky.  Built with
     * -DPORT_ON_SYS_CORE=1 the port is opened by the wire task instead, which
     * is pinned to the system core, and nothing else changes.
     */
#if PORT_ON_SYS_CORE
    const ag_err_t err = AG_OK;
#else
    const ag_err_t err = io->uart_config(LINK_PORT, LINK_BAUD, 8, 0, 1);
#endif
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
        /*
         * -DNO_AUDIO=1 leaves the speaker unpublished, so the game finds no
         * sink on this wire and its sound goes nowhere.  One variable, and it
         * decides whether what stalls the game is the picture leaving or the
         * sound trying to leave beside it: the game paces itself on audio, and
         * audio here shares the wire with a hundred and fifty kilobytes.
         */
#if NO_AUDIO
        const ag_err_t pcm_added = -AG_EINVAL;
#else
        const ag_err_t pcm_added = ag_dev_add(&pcm);
#endif
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
    /*
     * Internal first, and as big a mouthful as it will give: 32 KB means one
     * uart_write per two slices instead of seventy-five per frame, and the
     * port stops reading PSRAM altogether.  Whatever is refused falls back to
     * what was there before, so a system with no internal memory to spare
     * still gets a picture - just the slow one.
     */
    static const uint32_t want[] = { 32768u, 16384u, 8192u, 4096u };
    for (uint32_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        uint8_t *fast = (uint8_t *)ag_malloc_caps(want[i], AG_MEM_FAST);
        if (fast != NULL) {
            s_tx_out    = fast;
            s_tx_out_sz = want[i];
            break;
        }
    }
    /* The ring stays where it was on purpose: one variable per measurement,
     * and this run is about the buffer the port reads from. */
    s_tx = (uint8_t *)ag_malloc(TX_RING);
    ag_printf("REMDISP: port buffer %u B %s, ring %u B\n",
              (unsigned)s_tx_out_sz,
              (s_tx_out == s_tx_out_fallback) ? "in PSRAM (none spare)"
                                              : "internal",
              (unsigned)TX_RING);
    /*
     * One block per slice, and thirty-one bytes of slack so the first block
     * can be pushed up to the alignment the DMA wants.  ag_malloc promises
     * nothing about alignment and does not have to: this is the only place
     * that cares.
     */
    s_pkt_blk = PKT_HDR + RAW_ROWS * s_fb_w * (uint32_t)sizeof(uint16_t);
    {
        const size_t want =
            (size_t)((s_fb_h + RAW_ROWS - 1u) / RAW_ROWS) * s_pkt_blk;
        s_pkt_raw = (uint8_t *)ag_malloc(want + PKT_HDR - 1u);
        s_pkt = (s_pkt_raw == NULL)
                    ? NULL
                    : (uint8_t *)(((uintptr_t)s_pkt_raw + PKT_HDR - 1u) &
                                  ~(uintptr_t)(PKT_HDR - 1u));
    }
    /*
     * And the sound's own blocks, by the same arithmetic: 32 + 512 is a whole
     * number of cache lines, so aligning the front of the run aligns every one
     * of them.  Seventeen kilobytes.  A driver that cannot have them still
     * shows a picture - it simply has nowhere to put sound, and says so in the
     * counters rather than in a fault.
     */
    s_apkt_raw =
        (uint8_t *)ag_malloc((size_t)AUD_PKTS * AUD_PKT_BLK + PKT_HDR - 1u);
    s_apkt = (s_apkt_raw == NULL)
                 ? NULL
                 : (uint8_t *)(((uintptr_t)s_apkt_raw + PKT_HDR - 1u) &
                               ~(uintptr_t)(PKT_HDR - 1u));
    if (s_apkt == NULL) {
        ag_printf("REMDISP: no blocks for the sound (%u KB); the picture goes "
                  "out alone\n",
                  (unsigned)(AUD_PKTS * AUD_PKT_BLK / 1024u));
    }

    if (s_pkt == NULL || s_tx == NULL) {
        s_fb_w = 0;
        s_fb_h = 0;
        ag_printf("REMDISP: no frame of my own (%u KB); the wire will be fed "
                  "by whoever draws, and they will wait for it\n",
                  (unsigned)(320u * 240u * 2u / 1024u));
    } else {
        memset(s_pkt, 0,
               (size_t)((s_fb_h + RAW_ROWS - 1u) / RAW_ROWS) * s_pkt_blk);
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
#if CORE_METER
        /*
         * One probe, not three.  A module is allowed two tasks, and the wire
         * needs one of them: asking for more took the wire's slot and the
         * driver quietly fell back to sending from whoever draws - which sent
         * nothing at all, and made a run that looked like a cure.
         */
        ag_printf("RAW: tick probe: %s\n",
                  ag_module_task(tick_task, NULL, "remtick", 3072u, TICK_PRIO,
                                 AG_THREAD_APP_CORE)
                      ? "yes" : "no");
#endif
        /*
         * The priority is a knob because it is a claim, not a fact: six is
         * above every ordinary application, and a task above applications is
         * promising to block at once.  This one does not - it sits in a write
         * that lasts most of a frame.  -DWIRE_PRIO=3 puts it below them and
         * says whether that promise is what the game is paying for.
         */
        if (!ag_module_task(wire_task, NULL, "remwire", 6144u, WIRE_PRIO,
                            AG_THREAD_SYS_CORE)) {
            ag_free(s_pkt_raw);
            s_pkt = NULL;
            s_pkt_raw = NULL;
            s_fb_w = 0;
            s_fb_h = 0;
            ag_printf("REMDISP: no task of my own; sending from the caller\n");
        } else {
            s_task_ok = true;
            ag_module_on_unload(rem_stop);
        }
    }

    ag_printf("REMDISP: uart%d at %u baud, budget %u B/s; %ux%u cells, "
              "pixels in %u-row bands%s\n",
              LINK_PORT, (unsigned)LINK_BAUD, (unsigned)WIRE_BUDGET_BPS,
              (unsigned)REM_COLS, (unsigned)REM_ROWS, (unsigned)BAND_ROWS,
              (s_pkt != NULL) ? ", sent from a task of its own" : "");
    return AG_OK;
}
