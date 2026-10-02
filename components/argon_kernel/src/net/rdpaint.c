/*
 * ArgonOS - a neighbour's screen, kept up to date by what changed.  See
 * rdpaint.h for what and why.
 *
 * THE PAYLOAD: the surface's width and height (2 + 2 bytes), then runs, each
 * a row, a column, a count of pixels (up to RDP_CHUNK) and the length of its
 * packing (2 bytes each), and the packing (net/rdcodec.h).
 *
 * OR A RECTANGLE (29 Sep 2026): the count with RDP_RECT_BIT set is its width,
 * and a fifth word its height; the packing is its rows', one after another,
 * which unpacks as one run of width x height pixels.  A frame of eight balls
 * was some 500 runs of a row each - 8 bytes of header to 9 of packing - and
 * thirteen messages, more than the far screen's queue takes in a pass, so it
 * went in two, and a picture of any kind with many rows changed the same.
 * Sent only to a screen that says it draws them (rdp_far_rects).
 *
 * OR INDICES (30 Sep 2026): with RDP_IDX_BIT in the count as well, the
 * packing is of a byte a pixel (rdc_pack8), each an index into a palette
 * the far screen keeps; a run with the row 0xFFFF sets entries of it - the
 * column is the first index, the count how many, and the packing their
 * colours, two bytes each.  A colour is always set before a run uses it,
 * in the same stream.  Fallout's map scrolling is every pixel of the
 * screen changing in at most 256 colours, and in RGB565 it packed to 93%
 * of itself: the byte halves what crosses.  Sent only to a screen that
 * says it takes them (rdp_far_idx).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "net/rdpaint.h"
#include "net/wtrace.h"

#include <stdbool.h>
#include <stddef.h>
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
#define RDP_RECT_HDR 10u
#define RDP_RECT_BIT 0x8000u
#define RDP_IDX_BIT  0x4000u
#define RDP_PAL_ROW  0xFFFFu
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
/*
 * AT MOST WHAT THE FAR QUEUE HOLDS IN A PASS.  The first pass is the whole
 * screen, and on 26 Sep 2026 the far screen's reader waited two hundred
 * milliseconds on a full picture queue and read nothing else off the wire -
 * the sound behind it included, which arrived 60 ms late.  So a big change
 * goes out over several passes, and the next starts where this one stopped:
 * a picture that changes everywhere every frame is still sent round, not
 * only at the top.
 *
 * The cut was at 4 096 pixels until 29 Sep 2026, and it fell inside
 * rectangles.  The far screen draws a pass as soon as it has arrived, on the
 * glass people are looking at (the CYD has no memory for a second picture),
 * so the top of a ball came in one pass and its bottom in the next: 84
 * passes a second of torn balls ("the balls are torn", Maxim).  And pixels
 * are the wrong measure: eight balls on a few rows are 28 000 pixels of rows
 * that pack into two messages, and cut there for nothing.  What fills the far
 * queue is messages.
 *
 * So a pass is cut only where a rectangle begins, once it has RDP_PASS_MSGS
 * messages; inside one - a rectangle the size of the screen, the first
 * frame's - only at RDP_PASS_MSGS_HARD.  It is looked at where a row
 * begins, and a row of noise the width of the screen is two messages, so a
 * pass is at most two over it: twelve and twelve, with their markers, are
 * 26 of the 27 places the CYD's 28-place ring holds, so the pass being drawn
 * and the next fit without its reader waiting for room.  (Nine and twelve
 * made passes of fourteen, and the CYD's reader waited 46 times in three
 * minutes of noise.)  Nothing here is about any one picture: a frame goes
 * whole whenever the far screen can hold it.
 */
#define RDP_PASS_MSGS      8u
#define RDP_PASS_MSGS_HARD 10u
/* how long presents may pause before what they drew counts as a frame */
#define RDP_FRAME_QUIET_US 30000

typedef struct {
    uint16_t x0, x1; /* x1 <= x0: nothing */
} span_t;

/*
 * A ROW IN PIECES (27 Sep 2026).  A row was one span, its first changed pixel
 * to its last, and eight balls on the screen made most rows the whole width:
 * the background between two balls went over the wire and was drawn again on
 * a glass that draws about 157 000 pixels a second - 14 passes a second where
 * the square had 55.  A row keeps up to RDP_SPANS pieces; pieces closer than
 * RDP_SPAN_GAP pixels are one (a run's header costs about that much), and a
 * piece with no place left joins its nearest.
 */
#define RDP_SPANS    4u
#define RDP_SPAN_GAP 24u

static span_t *row_of(span_t *set, uint32_t y)
{
    return set + (size_t)y * RDP_SPANS;
}

static bool row_any(const span_t *r)
{
    for (uint32_t i = 0; i < RDP_SPANS; i++) {
        if (r[i].x1 > r[i].x0) {
            return true;
        }
    }
    return false;
}

static void row_clear(span_t *r)
{
    memset(r, 0, RDP_SPANS * sizeof(span_t));
}

static void row_whole(span_t *r, uint16_t w)
{
    row_clear(r);
    r[0].x1 = w;
}

/* in order of x, the empty ones last: see pass() for why */
static void row_sort(span_t *r)
{
    for (uint32_t i = 1; i < RDP_SPANS; i++) {
        const span_t v = r[i];
        const bool   v_empty = v.x1 <= v.x0;
        uint32_t     j = i;
        while (j > 0) {
            const span_t u = r[j - 1u];
            const bool   u_empty = u.x1 <= u.x0;
            if (v_empty || (!u_empty && u.x0 <= v.x0)) {
                break;
            }
            r[j] = u;
            j--;
        }
        r[j] = v;
    }
}

static void row_add(span_t *r, uint32_t x0, uint32_t x1)
{
    /* every piece it touches, or nearly, becomes part of it */
    for (bool again = true; again;) {
        again = false;
        for (uint32_t i = 0; i < RDP_SPANS; i++) {
            if (r[i].x1 <= r[i].x0) {
                continue;
            }
            if (r[i].x0 <= x1 + RDP_SPAN_GAP && x0 <= r[i].x1 + RDP_SPAN_GAP) {
                x0 = (r[i].x0 < x0) ? r[i].x0 : x0;
                x1 = (r[i].x1 > x1) ? r[i].x1 : x1;
                r[i].x0 = r[i].x1 = 0;
                again = true;
            }
        }
    }
    uint32_t best = 0, best_gap = UINT32_MAX;
    for (uint32_t i = 0; i < RDP_SPANS; i++) {
        if (r[i].x1 <= r[i].x0) {
            r[i].x0 = (uint16_t)x0;
            r[i].x1 = (uint16_t)x1;
            row_sort(r);
            return;
        }
        const uint32_t gap = (r[i].x0 > x1) ? r[i].x0 - x1 : x0 - r[i].x1;
        if (gap < best_gap) {
            best_gap = gap;
            best = i;
        }
    }
    /* no place: into the nearest, which may then reach another */
    x0 = (r[best].x0 < x0) ? r[best].x0 : x0;
    x1 = (r[best].x1 > x1) ? r[best].x1 : x1;
    r[best].x0 = r[best].x1 = 0;
    row_add(r, x0, x1);
}

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
/*
 * THE PAINTER'S PALETTE: colour to index, by a hash of the colour, and what
 * the far screen has been told.  Entries are handed out in order and never
 * moved, so the untold ones are always [told, n).  Full, a colour goes in
 * RGB565 and the table starts again with the next frame; a frame that
 * missed again turns indices off for a while (noise has 65 536 colours).
 */
