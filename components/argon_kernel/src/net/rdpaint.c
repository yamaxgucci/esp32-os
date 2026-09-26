/*
 * ArgonOS - a neighbour's screen, kept up to date by what changed.  See
 * rdpaint.h for what and why.
 *
 * THE PAYLOAD: the surface's width and height (2 + 2 bytes), then runs, each
 * a row, a column, a count of pixels (up to RDP_CHUNK) and the length of its
 * packing (2 bytes each), and the packing (net/rdcodec.h).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "net/rdpaint.h"

#include <stdbool.h>
#include <string.h>

#include <argon/log.h>

#include <argon/port/mem.h>
#include <argon/port/sync.h>
#include <argon/port/task.h>
#include <argon/port/time.h>

#include "net/rdcodec.h"

#define TAG "rdpaint"

/* the most pixels one run carries: its packing then always fits a payload */
#define RDP_CHUNK 256u
#define RDP_RUN_HDR 8u
#define RDP_HDR 4u
/* how long a pass waits for the wire before it gives the screen up as
 * unknown and sends the whole of it next time */
#define RDP_SEND_PATIENCE_MS 2000u
/*
 * AT MOST A PASS PER FRAME.  Woken by every band an application presents, the
 * painter made a pass per band - 389 a second of eight-row slivers for a
 * program showing 65 frames - and at an application's priority that kept a
 * core busy enough to starve the link's reader: a file copied to the Zero
 * while it drew on the CYD took two copies of five in four minutes, where
 * alone it took a second (26 Sep 2026).  A pass now starts no sooner than
 * RDP_MIN_PASS_US after the last: what changes meanwhile is gathered into
 * the next one, which is bigger, and there are fewer of them.
 */
#define RDP_MIN_PASS_US 16000

typedef struct {
    uint16_t x0, x1; /* x1 <= x0: nothing */
} span_t;

/*
 * ONE PICTURE, NOT TWO.  The painter keeps a single copy of the screen, and it
 * is what the far screen will show once every row marked in `dirty` has gone:
 * a present is compared with it there and then, and what differs is copied in
 * and marked.  The painter sends marked rows out of it.  A present that lands
 * in a row while it is being sent marks the row again, so the two screens
 * still end up the same.  A second picture - "what the far end has" beside
 * "the latest" - would have made the comparing simpler and cost another
 * 150 KB at 320x240, which most boards do not have to give.
 */
static struct {
    ag_port_mutex_t lock;
    ag_port_sem_t   wake;
    ag_port_task_t  task;
    bool            failed; /* could not start: the proxy falls back to raw */

    uint32_t    handle;
    uint16_t    room;
    rdp_send_fn send;

    uint16_t  w, h;      /* the surface now; the buffers hold cap pixels  */
    uint32_t  cap_px;
    uint16_t  cap_rows;
    uint16_t *pic;       /* the one picture */
    uint8_t  *known;     /* per row: is the far screen known to match pic */
    span_t   *dirty;     /* rows of pic not yet sent, under the lock      */
    span_t   *work;
    uint8_t  *msg;
    uint32_t  len;

    uint32_t passes, px_in, bytes_out;
    int64_t  said_us;
    uint32_t said_passes, said_px, said_bytes;
    /* the far end's pace: time waited in sync, this second - the total and
     * the longest; and the longest time from one pass's start to the next */
    int64_t  sync_us, sync_max_us, gap_max_us, last_pass_us;
} s_p;

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static rdp_sync_fn s_sync;

void rdp_set_sync(rdp_sync_fn sync) { s_sync = sync; }

static void lock(void) { (void)ag_port_mutex_take(s_p.lock, AG_PORT_FOREVER); }

static void unlock(void) { (void)ag_port_mutex_give(s_p.lock); }

/* The payload out, however long the wire takes; false if it would not go. */
static bool flush(uint32_t handle, rdp_send_fn send)
{
    if (s_p.len <= RDP_HDR) {
        return true;
    }
    const int64_t until = ag_port_us() + (int64_t)RDP_SEND_PATIENCE_MS * 1000;
    ag_err_t      r;
    for (;;) {
        r = send(handle, s_p.msg, (uint16_t)s_p.len);
        if (r != -AG_EAGAIN || ag_port_us() >= until) {
            break;
        }
        ag_port_task_delay(1);
    }
    if (r == AG_OK) {
        s_p.bytes_out += s_p.len;
    }
    s_p.len = RDP_HDR;
    return r == AG_OK;
}

