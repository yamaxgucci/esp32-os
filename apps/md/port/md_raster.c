/*
 * ArgonOS Mega Drive - what a frame did to the VDP while its lines were shown
 * (the CYD).
 *
 * The CYD's picture is drawn by the system core and the application core's
 * spare time, a pass over the screen at their own pace, from the VDP as it
 * is when each line is drawn (md_main.c).  A game that changes the VDP while
 * the screen is being drawn - a raster effect - comes out wrong that way:
 * Road Rash II sets the vertical scroll a dozen times a frame to bend its
 * road, Gunstar Heroes moves its planes, Sonic 2 splits the screen, Sonic's
 * water swaps the palette (host harness, 31 Oct 2026).  Each line got the
 * scroll of whatever line the 68000 was at.
 *
 * So the frame is written down as it runs: at its first line the registers
 * and the vertical scroll as they are, and every write to them or to the
 * palette while a visible line runs, with its line.  A pass starts from that
 * copy and puts in the writes as its lines reach them - so each band of the
 * road gets its own scroll.
 *
 * The copy is the frame's start, not the VDP as the pass finds it: a pass
 * that began while the 68000 was halfway down the next frame took one band's
 * scroll and backdrop for every line above the first write, and one that
 * began in a vertical blank with the display off drew the whole screen black
 * (he saw black bands and a black screen flashing, 31 Oct 2026).  The palette
 * is the live one unless the frame wrote it while lines were shown: it is
 * written in the vertical blank, for the VRAM the pass draws from.
 *
 * A pass is drawn by both cores all the same: a line with writes before it
 * is drawn only once every line above it has been, and the core that takes
 * it puts the writes in first (md_main.c, claim_line).  Gunstar Heroes
 * changes its registers at the status bar every frame, and drawn by one core
 * those frames gave 20-28 pictures a second (31 Oct 2026).
 *
 * Two records: the 68000 writes one while a pass draws from the other.  A
 * record grows in pieces of 32 writes, as many as the heap's leftovers give
 * (up to 256), after the scroll and palette copies - on the CYD that is one
 * piece.  Gunstar Heroes' intro sets the vertical scroll on every line, 223
 * writes a frame in 669 frames of 3600 (host harness, 31 Oct 2026); a frame
 * with more writes than the record holds is drawn as before.
 *
 * The horizontal scroll table is in VRAM, which the game rewrites in the
 * vertical blank for the next frame - the pass, drawn after it, gave each
 * band of Road Rash II's road this frame's height and the next frame's curve,
 * and the horizon shook (31 Oct 2026).  So the table is copied when the last
 * visible line has run (md_raster_visible), before the vertical blank's DMA,
 * and the frame published then.  Gunstar Heroes also rewrites the table while
 * the screen is shown, and the bands of its town came out shifted (host
 * harness, 31 Oct 2026): a write into the table while lines are shown first
 * copies the lines above it (md_raster_hs_write, from the VDP's VRAM write).
 * Taking every line's words as its picture started did the same with a call
 * into flash on every line, and Thunder Force IV's 68000 took 0.45 ms a frame
 * longer (31 Oct 2026).
 *
 * There is one copy: while a pass draws from it the frames that finish are
 * not published, and the drawer waits for the next one rather than draw the
 * same frame again.
 *
 * All of it is taken from what the heap has left once the machine's pages
 * are cut (md_raster_init): blocks too small for a 512-byte page, which the
 * machine cannot use.  As statics it made the machine a page short on one
 * start in two (31 Oct 2026).  Without the blocks, frames are drawn as before.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
/* the CYD (and its host harness) only: the small build's paged VRAM */
#ifdef ARGON_MD_SMALL
#include <stdint.h>
#include <string.h>

#include "gwenesis_bus.h"
#include "gwenesis_vdp.h"
#include "md_raster.h"

#define MD_RAS_HBLANK 860 /* md_main.c's MD_LINE_HBLANK */

extern unsigned char  gwenesis_vdp_regs[];
extern unsigned short CRAM[];
extern unsigned short VSRAM[];
extern unsigned short CRAM565[];
extern int scan_line;
extern int screen_height;

