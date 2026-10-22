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
 *   gfxnoise [seconds] [rows] [noise|box|flat|balls [count]]
 *
 *   noise  every frame new noise: nothing to pack, nothing unchanged - the
 *          wire's own number (the default)
 *   box    one still picture of noise and a 48x48 square moving across it:
 *          what a screen that sends only what changed is for
 *   flat   eight bars of flat colour, a new colour every frame: every pixel
 *          changes, and every row packs
 *   balls  `count` solid balls (eight by default, sixteen at most) on a plain
 *          ground, bouncing off the walls and off each other: a few small
 *          regions changing in every band, and flat rows between them
 *   scroll a map of painted tiles in 256 colours sliding under the screen,
 *          `count` pixels a frame (four by default): every pixel changes
 *          every frame, as when a game moves its whole map - short runs, no
 *          flat ground, and never more than 256 colours
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

/*
 * THE MAP FOR `scroll`: eight tiles of 64x64 palette indices, laid out by a
 * hash of where they are, in 256 fixed colours.  A painted game's art is not
 * noise and not flat: it has short runs, dithering, and a palette - so that is
 * what this is, and it is cheap enough to make every frame.
 */
#define TILE      64u
#define TILES     8u
static uint8_t  s_tile[TILES][TILE * TILE];
static uint16_t s_pal[256];
static uint32_t xorshift(void);

static void map_start(void)
{
    for (uint32_t i = 0; i < 256u; i++) {
        s_pal[i] = (uint16_t)xorshift();
    }
    for (uint32_t t = 0; t < TILES; t++) {
        uint8_t v = (uint8_t)xorshift();
        for (uint32_t k = 0; k < TILE * TILE; k++) {
            /* a third of the time the pixel repeats its left neighbour */
            if ((xorshift() % 3u) != 0u) {
                v = (uint8_t)xorshift();
            }
            s_tile[t][k] = v;
        }
    }
}

static void map_band(uint16_t *band, uint16_t w, uint16_t y0, uint16_t n,
                     uint32_t sx, uint32_t sy)
{
    for (uint32_t r = 0; r < n; r++) {
        const uint32_t my = y0 + r + sy;
        const uint32_t ty = my / TILE;
        for (uint32_t x = 0; x < w; x++) {
            const uint32_t mx = x + sx;
            const uint32_t t = ((mx / TILE) * 73856093u ^ ty * 19349663u) % TILES;
            band[r * w + x] = s_pal[s_tile[t][(my % TILE) * TILE + mx % TILE]];
        }
    }
}

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

/*
 * The balls.  Everything is fixed point, 1/256 of a pixel, because not every
 * core this runs on has a float unit (the C3 has none), and it all fits
 * int32: two balls are tested only once they are within a diameter on both
 * axes, and a diameter squared in these units is 10240^2, a hundred million.
 */
#define BALLS_MAX  16u
#define BALL_R_MAX 20u
#define BALL_BG    0x0841u /* nearly black, so the balls are all there is */
#define ONE        4096    /* a unit vector's length, for the line of centres */

typedef struct {
    int32_t x, y;   /* the centre, 1/256 px */
    int32_t vx, vy; /* 1/256 px a frame */
} ball_t;

static ball_t s_balls[BALLS_MAX];
/* half the width of a ball's row, by how far the row is from the centre */
static uint16_t s_span[BALL_R_MAX + 1u];

static const uint16_t s_ball_rgb[BALLS_MAX] = {
    0xF800u, 0x07E0u, 0x001Fu, 0xFFE0u, 0xF81Fu, 0x07FFu, 0xFD20u, 0xFFFFu,
    0x8010u, 0x0400u, 0x8400u, 0x041Fu, 0xFB2Cu, 0x9E66u, 0xAD55u, 0x5ACBu,
};

