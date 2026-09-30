/*
 * ArgonOS host test - a neighbour's screen, end to end, without a board.
 *
 * What GFXNOISE's `box` mode shows - one still picture of noise and a 48x48
 * white square moving four pixels a frame - presented in bands of eight rows
 * to the painter of net/rdpaint.c; every payload it sends is drawn by the
 * receiving half (rdp_draw) into a framebuffer that stands for the far glass;
 * and after each pass the glass must equal the frame, pixel for pixel.
 *
 * Why it exists: on 26 Sep 2026 the CYD showed a square that ran across the
 * screen instead of along it and left a trail, while every counter on the
 * wire and on the receiver read clean.  A picture sent as changes can be
 * wrong without anything being lost, so this checks the picture itself.
 *
 * The painter's own source is included, so its private pass is reachable and
 * its port is the stubs in rdpaint_stub/.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../components/argon_kernel/src/net/rdpaint.c"

int64_t rdpaint_stub_now_us;

void ag_log(ag_log_level_t level, const char *tag, const char *fmt, ...)
{
    (void)level;
    (void)tag;
    (void)fmt;
}

static int s_checks, s_fails;

#define CHECK(cond)                                                            \
    do {                                                                       \
        s_checks++;                                                            \
        if (!(cond)) {                                                         \
            s_fails++;                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                      \
    } while (0)

#define W    320u
#define H    240u
#define BOX  48u
#define ROWS 8u
#define ROOM 640u /* rd_pour_payload to the CYD: RD_SCR_CAP */

static uint16_t s_frame[W * H]; /* what the program presented             */
static uint16_t s_glass[W * H]; /* what the far screen shows              */
static uint32_t s_rects, s_px_drawn;

/* The far panel: a rectangle into the glass, exactly as a driver takes it. */
static void glass_blit(ag_handle_t h, const ag_blit_t *b)
{
    (void)h;
    s_rects++;
    CHECK(b->surf_w == W && b->surf_h == H);
    CHECK((uint32_t)b->x + b->w <= W && (uint32_t)b->y + b->h <= H);
    for (uint32_t r = 0; r < b->h; r++) {
        const uint16_t *in = (const uint16_t *)(const void *)(
            (const uint8_t *)b->px + (size_t)r * b->stride);
        memcpy(&s_glass[(size_t)(b->y + r) * W + b->x], in,
               (size_t)b->w * 2u);
        s_px_drawn += b->w;
    }
}

static ag_display_ops_t s_ops;

/* The wire: every payload straight into the far end's drawing. */
static uint32_t s_sends; /* payloads the painter sent */

static ag_err_t send_to_glass(uint32_t handle, const uint8_t *pay, uint16_t len)
{
    s_sends++;
    (void)handle;
    CHECK(len <= ROOM);
    (void)rdp_draw(&s_ops, pay, len);
    return AG_OK;
}

static uint32_t s_rng = 2463534242u;

static uint16_t noise(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (uint16_t)s_rng;
}

static bool s_all_new; /* every pixel new every frame, as `noise` */

/* Anything still marked for the far screen. */
static bool dirty_left(void)
{
    if (s_p.cont) {
        return true; /* the rest of a frame, out of the same snapshot */
    }
    for (uint32_t y = 0; y < s_p.h; y++) {
        if (row_any(row_of(s_p.dirty, y))) {
            return true;
        }
    }
    return false;
}

/* One frame of `box`: the still noise, the square where it is now. */
static void make_frame(const uint16_t *still, uint32_t box_x)
{
    memcpy(s_frame, still, sizeof(s_frame));
    if (s_all_new) {
        for (uint32_t i = 0; i < W * H; i++) {
            s_frame[i] = noise();
        }
    }
    const uint32_t by = (H - BOX) / 2u;
    for (uint32_t y = by; y < by + BOX; y++) {
        for (uint32_t x = box_x; x < box_x + BOX && x < W; x++) {
            s_frame[y * W + x] = 0xFFFFu;
        }
    }
}

