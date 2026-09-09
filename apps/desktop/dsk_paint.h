/*
 * ArgonOS DESKTOP - the painter, and the six operations it is built from.
 *
 * Everything this shell draws goes through the table below, and that is a
 * decision with a reason: the board with a touchscreen (the CYD) cannot hold a
 * 320x240 surface at all, and the only way it will ever show this desktop is a
 * rasteriser that renders a damage rectangle in bands and hands each band
 * straight to the panel.  A second backend is affordable while it has six
 * functions to implement and impossible once the shell calls ag_gfx_* from
 * forty places.  See docs/plans/desktop.md §9.2.
 *
 * `read` is the one operation a band rasteriser will not have, because there is
 * nothing to read back from.  It is used for exactly two things - the pixels
 * under the pointer and under a drag outline - and in band mode both of those
 * stop being save-and-restore and become ordinary damage.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_DSK_PAINT_H
#define ARGON_DSK_PAINT_H

#include "dsk.h"

typedef struct dsk_painter {
    void (*fill)(dsk_rect_t r, uint32_t rgb);
    /* Opaque RGB565 pixels, `stride_px` pixels between rows of `src`. */
    void (*blit)(dsk_rect_t r, const uint16_t *src, uint32_t stride_px);
    /*
     * One line of the built-in 8x16 font.  `max_w` of 0 is unlimited; anything
     * else ends the string with "..." rather than letting it run on.
     * `bg` may be DSK_TRANS.
     */
    void (*text)(int16_t x, int16_t y, int16_t max_w, const char *s,
                 uint32_t fg, uint32_t bg);
    /* An empty rectangle resets the clip to the whole surface. */
    void (*clip)(dsk_rect_t r);
    /* Copy r's pixels out of the surface; dst holds r.w * r.h of them. */
    void (*read)(dsk_rect_t r, uint16_t *dst);
    void (*flush)(dsk_rect_t r);
} dsk_painter_t;

/*
 * Bind the painter to the acquired system surface.  `fb` is what acquire()
 * reported and must not be NULL - a board with no surface needs the band
 * backend, which does not exist yet.
 */
void dsk_paint_bind_surface(void *fb, uint32_t stride_bytes, int16_t w,
                            int16_t h);

/* The painter in force.  Never NULL after a bind. */
const dsk_painter_t *dsk_paint(void);

/* Swap in another backend (the host test's recorder, later the band one). */
void dsk_paint_bind(const dsk_painter_t *p, int16_t w, int16_t h);

/*
 * Bind the band backend: no surface, a damage rectangle rasterised one
 * horizontal strip at a time and each strip handed to the panel.  For a board
 * whose acquire() reports no framebuffer, which is the only way a 320x240
 * screen happens in 179 KB of RAM.  See dsk_paint_band.c.
 */
void dsk_paint_bind_band(int16_t w, int16_t h);

/*
 * How a repaint is cut into pieces, when it has to be.
 *
 * `begin` sets the strip that the next drawing lands in and answers how many
 * rows it took (0 when there is nothing left); `present` hands the finished
 * strip to the glass.  Registered by the band backend and by nothing else:
 * with a surface there is one piece, which is the whole rectangle.
 */
typedef struct dsk_bander {
    int16_t (*begin)(dsk_rect_t r, int16_t y);
    void (*present)(void);
    /*
     * "That is the frame."  Optional, and worth having: a panel that is told
     * once per repaint rather than once per strip does the same work in a
     * fraction of the requests, and under an emulator the requests are what
     * costs - 339 ms a drag step against 22 ms on real glass, all of it
     * waiting for the previous strip to be acknowledged.
     */
    void (*frame_done)(void);
} dsk_bander_t;

void dsk_paint_bind_bander(const dsk_bander_t *b);
bool dsk_paint_banded(void);

