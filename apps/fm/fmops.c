/*
 * ArgonOS file manager - the things the function keys do, and the questions they
 * ask first.
 *
 * Every operation here is written to fail out loud: an error goes on the message
 * line in the words the system used, and nothing is half-done silently.  On a
 * machine with no undo that matters more than convenience.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "fm.h"
#include "fm_ui.h"

#ifdef AG_BUILTIN
int ag_edit_main(int argc, char **argv);
#endif

/* Big enough that copying is not a syscall per kilobyte, small enough that two
 * of them are nothing next to the arena. */
#define FM_COPY_CHUNK (8u * 1024u)

/* How often the progress dialog may change during a copy.  Half a second is
 * often enough to look alive without burning the serial console on a fast
 * RAM-disk copy (where every chunk would otherwise redraw). */
#define FM_COPY_UI_MS 500u

/* Dialog sits over the panels (not the chrome rows).
 * Interior text width is DLG_W-4; the bar is [ + track + ], so the track is
 * two shorter — otherwise fm_put_clipped eats the closing ']'. */
#define FM_COPY_DLG_W 52
#define FM_COPY_DLG_H 7
#define FM_COPY_DLG_Y 6
#define FM_COPY_BAR_W (FM_COPY_DLG_W - 6)

/* Solid yellow cells for the filled part of the copy bar. */
#define FM_ATTR_BAR_FILL AG_ATTR(AG_BLACK, AG_YELLOW)

/* What the viewer will hold.  A bound, because a viewer that tries to load a
 * card-sized file is a viewer that fails on the interesting file. */
#define FM_VIEW_MAX (128u * 1024u)
#define FM_VIEW_LINES 4096

/* ---------------------------------------------------------------------- */
/* Asking                                                                 */
/* ---------------------------------------------------------------------- */

bool fm_ask(const char *prompt, char *buf, size_t len)
{
    const int start = (int)strlen(prompt) + 2;
    size_t    at = strlen(buf); /* a default answer may already be there */

    for (;;) {
        fm_clear_row(FM_ROW_MESSAGE, FM_ATTR_DIALOG);
        fm_put(1, FM_ROW_MESSAGE, prompt, FM_ATTR_DIALOG);
        fm_put_clipped(start, FM_ROW_MESSAGE, FM_COLS - start - 1, buf,
                       FM_ATTR_DIALOG);
        /* A block where the next character will go, since the real cursor is
         * hidden while the manager owns the screen. */
        if (start + (int)at < FM_COLS - 1) {
            fm_ui_poke(start + (int)at, FM_ROW_MESSAGE, '_', FM_ATTR_CURSOR);
        }
        fm_ui_present();

        ag_event_t ev;
        if (!ag_poll_event(&ev, UINT32_MAX)) {
            continue;
        }
        if (ev.type == AG_EV_QUIT) {
            return false;
        }
        if (ev.type != AG_EV_KEY_DOWN) {
            continue;
        }

        if (ev.key.keycode == AG_KEY_ENTER) {
            return true;
        }
        if (ev.key.keycode == AG_KEY_ESC) {
            return false;
        }
        if (ev.key.keycode == AG_KEY_BACKSPACE) {
            if (at > 0) {
                buf[--at] = '\0';
            }
            continue;
        }
        if (ev.key.unicode >= 0x20 && ev.key.unicode < 0x7f && at + 1 < len) {
            buf[at++] = (char)ev.key.unicode;
            buf[at] = '\0';
        }
    }
}

bool fm_confirm(const char *question)
{
    char line[FM_COLS];

    ag_strlcpy(line, question, sizeof(line));
    ag_strlcat(line, "  [y/N]", sizeof(line));

    fm_clear_row(FM_ROW_MESSAGE, FM_ATTR_DIALOG);
    fm_put_clipped(1, FM_ROW_MESSAGE, FM_COLS - 2, line, FM_ATTR_DIALOG);
    fm_ui_present();

    for (;;) {
        ag_event_t ev;
        if (!ag_poll_event(&ev, UINT32_MAX)) {
            continue;
        }
        if (ev.type == AG_EV_QUIT) {
            return false;
        }
        if (ev.type != AG_EV_KEY_DOWN) {
            continue;
        }
        if (ev.key.keycode == AG_KEY_Y) {
            return true;
        }
        return false; /* anything else means no, which is the safe default */
    }
}

