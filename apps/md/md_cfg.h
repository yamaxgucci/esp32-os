/*
 * ArgonOS - Mega Drive port configuration.
 *
 * gwenesis is a library of cores with no frame loop and no platform layer: the
 * embedding target picks the rendering path and owns the buffers.  This header
 * is forced into every translation unit of the app (-include), which is how the
 * vendored core learns it is being built for ArgonOS.
 *
 * Timings and geometry the core already defines for itself (VDP_CYCLES_PER_LINE,
 * SCREEN_WIDTH, LINES_PER_FRAME_*) are deliberately not repeated here.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_MD_CFG_H
#define ARGON_MD_CFG_H

/*
 * Selects gwenesis' embedded rendering path - the one that writes RGB565 through
 * CRAM565 straight into the caller's buffer, two pixels per 32-bit store.  The
 * other path emits 8-bit palette indices for a host to convert, which would cost
 * a second pass over every pixel.  Upstream gates this on the Game & Watch
 * targets; the vendored copy also accepts ARGON_TARGET, and the same macro picks
 * the pointer form of ROM_DATA.  See README, "Local changes".
 */
#define ARGON_TARGET 1

/*
 * MD_SPLIT: the image is built in two code parts (mkaxe --split-hot).  The
 * handlers a game actually runs (md_hot.h, from a measured session) and the
 * 68000's loop go in the hot part, which the loader puts in internal memory
 * as [memory] app.code says; the rest, and the 128 KB decode tables, stay in
 * flash.  On the CYD a busy scene paid ~1000 cycles an instruction in flash
 * cache misses.
 */
#ifdef MD_SPLIT
#define MD_HOT __attribute__((section(".hot.text")))
#define MD_TABLE_SECTION ".cold_rodata"
#else
#define MD_HOT
#define MD_TABLE_SECTION ".hot_rodata"
#endif

/*
 * -DMD_VDP_HOT puts the VDP's line renderer (9.5 KB with all it inlines) in
 * the hot part too.  Measured on Sonic, 29 Oct 2026, with the 68000's
 * handlers moved out to make room (-DMD_68K_COLD): 8.3-9.4 whole passes a
 * second against 7.9-8.9 with the renderer in flash, the system core 40-50%
 * busy either way - the wire, not the renderer, sets the pace - and the
 * emulation fell from 43-51 fps to 34-39.  With both hot VRAM is short.
 */
#if defined(MD_SPLIT) && defined(MD_VDP_HOT)
#define MD_HOT_VDP MD_HOT
#else
#define MD_HOT_VDP
#endif

/* Visible lines: 224 normally, 240 when the VDP is put in 240-line mode. */
#define VISIBLE_LINES_NTSC 224
#define VISIBLE_LINES_PAL 240

#endif /* ARGON_MD_CFG_H */