/* what the renderer reads (gwenesis_vdp_gfx.c): the live VDP, or a copy */
unsigned char  *md_vr_regs = gwenesis_vdp_regs;
unsigned short *md_vr_vsram = VSRAM;
unsigned short *md_vr_cram565 = CRAM565;

/* the writes a record holds, in pieces */
#define MD_RAS_PIECE  32
#ifndef MD_RAS_PIECES
#define MD_RAS_PIECES 4 /* a record of 124 bytes: two and a copy fit 256-byte holes */
#endif
#define MD_RAS_EVENTS (MD_RAS_PIECE * MD_RAS_PIECES)

typedef struct {
    uint8_t  regs[MD_RAS_REGS];
    uint16_t vsram[MD_RAS_VSRAM];
    uint16_t n;    /* events; cap + 1 once it overflowed */
    uint16_t cram; /* palette writes among them */
    /* line << 20 | kind << 18 | index << 12 | value */
    uint32_t *ev[MD_RAS_PIECES];
} md_ras_t;

#define MD_RAS_EV(r, i) ((r)->ev[(i) / MD_RAS_PIECE][(i) % MD_RAS_PIECE])

static unsigned          s_cap; /* events a record holds: whole pieces */

static md_ras_t         *s_rec[2];

/* the horizontal scroll a line of the published frame was drawn with, plane
 * A and B, in 16-line pieces of 64 bytes: the heap's leftovers are small, and
 * after the lines met on the CYD (1 Nov 2026) not one 128-byte piece of a
 * table was left - MD_RAS_HS_PIECE and gwenesis_vdp_gfx.c's md_hs_fetch
 * agree on the size */
#define MD_RAS_HS_LINES 224 /* NTSC; a PAL frame is not published */
#define MD_RAS_HS_PIECE 16
static uint16_t         *s_hs[MD_RAS_HS_LINES / MD_RAS_HS_PIECE];
static int               s_hs_ok;
/* the frame being run's: its lines up to s_hs_upto copied so far; swapped
 * with s_hs when the frame is published */
static uint16_t         *s_hs_run[MD_RAS_HS_LINES / MD_RAS_HS_PIECE];
static int               s_hs_run_ok;
static int               s_hs_upto;
/* the bytes of the table the scroll mode reads, while the frame's lines are
 * shown, for the VRAM write path (gwenesis_vdp_mem.c); 0x80000000 when it is
 * not to be watched */
unsigned int             md_ras_hs_lo = 0x80000000u;
unsigned int             md_ras_hs_len;
static volatile int      s_hs_busy;     /* a pass draws from it */
static volatile uint32_t s_pub_seq, s_took_seq;
uint16_t *const *md_vr_hs;              /* the renderer's: s_hs, or NULL */
static int               s_rec_k;       /* the one the 68000 writes */
static volatile int      s_pub = -1;    /* the last whole frame's, -1: none */
extern uint32_t         *md_sync; /* [2]: the lock, in memory with atomics (md_main.c) */
#define s_lock (((volatile uint32_t *)md_sync)[2])

/* the drawer's copy of the state a line is drawn with; the writes it reads
 * from the published record, which nothing writes while a pass draws */
typedef struct {
    uint8_t  regs[MD_RAS_REGS];
    uint16_t vsram[MD_RAS_VSRAM];
} md_ras_copy_t;
static md_ras_copy_t   *s_cp;
static const md_ras_t  *s_cp_rec;
static uint16_t        *s_cram565; /* NULL: palette writes are not followed */
static int              s_ev_n, s_ev_at;

int md_raster_on;

/*
 * What the heap has left is a few kilobytes in small holes, and every block
 * taken is one of the process's 32 resources - the machine's pages are about
 * 21 of them.  A block apiece came to 34 once the lines met, the scroll tables
 * were refused, and frames with raster effects were drawn torn between two
 * frames (he saw it on the CYD, 1 Nov 2026); one block for all of it found no
 * hole that large.  So the largest holes there are, taken whole and cut here.
 */
static void *(*s_alloc)(size_t);
static uint8_t *s_cut, *s_cut_end;
unsigned md_raster_blocks, md_raster_bytes; /* what it got (the stats) */
static void *s_blk[24]; /* the blocks it was cut from, for md_raster_free */

