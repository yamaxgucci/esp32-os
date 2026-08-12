/*
 * ArgonOS - local display (soft framebuffer for QEMU; panel drivers later).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
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
 * the gfx ABI.  Size comes from BOARD.CFG `[display]`; default 640x400 soft
 * (80×25 text cells at 8×16).
 * Under Espressif QEMU, also attaches the virtual RGB panel so `gfx_flush`
 * and console blits appear in an SDL window (`argon run -Gfx`).
 * A driver of `none` or a zero size skips the device (headless).
 */
ag_err_t ag_display_init(void);

bool ag_display_ready(void);

/* True while an application holds the framebuffer (text blit is suspended). */
bool ag_display_acquired(void);

/* Pid that called acquire, or AG_PID_KERNEL when free. */
ag_pid_t ag_display_owner(void);

/*
 * Force-release graphics (focus lost / break-in).  Keeps the snapshot for
 * Alt-Tab preview.  Safe when not acquired.
 */
void ag_display_force_release(void);

/* Present the last snap with a one-line label (Alt-Tab overlay). */
void ag_display_show_overlay(const char *label);

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