#define RDP_PAL_SLOTS 512u
#define RDP_PAL_REST  30u /* frames without indices after two that missed */
typedef struct {
    uint16_t col[256];
    uint16_t key[RDP_PAL_SLOTS];
    int16_t  slot[RDP_PAL_SLOTS]; /* the key's index, or -1 */
    uint16_t n, told;
    bool     missed;
    uint8_t  misses_in_row, rest;
    bool     last_ok;
    uint16_t last_c;
    uint8_t  last_i;
    uint8_t  ix[]; /* one row's indices */
} rpal_t;

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
    uint16_t *pic;       /* the one picture, when presents are copied */
    /*
     * WHAT A PASS READS (1 Oct 2026): pic, or the screen's own framebuffer
     * (rdp_stable_surface) when that is where the presents come from.  A
     * game drawing into it and flushing handed the painter every frame to
     * copy, 150 KB read and 150 KB written in PSRAM in the game's own
     * thread - 4-5 ms a frame while its map scrolled - into a picture that
     * was the framebuffer again.  Read where it is, and the 150 KB of pic
     * are free: under Fallout, the snapshot fits in them.
     */
    const uint16_t *src;
    const uint16_t *st_px; /* the screen's framebuffer, or NULL */
    uint16_t        st_w, st_h;
    uint16_t *snap;      /* a pass's rows, copied at one instant (or NULL) */
    volatile bool    frame_end;  /* a present reached the bottom row      */
    volatile int64_t present_us; /* when the last present came            */
    uint8_t  *known;     /* per row: is the far screen known to match pic */
    span_t   *dirty;     /* rows of pic not yet sent, under the lock      */
    span_t   *work;
    uint8_t  *msg;
    uint32_t  len;

    uint32_t passes, frames, px_in, bytes_out;
    uint16_t next_y; /* where a pass cut short goes on from */
    bool     cont;   /* ...and it goes on out of the same snapshot */
    int64_t  said_us;
    uint32_t said_passes, said_px, said_bytes;
    /* the far end's pace: time waited in sync, this second - the total and
     * the longest; and the longest time from one pass's start to the next */
    int64_t  sync_us, sync_max_us, gap_max_us, last_pass_us;
    int64_t  sync_total_us; /* since the board started, for rdp_counts */
    bool     pass_whole;    /* the pass just sent ended a frame */
    uint32_t pass_msgs;     /* messages this pass has sent so far */
    bool     rects;         /* the far screen draws a rectangle as one run */
    uint32_t rect_at;       /* where the open rectangle's header is, or 0 */
    uint16_t rect_x, rect_y, rect_w, rect_h, rect_plen;
    uint32_t cuts_inside;   /* passes cut inside a rectangle, for tests */
    uint32_t pass_msgs_max; /* the most messages a pass took, for tests */
    /* the snapshot holds every pixel as the far screen shows it: a new one
     * sends only what differs from it (trim_to_far) */
    bool     snap_is_far;
    uint32_t redraws;       /* times every row had to go again */
    /* the program let the screen go: said by a marker once what it drew
     * has gone (rdp_screen_done), and the marker carrying it */
    volatile bool done_asked;
    bool          pass_done;
    volatile bool done_sent; /* ...and the far screen was told */
    /* the last pass was cut with no snapshot to go on from: the rest is in
     * dirty, and goes at once like the rest of a snapshot */
    volatile bool more;
    bool          far_idx;     /* the far screen takes indices */
    rpal_t       *pal;         /* or NULL: no memory for it */
    volatile bool pal_forget;  /* start the palette again */
    bool          rect_idx;    /* the open rectangle is of indices */
    uint32_t      idx_px, rgb_px; /* pixels sent each way, for tests */
    /* this second's time: taking a pass's snapshot, packing and sending
     * it, and the program's own presents marking what changed */
    int64_t t_snap_us, t_send_us, t_damage_us, t_damage_wait_us;
    int64_t t_wire_us;  /* of t_send_us: inside send(), its sleeps included */
    uint32_t eagains;   /* sends the wire refused for want of room */
    /* since the board started, for `link`: passes, send() inside them, sends */
    int64_t  tot_pass_us, tot_wire_us;
    uint32_t tot_sends;
    /* ...and between passes: for the program's frame, for the least pass */
    int64_t  tot_frame_wait_us, tot_min_wait_us;
    /* ...for anything to be marked at all, and the presents that marked */
    int64_t  tot_wake_wait_us;
    uint32_t tot_presents;
    uint32_t damages;
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
    const int64_t t0 = ag_port_us();
    const int64_t until = t0 + (int64_t)RDP_SEND_PATIENCE_MS * 1000;
    ag_err_t      r;
    for (;;) {
        r = send(handle, s_p.msg, (uint16_t)s_p.len);
        if (r != -AG_EAGAIN || ag_port_us() >= until) {
            break;
        }
        s_p.eagains++;
        ag_port_task_delay(1);
    }
    s_p.t_wire_us += ag_port_us() - t0;
    s_p.tot_wire_us += ag_port_us() - t0;
    s_p.tot_sends++;
    if (r == AG_OK) {
        s_p.bytes_out += s_p.len;
        s_p.pass_msgs++;
    }
    s_p.len = RDP_HDR;
    s_p.rect_at = 0;
    return r == AG_OK;
}

/* The rectangle rdp_draw can hold: its pixels are unpacked there whole.
 * 2048, not 4096 (2 Oct 2026): the CYD, a neighbour's screen and its
 * speaker at once with no PSRAM, had not the 8 KB in one piece for it. */
#define RDP_RECT_PX 2048u

static void pal_reset(rpal_t *pl)
{
    pl->n = pl->told = 0u;
    pl->missed = false;
    pl->last_ok = false;
    for (uint32_t i = 0; i < RDP_PAL_SLOTS; i++) {
        pl->slot[i] = -1;
    }
}

/* n pixels as indices into pl->ix, new colours given the next entries;
 * NULL when the palette is full (and it says so for the frame's end). */
static const uint8_t *pal_index(rpal_t *pl, const uint16_t *px, uint32_t n)
{
    for (uint32_t k = 0; k < n; k++) {
        const uint16_t c = px[k];
        if (pl->last_ok && c == pl->last_c) {
            pl->ix[k] = pl->last_i;
            continue;
        }
        uint32_t h = ((uint32_t)c * 40503u >> 7) & (RDP_PAL_SLOTS - 1u);
        while (pl->slot[h] >= 0 && pl->key[h] != c) {
            h = (h + 1u) & (RDP_PAL_SLOTS - 1u);
        }
        if (pl->slot[h] < 0) {
            if (pl->n >= 256u) {
                pl->missed = true;
                return NULL;
            }
            pl->key[h] = c;
            pl->slot[h] = (int16_t)pl->n;
            pl->col[pl->n] = c;
            pl->n++;
        }
        pl->last_c = c;
        pl->last_i = (uint8_t)pl->slot[h];
        pl->last_ok = true;
        pl->ix[k] = pl->last_i;
    }
    return pl->ix;
}

/* The entries the far screen has not been told, ahead of what uses them. */
static bool pal_tell(rpal_t *pl, uint16_t room, uint32_t handle,
                     rdp_send_fn send)
{
    while (pl->told < pl->n) {
        s_p.rect_at = 0; /* a palette between rows ends the rectangle */
        uint32_t fit = (s_p.len + RDP_RUN_HDR + 2u <= room)
                           ? (room - s_p.len - RDP_RUN_HDR) / 2u
                           : 0u;
        const uint32_t want = (uint32_t)(pl->n - pl->told);
        if (fit == 0u) {
            if (!flush(handle, send)) {
                return false;
            }
            continue;
        }
        const uint32_t k = (want < fit) ? want : fit;
        uint8_t *const at = s_p.msg + s_p.len;
        put16(at, RDP_PAL_ROW);
        put16(at + 2, pl->told);
        put16(at + 4, (uint16_t)k);
        put16(at + 6, (uint16_t)(k * 2u));
        for (uint32_t i = 0; i < k; i++) {
            put16(at + RDP_RUN_HDR + i * 2u, pl->col[pl->told + i]);
        }
        s_p.len += RDP_RUN_HDR + k * 2u;
        pl->told = (uint16_t)(pl->told + k);
    }
    return true;
}

/* n pixels packed whole, as indices when ix is given: 0 if they do not fit. */
static uint32_t pack_whole(const uint16_t *px, const uint8_t *ix, uint32_t n,
                           uint8_t *out, uint32_t cap)
{
    if (ix == NULL) {
        return rdc_pack(px, n, out, cap);
    }
    uint32_t       taken = 0;
    const uint32_t at = rdc_pack8_some(ix, n, out, cap, &taken);
    return (taken == n) ? at : 0u;
}

/*
 * A row of a rectangle into the payload: onto the rectangle open there when
 * it is the next row of it, else as a new one.  1 if it went, -1 if it
 * does not fit what is left of the payload: then it goes as runs, which
 * fill the payload to its end and go on in the next.  Sending the payload
 * first and trying again in an empty one sent a row of noise as two
 * messages half full (30 Sep 2026): 643 bytes do not fit in 640, and the
 * 64 pixels over went alone.
 */