static void *carve(size_t n)
{
    static const uint16_t k_sizes[] = {2048, 1536, 1024, 768, 640, 512, 384, 256, 192, 128, 64};
    n = (n + 3u) & ~(size_t)3u;
    if (s_cut == NULL || (size_t)(s_cut_end - s_cut) < n) {
        s_cut = NULL;
        for (unsigned i = 0; i < sizeof(k_sizes) / sizeof(k_sizes[0]); i++) {
            if (k_sizes[i] < n) {
                break;
            }
            uint8_t *const b = (uint8_t *)s_alloc(k_sizes[i]);
            if (b != NULL) {
                s_cut = b;
                s_cut_end = b + k_sizes[i];
                if (md_raster_blocks < sizeof(s_blk) / sizeof(s_blk[0])) {
                    s_blk[md_raster_blocks] = b;
                }
                md_raster_blocks++;
                md_raster_bytes += k_sizes[i];
                break;
            }
        }
        if (s_cut == NULL) {
            return NULL;
        }
    }
    void *const p = s_cut;
    s_cut += n;
    return p;
}

/* a record's next piece of writes, for both records at once */
static int add_piece(unsigned pieces)
{
    uint32_t *const a = (uint32_t *)carve(2u * MD_RAS_PIECE * sizeof(uint32_t));
    if (a == NULL) {
        return 0;
    }
    s_rec[0]->ev[pieces] = a;
    s_rec[1]->ev[pieces] = a + MD_RAS_PIECE;
    return 1;
}

/* a scroll table in the renderer's 16-line pieces; 0 if one did not come */
static int alloc_hs(uint16_t **t)
{
    for (int i = 0; i < MD_RAS_HS_LINES / MD_RAS_HS_PIECE; i++) {
        t[i] = (uint16_t *)carve(MD_RAS_HS_PIECE * 2u * sizeof(uint16_t));
        if (t[i] == NULL) {
            return 0;
        }
    }
    return 1;
}

/*
 * In the order it buys most: both records, the drawer's copy and a piece of
 * writes (without them nothing is followed), the scroll tables, the palette,
 * and only then more pieces.
 */
/*
 * Everything it took given back, for another cartridge: the CYD's game menu
 * needs the memory to mount the card again.  The drawer is between passes.
 */
void md_raster_free(void (*release)(void *))
{
    md_raster_on = 0;
    md_ras_hs_lo = 0x80000000u;
    md_vr_hs = NULL;
    for (unsigned i = 0; i < md_raster_blocks && i < sizeof(s_blk) / sizeof(s_blk[0]); i++) {
        release(s_blk[i]);
        s_blk[i] = NULL;
    }
    md_raster_blocks = md_raster_bytes = 0;
    s_cut = s_cut_end = NULL;
    s_rec[0] = s_rec[1] = NULL;
    s_cp = NULL;
    s_cp_rec = NULL;
    s_cram565 = NULL;
    s_hs_ok = s_hs_run_ok = 0;
    s_cap = 0;
    s_pub = -1;
    s_pub_seq = s_took_seq = 0;
    s_hs_busy = 0;
    s_rec_k = 0;
    md_vr_regs = gwenesis_vdp_regs;
    md_vr_vsram = VSRAM;
    md_vr_cram565 = CRAM565;
}

int md_raster_init(void *(*alloc)(size_t))
{
    s_alloc = alloc;
    /* in the order that wastes least of 256-byte holes, which is what the
     * CYD has left (six of them, measured 1 Nov 2026): a record and the copy
     * share one, the other record leaves room for two pieces of a table */
    s_rec[0] = (md_ras_t *)carve(sizeof(md_ras_t));
    s_cp = (md_ras_copy_t *)carve(sizeof(md_ras_copy_t));
    s_rec[1] = (md_ras_t *)carve(sizeof(md_ras_t));
    s_hs_ok = alloc_hs(s_hs); /* its first pieces in what the record left */
    if (s_rec[0] == NULL || s_rec[1] == NULL || s_cp == NULL || !add_piece(0)) {
        return 0;
    }
    unsigned pieces = 1;
    s_hs_run_ok = s_hs_ok && alloc_hs(s_hs_run);
    s_cram565 = (uint16_t *)carve(MD_RAS_CRAM * 4 * sizeof(uint16_t));
    while (pieces < MD_RAS_PIECES && add_piece(pieces)) {
        pieces++;
    }
    s_cap = pieces * MD_RAS_PIECE;
    s_rec[0]->n = s_rec[1]->n = 0;
    return 1;
}