static uint32_t isqrt(uint32_t v)
{
    uint32_t r = 0;
    uint32_t bit = 1u << 30;
    while (bit > v) {
        bit >>= 2;
    }
    while (bit != 0u) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/*
 * Lay the balls out on a grid of cells a diameter and a little apart, so none
 * starts inside another, spread over the whole grid rather than packed into
 * its first row.  Returns how many there are room for.
 */
static uint32_t balls_start(uint16_t w, uint16_t h, uint32_t count,
                            uint32_t rad)
{
    const uint32_t cell = 2u * rad + 4u;
    const uint32_t cols = w / cell;
    const uint32_t cells = cols * (h / cell);

    if (count > cells) {
        count = cells;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t k = i * cells / count;
        ball_t *const  b = &s_balls[i];
        b->x = (int32_t)(((k % cols) * cell + cell / 2u) << 8);
        b->y = (int32_t)(((k / cols) * cell + cell / 2u) << 8);
        /* 1.5 to 4.5 px a frame on each axis, either way */
        b->vx = 384 + (int32_t)(xorshift() % 768u);
        b->vy = 384 + (int32_t)(xorshift() % 768u);
        if (xorshift() & 1u) {
            b->vx = -b->vx;
        }
        if (xorshift() & 1u) {
            b->vy = -b->vy;
        }
    }
    for (uint32_t dy = 0; dy <= rad; dy++) {
        s_span[dy] = (uint16_t)isqrt(rad * rad - dy * dy);
    }
    return count;
}

static int32_t clamp_v(int32_t v, int32_t lim)
{
    return (v > lim) ? lim : (v < -lim) ? -lim : v;
}

/*
 * v * n / ONE, rounded to the nearest and the same way either side of zero.
 * Plain division rounds toward zero, which on this line always takes a little
 * from the pair that is parting: a host run of the first version lost seven
 * eighths of its speed in a minute of frames, and this one does not.
 */
static int32_t along(int32_t v, int32_t n)
{
    const int32_t p = v * n;
    return (p >= 0) ? (p + ONE / 2) / ONE : -((-p + ONE / 2) / ONE);
}

/*
 * One frame of motion.  Two equal masses meeting elastically trade the parts
 * of their velocities along the line between their centres and keep the rest,
 * and that is all a collision here is - plus pushing an overlapping pair
 * apart along the same line, half each, or they stick and spin.  The walls
 * come last, so what is drawn is always on the screen.
 */
static void balls_step(uint16_t w, uint16_t h, uint32_t count, uint32_t rad)
{
    const int32_t r8 = (int32_t)rad << 8;
    const int32_t dmin = 2 * r8;
    /* rounding in the exchange can pile speed into one ball over a long run;
     * half a radius a frame keeps it from ever passing through another */
    const int32_t vlim = r8 / 2;

    for (uint32_t i = 0; i < count; i++) {
        s_balls[i].x += s_balls[i].vx;
        s_balls[i].y += s_balls[i].vy;
    }

    for (uint32_t i = 0; i < count; i++) {
        ball_t *const a = &s_balls[i];
        for (uint32_t j = i + 1u; j < count; j++) {
            ball_t *const b = &s_balls[j];
            const int32_t dx = b->x - a->x;
            const int32_t dy = b->y - a->y;
            if (dx >= dmin || dx <= -dmin || dy >= dmin || dy <= -dmin) {
                continue;
            }
            const int32_t d2 = dx * dx + dy * dy;
            if (d2 >= dmin * dmin) {
                continue;
            }
            const int32_t d = (int32_t)isqrt((uint32_t)d2);
            int32_t nx = ONE;
            int32_t ny = 0;
            if (d != 0) {
                nx = (dx * ONE + ((dx < 0) ? -d : d) / 2) / d;
                ny = (dy * ONE + ((dy < 0) ? -d : d) / 2) / d;
            }

            const int32_t push = (dmin - d) / 2 + 1;
            a->x -= along(push, nx);
            a->y -= along(push, ny);
            b->x += along(push, nx);
            b->y += along(push, ny);

            /* closing speed along the line; a pair already parting is left
             * alone, or it would be pulled back together */
            const int32_t p =
                along(a->vx - b->vx, nx) + along(a->vy - b->vy, ny);
            if (p > 0) {
                const int32_t qx = along(p, nx);
                const int32_t qy = along(p, ny);
                a->vx = clamp_v(a->vx - qx, vlim);
                a->vy = clamp_v(a->vy - qy, vlim);
                b->vx = clamp_v(b->vx + qx, vlim);
                b->vy = clamp_v(b->vy + qy, vlim);
            }
        }
    }

    const int32_t xmax = ((int32_t)w << 8) - r8;
    const int32_t ymax = ((int32_t)h << 8) - r8;
    for (uint32_t i = 0; i < count; i++) {
        ball_t *const b = &s_balls[i];
        if (b->x < r8) {
            b->x = r8;
            b->vx = (b->vx < 0) ? -b->vx : b->vx;
        } else if (b->x > xmax) {
            b->x = xmax;
            b->vx = (b->vx > 0) ? -b->vx : b->vx;
        }
        if (b->y < r8) {
            b->y = r8;
            b->vy = (b->vy < 0) ? -b->vy : b->vy;
        } else if (b->y > ymax) {
            b->y = ymax;
            b->vy = (b->vy > 0) ? -b->vy : b->vy;
        }
    }
}

/*
 * One band of the balls: the ground first, which is what erases where every
 * ball was, and then a span of each ball's row that falls in the band.
 */
static void balls_band(uint16_t *band, uint16_t w, uint16_t y0, uint16_t n,
                       uint32_t count, uint32_t rad)
{
    for (uint32_t k = 0; k < (uint32_t)w * n; k++) {
        band[k] = BALL_BG;
    }
    for (uint32_t i = 0; i < count; i++) {
        const int32_t cx = s_balls[i].x >> 8;
        const int32_t cy = s_balls[i].y >> 8;
        if (cy + (int32_t)rad < (int32_t)y0 ||
            cy - (int32_t)rad >= (int32_t)y0 + (int32_t)n) {
            continue;
        }
        for (uint32_t r = 0; r < n; r++) {
            const int32_t dy = (int32_t)(y0 + r) - cy;
            if (dy < -(int32_t)rad || dy > (int32_t)rad) {
                continue;
            }
            const int32_t half = s_span[(dy < 0) ? -dy : dy];
            int32_t       xa = cx - half;
            int32_t       xb = cx + half;
            if (xa < 0) {
                xa = 0;
            }
            if (xb >= (int32_t)w) {
                xb = (int32_t)w - 1;
            }
            for (int32_t x = xa; x <= xb; x++) {
                band[r * w + (uint32_t)x] = s_ball_rgb[i];
            }
        }
    }
}

int ag_main(int argc, char **argv)
{
    ag_gfxinfo_t info;

    uint32_t secs = (argc >= 2) ? number(argv[1]) : 10u;
    uint32_t rows = (argc >= 3) ? number(argv[2]) : 8u;
    char mode = (argc >= 4) ? argv[3][0] : 'n'; /* n, b, f, o or s */
    /* box and balls share their first letter; the second tells them apart */
    if (mode == 'b' && argv[3][1] == 'a') {
        mode = 'o';
    }
    uint32_t count = (argc >= 5) ? number(argv[4]) : 8u;

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

    /* a ball a sixteenth of the short side across in radius, 12 to 20 px */
    uint32_t rad = ((w < h) ? w : h) / 16u;
    rad = (rad < 12u) ? 12u : (rad > BALL_R_MAX) ? BALL_R_MAX : rad;
    uint32_t sx = 0, sy = 0;
    const uint32_t step = (argc >= 5) ? count : 4u;
    if (mode == 's') {
        map_start();
    }
    if (mode == 'o') {
        if (count == 0u || count > BALLS_MAX) {
            count = (count == 0u) ? 8u : BALLS_MAX;
        }
        count = balls_start(w, h, count, rad);
        ag_printf("gfxnoise: %u balls of radius %u\n", (unsigned)count,
                  (unsigned)rad);
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
            } else if (mode == 'o') {
                balls_band(s_band, w, y0, n, count, rad);
                src = s_band;
            } else if (mode == 's') {
                map_band(s_band, w, y0, n, sx, sy);
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
        } else if (mode == 'o') {
            balls_step(w, h, count, rad);
        } else if (mode == 's') {
            sx += step; /* right and a little down, like a map walked across */
            sy += step / 2u;
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