/* A run of the row into the payload, sending the payload first if full. */
static bool put_run(uint32_t y, uint32_t x, const uint16_t *px, uint32_t n,
                    uint16_t room, uint32_t handle, rdp_send_fn send)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        if (s_p.len + RDP_RUN_HDR < room) {
            uint8_t *const at = s_p.msg + s_p.len;
            const uint32_t packed =
                rdc_pack(px, n, at + RDP_RUN_HDR, room - s_p.len - RDP_RUN_HDR);
            if (packed > 0u) {
                put16(at, (uint16_t)y);
                put16(at + 2, (uint16_t)x);
                put16(at + 4, (uint16_t)n);
                put16(at + 6, (uint16_t)packed);
                s_p.len += RDP_RUN_HDR + packed;
                return true;
            }
        }
        if (!flush(handle, send)) {
            return false;
        }
    }
    return false; /* cannot happen: RDP_CHUNK always fits an empty payload */
}

static void report(void)
{
    const int64_t now = ag_port_us();
    if (now - s_p.said_us < 1000000 || s_p.passes == s_p.said_passes) {
        return;
    }
    const uint32_t us = (uint32_t)(now - s_p.said_us);
    const uint32_t p10 = (uint32_t)((uint64_t)(s_p.passes - s_p.said_passes) *
                                    10000000u / us);
    const uint32_t kin = (uint32_t)((uint64_t)(s_p.px_in - s_p.said_px) * 2u *
                                    1000000u / 1024u / us);
    const uint32_t kout = (uint32_t)((uint64_t)(s_p.bytes_out - s_p.said_bytes) *
                                     1000000u / 1024u / us);
    const uint32_t passes = s_p.passes - s_p.said_passes;
    ag_log(AG_LOG_INFO, TAG,
           "%u.%u passes/s, %u KB/s of changed pixels sent as %u KB/s; far end "
           "waited for %u ms a pass (at most %u), longest between passes %u ms",
           (unsigned)(p10 / 10u), (unsigned)(p10 % 10u), (unsigned)kin,
           (unsigned)kout,
           (unsigned)(passes ? s_p.sync_us / 1000 / passes : 0),
           (unsigned)(s_p.sync_max_us / 1000), (unsigned)(s_p.gap_max_us / 1000));
    s_p.sync_us = 0;
    s_p.sync_max_us = 0;
    s_p.gap_max_us = 0;
    s_p.said_us = now;
    s_p.said_passes = s_p.passes;
    s_p.said_px = s_p.px_in;
    s_p.said_bytes = s_p.bytes_out;
}

static void painter(void *arg)
{
    (void)arg;
    int64_t began = 0;
    for (;;) {
        (void)ag_port_sem_take(s_p.wake, ag_port_ms_to_ticks(1000));
        const int64_t since = ag_port_us() - began;
        if (began != 0 && since < RDP_MIN_PASS_US) {
            ag_port_task_delay(ag_port_ms_to_ticks(
                (uint32_t)((RDP_MIN_PASS_US - since) / 1000) + 1u));
        }
        began = ag_port_us();
        lock();
        const uint16_t w = s_p.w;
        const uint16_t h = s_p.h;
        memcpy(s_p.work, s_p.dirty, (size_t)h * sizeof(span_t));
        memset(s_p.dirty, 0, (size_t)h * sizeof(span_t));
        const uint32_t    handle = s_p.handle;
        const uint16_t    room = s_p.room;
        const rdp_send_fn send = s_p.send;
        unlock();

        bool     any = false;
        bool     ok = true;
        uint32_t changed = 0;
        put16(s_p.msg, w);
        put16(s_p.msg + 2, h);
        s_p.len = RDP_HDR;
        for (uint32_t y = 0; y < h && ok; y++) {
            const uint32_t x0 = s_p.work[y].x0;
            const uint32_t x1 = s_p.work[y].x1;
            if (x1 <= x0) {
                continue;
            }
            any = true;
            const uint16_t *row = s_p.pic + (size_t)y * w;
            for (uint32_t x = x0; x < x1 && ok; x += RDP_CHUNK) {
                const uint32_t n = (x1 - x < RDP_CHUNK) ? x1 - x : RDP_CHUNK;
                ok = put_run(y, x, row + x, n, room, handle, send);
                changed += n;
            }
        }
        if (ok) {
            ok = flush(handle, send);
        }
        if (!ok) {
            /* the far screen is not known to show what the picture says:
             * all of it again, next time */
            lock();
            for (uint32_t y = 0; y < s_p.h; y++) {
                s_p.dirty[y].x0 = 0;
                s_p.dirty[y].x1 = s_p.w;
            }
            unlock();
        } else if (any) {
            s_p.passes++;
            s_p.px_in += changed;
            const rdp_sync_fn sync = s_sync;
            if (sync != NULL) {
                const int64_t t0 = ag_port_us();
                sync(handle);
                const int64_t w = ag_port_us() - t0;
                s_p.sync_us += w;
                if (w > s_p.sync_max_us) {
                    s_p.sync_max_us = w;
                }
            }
            const int64_t now = ag_port_us();
            if (s_p.last_pass_us != 0 && now - s_p.last_pass_us > s_p.gap_max_us) {
                s_p.gap_max_us = now - s_p.last_pass_us;
            }
            s_p.last_pass_us = now;
        }
        report();
    }
}