static int put_rect_row(uint32_t y, uint32_t x, const uint16_t *px,
                        const uint8_t *ix, uint32_t n, uint16_t room,
                        uint32_t handle, rdp_send_fn send)
{
    (void)handle;
    (void)send;
    {
        if (s_p.rect_at != 0u && s_p.rect_x == x && s_p.rect_w == n &&
            s_p.rect_idx == (ix != NULL) &&
            (uint32_t)s_p.rect_y + s_p.rect_h == y &&
            ((uint32_t)s_p.rect_h + 1u) * n <= RDP_RECT_PX) {
            const uint32_t packed =
                pack_whole(px, ix, n, s_p.msg + s_p.len, room - s_p.len);
            if (packed > 0u) {
                s_p.len += packed;
                s_p.rect_h++;
                s_p.rect_plen = (uint16_t)(s_p.rect_plen + packed);
                put16(s_p.msg + s_p.rect_at + 6, s_p.rect_plen);
                put16(s_p.msg + s_p.rect_at + 8, s_p.rect_h);
                return 1;
            }
        }
        s_p.rect_at = 0;
        if (s_p.len + RDP_RECT_HDR < room) {
            uint8_t *const at = s_p.msg + s_p.len;
            const uint32_t packed = pack_whole(px, ix, n, at + RDP_RECT_HDR,
                                               room - s_p.len - RDP_RECT_HDR);
            if (packed > 0u) {
                put16(at, (uint16_t)y);
                put16(at + 2, (uint16_t)x);
                put16(at + 4, (uint16_t)(n | RDP_RECT_BIT |
                                         ((ix != NULL) ? RDP_IDX_BIT : 0u)));
                put16(at + 6, (uint16_t)packed);
                put16(at + 8, 1u);
                s_p.rect_at = s_p.len;
                s_p.rect_x = (uint16_t)x;
                s_p.rect_y = (uint16_t)y;
                s_p.rect_w = (uint16_t)n;
                s_p.rect_h = 1u;
                s_p.rect_idx = (ix != NULL);
                s_p.rect_plen = (uint16_t)packed;
                s_p.len += RDP_RECT_HDR + packed;
                return 1;
            }
        }
    }
    return -1;
}

/* A run of the row into the payload: as much of it as there is room for,
 * and the rest in the next payload after sending this one. */
static bool put_run(uint32_t y, uint32_t x, const uint16_t *px,
                    const uint8_t *ix, uint32_t n, uint16_t room,
                    uint32_t handle, rdp_send_fn send)
{
    s_p.rect_at = 0; /* what follows is not the open rectangle's */
    while (n != 0u) {
        uint32_t taken = 0;
        if (s_p.len + RDP_RUN_HDR < room) {
            uint8_t *const at = s_p.msg + s_p.len;
            const uint32_t cap = room - s_p.len - RDP_RUN_HDR;
            const uint32_t packed =
                (ix != NULL)
                    ? rdc_pack8_some(ix, n, at + RDP_RUN_HDR, cap, &taken)
                    : rdc_pack_some(px, n, at + RDP_RUN_HDR, cap, &taken);
            if (taken != 0u) {
                put16(at, (uint16_t)y);
                put16(at + 2, (uint16_t)x);
                put16(at + 4, (uint16_t)(taken | ((ix != NULL) ? RDP_IDX_BIT : 0u)));
                put16(at + 6, (uint16_t)packed);
                s_p.len += RDP_RUN_HDR + packed;
                px += taken;
                if (ix != NULL) {
                    ix += taken;
                }
                x += taken;
                n -= taken;
            }
        }
        if (n != 0u) {
            if (taken == 0u && s_p.len <= RDP_HDR) {
                return false; /* cannot happen: a pixel fits an empty payload */
            }
            if (!flush(handle, send)) {
                return false;
            }
        }
    }
    return true;
}

/*
 * A second's line in the journal only when asked (`link report on`, 27 Sep
 * 2026): it went out of a 115 200 console every second, some 16 ms under
 * the console's lock, from the painter's own task - as the sound's meter
 * did before it (cmd_link.c).
 */
static volatile bool s_report;

void rdp_report(bool on)
{
    s_report = on;
}

static void report(void)
{
    const int64_t now = ag_port_us();
    if (!s_report) {
        return;
    }
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
    /* apart: a journal line has a length, and the end of one line was lost */
    ag_log(AG_LOG_INFO, TAG,
           "the painter's second: %u ms snapshots, %u ms packing and sending "
           "(%u in send, %u refused); the program's: %u ms in %u presents, %u "
           "of it waiting for the lock",
           (unsigned)(s_p.t_snap_us / 1000), (unsigned)(s_p.t_send_us / 1000),
           (unsigned)(s_p.t_wire_us / 1000), (unsigned)s_p.eagains,
           (unsigned)(s_p.t_damage_us / 1000), (unsigned)s_p.damages,
           (unsigned)(s_p.t_damage_wait_us / 1000));
    s_p.t_damage_wait_us = 0;
    s_p.t_wire_us = 0;
    s_p.eagains = 0;
    s_p.t_snap_us = s_p.t_send_us = s_p.t_damage_us = 0;
    s_p.damages = 0;
    s_p.sync_us = 0;
    s_p.sync_max_us = 0;
    s_p.gap_max_us = 0;
    s_p.said_us = now;
    s_p.said_passes = s_p.passes;
    s_p.said_px = s_p.px_in;
    s_p.said_bytes = s_p.bytes_out;
}

/* The piece of row `r` that has exactly columns x0..x1, or NULL. */
static span_t *piece_at(span_t *r, uint16_t x0, uint16_t x1)
{
    for (uint32_t i = 0; i < RDP_SPANS; i++) {
        if (r[i].x0 == x0 && r[i].x1 == x1 && x1 > x0) {
            return &r[i];
        }
    }
    return NULL;
}

/*
 * PIECES SQUARED INTO RECTANGLES (27 Sep 2026).  A ball is thirty rows of
 * pieces each a little wider or narrower than the one above, so each was a
 * rectangle of its own at the far end - and the CYD's panel pays about 420
 * microseconds for a rectangle before its first pixel: five SPI transfers to
 * set a window, some seventy each.  Eight balls were 250 windows a frame,
 * about 100 ms, and 2.9 whole frames a second.  So the pieces that touch the
 * piece above or below them take the columns of all of them: a ball goes as
 * its box, a quarter more pixels in one window instead of thirty.
 *
 * Twice, down and then up, so the union reaches every row of a shape; and
 * never wider than RDP_SQUARE_W or half as wide again as the wider of the
 * two, so a diagonal line does not become the rectangle around it.
 */
#define RDP_SQUARE_W 64u

static bool pieces_meet(const span_t *a, const span_t *b)
{
    return a->x1 > a->x0 && b->x1 > b->x0 && a->x0 < b->x1 && b->x0 < a->x1;
}

static void square_up(uint16_t h)
{
    for (int dir = 0; dir < 2; dir++) {
        for (uint32_t k = 1; k < h; k++) {
            const uint32_t y = (dir == 0) ? k : (uint32_t)h - 1u - k;
            const uint32_t n = (dir == 0) ? y - 1u : y + 1u;
            span_t *const       r = row_of(s_p.work, y);
            const span_t *const q = row_of(s_p.work, n);
            bool                grew = false;
            for (uint32_t i = 0; i < RDP_SPANS; i++) {
                for (uint32_t j = 0; j < RDP_SPANS; j++) {
                    if (!pieces_meet(&r[i], &q[j])) {
                        continue;
                    }
                    const uint32_t x0 = (q[j].x0 < r[i].x0) ? q[j].x0 : r[i].x0;
                    const uint32_t x1 = (q[j].x1 > r[i].x1) ? q[j].x1 : r[i].x1;
                    const uint32_t wa = (uint32_t)r[i].x1 - r[i].x0;
                    const uint32_t wb = (uint32_t)q[j].x1 - q[j].x0;
                    const uint32_t wide = (wa > wb) ? wa : wb;
                    if (x1 - x0 > RDP_SQUARE_W && x1 - x0 > wide + wide / 2u) {
                        continue;
                    }
                    if (x0 != r[i].x0 || x1 != r[i].x1) {
                        r[i].x0 = (uint16_t)x0;
                        r[i].x1 = (uint16_t)x1;
                        grew = true;
                    }
                }
            }
            if (grew) {
                /* a piece that grew may now meet its neighbour in the row */
                span_t tmp[RDP_SPANS];
                memcpy(tmp, r, sizeof(tmp));
                row_clear(r);
                for (uint32_t i = 0; i < RDP_SPANS; i++) {
                    if (tmp[i].x1 > tmp[i].x0) {
                        row_add(r, tmp[i].x0, tmp[i].x1);
                    }
                }
            }
        }
    }
}

