/*
 * ArgonOS file manager - cell UI backend (text console or soft gfx).
 *
 * Drawing and modal chrome go through these hooks so fm.c / fmops.c stay shared
 * between the built-in text `fm` and GFXFM.AXE (80×25 cells → 640×400 pixels).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_FM_UI_H
#define ARGON_FM_UI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void fm_ui_begin(void);
void fm_ui_end(void);
void fm_ui_cls(void);
void fm_ui_poke(int x, int y, char ch, uint8_t attr);
void fm_ui_fill(int x, int y, int w, int h, char ch, uint8_t attr);
/* Box chrome: text backend uses +/-/|; gfx fills the interior and strokes edges. */
void fm_ui_frame(int x, int y, int w, int h, uint8_t attr);
void fm_ui_present(void);
void fm_ui_set_cursor_visible(bool on);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_FM_UI_H */