/* The buffers, once, for the first surface's size; false if there is no
 * memory for them - then the proxy sends raw, as it did. */
static bool buffers(uint16_t w, uint16_t h, uint16_t room)
{
    s_p.cap_px = (uint32_t)w * h;
    s_p.cap_rows = h;
    s_p.pic = ag_port_alloc((size_t)s_p.cap_px * 2u, AG_MEM_BYTE);
    s_p.known = ag_port_alloc(h, AG_MEM_BYTE);
    s_p.dirty = ag_port_alloc((size_t)h * sizeof(span_t), AG_MEM_BYTE);
    s_p.work = ag_port_alloc((size_t)h * sizeof(span_t), AG_MEM_BYTE);
    s_p.msg = ag_port_alloc(room, AG_MEM_BYTE);
    if (s_p.pic == NULL || s_p.known == NULL || s_p.dirty == NULL ||
        s_p.work == NULL || s_p.msg == NULL) {
        ag_port_free(s_p.pic);
        ag_port_free(s_p.known);
        ag_port_free(s_p.dirty);
        ag_port_free(s_p.work);
        ag_port_free(s_p.msg);
        s_p.pic = NULL;
        return false;
    }
    memset(s_p.known, 0, h);
    memset(s_p.dirty, 0, (size_t)h * sizeof(span_t));
    s_p.w = w;
    s_p.h = h;
    s_p.room = room;
    return true;
}

bool rdp_damage(const ag_blit_t *b, uint32_t handle, uint16_t room,
                rdp_send_fn send)
{
    if (b == NULL || b->px == NULL || b->w == 0 || b->h == 0 || send == NULL ||
        room < RDP_HDR + RDP_RUN_HDR + RDC_PACK_MAX(RDP_CHUNK)) {
        return false;
    }
    if (s_p.task == NULL) {
        if (s_p.failed) {
            return false;
        }
        if (s_p.lock == NULL) {
            s_p.lock = ag_port_mutex_new();
        }
        if (s_p.wake == NULL) {
            s_p.wake = ag_port_sem_new_binary();
        }
        /* At an application's priority, on either core: it is the frame
         * rate, and below the application it would only paint when the
         * application waited. */
        if (s_p.lock == NULL || s_p.wake == NULL ||
            !buffers(b->surf_w, b->surf_h, room) ||
            !ag_port_task_create(painter, "rdpaint", 3072, NULL, 5,
                                 AG_PORT_ANY_CORE, 0, &s_p.task)) {
            s_p.failed = true;
            ag_log(AG_LOG_WARN, TAG,
                   "no memory for the %ux%u picture: the far screen gets "
                   "every pixel, raw",
                   (unsigned)b->surf_w, (unsigned)b->surf_h);
            return false;
        }
        s_p.said_us = ag_port_us();
    }
    lock();
    if (b->surf_w != s_p.w || b->surf_h != s_p.h) {
        if ((uint32_t)b->surf_w * b->surf_h > s_p.cap_px ||
            b->surf_h > s_p.cap_rows || room > s_p.room) {
            unlock();
            return false; /* bigger than the picture was made for */
        }
        /* another shape: nothing of the far screen is known any more */
        s_p.w = b->surf_w;
        s_p.h = b->surf_h;
        memset(s_p.known, 0, s_p.h);
        memset(s_p.dirty, 0, (size_t)s_p.h * sizeof(span_t));
    }
    s_p.handle = handle;
    s_p.send = send;
    const uint32_t y1 = ((uint32_t)b->y + b->h < s_p.h) ? b->y + b->h : s_p.h;
    const uint32_t xe = ((uint32_t)b->x + b->w < s_p.w) ? b->x + b->w : s_p.w;
    bool           marked = false;
    for (uint32_t y = b->y; y < y1 && b->x < xe; y++) {
        const uint16_t *in = (const uint16_t *)(const void *)(
            (const uint8_t *)b->px + (size_t)(y - b->y) * b->stride);
        uint16_t *row = s_p.pic + (size_t)y * s_p.w;
        uint32_t  x0 = b->x;
        uint32_t  x1 = xe;
        if (s_p.known[y]) {
            /* what differs from what the far screen will show, and only
             * that: a pointer that moved is a few pixels of a few rows */
            while (x0 < x1 && in[x0 - b->x] == row[x0]) {
                x0++;
            }
            while (x1 > x0 && in[x1 - 1u - b->x] == row[x1 - 1u]) {
                x1--;
            }
            if (x1 <= x0) {
                continue;
            }
        } else if (b->x == 0 && xe == s_p.w) {
            s_p.known[y] = 1; /* a whole row: from now on it is known */
        }
        memcpy(row + x0, in + (x0 - b->x), (size_t)(x1 - x0) * 2u);
        span_t *sp = &s_p.dirty[y];
        if (sp->x1 <= sp->x0) {
            sp->x0 = (uint16_t)x0;
            sp->x1 = (uint16_t)x1;
        } else {
            sp->x0 = (x0 < sp->x0) ? (uint16_t)x0 : sp->x0;
            sp->x1 = (x1 > sp->x1) ? (uint16_t)x1 : sp->x1;
        }
        marked = true;
    }
    unlock();
    if (marked) {
        (void)ag_port_sem_give(s_p.wake);
    }
    return true;
}