/*
 * WHAT DIFFERS, NOT THE WAY THERE (29 Sep 2026).  A present marks what it
 * changed against the present before it, so a ball that moved for 600 ms
 * while a big frame was going out had marked its whole path - and the next
 * frame sent the path, which took longer still, while the balls went on
 * moving.  It fed itself: Maxim saw a picture standing still most of the
 * time; the far screen counted 304 waits of a quarter of a second or more
 * between two whole frames, 61% of five minutes, at "9.6 frames a second".
 * The snapshot is what the far screen shows once all of it has gone - every
 * pixel in it was sent - so what is marked is cut down to what differs from
 * it: the ball where it was and where it is.
 */
static void trim_to_far(uint16_t w, uint16_t h)
{
    for (uint32_t y = 0; y < h; y++) {
        span_t *const r = row_of(s_p.work, y);
        if (!row_any(r)) {
            continue;
        }
        span_t was[RDP_SPANS];
        memcpy(was, r, sizeof(was));
        row_clear(r);
        const uint16_t *const pic = s_p.src + (size_t)y * w;
        const uint16_t *const far = s_p.snap + (size_t)y * w;
        for (uint32_t i = 0; i < RDP_SPANS; i++) {
            uint32_t       x = was[i].x0;
            const uint32_t xe = was[i].x1;
            while (x < xe) {
                while (x < xe && pic[x] == far[x]) {
                    x++;
                }
                if (x >= xe) {
                    break;
                }
                /* a piece ends at RDP_SPAN_GAP pixels that are the same */
                uint32_t e = x + 1u;
                uint32_t same = 0;
                while (e < xe && same < RDP_SPAN_GAP) {
                    same = (pic[e] == far[e]) ? same + 1u : 0u;
                    e++;
                }
                row_add(r, x, e - same);
                /* and into the snapshot as it is found: the pixels that
                 * differ are the ones it has to take, and this is the one
                 * pass over them (1 Oct 2026: compared here, then copied
                 * again below - the frame through PSRAM twice more than
                 * needed, under a game that wanted the bus) */
                memcpy(s_p.snap + (size_t)y * w + x, pic + x,
                       (size_t)(e - same - x) * 2u);
                x = e;
            }
        }
    }
}

/* Every row of the work whole: a snapshot of it is all of the far screen. */
static bool work_is_all(uint16_t w, uint16_t h)
{
    for (uint32_t y = 0; y < h; y++) {
        const span_t *const r = row_of(s_p.work, y);
        if (r[0].x0 != 0 || r[0].x1 != w) {
            return false;
        }
    }
    return true;
}

/* One pass: every row marked since the last, out of the picture and into
 * the wire.  Apart from the loop so the host test can make passes itself. */
/*
 * THE SNAPSHOT ONLY WHILE MEMORY IS PLENTIFUL (1 Oct 2026).  It is 150 KB of
 * the same picture again, taken at the first present - at boot, before any
 * program - and a game started after it found that much less for itself:
 * Fallout could not open a script and faulted.  So it is taken when a new
 * frame starts and there is RDP_SNAP_SPARE left beside it, and given back
 * when the free memory falls under RDP_SNAP_FLOOR.  Without it a frame goes
 * out of the picture as it is (s_p.more), which a game under memory
 * pressure can afford and 150 KB it cannot.
 */
#define RDP_SNAP_SPARE (1024u * 1024u)
#define RDP_SNAP_FLOOR (384u * 1024u)
static void snap_mind(void)
{
    const size_t bytes = (size_t)s_p.cap_px * 2u;
    const size_t free_slow = ag_port_mem_free(AG_MEM_SLOW | AG_MEM_BYTE);
    if (s_p.snap == NULL) {
        if (ag_port_mem_largest(AG_MEM_SLOW | AG_MEM_BYTE) >= bytes + RDP_SNAP_SPARE) {
            s_p.snap = ag_port_alloc(bytes, AG_MEM_SLOW | AG_MEM_BYTE);
            s_p.snap_is_far = false;
        }
    } else if (free_slow < RDP_SNAP_FLOOR) {
        ag_port_free(s_p.snap);
        s_p.snap = NULL;
        s_p.snap_is_far = false;
        s_p.cont = false;
        ag_log(AG_LOG_INFO, TAG, "memory short (%u KB free): the snapshot "
                                 "given back",
               (unsigned)(free_slow / 1024u));
    }
}

