/*
 * ArgonOS - a picture that will not compress, so a wire can be measured.
 *
 * Every other graphics test on this system draws shapes on a flat ground, and
 * REMDISP is good at exactly that: the demo picture that took 75 000 pixels to
 * the far board crossed as THREE KILOBYTES, because a band of one colour packs
 * to almost nothing.  Comparing two transports on it measures the encoder and
 * the cost of a transaction, not the copper - both runs were nowhere near the
 * wire's capacity, and the wire was never the thing under test.
 *
 * This fills the surface with noise instead.  Noise does not pack, does not
 * index, and does not match the previous frame, so every band is sent whole
 * and the number that comes out the other end is the transport's own.  It is
 * the video case, made reproducible: a frame that has to cross in full.
 *
 *   gfxnoise [seconds] [rows] [noise|box|flat]
 *
 *   noise  every frame new noise: nothing to pack, nothing unchanged - the
 *          wire's own number (the default)
 *   box    one still picture of noise and a 48x48 square moving across it:
 *          what a screen that sends only what changed is for
 *   flat   eight bars of flat colour, a new colour every frame: every pixel
 *          changes, and every row packs
 *
 * `rows` is the height of one presented band, eight by default, which is what
 * REMDISP cuts a frame into.  The run reports the frames it pushed and the
 * bytes they were, so the sender's side of the measurement is readable without
 * the receiver.
 *
 * The generator is xorshift32 and deliberately not seeded from the clock: two
 * runs draw the same pixels, so a difference between two transports is the
 * transport.
 */
#include <argon/argon.h>

AG_APP("GFXNOISE", "1.0", "argon", AG_AXE_NEEDS_GFX);

#define NOISE_BAND_MAX 16u
#define NOISE_W_MAX    640u
/*
 * One band, plus slack to slide the window through.  The slack is what makes
 * consecutive frames differ without costing anything to produce.
 */
#define POOL_SLACK     4096u

static uint16_t s_pool[NOISE_W_MAX * NOISE_BAND_MAX + POOL_SLACK];
/* one band being made, for the modes that are not a slice of the pool */
static uint16_t s_band[NOISE_W_MAX * NOISE_BAND_MAX];

#define BOX 48u

static uint32_t s_rng = 0x2545F491u;

static uint32_t xorshift(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static uint32_t number(const char *s)
{
    uint32_t v = 0;
    if (s == NULL) {
        return 0;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s++ - '0');
    }
    return v;
}