static void lock(void)
{
    while (__atomic_exchange_n((uint32_t *)&s_lock, 1u, __ATOMIC_ACQUIRE) != 0u) {
    }
}

static void unlock(void)
{
    __atomic_store_n((uint32_t *)&s_lock, 0u, __ATOMIC_RELEASE);
}

/* the VDP's 9-bit colour as the renderer keeps it (gwenesis_vdp_mem.c) */
static inline uint16_t to565(uint16_t v)
{
    return (uint16_t)(((v & 0xe00u) >> 7) | ((v & 0x0e0u) << 3) | ((v & 0x00eu) << 12));
}

/*
 * Watch only the bytes the scroll mode reads: 4 for the whole screen, 32 for
 * the first eight lines, a line's worth each for cells and lines - not the
 * writes that land beside a 4-byte table.
 */
static void hs_watch(void)
{
    const int mode = gwenesis_vdp_regs[11] & 3;
    md_ras_hs_lo = (unsigned)(gwenesis_vdp_regs[13] & 0x3f) << 10;
    md_ras_hs_len = (mode == 0) ? 4u : (mode == 1) ? 32u : (unsigned)screen_height * 4u;
}

void md_raster_begin(void)
{
    md_ras_t *r = s_rec[s_rec_k];
    memcpy(r->regs, gwenesis_vdp_regs, sizeof(r->regs));
    memcpy(r->vsram, VSRAM, sizeof(r->vsram));
    r->n = 0;
    r->cram = 0;
    s_hs_upto = 0;
    if (s_hs_run_ok) {
        hs_watch();
    } else {
        md_ras_hs_lo = 0x80000000u;
    }
}

/*
 * The registers a line is drawn with: display on (1), the plane, window,
 * sprite and scroll tables (2-5, 13), the backdrop (7), the scroll modes (11),
 * the width and shadow (12), the plane size and the window (16-18).  The
 * rest - the line interrupt's counter (10), DMA (15, 19-23) - change nothing
 * drawn and are not noted: every noted line is a gate both cores wait at.
 */
#define MD_RAS_DRAWN 0x000738beu

void md_raster_note(int kind, int index)
{
    if (kind == MD_RAS_REG && !((MD_RAS_DRAWN >> (index & 31)) & 1u)) {
        return;
    }
    if (kind == MD_RAS_REG && (index == 11 || index == 13) && md_ras_hs_lo != 0x80000000u) {
        hs_watch(); /* the table moved, or the mode reads another part of it */
    }
    /*
     * Its line: the one being shown when it came.  Past the horizontal blank
     * (860 clocks into the line, gwenesis' lines start with it) the line's
     * picture has started, and a write counts from the next line - Road
     * Rash II's band changes come ~1900 clocks in (md_main.c, run_frame).
     */
    extern int m68k_cycles_master(void);
    int line = scan_line;
    if (m68k_cycles_master() - scan_line * VDP_CYCLES_PER_LINE > MD_RAS_HBLANK) {
        line++;
    }
    if (line < 0 || line >= screen_height) {
        return; /* between frames: the next one's copy has it */
    }
    md_ras_t *r = s_rec[s_rec_k];
    if (r->n >= s_cap) {
        r->n = (uint16_t)(s_cap + 1u);
        return;
    }
    uint32_t v;
    if (kind == MD_RAS_REG) {
        v = gwenesis_vdp_regs[index & 0x1f];
    } else if (kind == MD_RAS_VS) {
        v = VSRAM[index & 0x3f];
    } else {
        v = CRAM[index & 0x3f];
        r->cram++;
    }
#ifdef MD_HOST_RASDUMP
    { extern int m68k_cycles_master(void); extern int md_dbg_off[2][MD_RAS_EVENTS];
      md_dbg_off[s_rec_k][r->n] = m68k_cycles_master() - scan_line * 3420; }
#endif
    MD_RAS_EV(r, r->n) = ((uint32_t)line << 20) | ((uint32_t)kind << 18) |
                    ((uint32_t)(index & 0x3f) << 12) | (v & 0xfffu);
    r->n++;
}