static void pass(void)
{
    {
        const int64_t t_in = ag_port_us();
        wt_note(WT_PASS_IN, s_p.passes);
        lock();
        const uint16_t w = s_p.w;
        const uint16_t h = s_p.h;
        /*
         * THE REST OF THE SAME FRAME (27 Sep 2026).  A pass cut short
         * (RDP_PASS_MSGS) gave its rows back to the dirty set, and the next
         * pass took them out of the picture again - a later frame's: the top
         * of a big square from one instant and its bottom from the next, the
         * tear Maxim still saw with the snapshot in.  Now what is left goes on
         * from this snapshot, and the next is taken when all of it has gone.
         */
        if (!s_p.cont && !s_p.more) {
            snap_mind(); /* between frames: take it, or give it back */
        }
        const bool cont = s_p.cont && s_p.snap != NULL;
        s_p.cont = false;
        rpal_t *const pl = (s_p.far_idx && s_p.pal != NULL) ? s_p.pal : NULL;
        if (s_p.pal != NULL && s_p.pal_forget) {
            s_p.pal_forget = false;
            pal_reset(s_p.pal);
        }
        if (pl != NULL && !cont && !s_p.more && pl->missed) {
            /* a new frame: the colours that did not fit, a table again -
             * and after two frames that missed, a rest from indices */
            pal_reset(pl);
            if (++pl->misses_in_row >= 2u) {
                pl->misses_in_row = 0;
                pl->rest = RDP_PAL_REST;
            }
        } else if (pl != NULL && !cont && !s_p.more) {
            pl->misses_in_row = 0;
            if (pl->rest != 0u) {
                pl->rest--;
            }
        }
        /* asked before this snapshot: it holds the program's last frame */
        const bool done = s_p.done_asked;
        if (!cont) {
        memcpy(s_p.work, s_p.dirty, (size_t)h * RDP_SPANS * sizeof(span_t));
        memset(s_p.dirty, 0, (size_t)h * RDP_SPANS * sizeof(span_t));
        bool taken = false; /* the snapshot has what goes already */
        if (s_p.snap != NULL && s_p.snap_is_far) {
            trim_to_far(w, h);
            taken = true;
        } else if (s_p.snap != NULL && work_is_all(w, h)) {
            s_p.snap_is_far = true; /* this snapshot is the whole screen */
        }
        square_up(h); /* before the snapshot: it copies what goes */
        /*
         * ONE INSTANT, NOT A PASS'S LENGTH OF THEM (27 Sep 2026).  The rows
         * were sent out of the picture itself while the program went on
         * presenting into it, so the top of the square went in one frame and
         * its bottom in the next - "the bottom lags the top", Maxim.  What
         * this pass sends is copied here, under the lock the presents take.
         */
        if (s_p.snap != NULL && !taken) {
            for (uint32_t y = 0; y < h; y++) {
                const span_t *const r = row_of(s_p.work, y);
                for (uint32_t i = 0; i < RDP_SPANS; i++) {
                    if (r[i].x1 > r[i].x0) {
                        memcpy(s_p.snap + (size_t)y * w + r[i].x0,
                               s_p.src + (size_t)y * w + r[i].x0,
                               (size_t)(r[i].x1 - r[i].x0) * 2u);
                    }
                }
            }
        }
        }
        const uint32_t    handle = s_p.handle;
        const uint16_t    room = s_p.room;
        const rdp_send_fn send = s_p.send;
        unlock();
        const int64_t t_snapped = ag_port_us();
        wt_note(WT_SNAP_OUT, (uint32_t)(t_snapped - t_in));
        s_p.t_snap_us += t_snapped - t_in;

        bool     any = false;
        bool     ok = true;
        uint32_t changed = 0;
        put16(s_p.msg, w);
        put16(s_p.msg + 2, h);
        s_p.len = RDP_HDR;
        s_p.pass_msgs = 0;
        const uint32_t first = (s_p.next_y < h) ? s_p.next_y : 0u;
        s_p.next_y = 0;
        /*
         * RECTANGLE BY RECTANGLE, not row by row: the far end joins runs of
         * one x and width on rows one under another into a single blit, and
         * a row's pieces taken in turn - the square's left edge, its right
         * edge, the left again - joined none of them.  So a piece goes with
         * every piece under it that has its columns (square_up made most of
         * them so), down to the first row that has not.
         */
        bool cut = false;
        for (uint32_t k = 0; k < h && ok && !cut; k++) {
            const uint32_t y = (first + k) % h;
            for (uint32_t i = 0; i < RDP_SPANS && ok && !cut; i++) {
                span_t *p = row_of(s_p.work, y) + i;
                if (p->x1 <= p->x0) {
                    continue;
                }
                const uint16_t x0 = p->x0;
                const uint16_t x1 = p->x1;
                /* a new rectangle: the one place a pass is cut */
                if (s_p.pass_msgs >= RDP_PASS_MSGS) {
                    cut = true;
                    s_p.next_y = (uint16_t)y;
                    break;
                }
                for (uint32_t yy = y; p != NULL && ok;) {
                    /* inside one only when it alone is more than a pass */
                    if (s_p.pass_msgs >= RDP_PASS_MSGS_HARD) {
                        cut = true;
                        s_p.cuts_inside++;
                        s_p.next_y = (uint16_t)yy;
                        break;
                    }
                    any = true;
                    const uint16_t *row =
                        ((s_p.snap != NULL) ? s_p.snap : s_p.src) +
                        (size_t)yy * w;
                    const uint8_t *ix =
                        (pl != NULL && pl->rest == 0u)
                            ? pal_index(pl, row + x0, (uint32_t)(x1 - x0))
                            : NULL;
                    if (ix != NULL && pl->told < pl->n) {
                        ok = pal_tell(pl, room, handle, send);
                        if (!ok) {
                            break;
                        }
                    }
                    if (ix != NULL) {
                        s_p.idx_px += (uint32_t)(x1 - x0);
                    } else {
                        s_p.rgb_px += (uint32_t)(x1 - x0);
                    }
                    const int rr =
                        (s_p.rects && (uint32_t)(x1 - x0) <= RDP_RECT_PX)
                            ? put_rect_row(yy, x0, row + x0, ix,
                                           (uint32_t)(x1 - x0), room, handle,
                                           send)
                            : -1;
                    if (rr >= 0) {
                        ok = rr > 0;
                        changed += (uint32_t)(x1 - x0);
                    }
                    for (uint32_t x = x0; rr < 0 && x < x1 && ok;
                         x += RDP_CHUNK) {
                        const uint32_t n = (x1 - x < RDP_CHUNK) ? x1 - x
                                                                : RDP_CHUNK;
                        ok = put_run(yy, x, row + x,
                                     (ix != NULL) ? ix + (x - x0) : NULL, n, room,
                                     handle, send);
                        changed += n;
                    }
                    p->x0 = p->x1 = 0; /* sent: not again if continued */
                    yy++;
                    p = (yy < h) ? piece_at(row_of(s_p.work, yy), x0, x1)
                                 : NULL;
                }
            }
        }
        s_p.more = cut && ok && s_p.snap == NULL;
        if (cut && ok) {
            if (s_p.snap != NULL) {
                /* the rest stays in work, out of the snapshot */
                s_p.cont = true;
            } else {
                /* the rest for the next pass, back where the presents
                 * meanwhile have been marking */
                lock();
                for (uint32_t y = 0; y < h; y++) {
                    const span_t *const ws = row_of(s_p.work, y);
                    for (uint32_t i = 0; i < RDP_SPANS; i++) {
                        if (ws[i].x1 > ws[i].x0) {
                            row_add(row_of(s_p.dirty, y), ws[i].x0, ws[i].x1);
                        }
                    }
                }
                unlock();
            }
            (void)ag_port_sem_give(s_p.wake);
        }
        if (ok) {
            ok = flush(handle, send);
        }
        s_p.t_send_us += ag_port_us() - t_snapped;
        if (s_p.pass_msgs > s_p.pass_msgs_max) {
            s_p.pass_msgs_max = s_p.pass_msgs;
        }
        if (!ok) {
            /* the far screen is not known to show what the picture says:
             * all of it again, next time */
            s_p.cont = false;
            s_p.redraws++;
            s_p.pal_forget = true;
            lock();
            s_p.snap_is_far = false;
            for (uint32_t y = 0; y < s_p.h; y++) {
                row_whole(row_of(s_p.dirty, y), s_p.w);
            }
            unlock();
        } else if (any) {
            s_p.passes++;
            s_p.pass_whole = !cut;
            s_p.pass_done = done && !cut;
            if (s_p.pass_done) {
                s_p.done_asked = false;
                s_p.done_sent = true;
            }
            if (!cut) {
                s_p.frames++; /* nothing of it left: a whole frame went */
            }
            s_p.px_in += changed;
            const rdp_sync_fn sync = s_sync;
            if (sync != NULL) {
                const int64_t t0 = ag_port_us();
                sync(handle);
                const int64_t w = ag_port_us() - t0;
                s_p.sync_us += w;
                s_p.sync_total_us += w;
                if (w > s_p.sync_max_us) {
                    s_p.sync_max_us = w;
                }
            }
            const int64_t now = ag_port_us();
            if (s_p.last_pass_us != 0 && now - s_p.last_pass_us > s_p.gap_max_us) {
                s_p.gap_max_us = now - s_p.last_pass_us;
            }
            s_p.last_pass_us = now;
            s_p.pass_done = false;
        } else if (ok && done && s_sync != NULL) {
            /* nothing left to draw: the marker alone, and it is no frame */
            s_p.done_asked = false;
            s_p.done_sent = true;
            s_p.pass_whole = false;
            s_p.pass_done = true;
            s_sync(handle);
            s_p.pass_done = false;
        }
        report();
        wt_note(WT_PASS_OUT, s_p.pass_msgs);
        s_p.tot_pass_us += ag_port_us() - t_in;
    }
}

static void painter(void *arg)
{
    (void)arg;
    int64_t began = 0;
    for (;;) {
        const int64_t ww0 = ag_port_us();
        wt_note(WT_WAKE_IN, 0u);
        (void)ag_port_sem_take(s_p.wake, ag_port_ms_to_ticks(1000));
        wt_note(WT_WAKE_OUT, 0u);
        s_p.tot_wake_wait_us += ag_port_us() - ww0;
        /*
         * A WHOLE FRAME, then a pass: a program presents a frame in bands
         * from the top, and a pass taken while it is half way down is half
         * one frame and half the other.  The band that reaches the bottom row
         * ends a frame; a program that only ever changes a corner never
         * reaches it, so a pause of RDP_FRAME_QUIET_US in the presents ends
         * one too.
         */
        /*
         * AND WITHOUT A SNAPSHOT TOO (30 Sep 2026).  Under Fallout there is no
         * memory for the second picture, and the rest of a cut pass went back
         * to the dirty rows to wait - for the program's next frame, and the
         * pause between passes after it: one pass of eight messages a frame
         * while the map scrolled, 45 passes and a third of the wire a second,
         * a picture Maxim saw at two frames a second.
         */
        const int64_t fw0 = ag_port_us();
        while (!s_p.cont && !s_p.more && !s_p.frame_end &&
               ag_port_us() - s_p.present_us < RDP_FRAME_QUIET_US) {
            (void)ag_port_sem_take(s_p.wake, ag_port_ms_to_ticks(2));
        }
        s_p.tot_frame_wait_us += ag_port_us() - fw0;
        /* the rest of a frame goes at once: it is the same frame */
        const bool rest = s_p.cont || s_p.more;
        if (!rest) {
            s_p.frame_end = false;
        }
        const int64_t since = ag_port_us() - began;
        if (!rest && began != 0 && since < RDP_MIN_PASS_US) {
            const int64_t mw0 = ag_port_us();
            ag_port_task_delay(ag_port_ms_to_ticks(
                (uint32_t)((RDP_MIN_PASS_US - since) / 1000) + 1u));
            s_p.tot_min_wait_us += ag_port_us() - mw0;
        }
        began = ag_port_us();
        pass();
    }
}

