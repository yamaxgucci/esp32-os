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
void md_raster_end(void);

/* The drawer's: a pass on the last whole frame's record, line by line. */
int  md_raster_take(void); /* 1: the pass draws from the record */
void md_raster_line(int line);
void md_raster_done(void);

/* a write while a line is shown, noted (gwenesis_vdp_mem.c) */
#define MD_VDP_NOTE(kind, index)                                             \
    do {                                                                     \
        if (md_raster_on) {                                                  \
            md_raster_note((kind), (index));                                 \
        }                                                                    \
    } while (0)

#endif /* MD_RASTER_H */
