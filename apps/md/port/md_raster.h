/*
 * ArgonOS Mega Drive - the VDP writes a frame made while its lines were
 * shown, for the CYD's picture drawn afterwards; see md_raster.c.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef MD_RASTER_H
#define MD_RASTER_H

#define MD_RAS_REG 0
#define MD_RAS_VS  1
#define MD_RAS_CR  2

#define MD_RAS_REGS  24
#define MD_RAS_VSRAM 40
#define MD_RAS_CRAM  64

#include <stddef.h>

/* Its memory, from `alloc` once the machine's pages are cut; 0 if short. */
int md_raster_init(void *(*alloc)(size_t));

/* The 68000's side (the frame loop and the VDP's write paths). */
extern int md_raster_on;
void md_raster_begin(void);
void md_raster_note(int kind, int index);
/* the scroll table's bytes the mode reads while lines are shown (0x80000000:
 * none), and a write into them coming (gwenesis_vdp_mem.c) */
extern unsigned int md_ras_hs_lo, md_ras_hs_len;
void md_raster_hs_write(void);
/* `bytes` of VRAM from `start` about to be written: a data port write, or a
 * whole DMA checked once before its loop rather than at every byte of it */
#define MD_RAS_VRAM(start, bytes)                                            \
    do {                                                                     \
        const unsigned lo_ = md_ras_hs_lo;                                   \
        if (lo_ != 0x80000000u &&                                            \
            ((((unsigned)(start) - lo_) & 0xffffu) < md_ras_hs_len ||        \
             ((lo_ - (unsigned)(start)) & 0xffffu) < (unsigned)(bytes))) {   \
            md_raster_hs_write();                                            \
        }                                                                    \
    } while (0)
void md_raster_visible(void); /* the last visible line has run */
void md_raster_end(void);
unsigned md_raster_cap(void); /* writes a record holds */
const char *md_raster_hs_mode(void); /* how the scroll table is followed */

/* The drawer's: a pass on the last whole frame's record, line by line. */
/* 1: the pass draws from the record; 0: from the live VDP; 2: the last
 * frame published was drawn already - wait for the next */
int  md_raster_take(void);
void md_raster_line(int line); /* the writes up to `line` put in */
int  md_raster_next(void);     /* the next write's line */
void md_raster_done(void);

/* a write while a line is shown, noted (gwenesis_vdp_mem.c) */
#define MD_VDP_NOTE(kind, index)                                             \
    do {                                                                     \
        if (md_raster_on) {                                                  \
            md_raster_note((kind), (index));                                 \
        }                                                                    \
    } while (0)

#endif /* MD_RASTER_H */
