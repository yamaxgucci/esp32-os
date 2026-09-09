/*
 * ArgonOS DESKTOP - the windows, and who is on top of whom.
 *
 * Every overlapping window on this desktop belongs to the shell itself: a
 * folder, a dialog, a menu.  Applications do not get windows (see
 * docs/plans/desktop.md §3.1 and §9.3) - they get the whole screen, the way a
 * DOS program under Windows 3.11 did.
 *
 * Windows keep no pixels of their own.  There is a list of them, bottom to
 * top, and a rectangle that needs repainting; the repaint walks the list under
 * a clip.  That is the whole scheme, and it is what makes a window cost
 * ninety-six bytes instead of a framebuffer.
 *
 * This file has no drawing in it beyond the painter table, so it compiles on
 * the host and host-tests/test_desktop.c checks the parts that are easy to get
 * wrong and impossible to see: the geometry of a frame, what a point lands on,
 * and which windows a damage rectangle has to repaint, in what order.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_DSK_WM_H
#define ARGON_DSK_WM_H

#include "dsk.h"

#define DSK_WIN_MAX   12
#define DSK_TITLE_MAX 40

typedef struct dsk_win dsk_win_t;

/* What a point on the screen lands on.  The order matters to nothing. */
typedef enum {
    DSK_HIT_NONE = 0,
    DSK_HIT_CLIENT,
    DSK_HIT_TITLE,
    DSK_HIT_SYSMENU,
    DSK_HIT_MINIMISE,
    DSK_HIT_MAXIMISE,
    DSK_HIT_EDGE_N,
    DSK_HIT_EDGE_S,
    DSK_HIT_EDGE_W,
    DSK_HIT_EDGE_E,
    DSK_HIT_CORNER_NW,
    DSK_HIT_CORNER_NE,
    DSK_HIT_CORNER_SW,
    DSK_HIT_CORNER_SE,
    DSK_HIT_ICON, /* the plate a minimised window shrank to */
} dsk_hit_t;

typedef enum {
    DSK_WIN_NORMAL = 0,
    DSK_WIN_MINIMISED,
    DSK_WIN_MAXIMISED,
} dsk_win_state_t;

/*
 * What a window's owner supplies.  Only `draw` is required, and it is called
 * with the clip already set to the part of the client area that needs paint -
 * so an owner may draw its whole content every time and pay only for the part
 * that shows.
 */
typedef struct {
    void (*draw)(dsk_win_t *w, dsk_rect_t client);
    /* True when the event was used up.  NULL means "not interested". */
    bool (*key)(dsk_win_t *w, uint16_t keycode, uint32_t unicode,
                uint16_t mods);
    bool (*pointer)(dsk_win_t *w, dsk_hit_t where, int16_t x, int16_t y,
                    uint8_t buttons, bool down, bool dbl);
    /* Last chance to free anything hung off `user`. */
    void (*closed)(dsk_win_t *w);
} dsk_win_ops_t;

struct dsk_win {
    dsk_rect_t frame;   /* the whole window, border and caption included   */
    dsk_rect_t restore; /* what frame was before maximise or minimise      */
    char       title[DSK_TITLE_MAX];
    dsk_win_state_t state;
    bool       resizable;
    bool       modal;   /* while one of these is open, nothing else is fed */
    int16_t    min_w, min_h;
    const dsk_win_ops_t *ops;
    void      *user;
    /* Private to the window manager. */
    bool       used;
    uint8_t    slot;
};

/*
 * `damage` is how the manager says a rectangle needs repainting.  It is a
 * callback rather than a call into the shell so that the tests can watch it,
 * and so that this file needs to know nothing about who owns the damage list.
 */
void dsk_wm_init(const dsk_metrics_t *m, void (*damage)(dsk_rect_t r));

/*
 * Ask for a rectangle to be repainted.  For a window's owner, which knows
 * that its own content changed and has no other way to say so.
 */
void dsk_wm_damage_rect(dsk_rect_t r);

/* NULL when there is no room left (DSK_WIN_MAX) or the arguments make no sense. */
dsk_win_t *dsk_wm_open(const char *title, dsk_rect_t frame,
                       const dsk_win_ops_t *ops, void *user);
void       dsk_wm_close(dsk_win_t *w);
void       dsk_wm_close_all(void);

int        dsk_wm_count(void);
/* z = 0 is the bottom of the pile.  NULL past the top. */
dsk_win_t *dsk_wm_at(int z);
dsk_win_t *dsk_wm_active(void);
void       dsk_wm_activate(dsk_win_t *w);
/* Alt+Tab: the window one below the top comes up.  No-op with fewer than two. */
void       dsk_wm_cycle(void);

void       dsk_wm_set_title(dsk_win_t *w, const char *title);
void       dsk_wm_move(dsk_win_t *w, dsk_rect_t frame);
void       dsk_wm_minimise(dsk_win_t *w);
void       dsk_wm_maximise(dsk_win_t *w);
void       dsk_wm_restore(dsk_win_t *w);
void       dsk_wm_cascade(void);
void       dsk_wm_tile(void);

/* ---- geometry: pure functions of the frame and the metrics ------------- */

dsk_rect_t dsk_wm_client(const dsk_win_t *w);
dsk_rect_t dsk_wm_title(const dsk_win_t *w);
dsk_rect_t dsk_wm_sysmenu(const dsk_win_t *w);
dsk_rect_t dsk_wm_minbox(const dsk_win_t *w);
dsk_rect_t dsk_wm_maxbox(const dsk_win_t *w);
/* Where a minimised window sits, by its position among the minimised ones. */
dsk_rect_t dsk_wm_icon_rect(const dsk_win_t *w);

/* The topmost window under the point, and what part of it.  NULL for none. */
dsk_win_t *dsk_wm_hit(int16_t x, int16_t y, dsk_hit_t *what);
dsk_hit_t  dsk_wm_hit_in(const dsk_win_t *w, int16_t x, int16_t y);

/* ---- painting ---------------------------------------------------------- */

/*
 * Paint every window that shows inside `area`, bottom to top.  The caller has
 * already painted whatever is behind them and set no clip of its own: this
 * sets one per window.
 */
void dsk_wm_draw(dsk_rect_t area);

/* ---- input ------------------------------------------------------------- */

/*
 * Feed the manager a pointer event.  Returns true when it was used up - by a
 * title bar, a button, a drag, or a window's own handler - and false when
 * nothing wanted it, which means the click was on the desktop.
 *
 * `dbl` is the caller's business: it knows the double-click time.
 */
bool dsk_wm_pointer(dsk_ptr_t type, int16_t x, int16_t y, uint8_t buttons,
                    bool dbl);
bool dsk_wm_key(uint16_t keycode, uint32_t unicode, uint16_t mods);

/* True while a window is being dragged or resized by its outline. */
bool dsk_wm_tracking(void);

/*
 * Take the drag outline off the glass around a repaint, and put it back.
 *
 * The outline saves the pixels under it, so anything that repaints beneath a
 * live one makes that copy stale - and the next movement then restores a
 * picture that is no longer there.  A repaint therefore goes: outline off,
 * paint, outline on, exactly as it does for the pointer.  Both no-ops when
 * nothing is being dragged.
 */
void dsk_wm_outline_off(void);
void dsk_wm_outline_on(void);

/*
 * Band mode: the outline is scenery, painted into each strip after the
 * windows and before the pointer.  Both calls above are then no-ops, because
 * there is nothing saved to take off or put back.
 */
void dsk_wm_outline_paint(void);

#endif /* ARGON_DSK_WM_H */