/*
 * Repaint one rectangle, whichever way the bound backend needs.
 *
 * With a surface: `draw` once, then flush.  In band mode: `draw` once per
 * strip, each time with the strip's own rectangle, and the strip goes to the
 * panel as soon as it is finished.  Anything that reads the background - the
 * pointer, a drag outline - must therefore be painted by `draw` itself, not
 * saved and restored around the call, because outside the strip there is
 * nothing to read.
 */
void dsk_paint_region(dsk_rect_t r, void (*draw)(dsk_rect_t r));

/* Say that the strips handed over since the last one make a whole frame. */
void dsk_paint_frame_done(void);

/* ---- what the shell actually calls -------------------------------------- */

void dsk_fill(dsk_rect_t r, uint32_t rgb);
void dsk_clip(dsk_rect_t r);
void dsk_clip_reset(void);
dsk_rect_t dsk_clip_now(void);
/*
 * Would anything drawn in r survive the clip in force?
 *
 * The repaint hands each piece of furniture the whole damage rectangle and
 * lets the clip sort it out.  That is correct, and for anything that has to
 * build its pixels before it can hand them over - a line of small text is
 * sixty glyphs composed one at a time - it is work done in order to be thrown
 * away.  One test at the top of each of those is the difference.
 */
bool dsk_visible(dsk_rect_t r);
void dsk_flush(dsk_rect_t r);
void dsk_text(int16_t x, int16_t y, const char *s, uint32_t fg, uint32_t bg);
void dsk_text_fit(int16_t x, int16_t y, int16_t max_w, const char *s,
                  uint32_t fg, uint32_t bg);
/*
 * Which font the shell's own text is drawn in.
 *
 * 8x16 is the kernel's font and the only one a console has; on a 640x400
 * screen it is right.  On 320x240 - the CYD's glass, the only one anybody
 * touches - it leaves fifteen lines and forty columns, and a file manager with
 * fifteen lines is a file manager you scroll instead of read.  The 8x8 font
 * doubles that, and Windows did the same thing for the same reason: the system
 * font of 3.11 at 640x480 is not the font of 320x200.
 *
 * Everything the shell draws goes through dsk_text/dsk_text_fit, so the choice
 * is made once, here, and every window, menu and list follows - including the
 * furniture, because the layout's heights are dsk_ui_h() rather than a
 * constant.  dsk_text_small stays 8x8 whatever this says: a caption under an
 * icon and the status strip are deliberately smaller than the body text, and
 * on a small screen they become the same size, which is what a small screen
 * looks like.
 */
typedef enum {
    DSK_UI_FONT_LARGE = 0, /* 8x16, the kernel's */
    DSK_UI_FONT_SMALL,     /* 8x8, the shell's own */
} dsk_ui_font_t;

void dsk_ui_font_set(dsk_ui_font_t f);
dsk_ui_font_t dsk_ui_font(void);
/* One cell of the chosen font.  The layout is built out of these. */
int16_t dsk_ui_w(void);
int16_t dsk_ui_h(void);

/* The 8x8 font, for icon captions and the status strip.  Opaque bg only. */
void dsk_text_small(int16_t x, int16_t y, const char *s, uint32_t fg,
                    uint32_t bg);
int16_t dsk_text_small_width(const char *s);

void dsk_hline(int16_t x, int16_t y, int16_t w, uint32_t rgb);
void dsk_vline(int16_t x, int16_t y, int16_t h, uint32_t rgb);
void dsk_frame(dsk_rect_t r, uint32_t rgb);

/*
 * The Windows 3.x bevel: one pixel of light along the top and left, one of
 * shadow along the bottom and right.  `raised` false swaps them, which is what
 * a pressed button and a sunken list box are.
 */
void dsk_bevel(dsk_rect_t r, bool raised);
/* Face colour, then the bevel around it. */
void dsk_panel(dsk_rect_t r, bool raised, uint32_t face);

#endif /* ARGON_DSK_PAINT_H */