int ag_main(int argc, char **argv)
{
    ag_gfxinfo_t info;

    uint32_t secs = (argc >= 2) ? number(argv[1]) : 10u;
    uint32_t rows = (argc >= 3) ? number(argv[2]) : 8u;
    const char mode = (argc >= 4) ? argv[3][0] : 'n'; /* n, b or f */

    if (secs == 0u) {
        secs = 10u;
    }
    if (rows == 0u || rows > NOISE_BAND_MAX) {
        rows = 8u;
    }

    if (ag_api()->gfx == NULL || !AG_HAS(ag_api()->gfx, present)) {
        ag_printf("gfxnoise: this kernel has no gfx->present (ABI 0.31)\n");
        return 1;
    }

    const ag_err_t err = ag_gfx_acquire(&info);
    if (err != AG_OK) {
        ag_printf("gfxnoise: acquire: %s\n", ag_strerror(err));
        return 1;
    }

    const uint16_t w = (info.width > NOISE_W_MAX) ? NOISE_W_MAX : info.width;
    const uint16_t h = info.height;

    ag_printf("gfxnoise: %ux%u, bands of %u rows, %u s\n", (unsigned)w,
              (unsigned)h, (unsigned)rows, (unsigned)secs);

    for (uint32_t i = 0; i < (uint32_t)(NOISE_W_MAX * NOISE_BAND_MAX +
                                       POOL_SLACK); i++) {
        s_pool[i] = (uint16_t)xorshift();
    }

    uint32_t        off = 0;
    uint32_t        box_x = 0;
    int32_t         box_dx = 4;
    uint32_t        frames = 0;
    uint32_t        bytes = 0;
    const ag_time_t t0 = ag_micros();
    const ag_time_t until = t0 + (ag_time_t)secs * 1000000u;

    while (ag_micros() < until) {
        for (uint16_t y0 = 0; y0 < h; y0 = (uint16_t)(y0 + rows)) {
            const uint16_t n =
                ((uint16_t)(h - y0) < rows) ? (uint16_t)(h - y0) : (uint16_t)rows;

            /*
             * THE NOISE IS MADE ONCE AND THEN SLID, and that is the whole
             * difference between measuring a wire and measuring this program.
             * Drawing every pixel with the generator cost more than the
             * transport did: the sender reported 1950 KB/s at 40 MHz while
             * the wire was idle waiting for it. The pool below is one band
             * longer than a band, so every frame starts at a different offset
             * in it - the bytes never repeat against the previous frame, so
             * nothing packs and nothing indexes, and the cost per band is one
             * pointer.
             */
            const uint16_t *src = s_pool + off;
            if (mode == 'b') {
                /* the same noise for this band every frame, and the square
                 * over it where it is now */
                const uint32_t by = (h - BOX) / 2u;
                for (uint32_t k = 0; k < (uint32_t)w * n; k++) {
                    s_band[k] = s_pool[((uint32_t)y0 * 97u + k) % POOL_SLACK];
                }
                for (uint32_t r = 0; r < n; r++) {
                    const uint32_t y = y0 + r;
                    if (y < by || y >= by + BOX) {
                        continue;
                    }
                    for (uint32_t c = 0; c < BOX && box_x + c < w; c++) {
                        s_band[r * w + box_x + c] = 0xFFFFu;
                    }
                }
                src = s_band;
            } else if (mode == 'f') {
                for (uint32_t r = 0; r < n; r++) {
                    for (uint32_t x = 0; x < w; x++) {
                        const uint32_t bar = x * 8u / w;
                        s_band[r * w + x] =
                            (uint16_t)((frames * 2654435761u) ^ (bar * 0x1111u));
                    }
                }
                src = s_band;
            } else {
                off += 61u; /* prime-ish, so the window does not fall into step */
                if (off > POOL_SLACK) {
                    off = 0;
                }
            }

            const ag_blit_t b = {
                .px = src,
                .stride = (uint32_t)w * sizeof(uint16_t),
                .surf_w = w,
                .surf_h = h,
                .x = 0,
                .y = y0,
                .w = w,
                .h = n,
            };
            if (ag_gfx_present(&b) != AG_OK) {
                ag_printf("gfxnoise: present at y=%u failed\n", (unsigned)y0);
                ag_gfx_release();
                return 1;
            }
            bytes += (uint32_t)w * n * 2u;
        }
        frames++;
        if (mode == 'b') {
            if ((int32_t)box_x + box_dx < 0 ||
                box_x + (uint32_t)box_dx + BOX > w) {
                box_dx = -box_dx;
            }
            box_x = (uint32_t)((int32_t)box_x + box_dx);
        }
        /*
         * Nothing sleeps here on purpose: the point is to keep the wire as
         * busy as the sender can make it, so what is measured is the wire and
         * not a delay this program chose.
         */
    }

    const uint32_t ms = (uint32_t)((ag_micros() - t0) / 1000u);
    ag_printf("gfxnoise: %u frames, %u KB of pixels in %u ms\n",
              (unsigned)frames, (unsigned)(bytes / 1024u), (unsigned)ms);
    if (ms != 0u) {
        ag_printf("gfxnoise: %u KB/s offered, %u frames/s\n",
                  (unsigned)(bytes / ms * 1000u / 1024u),
                  (unsigned)(frames * 1000u / ms));
    }

    ag_gfx_release();
    return 0;
}