/* events a record holds (the stats) */
unsigned md_raster_cap(void)
{
    return s_cap;
}

/* how the scroll table is followed (the stats): "as written" with the
 * frame's own copy, "at the end" without it, "not" without either */
const char *md_raster_hs_mode(void)
{
    return s_hs_run_ok ? "as written" : s_hs_ok ? "at the end" : "not";
}

/* frames recorded, and frames with too many writes to follow (the stats) */
unsigned md_raster_frames, md_raster_over;
/* the frame's end: nothing - it was published when its last line was shown */
void md_raster_end(void)
{
}

#define MD_RAS_RD16(A) (VRAM_RD8((A) + 1) | (VRAM_RD8(A) << 8))

/* a line's two horizontal scroll words, as the VDP reads them now, into o */
static inline void hs_words(int l, uint16_t *o)
{
    const int      mode = gwenesis_vdp_regs[11] & 3;
    const unsigned table = (unsigned)(gwenesis_vdp_regs[13] & 0x3f) << 10;
    const int idx = (mode == 0) ? 0 : (mode == 1) ? (l & 7) : (mode == 2) ? (l & ~7) : l;
    const unsigned a = (table + (unsigned)idx * 4u) & 0xffffu;
    o[0] = (uint16_t)MD_RAS_RD16(a);
    o[1] = (uint16_t)MD_RAS_RD16((a + 2u) & 0xffffu);
}

/* the frame's lines from s_hs_upto up to `to`, from the table as it is */
static void hs_upto(int to)
{
    const int h = (screen_height < MD_RAS_HS_LINES) ? screen_height : MD_RAS_HS_LINES;
    if (to > h) {
        to = h;
    }
    for (int l = s_hs_upto; l < to; l++) {
        hs_words(l, &s_hs_run[l / MD_RAS_HS_PIECE][(l % MD_RAS_HS_PIECE) * 2]);
    }
    if (to > s_hs_upto) {
        s_hs_upto = to;
    }
}

/*
 * A write into the scroll table is coming while lines are shown: the lines
 * already shown, and the one being shown if its picture has started, keep
 * the table as it was.
 */
void md_raster_hs_write(void)
{
    extern int m68k_cycles_master(void);
    int line = scan_line;
    if (m68k_cycles_master() - scan_line * VDP_CYCLES_PER_LINE > MD_RAS_HBLANK) {
        line++;
    }
    hs_upto(line);
}

/*
 * The last visible line has run: the horizontal scroll table copied before
 * the vertical blank rewrites it, and the frame published - unless a pass is
 * drawing from the copy, when it is let go.
 */
void md_raster_visible(void)
{
    md_ras_t *const r = s_rec[s_rec_k];
    md_ras_hs_lo = 0x80000000u; /* the vertical blank's writes are the next frame's */
    md_raster_frames++;
    if (r->n > s_cap) {
        md_raster_over++;
    }
    lock();
    if (s_hs_busy) {
        unlock();
        return;
    }
    if (!s_hs_ok || screen_height > MD_RAS_HS_LINES) {
        /*
         * No copy of the scroll table: the record's registers with the
         * table the vertical blank has rewritten for the next frame is a
         * picture torn between two frames (he saw it on the CYD, 1 Nov 2026).
         */
        s_pub = -1;
    } else {
        /*
         * More writes than the record holds - Gunstar Heroes' SEGA, one
         * scroll a line: the lines past the last one held keep its state.
         * Drawn from the live VDP instead, they were the next frame's lines
         * half-run on the other core, torn (he saw it, 1 Nov 2026).
         */
        if (r->n > s_cap) {
            r->n = (uint16_t)s_cap;
        }
        if (s_hs_run_ok) {
            hs_upto(MD_RAS_HS_LINES);
            for (int i = 0; i < MD_RAS_HS_LINES / MD_RAS_HS_PIECE; i++) {
                uint16_t *const t = s_hs[i];
                s_hs[i] = s_hs_run[i];
                s_hs_run[i] = t;
            }
        } else if (s_hs_ok) {
            const int h = (screen_height < MD_RAS_HS_LINES) ? screen_height : MD_RAS_HS_LINES;
            for (int l = 0; l < h; l++) {
                hs_words(l, &s_hs[l / MD_RAS_HS_PIECE][(l % MD_RAS_HS_PIECE) * 2]);
            }
        }
        s_pub = s_rec_k;
        s_rec_k ^= 1;
        s_pub_seq++;
    }
    unlock();
}