static void present_frame(void)
{
    for (uint32_t y0 = 0; y0 < H; y0 += ROWS) {
        const ag_blit_t b = {
            .px = &s_frame[y0 * W],
            .stride = W * 2u,
            .surf_w = W,
            .surf_h = H,
            .x = 0,
            .y = (uint16_t)y0,
            .w = W,
            .h = ROWS,
        };
        CHECK(rdp_damage(&b, 1u, ROOM, send_to_glass));
    }
}

/* Where the glass differs from the frame: the first pixel, and how many. */
static uint32_t diff(uint32_t *first)
{
    rdp_draw_flush(RDP_FL_FRAME); /* as a frame's marker does */
    uint32_t n = 0;
    for (uint32_t i = 0; i < W * H; i++) {
        if (s_glass[i] != s_frame[i]) {
            if (n == 0u) {
                *first = i;
            }
            n++;
        }
    }
    return n;
}

static void run(const char *name, uint32_t frames_a_pass)
{
    static uint16_t still[W * H];
    for (uint32_t i = 0; i < W * H; i++) {
        still[i] = noise();
    }
    memset(s_glass, 0x5a, sizeof(s_glass)); /* not what anyone drew */
    s_rects = s_px_drawn = 0;
    uint32_t box_x = 0;
    int32_t  dx = 4;
    uint32_t bad_passes = 0, first_bad = 0, first_px = 0, worst = 0;
    const uint32_t frames = 400;
    for (uint32_t f = 0; f < frames; f++) {
        make_frame(still, box_x);
        present_frame();
        if ((int32_t)box_x + dx < 0 || box_x + (uint32_t)dx + BOX > W) {
            dx = -dx;
        }
        box_x = (uint32_t)((int32_t)box_x + dx);
        if ((f + 1u) % frames_a_pass != 0u) {
            continue;
        }
        /* passes until nothing is left: a big change is cut into several
         * (RDP_PASS_MAX_PX), and the picture is judged when all have gone */
        uint32_t guard = 0;
        do {
            rdpaint_stub_now_us += 16000;
            pass();
        } while (dirty_left() && ++guard < 64u);
        CHECK(guard < 64u);
        uint32_t at = 0;
        const uint32_t n = diff(&at);
        if (n != 0u) {
            if (bad_passes == 0u) {
                first_bad = f;
                first_px = at;
            }
            bad_passes++;
            if (n > worst) {
                worst = n;
            }
        }
    }
    printf("  rdpaint %s: %u frames, %u rectangles, %u px drawn; %u passes "
           "wrong",
           name, (unsigned)frames, (unsigned)s_rects, (unsigned)s_px_drawn,
           (unsigned)bad_passes);
    if (bad_passes != 0u) {
        printf(" - first after frame %u at x=%u y=%u, worst %u px",
               (unsigned)first_bad, (unsigned)(first_px % W),
               (unsigned)(first_px / W), (unsigned)worst);
    }
    printf("\n");
    CHECK(bad_passes == 0u);
}

/*
 * GFXNOISE's `balls`: eight solid balls over a flat background, each moving
 * on its own.  The glass must match after every frame, and the count that
 * matters on the CYD is rectangles a frame - a window costs its panel about
 * 420 microseconds before the first pixel (27 Sep 2026).
 */