void rdp_times(uint32_t *pass_ms, uint32_t *send_ms, uint32_t *sync_ms,
               uint32_t *sends)
{
    *pass_ms = (uint32_t)(s_p.tot_pass_us / 1000);
    *send_ms = (uint32_t)(s_p.tot_wire_us / 1000);
    *sync_ms = (uint32_t)(s_p.sync_total_us / 1000);
    *sends = s_p.tot_sends;
}

void rdp_waits(uint32_t *frame_ms, uint32_t *min_ms, uint32_t *wake_ms,
               uint32_t *presents)
{
    *frame_ms = (uint32_t)(s_p.tot_frame_wait_us / 1000);
    *min_ms = (uint32_t)(s_p.tot_min_wait_us / 1000);
    *wake_ms = (uint32_t)(s_p.tot_wake_wait_us / 1000);
    *presents = s_p.tot_presents;
}

void rdp_counts(uint32_t *frames, uint32_t *passes, uint32_t *wait_us)
{
    *frames = s_p.frames;
    *passes = s_p.passes;
    *wait_us = (s_p.passes != 0u)
                   ? (uint32_t)(s_p.sync_total_us / (int64_t)s_p.passes)
                   : 0u;
}

void rdp_far_rects(bool yes)
{
    s_p.rects = yes;
}

void rdp_stable_surface(const uint16_t *px, uint16_t w, uint16_t h)
{
    s_p.st_px = px;
    s_p.st_w = w;
    s_p.st_h = h;
}

void rdp_far_idx(bool yes)
{
    if (yes && !s_p.far_idx) {
        s_p.pal_forget = true; /* whatever it had, it has from us no more */
    }
    s_p.far_idx = yes;
}

void rdp_idx_counts(uint32_t *idx_px, uint32_t *rgb_px)
{
    *idx_px = s_p.idx_px;
    *rgb_px = s_p.rgb_px;
}

bool rdp_pass_whole(void)
{
    return s_p.pass_whole;
}

bool rdp_pass_done(void)
{
    return s_p.pass_done;
}

void rdp_screen_done(void)
{
    if (s_p.task == NULL || s_p.src == NULL) {
        return; /* nothing was ever sent: nothing to say */
    }
    s_p.done_asked = true;
    (void)ag_port_sem_give(s_p.wake);
}

void rdp_screen_taken(void)
{
    s_p.done_asked = false; /* taken again before the marker went: no marker */
    if (s_p.done_sent) {
        s_p.done_sent = false;
        rdp_forget(); /* its console has the glass: all of it again */
    }
}

uint32_t rdp_redraws(void)
{
    return s_p.redraws;
}

void rdp_forget(void)
{
    if (s_p.task == NULL || s_p.src == NULL) {
        return;
    }
    s_p.redraws++;
    s_p.pal_forget = true;
    lock();
    s_p.snap_is_far = false;
    for (uint32_t y = 0; y < s_p.h; y++) {
        row_whole(row_of(s_p.dirty, y), s_p.w);
    }
    unlock();
    (void)ag_port_sem_give(s_p.wake);
}

/* The buffers, once, for the first surface's size; false if there is no
 * memory for them - then the proxy sends raw, as it did. */
