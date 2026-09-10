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
#include <argon/display.h>
#include <argon/input.h>
#include <argon/log.h>

#include <argon/port/mem.h>
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
        if (!AG_HAS(ops, text_row)) {
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
        bool     pixels = false;
        uint16_t span_w = 0, span_h = 0;

        ag_dev_lock_hold();
        ag_device_t *dev = ag_dev_find(info.name);
        if (dev != NULL && dev->class_ops != NULL) {
            const ag_input_ops_t *ops =
                (const ag_input_ops_t *)dev->class_ops;
            /*
             * As much of the table as `poll` needs, not as much as this
             * kernel's own struct has: demanding the whole of it means every
             * driver built against an older header stops being polled the day
             * a field is appended, which is the opposite of what `size` is
             * for.  (ABI 0.44 appended three.)
             */
            const uint32_t need = (uint32_t)(__builtin_offsetof(
                                                 ag_input_ops_t, poll) +
                                             sizeof(ops->poll));
            if (ops->size >= need && ops->poll != NULL) {
                /* Four is a finger's worth of movement in ten milliseconds; a
                 * driver with more to say is asked again on the next tick
                 * rather than being allowed to hold the console task. */
                n = ops->poll(0, evs, 4);
            }
            /*
             * Units in the same hold as the poll, not a second one.
             *
             * A separate lock and lookup per device per tick is a hundred
             * contentions a second against everything else that touches the
             * registry - and a panel being written to is exactly that.  The
             * pointer stalled and then caught up in a jump.
             */
            const uint32_t px_need = (uint32_t)(__builtin_offsetof(
                                                    ag_input_ops_t, span_h) +
                                                sizeof(uint16_t));
            if (ops->size >= px_need && ops->units == AG_PTR_PIXELS) {
                pixels = true;
                span_w = ops->span_w;
                span_h = ops->span_h;
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
        /*
         * Pixels straight through, when the driver says that is what it
         * measured (ABI 0.44).  Its span is its own glass; the surface is
         * what a pointer is measured in above here, and the two are usually
         * the same panel but need not be.
         */
        if (pixels) {
            uint16_t sw = 0, sh = 0;
            if (!ag_display_size(&sw, &sh) || sw == 0 || sh == 0) {
                sw = span_w;
                sh = span_h;
            }
            if (span_w == 0) {
                span_w = sw;
            }
            if (span_h == 0) {
                span_h = sh;
            }
            for (int32_t e = 0; e < n && e < 4; e++) {
                ag_event_t *ev = &evs[e];
                switch (ev->type) {
                case AG_EV_POINTER_DOWN:
                case AG_EV_POINTER_UP:
                case AG_EV_POINTER_MOVE:
                case AG_EV_WHEEL:
                    break;
                default:
                    (void)ag_console_inject_event(ev);
                    continue;
                }
                int32_t x = ev->ptr.x;
                int32_t y = ev->ptr.y;
                if (span_w != sw && span_w > 0) {
                    x = x * (int32_t)sw / (int32_t)span_w;
                }
                if (span_h != sh && span_h > 0) {
                    y = y * (int32_t)sh / (int32_t)span_h;
                }
                if (x < 0) {
                    x = 0;
                }
                if (y < 0) {
                    y = 0;
                }
                if (x > (int32_t)sw - 1) {
                    x = (int32_t)sw - 1;
                }
                if (y > (int32_t)sh - 1) {
                    y = (int32_t)sh - 1;
                }
                ev->ptr.x = (int16_t)x;
                ev->ptr.y = (int16_t)y;
                (void)ag_console_inject_event(ev);
            }
            continue;
        }

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
            /* An input driver's contract says cells; a pointer above here
             * is pixels (ABI 0.42), so scale on the way in rather than asking
             * every loadable driver to learn the surface size. */
            ag_input_to_pixels(ev);
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
/*
 * Off while the screen is off; and the first repaint after it comes back is a
 * full one, which is what s_seen == NULL means to render_locked below.
 */
static bool s_enabled = true;
static bool s_owe_full;

void ag_textpanel_enable(bool on)
{
    if (on == s_enabled) {
        return;
    }
    s_enabled = on;
    if (on) {
        s_owe_full = true;
    }
}

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
     *
     * Asked for on the first repaint rather than reserved, and sized to the
     * panel that actually turned up rather than to the widest console the system
     * allows.  As a static array it was four hundred and eighty bytes held on
     * every machine, including the ones with no such panel - and that is not an
     * abstraction: it is what put the S3 firmware over the end of its data
     * segment and stopped it linking.  Forty columns of glass need eighty bytes.
     */
    static ag_textcell_t *s_row;
    static uint16_t       s_row_cols;

    if (screen == NULL || !s_enabled) {
        return;
    }

    /*
     * While an application holds the display the panel is showing its pixels,
     * and a console row painted over them is a band of text through the middle
     * of somebody's picture.  Releasing marks the whole screen dirty, so
     * nothing has to be remembered here about what was missed.
     */
    if (ag_display_acquired()) {
        s_seen = NULL; /* the panel is not ours; owe it a full repaint */
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
    const bool full = (ops != s_seen) || s_owe_full;
    if (full) {
        s_owe_full = false;
        s_seen = ops;
        s_caret_col = 0xffffu;
        s_caret_row = 0xffffu;
        s_caret_lit = false;
    }

    uint16_t cols = screen->cols;
    uint16_t rows = screen->rows;
    /*
     * What the panel has, as opposed to what the console uses.  A 320x240
     * panel is 40x30 cells; a console of 40x25 leaves five rows - forty
     * pixels - that belong to nobody, and the console never writes there
     * because as far as it is concerned they do not exist.
     */
    uint16_t panel_rows = rows;
    if (ops->text_info != NULL) {
        uint16_t pcols = cols, prows = rows;
        if (ops->text_info(0, &pcols, &prows) == AG_OK) {
            panel_rows = prows;
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

    if (s_row == NULL || s_row_cols < cols) {
        ag_textcell_t *grown = (ag_textcell_t *)ag_port_alloc(
            (size_t)cols * sizeof(ag_textcell_t), AG_MEM_FAST | AG_MEM_BYTE);
        if (grown == NULL) {
            return; /* nothing to repaint through; the next tick tries again */
        }
        ag_port_free(s_row);
        s_row = grown;
        s_row_cols = cols;
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

    /*
     * The rows the panel has and the console does not.
     *
     * Only on a full repaint, which is exactly the moment they matter: the
     * panel has just appeared, or an application has just given the glass
     * back.  A released application leaves its last frame on the whole panel
     * and the console then repaints its own rows over it - so what a person
     * sees is text on top and a band of somebody else's picture along the
     * bottom, for ever, because nothing up here believes those rows exist.
     * Reported as "the console does not finish redrawing the screen", and it
     * is precisely that: it finished its screen, which is smaller than the
     * glass.
     *
     * Blanked as text rather than as pixels because text is the only road
     * this file has to the panel, and a row of spaces in the current attribute
     * is what the console would have put there had it been that tall.
     */
    if (full && panel_rows > rows) {
        for (uint16_t x = 0; x < cols; x++) {
            s_row[x].ch = ' ';
            s_row[x].attr = AG_ATTR_DEFAULT;
        }
        for (uint16_t y = rows; y < panel_rows; y++) {
            ops->text_row(0, y, s_row, cols);
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