static void balls(void)
{
    enum { N = 8, R = 15 };
    int32_t bx[N], by[N], vx[N], vy[N];
    for (int i = 0; i < N; i++) {
        bx[i] = 30 + i * 35;
        by[i] = 30 + (i * 53) % 180;
        vx[i] = (i & 1) ? 3 : -2;
        vy[i] = (i & 2) ? 2 : -3;
    }
    const uint32_t r0 = s_rects, p0 = s_px_drawn;
    uint32_t       pa0 = s_p.passes, in0 = s_p.cuts_inside;
    uint32_t       bad = 0;
    const uint32_t frames = 400;
    for (uint32_t f = 0; f < frames; f++) {
        for (uint32_t i = 0; i < W * H; i++) {
            s_frame[i] = 0x0841u;
        }
        for (int i = 0; i < N; i++) {
            for (int32_t y = by[i] - R; y <= by[i] + R; y++) {
                for (int32_t x = bx[i] - R; x <= bx[i] + R; x++) {
                    const int32_t dx = x - bx[i], dy = y - by[i];
                    if (dx * dx + dy * dy <= R * R && x >= 0 && y >= 0 &&
                        x < (int32_t)W && y < (int32_t)H) {
                        s_frame[(uint32_t)y * W + (uint32_t)x] =
                            (uint16_t)(0x1F00u + (uint32_t)i * 0x2345u);
                    }
                }
            }
            bx[i] += vx[i];
            by[i] += vy[i];
            if (bx[i] < R || bx[i] > (int32_t)W - 1 - R) {
                vx[i] = -vx[i];
            }
            if (by[i] < R || by[i] > (int32_t)H - 1 - R) {
                vy[i] = -vy[i];
            }
        }
        present_frame();
        uint32_t guard = 0;
        do {
            rdpaint_stub_now_us += 16000;
            pass();
        } while (dirty_left() && ++guard < 64u);
        if (f == 0u) {
            /* the first is the whole screen changing from the test before:
             * more than a pass, and rightly */
            pa0 = s_p.passes;
            in0 = s_p.cuts_inside;
        }
        uint32_t at = 0;
        if (diff(&at) != 0u) {
            bad++;
        }
    }
    printf("  rdpaint balls: %u frames, %u rectangles a frame, %u px a frame; "
           "%u frames wrong\n",
           (unsigned)frames, (unsigned)((s_rects - r0) / frames),
           (unsigned)((s_px_drawn - p0) / frames), (unsigned)bad);
    CHECK(bad == 0u);
    printf("  rdpaint balls: %u passes for the %u frames after the first, "
           "%u cut inside a rectangle\n",
           (unsigned)(s_p.passes - pa0), (unsigned)(frames - 1u),
           (unsigned)(s_p.cuts_inside - in0));
    /* a frame of balls is one pass, and no ball is ever cut in two */
    CHECK(s_p.passes - pa0 == frames - 1u);
    CHECK(s_p.cuts_inside == in0);
}

/*
 * A frame bigger than a pass, and the program presenting the next one while
 * the rest of it is still to go: the glass must show the first frame whole -
 * none of the second - when the rest has gone (27 Sep 2026, "the bottom lags
 * the top").
 */
static void one_instant(void)
{
    static uint16_t first[W * H];
    for (uint32_t i = 0; i < W * H; i++) {
        s_frame[i] = noise();
    }
    present_frame();
    memcpy(first, s_frame, sizeof(first));
    rdpaint_stub_now_us += 16000;
    pass();
    CHECK(s_p.cont); /* cut short: the whole screen is more than a pass */
    for (uint32_t i = 0; i < W * H; i++) {
        s_frame[i] = noise();
    }
    present_frame();
    uint32_t guard = 0;
    while (s_p.cont && ++guard < 64u) {
        rdpaint_stub_now_us += 16000;
        pass();
    }
    CHECK(guard < 64u);
    rdp_draw_flush(RDP_FL_FRAME);
    uint32_t n = 0;
    for (uint32_t i = 0; i < W * H; i++) {
        n += (s_glass[i] != first[i]) ? 1u : 0u;
    }
    printf("  rdpaint one instant: %u px of the glass not the first frame\n",
           (unsigned)n);
    CHECK(n == 0u);
    /* and the second frame follows whole */
    guard = 0;
    do {
        rdpaint_stub_now_us += 16000;
        pass();
    } while (dirty_left() && ++guard < 64u);
    uint32_t at = 0;
    CHECK(diff(&at) == 0u);
}

/*
 * A ball that went a long way between two passes - thirty frames while a big
 * one was going out: the next pass sends where it was and where it is, not
 * the path (29 Sep 2026: the path made the next frame longer still, and the
 * picture stood still 61% of the time).
 */
