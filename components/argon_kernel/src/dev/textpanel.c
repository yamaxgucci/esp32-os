/*
 * ArgonOS - the console on a panel that has no framebuffer.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/textpanel.h>

#include <string.h>

#include <argon/abi.h>
#include <argon/console.h>
#include <argon/device.h>
#include <argon/log.h>

#include <argon/port/time.h>

/*
 * The device is looked up rather than remembered, because the driver that
 * publishes it is loadable: it can arrive at the modules stage, be replaced by
 * `drv install`, or be unloaded while the console is running.  A cached
 * pointer would outlive it by exactly one tick, which is the tick that writes
 * to a driver that is no longer there.
 *
 * The lookup walks at most AG_DEV_MAX entries and happens ten times a second.
 */
static const ag_display_ops_t *panel_ops(ag_device_t **out_dev)
{
    for (uint32_t i = 0;; i++) {
        ag_devinfo_t info;
        if (ag_dev_info(i, AG_DEV_DISPLAY, &info) != AG_OK) {
            return NULL;
        }
        ag_device_t *dev = ag_dev_find(info.name);
        if (dev == NULL || dev->class_ops == NULL) {
            continue;
        }
        const ag_display_ops_t *ops = (const ag_display_ops_t *)dev->class_ops;
        /*
         * A vtable is only as long as the driver that wrote it: a display
         * built against an older ABI has no text_row field at all, and reading
         * one would be reading whatever follows its structure in memory.
         */
        if (ops->size < sizeof(ag_display_ops_t) || ops->text_row == NULL) {
            continue;
        }
        if (out_dev != NULL) {
            *out_dev = dev;
        }
        return ops;
    }
}

/*
 * Input devices that have to be asked rather than waited for.
 *
 * Here rather than in a file of its own because it is the same three lines of
 * registry walk as the panel above, on the same tick, for the same reason: a
 * loadable driver cannot own a task, so the kernel does the asking.
 */
void ag_inputpoll_tick(void)
{
    for (uint32_t i = 0;; i++) {
        ag_devinfo_t info;
        if (ag_dev_info(i, AG_DEV_INPUT, &info) != AG_OK) {
            return;
        }

        ag_event_t evs[4];
        int32_t    n = 0;

        /*
         * The lookup and the call are one operation, under the registry's own
         * lock, because what is being called lives in a loadable module's
         * arena: between finding the vtable and calling it, `drv unload` can
         * free it.  Module unload takes this lock too.
         */
        ag_dev_lock_hold();
        ag_device_t *dev = ag_dev_find(info.name);
        if (dev != NULL && dev->class_ops != NULL) {
            const ag_input_ops_t *ops =
                (const ag_input_ops_t *)dev->class_ops;
            if (ops->size >= sizeof(ag_input_ops_t) && ops->poll != NULL) {
                /* Four is a finger's worth of movement in ten milliseconds; a
                 * driver with more to say is asked again on the next tick
                 * rather than being allowed to hold the console task. */
                n = ops->poll(0, evs, 4);
            }
        }
        ag_dev_lock_release();

        /*
         * Clamped to the screen, because the driver cannot know where it
         * ends.  A panel holds a whole number of cells and the console is
         * whatever fits the system: 320x240 is forty by thirty, the console is
         * forty by twenty-five, and the last five rows of glass are below the
         * bottom of the screen.  A tap there is still a tap and belongs to the
         * nearest row rather than to nowhere.
         */
        const ag_screen_t *screen = ag_console_screen();
        for (int32_t e = 0; e < n && e < 4; e++) {
            ag_event_t *ev = &evs[e];
            if (screen != NULL &&
                (ev->type == AG_EV_POINTER_DOWN ||
                 ev->type == AG_EV_POINTER_UP ||
                 ev->type == AG_EV_POINTER_MOVE)) {
                if (ev->ptr.x < 0) {
                    ev->ptr.x = 0;
                }
                if (ev->ptr.y < 0) {
                    ev->ptr.y = 0;
                }
                if (ev->ptr.x >= (int16_t)screen->cols) {
                    ev->ptr.x = (int16_t)(screen->cols - 1);
                }
                if (ev->ptr.y >= (int16_t)screen->rows) {
                    ev->ptr.y = (int16_t)(screen->rows - 1);
                }
            }
            (void)ag_console_inject_event(ev);
        }
    }
}

ag_err_t ag_textpanel_geometry(uint16_t *cols, uint16_t *rows)
{
    const ag_display_ops_t *ops = panel_ops(NULL);
    if (ops == NULL || ops->text_info == NULL) {
        return -AG_ENODEV;
    }
    return ops->text_info(0, cols, rows);
}

