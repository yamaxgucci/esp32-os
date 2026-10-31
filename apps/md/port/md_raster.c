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
 * Two records: the 68000 writes one while the drawer may be copying the
 * other.  A frame with more writes than a record holds is drawn as before.
 *
 * The horizontal scroll table is in VRAM, which the game rewrites in the
 * vertical blank for the next frame - the pass, drawn after it, gave each
 * band of Road Rash II's road this frame's height and the next frame's curve,
 * and the horizon shook (31 Oct 2026).  So the 68000 copies the table when
 * the last visible line has run (md_raster_visible), before the vertical
 * blank's DMA, and publishes the frame then.  There is one copy: while a pass
 * draws from it the frames that finish are not published, and the drawer
 * waits for the next one rather than draw the same frame again.
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

#define MD_RAS_EVENTS 32

typedef struct {
    uint8_t  regs[MD_RAS_REGS];
    uint16_t vsram[MD_RAS_VSRAM];
    uint16_t n;    /* events; MD_RAS_EVENTS + 1 once it overflowed */
    uint16_t cram; /* palette writes among them */
    /* line << 20 | kind << 18 | index << 12 | value */
    uint32_t ev[MD_RAS_EVENTS];
} md_ras_t;

static md_ras_t         *s_rec[2];

/* the horizontal scroll a line of the published frame was drawn with, plane
 * A and B, in 32-line pieces (the heap's leftovers are small) */
#define MD_RAS_HS_LINES 256
#define MD_RAS_HS_PIECE 32
static uint16_t         *s_hs[MD_RAS_HS_LINES / MD_RAS_HS_PIECE];
static int               s_hs_ok;
static volatile int      s_hs_busy;     /* a pass draws from it */
static volatile uint32_t s_pub_seq, s_took_seq;
uint16_t *const *md_vr_hs;              /* the renderer's: s_hs, or NULL */
static int               s_rec_k;       /* the one the 68000 writes */
static volatile int      s_pub = -1;    /* the last whole frame's, -1: none */
static volatile uint32_t s_lock;

/* the drawer's copy */
typedef struct {
    uint8_t  regs[MD_RAS_REGS];
    uint16_t vsram[MD_RAS_VSRAM];
    uint32_t ev[MD_RAS_EVENTS];
} md_ras_copy_t;
static md_ras_copy_t *s_cp;
static uint16_t      *s_cram565; /* NULL: palette writes are not followed */
static int            s_ev_n, s_ev_at;

int md_raster_on;

int md_raster_init(void *(*alloc)(size_t))
{
    s_rec[0] = (md_ras_t *)alloc(sizeof(md_ras_t));
    s_rec[1] = (md_ras_t *)alloc(sizeof(md_ras_t));
    s_cp = (md_ras_copy_t *)alloc(sizeof(md_ras_copy_t));
    s_cram565 = (uint16_t *)alloc(MD_RAS_CRAM * 4 * sizeof(uint16_t));
    if (s_rec[0] == NULL || s_rec[1] == NULL || s_cp == NULL) {
        return 0;
    }
    s_hs_ok = 1;
    for (int i = 0; i < MD_RAS_HS_LINES / MD_RAS_HS_PIECE; i++) {
        s_hs[i] = (uint16_t *)alloc(MD_RAS_HS_PIECE * 2 * sizeof(uint16_t));
        if (s_hs[i] == NULL) {
            s_hs_ok = 0;
        }
    }
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

void md_raster_begin(void)
{
    md_ras_t *r = s_rec[s_rec_k];
    memcpy(r->regs, gwenesis_vdp_regs, sizeof(r->regs));
    memcpy(r->vsram, VSRAM, sizeof(r->vsram));
    r->n = 0;
    r->cram = 0;
}

/*
 * The registers a line is drawn with: display on (1), the plane, window,
 * sprite and scroll tables (2-5, 13), the backdrop (7), the scroll modes (11),
 * the width and shadow (12), the plane size and the window (16-18).  The
 * rest - the line interrupt's counter (10), DMA (15, 19-23) - change nothing
 * drawn, and a pass with writes to note is drawn by one core instead of two.
 */
#define MD_RAS_DRAWN 0x000738beu

void md_raster_note(int kind, int index)
{
    if (kind == MD_RAS_REG && !((MD_RAS_DRAWN >> (index & 31)) & 1u)) {
        return;
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
    if (r->n >= MD_RAS_EVENTS) {
        r->n = MD_RAS_EVENTS + 1;
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
    r->ev[r->n++] = ((uint32_t)line << 20) | ((uint32_t)kind << 18) |
                    ((uint32_t)(index & 0x3f) << 12) | (v & 0xfffu);
}

/* frames recorded, and frames with too many writes to follow (the stats) */
unsigned md_raster_frames, md_raster_over;
/* the frame's end: nothing - it was published when its last line was shown */
void md_raster_end(void)
{
}

#define MD_RAS_RD16(A) (VRAM_RD8((A) + 1) | (VRAM_RD8(A) << 8))

/*
 * The last visible line has run: the horizontal scroll table copied before
 * the vertical blank rewrites it, and the frame published - unless a pass is
 * drawing from the copy, when it is let go.
 */
void md_raster_visible(void)
{
    const md_ras_t *r = s_rec[s_rec_k];
    md_raster_frames++;
    if (r->n > MD_RAS_EVENTS) {
        md_raster_over++;
    }
    lock();
    if (s_hs_busy) {
        unlock();
        return;
    }
    if (r->n == 0 || r->n > MD_RAS_EVENTS) {
        s_pub = -1; /* nothing to draw differently, or too much to */
    } else {
        if (s_hs_ok) {
            const int      mode = gwenesis_vdp_regs[11] & 3;
            const unsigned table = (unsigned)(gwenesis_vdp_regs[13] & 0x3f) << 10;
            const int      h = (screen_height < MD_RAS_HS_LINES) ? screen_height : MD_RAS_HS_LINES;
            for (int l = 0; l < h; l++) {
                const int idx = (mode == 0) ? 0 : (mode == 1) ? (l & 7) : (mode == 2) ? (l & ~7) : l;
                const unsigned a = (table + (unsigned)idx * 4u) & 0xffffu;
                uint16_t *const o = &s_hs[l / MD_RAS_HS_PIECE][(l % MD_RAS_HS_PIECE) * 2];
                o[0] = (uint16_t)MD_RAS_RD16(a);
                o[1] = (uint16_t)MD_RAS_RD16((a + 2u) & 0xffffu);
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
        s_hs_busy = s_hs_ok;
        const md_ras_t *r = s_rec[k];
        memcpy(s_cp->regs, r->regs, sizeof(s_cp->regs));
        memcpy(s_cp->vsram, r->vsram, sizeof(s_cp->vsram));
        memcpy(s_cp->ev, r->ev, (size_t)r->n * sizeof(s_cp->ev[0]));
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
    while (s_ev_at < s_ev_n && (int)(s_cp->ev[s_ev_at] >> 20) <= line) {
        const uint32_t e = s_cp->ev[s_ev_at++];
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
    for (unsigned i = 0; i < r->n && i < MD_RAS_EVENTS; i++) {
        const uint32_t e = r->ev[i];
        printf("  line %3u +%4d %s %2u = %03x\n", (unsigned)(e >> 20), md_dbg_off[s_rec_k ^ 1][i],
               ((e >> 18) & 3u) == 0 ? "reg  " : ((e >> 18) & 3u) == 1 ? "vsram" : "cram ",
               (unsigned)((e >> 12) & 0x3fu), (unsigned)(e & 0xfffu));
    }
}
#endif

#endif /* ARGON_MD_SMALL */
