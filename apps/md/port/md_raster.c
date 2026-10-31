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
 * So every write to the registers, the vertical scroll and the palette while
 * a visible line runs is written down with its line.  A pass starts from a
 * copy of the VDP as it is (the state the picture's VRAM goes with: a pass
 * is drawn after the frame, from the VRAM its vertical blank left) and puts
 * in the last whole frame's writes as its lines reach them - so each band of
 * the road gets its own scroll, a frame old.
 *
 * Two records: the 68000 writes one while the drawer may be copying the
 * other.  A frame with more writes than a record holds is drawn as before.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>
#include <string.h>

#include "gwenesis_vdp.h"
#include "md_raster.h"

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

#define MD_RAS_EVENTS 48

typedef struct {
    uint16_t n; /* events; MD_RAS_EVENTS + 1 once it overflowed */
    /* line << 20 | kind << 18 | index << 12 | value */
    uint32_t ev[MD_RAS_EVENTS];
} md_ras_t;

static md_ras_t          s_rec[2];
static int               s_rec_k;       /* the one the 68000 writes */
static volatile int      s_pub = -1;    /* the last whole frame's, -1: none */
static volatile uint32_t s_lock;

/* the drawer's copy */
static uint8_t  s_regs[MD_RAS_REGS];
static uint16_t s_vsram[MD_RAS_VSRAM];
static uint16_t s_cram565[MD_RAS_CRAM * 4];
static uint32_t s_ev[MD_RAS_EVENTS];
static int      s_ev_n, s_ev_at;

int md_raster_on;

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
    s_rec[s_rec_k].n = 0;
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
    if (scan_line < 0 || scan_line >= screen_height) {
        return; /* between frames: the next one's copy has it */
    }
    md_ras_t *r = &s_rec[s_rec_k];
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
    }
    r->ev[r->n++] = ((uint32_t)scan_line << 20) | ((uint32_t)kind << 18) |
                    ((uint32_t)(index & 0x3f) << 12) | (v & 0xfffu);
}

void md_raster_end(void)
{
    const md_ras_t *r = &s_rec[s_rec_k];
    lock();
    if (r->n == 0 || r->n > MD_RAS_EVENTS) {
        s_pub = -1; /* nothing to draw differently, or too much to */
    } else {
        s_pub = s_rec_k;
        s_rec_k ^= 1;
    }
    unlock();
}

int md_raster_take(void)
{
    lock();
    const int k = s_pub;
    if (k >= 0) {
        const md_ras_t *r = &s_rec[k];
        memcpy(s_ev, r->ev, (size_t)r->n * sizeof(s_ev[0]));
        s_ev_n = r->n;
    }
    unlock();
    if (k < 0) {
        return 0;
    }
    memcpy(s_regs, gwenesis_vdp_regs, sizeof(s_regs));
    memcpy(s_vsram, VSRAM, sizeof(s_vsram));
    memcpy(s_cram565, CRAM565, sizeof(s_cram565));
    s_ev_at = 0;
    md_vr_regs = s_regs;
    md_vr_vsram = s_vsram;
    md_vr_cram565 = s_cram565;
    return 1;
}

void md_raster_line(int line)
{
    while (s_ev_at < s_ev_n && (int)(s_ev[s_ev_at] >> 20) <= line) {
        const uint32_t e = s_ev[s_ev_at++];
        const int      kind = (int)((e >> 18) & 3u);
        const int      i = (int)((e >> 12) & 0x3fu);
        const uint16_t v = (uint16_t)(e & 0xfffu);
        if (kind == MD_RAS_REG) {
            if (i < MD_RAS_REGS) {
                s_regs[i] = (uint8_t)v;
            }
        } else if (kind == MD_RAS_VS) {
            if (i < MD_RAS_VSRAM) {
                s_vsram[i] = v;
            }
        } else {
            const uint16_t c = to565(v);
            s_cram565[i] = s_cram565[0x40 + i] = s_cram565[0x80 + i] = s_cram565[0xc0 + i] = c;
        }
    }
}

void md_raster_done(void)
{
    md_vr_regs = gwenesis_vdp_regs;
    md_vr_vsram = VSRAM;
    md_vr_cram565 = CRAM565;
}
