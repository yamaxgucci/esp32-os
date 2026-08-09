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

/* Visible lines: 224 normally, 240 when the VDP is put in 240-line mode. */
#define VISIBLE_LINES_NTSC 224
#define VISIBLE_LINES_PAL 240

/*
 * Mirror of AG_HOT_RODATA from argon.h.  Do not #include argon.h from this
 * forced header: vendored TUs (e.g. gwenesis_vdp_gfx.c) typedef their own
 * uint32_t and conflict with stdint pulled in by the SDK.
 */
#define AG_HOT_RODATA __attribute__((used, section(".hot_rodata")))

#endif /* ARGON_MD_CFG_H */