void fm_pause(const char *note)
{
    fm_clear_row(FM_ROW_MESSAGE, FM_ATTR_DIALOG);
    fm_put(1, FM_ROW_MESSAGE, (note != NULL) ? note : "Press any key",
           FM_ATTR_DIALOG);
    fm_ui_present();

    for (;;) {
        ag_event_t ev;
        if (!ag_poll_event(&ev, UINT32_MAX)) {
            continue;
        }
        if (ev.type == AG_EV_QUIT || ev.type == AG_EV_KEY_DOWN) {
            return;
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Viewing                                                                */
/* ---------------------------------------------------------------------- */

void fm_view(void)
{
    const fm_entry_t *e = fm_current();
    if (e == NULL || e->is_dir) {
        return;
    }

    char full[AG_PATH_MAX];
    fm_join(fm_active()->path, e->name, full, sizeof(full));

    const ag_handle_t h = ag_open(full, AG_O_RDONLY);
    if (h < 0) {
        fm_error(e->name, h);
        return;
    }

    uint32_t want = (e->size < FM_VIEW_MAX) ? (uint32_t)e->size : FM_VIEW_MAX;
    char    *text = (char *)ag_malloc(want + 1);
    uint32_t *lines = (uint32_t *)ag_malloc(sizeof(uint32_t) * FM_VIEW_LINES);

    if (text == NULL || lines == NULL) {
        ag_free(text);
        ag_free(lines);
        (void)ag_close(h);
        fm_message("not enough memory to view this file");
        return;
    }

    uint32_t got = 0;
    while (got < want) {
        const int32_t n = ag_read(h, &text[got], want - got);
        if (n <= 0) {
            break;
        }
        got += (uint32_t)n;
    }
    (void)ag_close(h);
    text[got] = '\0';

    /* Where each line starts.  Done once: scrolling then costs nothing. */
    int line_count = 0;
    lines[line_count++] = 0;
    for (uint32_t i = 0; i < got && line_count < FM_VIEW_LINES; i++) {
        if (text[i] == '\n') {
            lines[line_count++] = i + 1;
        }
    }

    const int rows = FM_ROWS - 2;
    int       top = 0;
    bool      viewing = true;

    while (viewing) {
        char header[FM_COLS];
        char number[24];
        ag_strlcpy(header, " ", sizeof(header));
        ag_strlcat(header, e->name, sizeof(header));
        ag_strlcat(header, "   line ", sizeof(header));
        ag_strlcat(header,
                   ag_utoa((uint64_t)(top + 1), number, sizeof(number), 0, false),
                   sizeof(header));
        ag_strlcat(header, " of ", sizeof(header));
        ag_strlcat(header,
                   ag_utoa((uint64_t)line_count, number, sizeof(number), 0,
                           false),
                   sizeof(header));
        if (e->size > got) {
            ag_strlcat(header, "   (first 128 KB)", sizeof(header));
        }
        fm_clear_row(0, FM_ATTR_KEYS);
        fm_put_clipped(0, 0, FM_COLS, header, FM_ATTR_KEYS);

        for (int row = 0; row < rows; row++) {
            const int index = top + row;
            char      out[FM_COLS + 1];
            int       n = 0;

            if (index < line_count) {
                const uint32_t from = lines[index];
                const uint32_t to =
                    (index + 1 < line_count) ? lines[index + 1] : got;
                for (uint32_t i = from; i < to && n < FM_COLS; i++) {
                    const unsigned char c = (unsigned char)text[i];
                    if (c == '\r' || c == '\n') {
                        continue;
                    }
                    if (c == '\t') {
                        /* Tabs to the next multiple of eight, so source lines up. */
                        do {
                            out[n++] = ' ';
                        } while ((n % 8) != 0 && n < FM_COLS);
                        continue;
                    }
                    out[n++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
                }
            }
            out[n] = '\0';
            fm_put_clipped(0, row + 1, FM_COLS, out,
                           AG_ATTR(AG_LGRAY, AG_BLACK));
        }

        fm_clear_row(FM_ROWS - 1, FM_ATTR_KEYS);
        fm_put(0, FM_ROWS - 1,
               " arrows and PgUp/PgDn scroll, Home/End jump, Esc or F3 returns",
               FM_ATTR_KEYS);
        fm_ui_present();

        ag_event_t ev;
        if (!ag_poll_event(&ev, UINT32_MAX)) {
            continue;
        }
        if (ev.type == AG_EV_QUIT) {
            break;
        }
        if (ev.type != AG_EV_KEY_DOWN) {
            continue;
        }

        switch (ev.key.keycode) {
        case AG_KEY_UP:       top -= 1; break;
        case AG_KEY_DOWN:     top += 1; break;
        case AG_KEY_PAGEUP:   top -= rows; break;
        case AG_KEY_PAGEDOWN: top += rows; break;
        case AG_KEY_HOME:     top = 0; break;
        case AG_KEY_END:      top = line_count - rows; break;
        case AG_KEY_ESC:
        case AG_KEY_F3:
        case AG_KEY_F10:
            viewing = false;
            break;
        default: break;
        }

        if (top > line_count - 1) {
            top = line_count - 1;
        }
        if (top < 0) {
            top = 0;
        }
    }

    ag_free(text);
    ag_free(lines);
    fm_ui_cls();
}

/* ---------------------------------------------------------------------- */
/* Editing                                                                */
/* ---------------------------------------------------------------------- */

#ifndef AG_BUILTIN
/* Guest FM.AXE / GFXFM: look for a packaged editor next to the usual app roots. */
static bool fm_exec_edit_axe(const char *file)
{
    static const char *const k_edit[] = {
        "/sd/EDIT.AXE",
        "/sd/apps/EDIT.AXE",
        "/tmp/EDIT.AXE",
        "/sys/EDIT.AXE",
        "EDIT.AXE",
    };

    for (unsigned i = 0; i < sizeof(k_edit) / sizeof(k_edit[0]); i++) {
        const ag_handle_t h = ag_open(k_edit[i], AG_O_RDONLY);
        if (h < 0) {
            continue;
        }
        (void)ag_close(h);

        ag_print("Loading EDIT.AXE ...\n");
        const char *argv[2] = {k_edit[i], file};
        (void)ag_exec(k_edit[i], 2, argv);
        return true;
    }
    return false;
}
#endif

void fm_edit(void)
{
    const fm_entry_t *e = fm_current();
    if (e == NULL || e->is_dir) {
        return;
    }

    char full[AG_PATH_MAX];
    fm_join(fm_active()->path, e->name, full, sizeof(full));

    fm_ui_end();

#ifdef AG_BUILTIN
    {
        char *eargv[2] = {(char *)"edit", full};
        (void)ag_edit_main(2, eargv);
    }
#else
    if (!fm_exec_edit_axe(full)) {
        ag_print("No EDIT.AXE found (tried /sd, /sd/apps, /tmp, /sys).\n");
    }
#endif

    fm_pause("press any key");
    fm_ui_begin();
    (void)fm_reload(fm_active());
    (void)fm_reload(fm_other());
}

/* ---------------------------------------------------------------------- */
/* Copying                                                                */
/* ---------------------------------------------------------------------- */

/*
 * Soft stop while a long copy runs.  Ctrl+C / signal cancels; Alt+N only sends
 * FOCUS_LOST and must not abort the copy (work continues in the background).
 *
 * While copy_file runs, the main loop is blocked — FOCUS_GAINED is handled here
 * so returning to the slot repaints panels under the dialog.
 */
static bool s_copy_need_panels;
static bool s_copy_was_focused;

static bool copy_cancelled(void)
{
    bool hit = false;

    if (ag_interrupted()) {
        hit = true;
    }

    ag_event_t ev;
    while (ag_poll_event(&ev, 0)) {
        if (ev.type == AG_EV_FOCUS_GAINED) {
            s_copy_need_panels = true;
            ag_log(AG_LOG_INFO, "fm", "copy: FOCUS_GAINED (will redraw panels)");
        } else if (ev.type == AG_EV_FOCUS_LOST) {
            s_copy_was_focused = false;
            ag_log(AG_LOG_INFO, "fm", "copy: FOCUS_LOST");
        } else if (ev.type == AG_EV_QUIT) {
            /* Same rule as the main loop: shell QUIT must not cancel bg copy. */
            if (ag_focused()) {
                hit = true;
            }
        }
    }
    return hit;
}

/* After a cancelled copy: do not let a leftover QUIT close the manager. */
static void absorb_copy_interrupt(void)
{
    (void)ag_interrupted();
    ag_flush_input();
}

/* Modal progress over the panels: title, bar, percent / KB — no spinner. */
static void copy_progress(const char *label, uint64_t done, uint64_t total)
{
    /* Background slot: keep copying, do not paint over the focused app. */
    if (!ag_focused()) {
        s_copy_was_focused = false;
        ag_heartbeat();
        ag_yield();
        return;
    }

    /*
     * Returning from another slot during copy: shell text is still on screen.
     * Repaint panels first, then the dialog on top.
     */
    if (!s_copy_was_focused || s_copy_need_panels) {
        s_copy_was_focused = true;
        s_copy_need_panels = false;
        fm_ui_begin();
        fm_draw_all();
    }

    const int w = FM_COPY_DLG_W;
    const int h = FM_COPY_DLG_H;
    const int x = (FM_COLS - w) / 2;
    const int y = FM_COPY_DLG_Y;
    const int inner = w - 4;
    char      number[24];
    char      title[FM_COLS];
    char      status[FM_COLS];

    fm_ui_fill(x, y, w, h, ' ', FM_ATTR_DIALOG);
    fm_frame(x, y, w, h, FM_ATTR_DIALOG);

    ag_strlcpy(title, "Copying ", sizeof(title));
    ag_strlcat(title, (label != NULL) ? label : "file", sizeof(title));
    ag_strlcat(title, "...", sizeof(title));
    fm_put_clipped(x + 2, y + 1, inner, title, FM_ATTR_DIALOG);

    uint32_t filled = 0;
    uint32_t pct = 0;
    if (total > 0) {
        pct = (uint32_t)((done * 100u) / total);
        if (pct > 100u) {
            pct = 100u;
        }
        filled = (uint32_t)((done * (uint64_t)FM_COPY_BAR_W) / total);
        if (filled > FM_COPY_BAR_W) {
            filled = FM_COPY_BAR_W;
        }
    }

    /* Brackets in dialog ink; filled track = yellow rectangles; empty = '-'. */
    {
        const int bar_x = x + 2;
        const int bar_y = y + 3;
        fm_ui_poke(bar_x, bar_y, '[', FM_ATTR_DIALOG);
        for (uint32_t i = 0; i < FM_COPY_BAR_W; i++) {
            if (i < filled) {
                fm_ui_poke(bar_x + 1 + (int)i, bar_y, ' ', FM_ATTR_BAR_FILL);
            } else {
                fm_ui_poke(bar_x + 1 + (int)i, bar_y, '-', FM_ATTR_DIALOG);
            }
        }
        fm_ui_poke(bar_x + 1 + (int)FM_COPY_BAR_W, bar_y, ']', FM_ATTR_DIALOG);
    }

    status[0] = '\0';
    if (total > 0) {
        ag_strlcat(status, ag_utoa(pct, number, sizeof(number), 0, false),
                   sizeof(status));
        ag_strlcat(status, "%   ", sizeof(status));
        ag_strlcat(status,
                   ag_utoa(done / 1024u, number, sizeof(number), 0, true),
                   sizeof(status));
        ag_strlcat(status, " / ", sizeof(status));
        ag_strlcat(status,
                   ag_utoa(total / 1024u, number, sizeof(number), 0, true),
                   sizeof(status));
        ag_strlcat(status, " KB", sizeof(status));
    } else {
        ag_strlcat(status,
                   ag_utoa(done / 1024u, number, sizeof(number), 0, true),
                   sizeof(status));
        ag_strlcat(status, " KB", sizeof(status));
    }
    fm_put_clipped(x + 2, y + 4, inner, status, FM_ATTR_DIALOG);
    fm_put_clipped(x + 2, y + 5, inner, "Ctrl+C to cancel", FM_ATTR_DIALOG);
    fm_ui_present();

    ag_yield();
}

/* Copies one file, reporting as it goes.  The error is returned, not printed:
 * the caller knows which of the two names to put in front of it. */
static ag_err_t copy_file(const char *from, const char *to, const char *label,
                          uint64_t total)
{
    /* HostFS readdir does not fill sizes; ask again so the bar can show %. */
    if (total == 0) {
        ag_stat_t st;
        if (ag_stat(from, &st) == AG_OK && (st.attr & AG_A_DIR) == 0) {
            total = st.size;
        }
    }

    const ag_handle_t src = ag_open(from, AG_O_RDONLY);
    if (src < 0) {
        return src;
    }

    const ag_handle_t dst = ag_open(to, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (dst < 0) {
        (void)ag_close(src);
        return dst;
    }

    char *buf = (char *)ag_malloc(FM_COPY_CHUNK);
    if (buf == NULL) {
        (void)ag_close(src);
        (void)ag_close(dst);
        return -AG_ENOMEM;
    }

    ag_err_t err = AG_OK;
    uint64_t done = 0;
    uint32_t last_ui_ms = 0;

    s_copy_need_panels = ag_focused();
    s_copy_was_focused = false;

    /* Something on screen before the first HostFS read, which can take a while. */
    copy_progress(label, 0, total);
    last_ui_ms = ag_millis();

    for (;;) {
        if (copy_cancelled()) {
            err = -AG_EKILLED;
            break;
        }
        /* FOCUS_GAINED while mid-copy: repaint immediately, not next UI tick. */
        if (s_copy_need_panels && ag_focused()) {
            last_ui_ms = ag_millis();
            copy_progress(label, done, total);
        }

        const int32_t n = ag_read(src, buf, FM_COPY_CHUNK);
        if (n < 0) {
            err = (ag_err_t)n;
            break;
        }
        if (n == 0) {
            break;
        }
        const int32_t written = ag_write(dst, buf, (size_t)n);
        if (written != n) {
            err = (written < 0) ? (ag_err_t)written : -AG_ENOSPC;
            break;
        }
        done += (uint64_t)n;

        const uint32_t now = ag_millis();
        if ((now - last_ui_ms) >= FM_COPY_UI_MS ||
            (total > 0 && done >= total)) {
            last_ui_ms = now;
            copy_progress(label, done, total);
        }
    }

    ag_free(buf);
    (void)ag_sync(dst);
    (void)ag_close(dst);
    (void)ag_close(src);

    if (err != AG_OK) {
        /* A half-written file is worse than none: it looks like a copy. */
        (void)ag_unlink(to);
    }
    return err;
}

/*
 * Turns whatever was typed into a destination path.  A name with no directory
 * goes into the other panel; a directory gets the source's name appended, which
 * is what "copy this there" means.
 */
static void resolve_target(const char *typed, const char *name, char *out,
                           size_t len)
{
    ag_strlcpy(out, typed, len);

    ag_stat_t st;
    if (ag_stat(out, &st) == AG_OK && (st.attr & AG_A_DIR) != 0) {
        char joined[AG_PATH_MAX];
        fm_join(out, name, joined, sizeof(joined));
        ag_strlcpy(out, joined, len);
    }
}

void fm_copy(void)
{
    const fm_entry_t *e = fm_current();
    if (e == NULL || e->is_up) {
        return;
    }
    if (e->is_dir) {
        fm_message("copying a whole directory is not implemented yet");
        return;
    }

    char answer[AG_PATH_MAX];
    ag_strlcpy(answer, fm_other()->path, sizeof(answer));

    char prompt[FM_COLS];
    ag_strlcpy(prompt, "Copy ", sizeof(prompt));
    ag_strlcat(prompt, e->name, sizeof(prompt));
    ag_strlcat(prompt, " to:", sizeof(prompt));

    if (!fm_ask(prompt, answer, sizeof(answer))) {
        fm_message("");
        return;
    }

    char from[AG_PATH_MAX];
    char to[AG_PATH_MAX];
    fm_join(fm_active()->path, e->name, from, sizeof(from));
    resolve_target(answer, e->name, to, sizeof(to));

    if (ag_stricmp(from, to) == 0) {
        fm_message("that is the same file");
        return;
    }

    const ag_err_t err = copy_file(from, to, e->name, e->size);
    if (err == -AG_EKILLED) {
        absorb_copy_interrupt();
        fm_message("cancelled");
    } else if (err != AG_OK) {
        fm_error(e->name, err);
    } else {
        fm_message("copied");
    }
    (void)fm_reload(fm_other());
    (void)fm_reload(fm_active());
}

/* ---------------------------------------------------------------------- */
/* Moving, making and removing                                            */
/* ---------------------------------------------------------------------- */

void fm_move(void)
{
    const fm_entry_t *e = fm_current();
    if (e == NULL || e->is_up) {
        return;
    }

    char answer[AG_PATH_MAX];
    ag_strlcpy(answer, fm_other()->path, sizeof(answer));

    char prompt[FM_COLS];
    ag_strlcpy(prompt, "Move or rename ", sizeof(prompt));
    ag_strlcat(prompt, e->name, sizeof(prompt));
    ag_strlcat(prompt, " to:", sizeof(prompt));

    if (!fm_ask(prompt, answer, sizeof(answer))) {
        fm_message("");
        return;
    }

    char from[AG_PATH_MAX];
    char to[AG_PATH_MAX];
    fm_join(fm_active()->path, e->name, from, sizeof(from));
    resolve_target(answer, e->name, to, sizeof(to));

    ag_err_t err = ag_rename(from, to);

    /*
     * Renaming only works inside one filesystem, and the two panels are often on
     * two.  Rather than explain that, offer the thing the user meant.
     */
    if (err != AG_OK && !e->is_dir) {
        char question[FM_COLS];
        ag_strlcpy(question, "Cannot rename across drives (", sizeof(question));
        ag_strlcat(question, ag_strerror(err), sizeof(question));
        ag_strlcat(question, "). Copy and delete instead?", sizeof(question));

        if (fm_confirm(question)) {
            err = copy_file(from, to, e->name, e->size);
            if (err == AG_OK) {
                err = ag_unlink(from);
            }
        }
    }

    if (err == -AG_EKILLED) {
        absorb_copy_interrupt();
        fm_message("cancelled");
    } else if (err != AG_OK) {
        fm_error(e->name, err);
    } else {
        fm_message("moved");
    }
    (void)fm_reload(fm_other());
    (void)fm_reload(fm_active());
}

void fm_mkdir(void)
{
    char name[FM_NAME_MAX] = "";

    if (!fm_ask("Name of the new directory:", name, sizeof(name)) ||
        name[0] == '\0') {
        fm_message("");
        return;
    }

    char path[AG_PATH_MAX];
    fm_join(fm_active()->path, name, path, sizeof(path));

    const ag_err_t err = ag_mkdir(path);
    if (err != AG_OK) {
        fm_error(name, err);
        return;
    }

    (void)fm_reload(fm_active());
    fm_message("created");
}

void fm_delete(void)
{
    const fm_entry_t *e = fm_current();
    if (e == NULL || e->is_up) {
        return;
    }

    char question[FM_COLS];
    ag_strlcpy(question, "Delete ", sizeof(question));
    ag_strlcat(question, e->is_dir ? "directory " : "", sizeof(question));
    ag_strlcat(question, e->name, sizeof(question));
    ag_strlcat(question, "?", sizeof(question));

    if (!fm_confirm(question)) {
        fm_message("");
        return;
    }

    char path[AG_PATH_MAX];
    fm_join(fm_active()->path, e->name, path, sizeof(path));

    const ag_err_t err = e->is_dir ? ag_rmdir(path) : ag_unlink(path);
    if (err != AG_OK) {
        /* A directory with anything in it is refused by the system, not by us,
         * and saying which is more useful than a general failure. */
        fm_error(e->name, err);
        return;
    }

    (void)fm_reload(fm_active());
    fm_message("deleted");
}

/* ---------------------------------------------------------------------- */
/* Running something, and telling how                                     */
/* ---------------------------------------------------------------------- */

void fm_run(const fm_entry_t *entry)
{
    if (entry == NULL) {
        return;
    }

    char path[AG_PATH_MAX];
    fm_join(fm_active()->path, entry->name, path, sizeof(path));

    /*
     * Hand the display back for the duration: whatever the child prints, it
     * prints over an ordinary console, and it has every right to.
     */
    fm_ui_end();

    /*
     * ag_exec blocks while the image loads (HostFS / XIP can take a while).
     * Without a line here the operator only sees a cleared black screen.
     */
    ag_color(AG_LGRAY, AG_BLACK);
    ag_print("Loading ");
    ag_print(entry->name);
    ag_print(" ...\n");
    ag_print("please wait\n");

    const char *argv[1] = {path};
    const int32_t status = ag_exec(path, 1, argv);

    char note[FM_COLS];
    char number[24];
    ag_strlcpy(note, entry->name, sizeof(note));
    ag_strlcat(note, " finished with ", sizeof(note));
    ag_strlcat(note, ag_utoa((uint64_t)(uint32_t)status, number, sizeof(number),
                             0, false),
               sizeof(note));
    ag_strlcat(note, " - press any key", sizeof(note));

    ag_print("\n");
    ag_print(note);
    ag_print("\n");

    for (;;) {
        ag_event_t ev;
        if (!ag_poll_event(&ev, UINT32_MAX)) {
            continue;
        }
        if (ev.type == AG_EV_KEY_DOWN || ev.type == AG_EV_QUIT) {
            break;
        }
    }
    fm_ui_begin();
}

void fm_help(void)
{
    static const char *k_lines[] = {
        "ArgonOS file manager",
        "",
        "  arrows, PgUp, PgDn, Home, End   move the cursor",
        "  Tab                             the other panel",
        "  Enter                           open a directory, run a .AXE,",
        "                                  open by assoc, or view",
        "  Backspace                       up one directory",
        "  Alt+F1 / Alt+F2                 pick a drive for the left / right",
        "",
        "  F1 help          F2 reread the panel     F3 view a file",
        "  F4 edit          F5 copy                 F6 move or rename",
        "  F7 make a directory  F8 delete           F10 or Esc quit",
        "",
        "  Ctrl+C cancels a long copy (or asks this program to stop);",
        "  Ctrl+\\ makes the system stop a runaway .AXE.",
        "",
        "A directory that is not empty cannot be deleted, and copying a whole",
        "directory is not implemented yet - both say so rather than pretending.",
    };

    const int lines = (int)(sizeof(k_lines) / sizeof(k_lines[0]));
    const int w = 74;
    const int h = lines + 3;
    const int x = (FM_COLS - w) / 2;
    /* Pinned to the top rather than centred: the bottom rows belong to the
     * message line and the key bar, and a box that reaches them loses its own
     * border to them. */
    const int y = 1;

    fm_ui_fill(x, y, w, h, ' ', FM_ATTR_DIALOG);
    fm_frame(x, y, w, h, FM_ATTR_DIALOG);

    for (int i = 0; i < lines; i++) {
        fm_put(x + 2, y + 2 + i, k_lines[i], FM_ATTR_DIALOG);
    }
    fm_ui_present();

    fm_pause("Press any key");
}