static void long_way(void)
{
    enum { R = 15, STEP = 4, FRAMES = 30 };
    const int32_t y0 = 120;
    int32_t       x = 40;
    for (uint32_t f = 0; f <= FRAMES; f++) {
        for (uint32_t i = 0; i < W * H; i++) {
            s_frame[i] = 0x0841u;
        }
        for (int32_t y = y0 - R; y <= y0 + R; y++) {
            for (int32_t xx = x - R; xx <= x + R; xx++) {
                const int32_t dx = xx - x, dy = y - y0;
                if (dx * dx + dy * dy <= R * R) {
                    s_frame[(uint32_t)y * W + (uint32_t)xx] = 0xF800u;
                }
            }
        }
        present_frame();
        if (f == 0u) {
            /* the ball's first place, on the glass */
            uint32_t guard = 0;
            do {
                rdpaint_stub_now_us += 16000;
                pass();
            } while (dirty_left() && ++guard < 64u);
        }
        x += STEP;
    }
    const uint32_t p1 = s_px_drawn;
    uint32_t guard = 0;
    do {
        rdpaint_stub_now_us += 16000;
        pass();
    } while (dirty_left() && ++guard < 64u);
    const uint32_t sent = s_px_drawn - p1;
    uint32_t       at = 0;
    const uint32_t n = diff(&at);
    printf("  rdpaint a long way: %u px sent for a ball that went %u px "
           "(its box is %u); %u px of the glass wrong\n",
           (unsigned)sent, (unsigned)(STEP * FRAMES),
           (unsigned)((2 * R + 1) * (2 * R + 1)), (unsigned)n);
    CHECK(n == 0u);
    /* two boxes, not the path: the path would be ~150 x 31 */
    CHECK(sent <= 2u * (2 * R + 1) * (2 * R + 1) + 200u);
}

/*
 * THE WHOLE SCREEN CHANGES - one flat picture for another, a window over the
 * desktop, a picture of noise: every kind of change, not only balls.  Each
 * must end on the glass exactly, from one instant (not half of the next),
 * and a flat change must be small.
 */
static void switches(void)
{
    static const uint16_t flat[3] = {0x001Fu, 0xF800u, 0x07E0u};
    uint32_t              bad = 0;
    uint32_t              flat_msgs = 0;
    for (uint32_t k = 0; k < 6; k++) {
        for (uint32_t i = 0; i < W * H; i++) {
            s_frame[i] = (k == 3u) ? noise() : flat[k % 3u];
        }
        if (k == 4u) {
            /* a window on the flat screen */
            for (uint32_t y = 40; y < 200; y++) {
                for (uint32_t x = 60; x < 260; x++) {
                    s_frame[y * W + x] = (y < 56u) ? 0x0010u : 0xFFFFu;
                }
            }
        }
        present_frame();
        const uint32_t m0 = s_sends;
        const uint32_t r0 = s_rects;
        uint32_t       guard = 0;
        do {
            rdpaint_stub_now_us += 16000;
            pass();
        } while (dirty_left() && ++guard < 400u);
        uint32_t at = 0;
        const uint32_t n = diff(&at);
        if (n != 0u) {
            bad++;
        }
        if (k < 3u) {
            flat_msgs += s_sends - m0;
        }
        if (k == 3u) {
            /* a screen of noise comes in pieces cut anywhere along a row;
             * gathered in strips it is a few dozen windows, not hundreds */
            printf("  rdpaint noise: %u windows\n", (unsigned)(s_rects - r0));
            CHECK(s_rects - r0 <= 60u);
        }
        printf("  rdpaint switch %u: %u messages, %u px wrong\n",
               (unsigned)k, (unsigned)(s_sends - m0), (unsigned)n);
    }
    CHECK(bad == 0u);
    /* a flat screen is 240 rows of one colour: a few messages, one pass,
     * where the rows alone were some twenty */
    CHECK(flat_msgs <= 15u);
}

int main(void)
{
    s_ops.size = sizeof(s_ops);
    s_ops.blit_rect = glass_blit;
    run("a pass a frame, rows only", 1u);
    rdp_far_rects(true); /* the far screen draws a rectangle as one run */
    run("a pass a frame", 1u);
    run("a pass every 3 frames", 3u);
    s_all_new = true;
    run("every pixel new every frame", 1u);
    /* whatever the picture, two passes and their markers fit the CYD's 27
     * places (RDP_PASS_MSGS_HARD, rdpaint.c) */
    printf("  rdpaint the biggest pass: %u messages\n",
           (unsigned)s_p.pass_msgs_max);
    CHECK(s_p.pass_msgs_max <= 12u);
    one_instant();
    s_all_new = false;
    balls();
    long_way();
    switches();
    printf("rdpaint_tests: %d checks, %d failed\n", s_checks, s_fails);
    return s_fails == 0 ? 0 : 1;
}