int md_raster_take(void)
{
    int cram = 0;
    lock();
    const int k = s_pub;
    if (k >= 0 && s_pub_seq == s_took_seq) {
        unlock();
        return 2; /* drawn already: wait for the next frame */
    }
    if (k >= 0) {
        s_took_seq = s_pub_seq;
        s_hs_busy = 1; /* the record and the scroll copy stay as they are */
        const md_ras_t *r = s_rec[k];
        memcpy(s_cp->regs, r->regs, sizeof(s_cp->regs));
        memcpy(s_cp->vsram, r->vsram, sizeof(s_cp->vsram));
        s_cp_rec = r;
        s_ev_n = r->n;
        cram = r->cram;
    }
    unlock();
    if (k < 0) {
        return 0;
    }
    s_ev_at = 0;
    if (s_hs_ok) {
        md_vr_hs = s_hs;
    }
    md_vr_regs = s_cp->regs;
    md_vr_vsram = s_cp->vsram;
    if (cram && s_cram565 != NULL) {
        memcpy(s_cram565, CRAM565, MD_RAS_CRAM * 4 * sizeof(uint16_t));
        md_vr_cram565 = s_cram565;
    }
    return 1;
}

void md_raster_line(int line)
{
    while (s_ev_at < s_ev_n && (int)(MD_RAS_EV(s_cp_rec, s_ev_at) >> 20) <= line) {
        const uint32_t e = MD_RAS_EV(s_cp_rec, s_ev_at);
        s_ev_at++;
        const int      kind = (int)((e >> 18) & 3u);
        const int      i = (int)((e >> 12) & 0x3fu);
        const uint16_t v = (uint16_t)(e & 0xfffu);
        if (kind == MD_RAS_REG) {
            if (i < MD_RAS_REGS) {
                s_cp->regs[i] = (uint8_t)v;
            }
        } else if (kind == MD_RAS_VS) {
            if (i < MD_RAS_VSRAM) {
                s_cp->vsram[i] = v;
            }
        } else if (md_vr_cram565 == s_cram565) {
            const uint16_t c = to565(v);
            s_cram565[i] = s_cram565[0x40 + i] = s_cram565[0x80 + i] = s_cram565[0xc0 + i] = c;
        }
    }
}

/* the line of the next write the pass has not put in yet; past the
 * screen when there is none */
int md_raster_next(void)
{
    return (s_ev_at < s_ev_n) ? (int)(MD_RAS_EV(s_cp_rec, s_ev_at) >> 20) : 0x7fffffff;
}

void md_raster_done(void)
{
    md_vr_hs = NULL;
    __atomic_store_n(&s_hs_busy, 0, __ATOMIC_RELEASE);
    md_vr_regs = gwenesis_vdp_regs;
    md_vr_vsram = VSRAM;
    md_vr_cram565 = CRAM565;
}

#ifdef MD_HOST_RASDUMP
#include <stdio.h>
/* host harness: the record just finished, one event a line */
int md_dbg_off[2][MD_RAS_EVENTS];
void md_raster_dump(int frame)
{
    const md_ras_t *r = s_rec[s_rec_k ^ 1];
    printf("frame %d: %u writes\n", frame, (unsigned)r->n);
    for (unsigned i = 0; i < r->n && i < s_cap; i++) {
        const uint32_t e = MD_RAS_EV(r, i);
        printf("  line %3u +%4d %s %2u = %03x\n", (unsigned)(e >> 20), md_dbg_off[s_rec_k ^ 1][i],
               ((e >> 18) & 3u) == 0 ? "reg  " : ((e >> 18) & 3u) == 1 ? "vsram" : "cram ",
               (unsigned)((e >> 12) & 0x3fu), (unsigned)(e & 0xfffu));
    }
}
#endif

#endif /* ARGON_MD_SMALL */
