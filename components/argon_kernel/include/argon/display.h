/*
 * ArgonOS - local display (soft framebuffer for QEMU; panel drivers later).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_DISPLAY_H
#define ARGON_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include <argon/abi.h>
#include <argon/screen.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Allocates the soft RGB565 framebuffer (PSRAM), registers `fb0`, and wires
 * the gfx ABI.  Size comes from BOARD.CFG `[display]`; default 320x240 soft.
 * A driver of `none` or a zero size skips the device (headless).
 */
ag_err_t ag_display_init(void);

bool ag_display_ready(void);

/* True while an application holds the framebuffer (text blit is suspended). */
bool ag_display_acquired(void);

/* The gfx subtable wired into ag_api_t.  acquire fails until init succeeds. */
extern const ag_gfx_api_t ag_gfx_api_table;

/*
 * When the soft display is local and not acquired, blit the virtual text
 * screen into the framebuffer.  Called from the console tick.
 */
void ag_display_render_console(const ag_screen_t *screen);

/*
 * Write a framebuffer as a binary PPM (P6) to `path`.
 * `cwd` is for a relative path (same convention as ag_vfs_open).
 * When `live` is false, prefer the last released graphics snapshot if one
 * exists - that is what `gfxdump` after `run` wants.  When true, dump the
 * live soft buffer (text console after a blit, or the acquired frame).
 */
ag_err_t ag_display_dump_ppm(const char *path, const char *cwd, bool live);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_DISPLAY_H */