/* Called with the device registry held; see ag_textpanel_render below. */
static void render_locked(const ag_screen_t *screen)
{
    static const ag_display_ops_t *s_seen;
    static uint16_t                s_caret_col = 0xffffu;
    static uint16_t                s_caret_row = 0xffffu;
    static bool                    s_caret_lit;
    /*
     * One row of cells, copied out of the screen.  Copied rather than passed
     * by pointer because ag_cell_t is the kernel's type and ag_textcell_t is
     * the ABI's: they have the same shape today, and a driver compiled against
     * the ABI must not depend on that staying true.
     */
    static ag_textcell_t s_row[AG_SCREEN_MAX_COLS];

    if (screen == NULL) {
        return;
    }

    const ag_display_ops_t *ops = panel_ops(NULL);
    if (ops == NULL) {
        s_seen = NULL;
        return;
    }

    /*
     * A panel that has just arrived has nothing on it, and the screen only
     * offers what changed since the last tick - which, on a machine sitting at
     * a prompt, is nothing at all.  So the first sight of a driver owes it the
     * whole screen.  The same applies when a driver is replaced: `drv install`
     * over a running one is a different panel as far as this is concerned.
     */
    const bool full = (ops != s_seen);
    if (full) {
        s_seen = ops;
        s_caret_col = 0xffffu;
        s_caret_row = 0xffffu;
        s_caret_lit = false;
    }

    uint16_t cols = screen->cols;
    uint16_t rows = screen->rows;
    if (ops->text_info != NULL) {
        uint16_t pcols = cols, prows = rows;
        if (ops->text_info(0, &pcols, &prows) == AG_OK) {
            if (pcols < cols) {
                cols = pcols;
            }
            if (prows < rows) {
                rows = prows;
            }
        }
    }
    if (cols > AG_SCREEN_MAX_COLS) {
        cols = AG_SCREEN_MAX_COLS;
    }

    for (uint16_t y = 0; y < rows; y++) {
        /*
         * Marking the screen dirty instead of drawing it here would not work:
         * the console clears the dirty set at the end of this same tick, so
         * the mark would be gone before anyone acted on it.  A panel that has
         * just appeared gets its full screen in the pass that noticed it.
         */
        if (!full && !ag_screen_row_dirty(screen, y)) {
            continue;
        }
        const ag_cell_t *src = ag_screen_row(screen, y);
        if (src == NULL) {
            continue;
        }
        for (uint16_t x = 0; x < cols; x++) {
            s_row[x].ch = (uint8_t)src[x].ch;
            s_row[x].attr = src[x].attr;
        }
        ops->text_row(0, y, s_row, cols);
        /* A repainted row has painted over the caret. */
        if (y == s_caret_row) {
            s_caret_lit = false;
        }
    }

    if (ops->text_cursor == NULL) {
        return;
    }

    /*
     * The caret blinks at the same rate as the one in the framebuffer path,
     * and for the same reason: it is the only thing on the screen that says
     * the machine is still running when nothing else is moving.
     */
    const bool lit = screen->cursor_visible &&
                     (((uint64_t)ag_port_us() / 530000ull) % 2ull == 0ull);
    const uint16_t cx = screen->cur_x;
    const uint16_t cy = screen->cur_y;

    if (lit == s_caret_lit && cx == s_caret_col && cy == s_caret_row) {
        return;
    }
    if (s_caret_lit && (cx != s_caret_col || cy != s_caret_row) &&
        s_caret_col < cols && s_caret_row < rows) {
        const ag_cell_t old = ag_screen_at(screen, s_caret_col, s_caret_row);
        const ag_textcell_t under = {(uint8_t)old.ch, old.attr};
        ops->text_cursor(0, s_caret_col, s_caret_row, under, false);
    }
    if (cx < cols && cy < rows) {
        const ag_cell_t at = ag_screen_at(screen, cx, cy);
        const ag_textcell_t under = {(uint8_t)at.ch, at.attr};
        ops->text_cursor(0, cx, cy, under, lit);
    }
    s_caret_col = cx;
    s_caret_row = cy;
    s_caret_lit = lit;
}

void ag_textpanel_render(const ag_screen_t *screen)
{
    /*
     * The whole repaint is one operation as far as the registry is concerned,
     * for the same reason the input poll is: every call in it lands in a
     * loadable module's arena, and unloading that module halfway through is
     * executing memory that has been handed back.  A full repaint is a couple
     * of milliseconds and an unload waits that long.
     */
    ag_dev_lock_hold();
    render_locked(screen);
    ag_dev_lock_release();
}