static bool buffers(uint16_t w, uint16_t h, uint16_t room, bool stable)
{
    s_p.cap_px = (uint32_t)w * h;
    s_p.cap_rows = h;
    s_p.pic = stable ? NULL : ag_port_alloc((size_t)s_p.cap_px * 2u, AG_MEM_BYTE);
    s_p.src = stable ? s_p.st_px : s_p.pic;
    /* the snapshot is taken later, when there is memory to spare for it
     * (snap_mind) */
    s_p.snap = NULL;
    s_p.known = ag_port_alloc(h, AG_MEM_BYTE);
    s_p.dirty = ag_port_alloc((size_t)h * RDP_SPANS * sizeof(span_t), AG_MEM_BYTE);
    s_p.work = ag_port_alloc((size_t)h * RDP_SPANS * sizeof(span_t), AG_MEM_BYTE);
    s_p.msg = ag_port_alloc(room, AG_MEM_BYTE);
    /* the palette is worth having, not worth failing for */
    s_p.pal = ag_port_alloc(sizeof(rpal_t) + w, AG_MEM_BYTE);
    if (s_p.pal != NULL) {
        memset(s_p.pal, 0, sizeof(rpal_t));
        pal_reset(s_p.pal);
    }
    if (s_p.src == NULL || s_p.known == NULL || s_p.dirty == NULL ||
        s_p.work == NULL || s_p.msg == NULL) {
        ag_port_free(s_p.pic);
        ag_port_free(s_p.snap);
        s_p.snap = NULL;
        ag_port_free(s_p.known);
        ag_port_free(s_p.dirty);
        ag_port_free(s_p.work);
        ag_port_free(s_p.msg);
        s_p.pic = NULL;
        s_p.src = NULL;
        return false;
    }
    memset(s_p.known, 0, h);
    memset(s_p.dirty, 0, (size_t)h * RDP_SPANS * sizeof(span_t));
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
    const bool stable =
        s_p.st_px != NULL && b->surf_w == s_p.st_w && b->surf_h == s_p.st_h &&
        b->stride == (uint32_t)s_p.st_w * 2u &&
        (const uint16_t *)b->px >= s_p.st_px &&
        (const uint16_t *)b->px < s_p.st_px + (size_t)s_p.st_w * s_p.st_h;
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
            !buffers(b->surf_w, b->surf_h, room, stable) ||
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
    const int64_t t_dmg = ag_port_us();
    lock();
    s_p.t_damage_wait_us += ag_port_us() - t_dmg;
    if (b->surf_w != s_p.w || b->surf_h != s_p.h) {
        if ((uint32_t)b->surf_w * b->surf_h > s_p.cap_px ||
            b->surf_h > s_p.cap_rows || room > s_p.room) {
            unlock();
            s_p.t_damage_us += ag_port_us() - t_dmg;
            s_p.damages++;
            s_p.tot_presents++;
            return false; /* bigger than the picture was made for */
        }
        /* another shape: nothing of the far screen is known any more */
        s_p.snap_is_far = false;
        s_p.w = b->surf_w;
        s_p.h = b->surf_h;
        memset(s_p.known, 0, s_p.h);
        memset(s_p.dirty, 0, (size_t)s_p.h * RDP_SPANS * sizeof(span_t));
    }
    s_p.handle = handle;
    s_p.send = send;
    const uint32_t y1 = ((uint32_t)b->y + b->h < s_p.h) ? b->y + b->h : s_p.h;
    const uint32_t xe = ((uint32_t)b->x + b->w < s_p.w) ? b->x + b->w : s_p.w;
    bool           marked = false;
    if (stable != (s_p.src == s_p.st_px && s_p.st_px != NULL)) {
        /* the presents changed where they come from: a picture of our own
         * (taken now, if it never was), or the screen's again - and the
         * far screen known from neither */
        if (!stable && s_p.pic == NULL) {
            s_p.pic = ag_port_alloc((size_t)s_p.cap_px * 2u, AG_MEM_BYTE);
            if (s_p.pic == NULL) {
                unlock();
                return false; /* no memory for it: raw */
            }
        }
        s_p.src = stable ? s_p.st_px : s_p.pic;
        s_p.snap_is_far = false;
        memset(s_p.known, 0, s_p.h);
        for (uint32_t y = 0; y < s_p.h && stable; y++) {
            row_whole(row_of(s_p.dirty, y), s_p.w);
        }
        marked = stable;
    }
    if (stable) {
        /* the pixels are where the painter will read them: only mark */
        for (uint32_t y = b->y; y < y1 && b->x < xe; y++) {
            row_add(row_of(s_p.dirty, y), b->x, xe);
        }
        marked = marked || (b->y < y1 && b->x < xe);
    }
    for (uint32_t y = b->y; y < y1 && b->x < xe && !stable; y++) {
        const uint16_t *in = (const uint16_t *)(const void *)(
            (const uint8_t *)b->px + (size_t)(y - b->y) * b->stride);
        uint16_t *row = s_p.pic + (size_t)y * s_p.w;
        span_t *const   r = row_of(s_p.dirty, y);
        if (!s_p.known[y]) {
            if (b->x == 0 && xe == s_p.w) {
                s_p.known[y] = 1; /* a whole row: from now on it is known */
            }
            memcpy(row + b->x, in, (size_t)(xe - b->x) * 2u);
            row_add(r, b->x, xe);
            marked = true;
            continue;
        }
        /*
         * ALREADY GOING WHOLE: a copy, and nothing to compare (30 Sep 2026).
         * While the far screen is behind a picture that changes everywhere,
         * the row is still marked from the last present, and comparing it
         * pixel by pixel found what was already known - the program spent
         * 360 ms of every second doing it, scrolling at 26 frames a second
         * where noise went at 52.
         */
        bool covered = false;
        for (uint32_t i = 0; i < RDP_SPANS && !covered; i++) {
            covered = r[i].x0 <= b->x && r[i].x1 >= xe && r[i].x1 > r[i].x0;
        }
        if (covered) {
            memcpy(row + b->x, in, (size_t)(xe - b->x) * 2u);
            marked = true; /* and the painter woken: it may be waiting */
            continue;
        }
        /* what differs from what the far screen will show, and only that,
         * piece by piece: a pointer that moved is a few pixels of a few
         * rows, and two balls are two pieces, not the row between them */
        uint32_t x = b->x;
        while (x < xe) {
            while (x < xe && in[x - b->x] == row[x]) {
                x++;
            }
            if (x >= xe) {
                break;
            }
            /* a piece ends at RDP_SPAN_GAP pixels that are the same */
            uint32_t e = x + 1u;
            uint32_t same = 0;
            while (e < xe && same < RDP_SPAN_GAP) {
                same = (in[e - b->x] == row[e]) ? same + 1u : 0u;
                e++;
            }
            const uint32_t end = e - same;
            memcpy(row + x, in + (x - b->x), (size_t)(end - x) * 2u);
            row_add(r, x, end);
            marked = true;
            x = e;
        }
    }
    s_p.present_us = ag_port_us();
    if ((uint32_t)b->y + b->h >= s_p.h) {
        s_p.frame_end = true;
    }
    unlock();
    s_p.t_damage_us += ag_port_us() - t_dmg;
    s_p.damages++;
    s_p.tot_presents++;
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
/*
 * TWO STRIPS AT ONCE, AND ACROSS MESSAGES (30 Sep 2026).  A row of noise the
 * width of the screen does not fit one message: each message ends where its
 * bytes run out, somewhere along a row, and the next begins there.  Drawn a
 * message at a time that was some 480 windows a frame at about 420 us each,
 * and the CYD drew noise at 2.3 frames a second off the wire and at 19 from
 * its own program.  Gathered only where a piece lies exactly under the last,
 * it was still 386 windows a second of 960 pixels each, because the cuts
 * move along the row from one message to the next.
 *
 * So a slot gathers a strip in reading order: the next piece on the row it
 * is filling, where the last one ended, or a whole rectangle under it the
 * strip's width.  Its first row sets the width, and may come in any number
 * of pieces.  Two slots, because rectangles arrive side by side as well as
 * one under another.  Kept from one message to the next and drawn when
 * full, when a piece fits neither, when a piece would land on what one
 * holds (so nothing is drawn out of turn), and by rdp_draw_flush: at the
 * end of a frame and when nothing more arrives.  Drawn at the end of
 * every pass instead, a strip was a pass long - some 2 500 pixels in three
 * windows - since a pass of noise ends in the middle of a row.
 */
#define RDP_SLOTS   2u
#define RDP_SLOT_PX (RDP_RECT_PX / RDP_SLOTS)
typedef struct {
    uint16_t x, y, w; /* where the strip is, and its width                */
    uint16_t h;       /* whole rows held                                  */
    uint16_t part;    /* pixels held of row y + h; a first row is all part */
    bool     live;
} rslot_t;
/*
 * All of it in the one block taken for the pixels, and only the pointer in
 * static memory: the emulated S3's DRAM had 48 bytes too few for the slots
 * and their counters as statics (30 Sep 2026).
 */
typedef struct {
    rslot_t                 slot[RDP_SLOTS];
    const ag_display_ops_t *d;
    uint16_t                sw, sh;
    uint8_t                 next;
    uint32_t                t_unpack_us, t_blit_us, blits;
    uint32_t                fl[RDP_FL_N]; /* why a strip went to the glass */
    uint16_t                pal[256];     /* the painter's palette, as told */
    uint16_t                px[RDP_RECT_PX];
} rdraw_t;
static rdraw_t *s_dr;
static uint32_t s_draw_faults;

uint32_t rdp_draw_faults(void) { return s_draw_faults; }

void rdp_draw_times(uint32_t *unpack_us, uint32_t *blit_us, uint32_t *blits,
                    uint32_t why[RDP_FL_N])
{
    rdraw_t *const r = s_dr;
    *unpack_us = (r != NULL) ? r->t_unpack_us : 0u;
    *blit_us = (r != NULL) ? r->t_blit_us : 0u;
    *blits = (r != NULL) ? r->blits : 0u;
    for (unsigned i = 0; i < RDP_FL_N; i++) {
        why[i] = (r != NULL) ? r->fl[i] : 0u;
    }
    if (r != NULL) {
        r->t_unpack_us = r->t_blit_us = r->blits = 0u;
        memset(r->fl, 0, sizeof(r->fl));
    }
}

bool rdp_draws_rects(void) { return s_dr != NULL; }

bool rdp_draws_idx(void) { return s_dr != NULL; }

static void blit_px(const ag_display_ops_t *d, uint16_t sw, uint16_t sh,
                    const uint16_t *px, uint16_t x, uint16_t y, uint16_t w,
                    uint16_t h)
{
    const ag_blit_t bl = {
        .px = px,
        .stride = (uint32_t)w * 2u,
        .surf_w = sw,
        .surf_h = sh,
        .x = x,
        .y = y,
        .w = w,
        .h = h,
    };
    const int64_t t0 = ag_port_us();
    d->blit_rect(0, &bl);
    if (s_dr != NULL) {
        s_dr->t_blit_us += (uint32_t)(ag_port_us() - t0);
        s_dr->blits++;
    }
}

static uint16_t *slot_px(unsigned i) { return s_dr->px + (size_t)i * RDP_SLOT_PX; }

static void slot_flush(unsigned i, unsigned why)
{
    rslot_t *const sl = &s_dr->slot[i];
    if (sl->live && s_dr->d != NULL) {
        s_dr->fl[why]++;
        uint16_t *const base = slot_px(i);
        if (sl->h != 0u) {
            blit_px(s_dr->d, s_dr->sw, s_dr->sh, base, sl->x, sl->y, sl->w,
                    sl->h);
        }
        if (sl->part != 0u) {
            blit_px(s_dr->d, s_dr->sw, s_dr->sh, base + (size_t)sl->h * sl->w,
                    sl->x, (uint16_t)(sl->y + sl->h), sl->part, 1u);
        }
    }
    sl->live = false;
    sl->h = sl->part = 0u;
}

bool rdp_draw_holds(void)
{
    if (s_dr == NULL) {
        return false;
    }
    for (unsigned i = 0; i < RDP_SLOTS; i++) {
        if (s_dr->slot[i].live) {
            return true;
        }
    }
    return false;
}

/* Draws every slot but `keep` the rectangle would land on: what it holds is
 * older. */
static void slot_clear_under(uint16_t x, uint16_t y, uint16_t rw, uint16_t rh,
                             unsigned keep)
{
    for (unsigned i = 0; i < RDP_SLOTS; i++) {
        const rslot_t *const sl = &s_dr->slot[i];
        if (!sl->live || i == keep) {
            continue;
        }
        /* its bounds, the row being filled whole: at worst a draw early */
        const uint32_t y1 = (uint32_t)sl->y + sl->h + (sl->part != 0u ? 1u : 0u);
        if ((uint32_t)x < (uint32_t)sl->x + sl->w && (uint32_t)x + rw > sl->x &&
            (uint32_t)y < y1 && (uint32_t)y + rh > sl->y) {
            slot_flush(i, RDP_FL_UNDER);
        }
    }
}

void rdp_draw_flush(unsigned why)
{
    if (s_dr == NULL) {
        return;
    }
    for (unsigned i = 0; i < RDP_SLOTS; i++) {
        slot_flush(i, why);
    }
}

/* Where in slot `sl` the piece goes if it continues the strip, else NULL;
 * and the strip takes it. */
static uint16_t *slot_continue(rslot_t *sl, uint16_t *base, uint16_t x,
                               uint16_t y, uint16_t rw, uint16_t rh)
{
    if (!sl->live) {
        return NULL;
    }
    if (sl->h == 0u) {
        /* the first row: it grows to the right while it lasts */
        if (rh == 1u && y == sl->y && x == sl->x + sl->part &&
            (uint32_t)sl->part + rw <= RDP_SLOT_PX) {
            uint16_t *const into = base + sl->part;
            sl->part = (uint16_t)(sl->part + rw);
            sl->w = sl->part;
            return into;
        }
        /* and ends when a piece starts the row under it at its left edge */
        if (y != sl->y + 1u || x != sl->x || rw > sl->w) {
            return NULL;
        }
        if ((uint32_t)(1u + rh) * sl->w > RDP_SLOT_PX ||
            (rh > 1u && rw != sl->w)) {
            return NULL;
        }
        sl->h = 1u;
        sl->part = 0u;
    }
    if ((uint32_t)y != (uint32_t)sl->y + sl->h ||
        (uint32_t)x != (uint32_t)sl->x + sl->part) {
        return NULL;
    }
    if (rh == 1u) {
        if ((uint32_t)sl->part + rw > sl->w ||
            (uint32_t)(sl->h + 1u) * sl->w > RDP_SLOT_PX) {
            return NULL;
        }
        uint16_t *const into = base + (size_t)sl->h * sl->w + sl->part;
        sl->part = (uint16_t)(sl->part + rw);
        if (sl->part == sl->w) {
            sl->h++;
            sl->part = 0u;
        }
        return into;
    }
    if (sl->part != 0u || rw != sl->w ||
        (uint32_t)(sl->h + rh) * sl->w > RDP_SLOT_PX) {
        return NULL;
    }
    uint16_t *const into = base + (size_t)sl->h * sl->w;
    sl->h = (uint16_t)(sl->h + rh);
    return into;
}

/* Where `rw` x `rh` at (x, y) goes: on in a strip it continues, else into a
 * free slot, else into the oldest after drawing it.  NULL if it is bigger
 * than a slot. */
static uint16_t *slot_take(uint16_t x, uint16_t y, uint16_t rw, uint16_t rh)
{
    if ((uint32_t)rw * rh > RDP_SLOT_PX) {
        return NULL;
    }
    for (unsigned i = 0; i < RDP_SLOTS; i++) {
        uint16_t *const into =
            slot_continue(&s_dr->slot[i], slot_px(i), x, y, rw, rh);
        if (into != NULL) {
            slot_clear_under(x, y, rw, rh, i);
            return into;
        }
    }
    slot_clear_under(x, y, rw, rh, RDP_SLOTS);
    int pick = -1;
    for (unsigned i = 0; i < RDP_SLOTS && pick < 0; i++) {
        if (!s_dr->slot[i].live) {
            pick = (int)i;
        }
    }
    if (pick < 0) {
        pick = s_dr->next;
        s_dr->next = (uint8_t)((s_dr->next + 1u) % RDP_SLOTS);
        slot_flush((unsigned)pick, RDP_FL_EVICT);
    }
    rslot_t *const sl = &s_dr->slot[pick];
    sl->live = true;
    sl->x = x;
    sl->y = y;
    sl->w = rw;
    if (rh == 1u) {
        sl->h = 0u; /* a first row, open to the right */
        sl->part = rw;
    } else {
        sl->h = rh;
        sl->part = 0u;
    }
    return slot_px((unsigned)pick);
}

/*
 * The strips, made at start-up on a board with a screen (2 Oct 2026): 9 KB in
 * one piece, and on the CYD, once sound had its places, the largest piece
 * left was 4 KB - the neighbour's picture could not be drawn at all.  In the
 * slow memory where there is one.
 */
bool rdp_draw_prepare(void)
{
    if (s_dr != NULL) {
        return true;
    }
    s_dr = ag_port_alloc(sizeof(rdraw_t), AG_MEM_SLOW | AG_MEM_BYTE);
    if (s_dr == NULL) {
        s_dr = ag_port_alloc(sizeof(rdraw_t), AG_MEM_BYTE);
    }
    if (s_dr != NULL) {
        memset(s_dr, 0, offsetof(rdraw_t, px));
    }
    return s_dr != NULL;
}

uint32_t rdp_draw(const ag_display_ops_t *d, const uint8_t *pay, uint16_t len)
{
    if (d == NULL || d->blit_rect == NULL || len < RDP_HDR) {
        return 0u;
    }
    if (s_dr == NULL) {
        s_dr = ag_port_alloc(sizeof(rdraw_t), AG_MEM_BYTE);
        if (s_dr == NULL) {
            /*
             * No block, no drawing: there used to be a row of 256 pixels in
             * static memory to fall back on, and the emulated S3 had 64 bytes
             * too few of it for anything else (30 Sep 2026).  A screen that
             * cannot find 8 KB is not drawing a neighbour's picture anyway.
             */
            s_draw_faults++;
            static bool said;
            if (!said) {
                said = true;
                ag_log(AG_LOG_WARN, TAG, "no memory to draw a neighbour's "
                                         "picture (%u bytes)",
                       (unsigned)sizeof(rdraw_t));
            }
            return 0u;
        }
        memset(s_dr, 0, offsetof(rdraw_t, px));
    }
    rdraw_t *const r = s_dr;
    const uint16_t w = get16(pay);
    const uint16_t h = get16(pay + 2);
    if (r != NULL) {
        if (r->d != d || w != r->sw || h != r->sh) {
            /* another screen or shape: what is held is its own */
            rdp_draw_flush(RDP_FL_OTHER);
        }
        r->d = d;
        r->sw = w;
        r->sh = h;
    }
    uint32_t at = RDP_HDR;
    uint32_t drawn = 0;
    while (at + RDP_RUN_HDR <= len) {
        const uint16_t y = get16(pay + at);
        const uint16_t x = get16(pay + at + 2);
        const uint16_t n = get16(pay + at + 4);
        const uint16_t plen = get16(pay + at + 6);
        if (y == RDP_PAL_ROW) {
            /* palette entries: x the first, n how many */
            at += RDP_RUN_HDR;
            if (r == NULL || n == 0u || (uint32_t)x + n > 256u ||
                plen != (uint32_t)n * 2u || at + plen > len) {
                s_draw_faults++;
                break;
            }
            for (uint32_t i = 0; i < n; i++) {
                r->pal[x + i] = get16(pay + at + i * 2u);
            }
            at += plen;
            continue;
        }
        const bool     idx = (n & RDP_IDX_BIT) != 0u;
        uint16_t       rw = (uint16_t)(n & ~RDP_IDX_BIT), rh = 1;
        if (idx && r == NULL) {
            s_draw_faults++;
            break;
        }
        if ((n & RDP_RECT_BIT) != 0u) {
            /* a rectangle: its width, and its height in a fifth word */
            if (at + RDP_RECT_HDR > len) {
                s_draw_faults++;
                break;
            }
            rw = (uint16_t)(n & ~(RDP_RECT_BIT | RDP_IDX_BIT));
            rh = get16(pay + at + 8);
            at += RDP_RECT_HDR;
            if (r == NULL || rw == 0u || rh == 0u ||
                (uint32_t)rw * rh > RDP_RECT_PX || at + plen > len ||
                (uint32_t)y + rh > h || (uint32_t)x + rw > w) {
                s_draw_faults++;
                break;
            }
        } else {
            at += RDP_RUN_HDR;
            if (rw == 0u || rw > RDP_CHUNK || at + plen > len || y >= h ||
                (uint32_t)x + rw > w) {
                s_draw_faults++;
                break; /* not a run: the rest cannot be trusted either */
            }
        }
        const uint32_t px = (uint32_t)rw * rh;
        uint16_t      *into = slot_take(x, y, rw, rh);
        bool           now = false;
        if (into == NULL) {
            /* bigger than a slot: drawn at once, alone */
            rdp_draw_flush(RDP_FL_OTHER);
            into = r->px;
            now = true;
        }
        const int64_t  t0 = ag_port_us();
        const uint32_t took = idx ? rdc_unpack8(pay + at, plen, r->pal, into, px)
                                  : rdc_unpack(pay + at, plen, into, px);
        r->t_unpack_us += (uint32_t)(ag_port_us() - t0);
        if (took != plen) {
            s_draw_faults++;
            break;
        }
        at += plen;
        drawn += px;
        if (now) {
            blit_px(d, w, h, into, x, y, rw, rh);
        }
    }
    return drawn;
}