/* ---------------------------------------------------------------------- */
/* The receiving board                                                     */
/* ---------------------------------------------------------------------- */

/*
 * RECTANGLES, NOT ROWS.  Each run was drawn as a blit of its own one row
 * high, and a panel pays for a blit before it draws a pixel: the ILI9341
 * sets a window with three commands and their arguments, each a transfer of
 * its own.  A 48x48 square moving over a still picture was 96 blits a frame;
 * the CYD spent 250-380 ms of every second drawing 30-60 thousand pixels -
 * ten microseconds a pixel on a bus that moves one in a fifth of one - and
 * showed about seven frames a second of a program making 65 (26 Sep 2026).
 *
 * So runs that continue each other - the same columns, the next row - are
 * gathered into one rectangle and drawn as one blit.  Anything else, and a
 * full buffer, draws what is gathered first.  The buffer is taken the first
 * time a board draws a neighbour's picture, not by every board.
 */
#define RDP_RECT_PX 4096u
static uint16_t *s_rect;
static uint16_t  s_rx, s_ry, s_rw, s_rh, s_sw, s_sh;

static void rect_flush(const ag_display_ops_t *d)
{
    if (s_rh == 0u) {
        return;
    }
    const ag_blit_t bl = {
        .px = s_rect,
        .stride = (uint32_t)s_rw * 2u,
        .surf_w = s_sw,
        .surf_h = s_sh,
        .x = s_rx,
        .y = s_ry,
        .w = s_rw,
        .h = s_rh,
    };
    d->blit_rect(0, &bl);
    s_rh = 0;
}

uint32_t rdp_draw(const ag_display_ops_t *d, const uint8_t *pay, uint16_t len)
{
    static uint16_t line[RDP_CHUNK];
    if (d == NULL || d->blit_rect == NULL || len < RDP_HDR) {
        return 0u;
    }
    if (s_rect == NULL) {
        s_rect = ag_port_alloc(RDP_RECT_PX * 2u, AG_MEM_BYTE);
    }
    const uint16_t w = get16(pay);
    const uint16_t h = get16(pay + 2);
    uint32_t       at = RDP_HDR;
    uint32_t       drawn = 0;
    s_rh = 0;
    s_sw = w;
    s_sh = h;
    while (at + RDP_RUN_HDR <= len) {
        const uint16_t y = get16(pay + at);
        const uint16_t x = get16(pay + at + 2);
        const uint16_t n = get16(pay + at + 4);
        const uint16_t plen = get16(pay + at + 6);
        at += RDP_RUN_HDR;
        if (n == 0u || n > RDP_CHUNK || at + plen > len || y >= h ||
            (uint32_t)x + n > w) {
            break; /* not a run: the rest cannot be trusted either */
        }
        const bool joins = s_rect != NULL && s_rh != 0u && x == s_rx &&
                           n == s_rw && y == (uint16_t)(s_ry + s_rh) &&
                           (uint32_t)(s_rh + 1u) * n <= RDP_RECT_PX;
        if (!joins) {
            rect_flush(d);
        }
        uint16_t *const into = (s_rect != NULL && (uint32_t)n <= RDP_RECT_PX)
                                   ? s_rect + (size_t)s_rh * n
                                   : line;
        if (rdc_unpack(pay + at, plen, into, n) != plen) {
            break;
        }
        at += plen;
        drawn += n;
        if (into == line) {
            /* no buffer: one row at a time, as before */
            const ag_blit_t bl = {
                .px = line, .stride = (uint32_t)n * 2u, .surf_w = w,
                .surf_h = h, .x = x, .y = y, .w = n, .h = 1,
            };
            d->blit_rect(0, &bl);
            continue;
        }
        if (s_rh == 0u) {
            s_rx = x;
            s_ry = y;
            s_rw = n;
        }
        s_rh++;
    }
    rect_flush(d);
    return drawn;
}
