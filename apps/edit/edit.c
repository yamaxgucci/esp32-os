/*
 * ArgonOS text editor.
 *
 * A small full-screen editor so a program can be typed on the board itself -
 * which is what the on-device compiler needs, and what a machine with only
 * `type` and `copy` is missing.  Looks and feels like the DOS editor everyone
 * already knows: one file, arrows, insert, F2 to save, Esc to leave.
 *
 *   edit t:\hello.c
 *   edit                  asks for a name
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "edit.h"

#ifndef AG_BUILTIN
AG_APP("EDIT", "1.0", "argon", 0);
#endif

/* ---------------------------------------------------------------------- */
/* Buffer                                                                  */
/* ---------------------------------------------------------------------- */

static char  **s_lines;
static int     s_count;
static int     s_cap;
static size_t  s_bytes; /* total characters, not counting terminators     */
static int     s_row;
static int     s_col;
static int     s_top;
static int     s_left;
static bool    s_dirty;
static bool    s_running;
static char    s_path[AG_PATH_MAX];
static char    s_status[EDIT_COLS];

static void set_status(const char *text)
{
    ag_strlcpy(s_status, (text != NULL) ? text : "", sizeof(s_status));
}

static void set_error(const char *what, ag_err_t err)
{
    char line[EDIT_COLS];

    ag_strlcpy(line, (what != NULL) ? what : "failed", sizeof(line));
    ag_strlcat(line, ": ", sizeof(line));
    ag_strlcat(line, ag_strerror(err), sizeof(line));
    set_status(line);
}

static void free_buffer(void)
{
    if (s_lines != NULL) {
        for (int i = 0; i < s_count; i++) {
            ag_free(s_lines[i]);
        }
        ag_free(s_lines);
    }
    s_lines = NULL;
    s_count = 0;
    s_cap = 0;
    s_bytes = 0;
    s_row = 0;
    s_col = 0;
    s_top = 0;
    s_left = 0;
    s_dirty = false;
}

static bool ensure_line_cap(int need)
{
    if (need <= s_cap) {
        return true;
    }
    int ncap = (s_cap > 0) ? s_cap : 32;
    while (ncap < need) {
        ncap *= 2;
    }
    if (ncap > EDIT_MAX_LINES) {
        ncap = EDIT_MAX_LINES;
    }
    if (need > ncap) {
        set_status("too many lines");
        return false;
    }

    char **nlines = (char **)ag_malloc((size_t)ncap * sizeof(char *));
    if (nlines == NULL) {
        set_status("out of memory");
        return false;
    }
    for (int i = 0; i < s_count; i++) {
        nlines[i] = s_lines[i];
    }
    for (int i = s_count; i < ncap; i++) {
        nlines[i] = NULL;
    }
    ag_free(s_lines);
    s_lines = nlines;
    s_cap = ncap;
    return true;
}

static char *dup_line(const char *s, size_t n)
{
    char *p = (char *)ag_malloc(n + 1u);
    if (p == NULL) {
        return NULL;
    }
    if (n > 0) {
        memcpy(p, s, n);
    }
    p[n] = '\0';
    return p;
}

static bool add_empty_line(void)
{
    if (s_count >= EDIT_MAX_LINES) {
        set_status("too many lines");
        return false;
    }
    if (!ensure_line_cap(s_count + 1)) {
        return false;
    }
    char *line = dup_line("", 0);
    if (line == NULL) {
        set_status("out of memory");
        return false;
    }
    s_lines[s_count++] = line;
    return true;
}

static int line_len(int row)
{
    return (row >= 0 && row < s_count && s_lines[row] != NULL)
               ? (int)strlen(s_lines[row])
               : 0;
}

static void clamp_cursor(void)
{
    if (s_count <= 0) {
        s_row = 0;
        s_col = 0;
        return;
    }
    if (s_row < 0) {
        s_row = 0;
    }
    if (s_row >= s_count) {
        s_row = s_count - 1;
    }
    const int len = line_len(s_row);
    if (s_col < 0) {
        s_col = 0;
    }
    if (s_col > len) {
        s_col = len;
    }

    if (s_row < s_top) {
        s_top = s_row;
    }
    if (s_row >= s_top + EDIT_TEXT_ROWS) {
        s_top = s_row - EDIT_TEXT_ROWS + 1;
    }
    if (s_top < 0) {
        s_top = 0;
    }

    if (s_col < s_left) {
        s_left = s_col;
    }
    if (s_col >= s_left + EDIT_COLS) {
        s_left = s_col - EDIT_COLS + 1;
    }
    if (s_left < 0) {
        s_left = 0;
    }
}

/* ---------------------------------------------------------------------- */
/* Load / save                                                             */
/* ---------------------------------------------------------------------- */

static bool load_file(const char *path)
{
    free_buffer();
    ag_strlcpy(s_path, path, sizeof(s_path));

    const ag_handle_t h = ag_open(path, AG_O_RDONLY);
    if (h == -AG_ENOENT) {
        /* New file: one empty line, nothing on disk yet. */
        if (!add_empty_line()) {
            return false;
        }
        set_status("new file");
        return true;
    }
    if (h < 0) {
        set_error(path, h);
        (void)add_empty_line();
        return false;
    }

    char    *buf = (char *)ag_malloc(EDIT_MAX_BYTES + 1u);
    if (buf == NULL) {
        ag_close(h);
        set_status("out of memory");
        (void)add_empty_line();
        return false;
    }

    size_t got = 0;
    bool   truncated = false;
    while (got < EDIT_MAX_BYTES) {
        const int32_t n = ag_read(h, buf + got, EDIT_MAX_BYTES - got);
        if (n < 0) {
            ag_close(h);
            ag_free(buf);
            set_error("read", n);
            (void)add_empty_line();
            return false;
        }
        if (n == 0) {
            break;
        }
        got += (size_t)n;
    }
    if (got == EDIT_MAX_BYTES) {
        char probe;
        if (ag_read(h, &probe, 1) > 0) {
            truncated = true;
        }
    }
    ag_close(h);
    buf[got] = '\0';

    /*
     * A final newline ends the last line; it does not create an extra empty
     * one.  So "hi\n" loads as one line, and saving it writes "hi\n" again.
     */
    size_t at = 0;
    while (at < got) {
        size_t end = at;
        while (end < got && buf[end] != '\n') {
            end++;
        }
        size_t len = end - at;
        if (len > 0 && buf[at + len - 1u] == '\r') {
            len--;
        }
        if (len > EDIT_LINE_MAX - 1u) {
            len = EDIT_LINE_MAX - 1u;
        }
        if (s_count >= EDIT_MAX_LINES) {
            truncated = true;
            break;
        }
        if (!ensure_line_cap(s_count + 1)) {
            ag_free(buf);
            free_buffer();
            (void)add_empty_line();
            return false;
        }
        char *line = dup_line(buf + at, len);
        if (line == NULL) {
            ag_free(buf);
            free_buffer();
            set_status("out of memory");
            (void)add_empty_line();
            return false;
        }
        s_lines[s_count++] = line;
        s_bytes += len;
        if (end >= got) {
            break;
        }
        at = end + 1u;
    }
    ag_free(buf);

    if (s_count == 0 && !add_empty_line()) {
        return false;
    }

    s_dirty = false;
    clamp_cursor();
    set_status(truncated ? "loaded (truncated to 64 KB)" : "loaded");
    return true;
}

static bool save_file(void)
{
    if (s_path[0] == '\0') {
        set_status("no file name");
        return false;
    }

    const ag_handle_t h =
        ag_open(s_path, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (h < 0) {
        set_error("save", h);
        return false;
    }

    for (int i = 0; i < s_count; i++) {
        const char  *line = s_lines[i];
        const size_t len = strlen(line);
        if (len > 0) {
            const int32_t n = ag_write(h, line, len);
            if (n < 0 || (size_t)n != len) {
                ag_close(h);
                set_error("write", (n < 0) ? n : -AG_EIO);
                return false;
            }
        }
        /* A newline after every line, including the last: that is what `type`
         * and the next load both expect, and it keeps an empty final line. */
        const int32_t n = ag_write(h, "\n", 1);
        if (n != 1) {
            ag_close(h);
            set_error("write", (n < 0) ? n : -AG_EIO);
            return false;
        }
    }
    ag_close(h);
    s_dirty = false;
    set_status("saved");
    return true;
}

/* ---------------------------------------------------------------------- */
/* Drawing                                                                 */
/* ---------------------------------------------------------------------- */

static void put_clipped(int x, int y, int width, const char *s, uint8_t attr)
{
    int i = 0;
    for (; s != NULL && s[i] != '\0' && i < width; i++) {
        ag_poke((uint16_t)(x + i), (uint16_t)y, s[i], attr);
    }
    for (; i < width; i++) {
        ag_poke((uint16_t)(x + i), (uint16_t)y, ' ', attr);
    }
}

static void draw_text(void)
{
    for (int row = 0; row < EDIT_TEXT_ROWS; row++) {
        const int index = s_top + row;
        char      out[EDIT_COLS + 1];
        int       n = 0;

        if (index < s_count && s_lines[index] != NULL) {
            const char *line = s_lines[index];
            const int   len = (int)strlen(line);
            for (int i = s_left; i < len && n < EDIT_COLS; i++) {
                const unsigned char c = (unsigned char)line[i];
                if (c == '\t') {
                    do {
                        out[n++] = ' ';
                    } while ((n % 4) != 0 && n < EDIT_COLS);
                } else if (c >= 0x20) {
                    out[n++] = (char)c;
                } else {
                    out[n++] = '.';
                }
            }
        }
        out[n] = '\0';
        put_clipped(0, row, EDIT_COLS, out, EDIT_ATTR_TEXT);
    }

    /* The cursor is a highlighted cell - the hardware cursor stays hidden. */
    const int cy = s_row - s_top;
    const int cx = s_col - s_left;
    if (cy >= 0 && cy < EDIT_TEXT_ROWS && cx >= 0 && cx < EDIT_COLS) {
        char ch = ' ';
        if (s_row < s_count && s_lines[s_row] != NULL) {
            const int len = line_len(s_row);
            if (s_col < len) {
                ch = s_lines[s_row][s_col];
                if ((unsigned char)ch < 0x20) {
                    ch = ' ';
                }
            }
        }
        ag_poke((uint16_t)cx, (uint16_t)cy, ch, EDIT_ATTR_CURSOR);
    }
}

static void draw_chrome(void)
{
    put_clipped(0, EDIT_ROW_KEYS, EDIT_COLS,
                " F1 Help  F2 Save  Esc/F10 Quit   arrows move   Enter new line",
                EDIT_ATTR_KEYS);

    char       line[EDIT_COLS];
    char       num[16];
    const char *name = (s_path[0] != '\0') ? s_path : "(untitled)";

    ag_strlcpy(line, " ", sizeof(line));
    ag_strlcat(line, name, sizeof(line));
    if (s_dirty) {
        ag_strlcat(line, " *", sizeof(line));
    }
    ag_strlcat(line, "  Ln ", sizeof(line));
    ag_strlcat(line,
               ag_utoa((uint64_t)(s_row + 1), num, sizeof(num), 0, false),
               sizeof(line));
    ag_strlcat(line, " Col ", sizeof(line));
    ag_strlcat(line,
               ag_utoa((uint64_t)(s_col + 1), num, sizeof(num), 0, false),
               sizeof(line));
    ag_strlcat(line, "  ", sizeof(line));
    ag_strlcat(line, s_status, sizeof(line));

    put_clipped(0, EDIT_ROW_STATUS, EDIT_COLS, line, EDIT_ATTR_STATUS);
}

static void redraw(void)
{
    clamp_cursor();
    draw_text();
    draw_chrome();
}

/* ---------------------------------------------------------------------- */
/* Edits                                                                   */
/* ---------------------------------------------------------------------- */

static bool replace_line(int row, const char *text)
{
    const size_t n = strlen(text);
    if (n >= EDIT_LINE_MAX) {
        set_status("line too long");
        return false;
    }
    const size_t old = (size_t)line_len(row);
    if (s_bytes - old + n > EDIT_MAX_BYTES) {
        set_status("file too large (64 KB)");
        return false;
    }
    char *copy = dup_line(text, n);
    if (copy == NULL) {
        set_status("out of memory");
        return false;
    }
    ag_free(s_lines[row]);
    s_lines[row] = copy;
    s_bytes = s_bytes - old + n;
    s_dirty = true;
    return true;
}

static void insert_char(char ch)
{
    if (s_count == 0 && !add_empty_line()) {
        return;
    }
    const int len = line_len(s_row);
    if (len + 1 >= EDIT_LINE_MAX) {
        set_status("line too long");
        return;
    }
    if (s_bytes + 1u > EDIT_MAX_BYTES) {
        set_status("file too large (64 KB)");
        return;
    }

    char tmp[EDIT_LINE_MAX];
    const char *src = s_lines[s_row];
    if (s_col > len) {
        s_col = len;
    }
    memcpy(tmp, src, (size_t)s_col);
    tmp[s_col] = ch;
    memcpy(tmp + s_col + 1, src + s_col, (size_t)(len - s_col));
    tmp[len + 1] = '\0';

    if (!replace_line(s_row, tmp)) {
        return;
    }
    s_col++;
    set_status("");
}

static void do_backspace(void)
{
    if (s_col > 0) {
        const int   len = line_len(s_row);
        char        tmp[EDIT_LINE_MAX];
        const char *src = s_lines[s_row];
        memcpy(tmp, src, (size_t)(s_col - 1));
        memcpy(tmp + s_col - 1, src + s_col, (size_t)(len - s_col));
        tmp[len - 1] = '\0';
        if (replace_line(s_row, tmp)) {
            s_col--;
            set_status("");
        }
        return;
    }
    if (s_row == 0) {
        return;
    }

    /* Join with the previous line. */
    const int prev = s_row - 1;
    const int plen = line_len(prev);
    const int clen = line_len(s_row);
    if (plen + clen >= EDIT_LINE_MAX) {
        set_status("line too long");
        return;
    }
    char tmp[EDIT_LINE_MAX];
    memcpy(tmp, s_lines[prev], (size_t)plen);
    memcpy(tmp + plen, s_lines[s_row], (size_t)clen);
    tmp[plen + clen] = '\0';

    if (!replace_line(prev, tmp)) {
        return;
    }
    ag_free(s_lines[s_row]);
    for (int i = s_row; i + 1 < s_count; i++) {
        s_lines[i] = s_lines[i + 1];
    }
    s_count--;
    s_lines[s_count] = NULL;
    s_row = prev;
    s_col = plen;
    s_dirty = true;
    set_status("");
}

static void do_delete(void)
{
    const int len = line_len(s_row);
    if (s_col < len) {
        char        tmp[EDIT_LINE_MAX];
        const char *src = s_lines[s_row];
        memcpy(tmp, src, (size_t)s_col);
        memcpy(tmp + s_col, src + s_col + 1, (size_t)(len - s_col - 1));
        tmp[len - 1] = '\0';
        if (replace_line(s_row, tmp)) {
            set_status("");
        }
        return;
    }
    if (s_row + 1 >= s_count) {
        return;
    }

    const int nlen = line_len(s_row + 1);
    if (len + nlen >= EDIT_LINE_MAX) {
        set_status("line too long");
        return;
    }
    char tmp[EDIT_LINE_MAX];
    memcpy(tmp, s_lines[s_row], (size_t)len);
    memcpy(tmp + len, s_lines[s_row + 1], (size_t)nlen);
    tmp[len + nlen] = '\0';
    if (!replace_line(s_row, tmp)) {
        return;
    }
    ag_free(s_lines[s_row + 1]);
    for (int i = s_row + 1; i + 1 < s_count; i++) {
        s_lines[i] = s_lines[i + 1];
    }
    s_count--;
    s_lines[s_count] = NULL;
    s_dirty = true;
    set_status("");
}

static void do_enter(void)
{
    if (s_count >= EDIT_MAX_LINES) {
        set_status("too many lines");
        return;
    }
    const int   len = line_len(s_row);
    const char *src = s_lines[s_row];
    if (s_col > len) {
        s_col = len;
    }

    char left[EDIT_LINE_MAX];
    char right[EDIT_LINE_MAX];
    memcpy(left, src, (size_t)s_col);
    left[s_col] = '\0';
    memcpy(right, src + s_col, (size_t)(len - s_col));
    right[len - s_col] = '\0';

    if (!ensure_line_cap(s_count + 1)) {
        return;
    }
    char *newline = dup_line(right, strlen(right));
    if (newline == NULL) {
        set_status("out of memory");
        return;
    }
    if (!replace_line(s_row, left)) {
        ag_free(newline);
        return;
    }
    for (int i = s_count; i > s_row + 1; i--) {
        s_lines[i] = s_lines[i - 1];
    }
    s_lines[s_row + 1] = newline;
    s_count++;
    s_row++;
    s_col = 0;
    s_dirty = true;
    set_status("");
}

/* ---------------------------------------------------------------------- */
/* Dialogs                                                                 */
/* ---------------------------------------------------------------------- */

static bool confirm(const char *question)
{
    char line[EDIT_COLS];

    ag_strlcpy(line, question, sizeof(line));
    ag_strlcat(line, "  [y/N]", sizeof(line));
    put_clipped(0, EDIT_ROW_STATUS, EDIT_COLS, line, EDIT_ATTR_DIALOG);

    for (;;) {
        ag_event_t ev;
        if (!ag_poll_event(&ev, 50)) {
            ag_heartbeat();
            continue;
        }
        if (ev.type == AG_EV_FOCUS_GAINED) {
            put_clipped(0, EDIT_ROW_STATUS, EDIT_COLS, line, EDIT_ATTR_DIALOG);
            continue;
        }
        if (ev.type == AG_EV_QUIT) {
            if (!ag_focused()) {
                continue;
            }
            return false;
        }
        if (!ag_focused() || ev.type != AG_EV_KEY_DOWN) {
            continue;
        }
        if (ev.key.keycode == AG_KEY_Y) {
            return true;
        }
        return false;
    }
}

static bool ask_path(char *out, size_t out_len)
{
    char   buf[AG_PATH_MAX];
    size_t at = 0;
    buf[0] = '\0';

    for (;;) {
        char line[EDIT_COLS];
        ag_strlcpy(line, " File: ", sizeof(line));
        ag_strlcat(line, buf, sizeof(line));
        put_clipped(0, EDIT_ROW_STATUS, EDIT_COLS, line, EDIT_ATTR_DIALOG);
        const int cur = 7 + (int)at;
        if (cur < EDIT_COLS) {
            ag_poke((uint16_t)cur, EDIT_ROW_STATUS, '_', EDIT_ATTR_CURSOR);
        }

        ag_event_t ev;
        if (!ag_poll_event(&ev, 50)) {
            ag_heartbeat();
            continue;
        }
        if (ev.type == AG_EV_FOCUS_GAINED) {
            continue; /* redraw on next loop iteration */
        }
        if (ev.type == AG_EV_QUIT) {
            if (!ag_focused()) {
                continue;
            }
            return false;
        }
        if (!ag_focused() || ev.type != AG_EV_KEY_DOWN) {
            continue;
        }
        if (ev.key.keycode == AG_KEY_ESC) {
            return false;
        }
        if (ev.key.keycode == AG_KEY_ENTER) {
            if (at == 0) {
                continue;
            }
            ag_strlcpy(out, buf, out_len);
            return true;
        }
        if (ev.key.keycode == AG_KEY_BACKSPACE) {
            if (at > 0) {
                buf[--at] = '\0';
            }
            continue;
        }
        if (ev.key.unicode >= 0x20 && ev.key.unicode < 0x7f &&
            at + 1u < sizeof(buf) && at + 1u < out_len) {
            buf[at++] = (char)ev.key.unicode;
            buf[at] = '\0';
        }
    }
}

static void show_help(void)
{
    static const char *const lines[] = {
        " ArgonOS editor",
        "",
        "  Arrows, Home/End, PgUp/PgDn  move",
        "  Enter                       split line",
        "  Backspace / Delete          delete",
        "  Tab                         four spaces",
        "  F2 or Ctrl+S                save",
        "  Esc or F10                  quit",
        "",
        " Files are at most 64 KB and 2048 lines.",
        " Typed characters are the code page's",
        " single-byte set (ASCII for source).",
        "",
        " Press any key to return.",
    };
    const int n = (int)(sizeof(lines) / sizeof(lines[0]));

    ag_fill(0, 0, EDIT_COLS, EDIT_ROWS, ' ', EDIT_ATTR_DIALOG);
    for (int i = 0; i < n; i++) {
        put_clipped(0, i + 2, EDIT_COLS, lines[i], EDIT_ATTR_DIALOG);
    }
    for (;;) {
        ag_event_t ev;
        if (!ag_poll_event(&ev, 50)) {
            ag_heartbeat();
            continue;
        }
        if (ev.type == AG_EV_FOCUS_GAINED) {
            ag_fill(0, 0, EDIT_COLS, EDIT_ROWS, ' ', EDIT_ATTR_DIALOG);
            for (int i = 0; i < n; i++) {
                put_clipped(0, i + 2, EDIT_COLS, lines[i], EDIT_ATTR_DIALOG);
            }
            continue;
        }
        if (ev.type == AG_EV_QUIT) {
            if (!ag_focused()) {
                continue;
            }
            break;
        }
        if (ag_focused() && ev.type == AG_EV_KEY_DOWN) {
            break;
        }
    }
}

static bool try_quit(void)
{
    if (!s_dirty) {
        return true;
    }
    /* Save is F2; quitting only asks whether to throw the work away. */
    return confirm("Abandon unsaved changes?");
}

/* ---------------------------------------------------------------------- */
/* Main loop                                                               */
/* ---------------------------------------------------------------------- */

static void handle_key(const ag_event_t *ev)
{
    const uint16_t key = ev->key.keycode;
    const uint16_t mods = ev->key.mods;
    const bool     ctrl = (mods & AG_MOD_CTRL) != 0;

    if (key == AG_KEY_F1) {
        show_help();
        return;
    }
    if (key == AG_KEY_F2 || (ctrl && key == AG_KEY_S)) {
        (void)save_file();
        return;
    }
    if (key == AG_KEY_ESC || key == AG_KEY_F10) {
        if (try_quit()) {
            s_running = false;
        }
        return;
    }

    switch (key) {
    case AG_KEY_UP:
        s_row--;
        break;
    case AG_KEY_DOWN:
        s_row++;
        break;
    case AG_KEY_LEFT:
        if (s_col > 0) {
            s_col--;
        } else if (s_row > 0) {
            s_row--;
            s_col = line_len(s_row);
        }
        break;
    case AG_KEY_RIGHT: {
        const int len = line_len(s_row);
        if (s_col < len) {
            s_col++;
        } else if (s_row + 1 < s_count) {
            s_row++;
            s_col = 0;
        }
        break;
    }
    case AG_KEY_HOME:
        s_col = 0;
        break;
    case AG_KEY_END:
        s_col = line_len(s_row);
        break;
    case AG_KEY_PAGEUP:
        s_row -= EDIT_TEXT_ROWS;
        break;
    case AG_KEY_PAGEDOWN:
        s_row += EDIT_TEXT_ROWS;
        break;
    case AG_KEY_ENTER:
    case AG_KEY_KP_ENTER:
        do_enter();
        break;
    case AG_KEY_BACKSPACE:
        do_backspace();
        break;
    case AG_KEY_DELETE:
        do_delete();
        break;
    case AG_KEY_TAB:
        for (int i = 0; i < 4; i++) {
            insert_char(' ');
        }
        break;
    default:
        if (!ctrl && ev->key.unicode >= 0x20 && ev->key.unicode < 0x7f) {
            insert_char((char)ev->key.unicode);
        }
        break;
    }
}

int EDIT_ENTRY(int argc, char **argv)
{
    char path[AG_PATH_MAX];
    path[0] = '\0';

    if (argc >= 2 && argv[1] != NULL && argv[1][0] != '\0') {
        ag_strlcpy(path, argv[1], sizeof(path));
    }

    ag_cursor(false);
    ag_cls();
    set_status("");

    if (path[0] == '\0') {
        redraw();
        if (!ask_path(path, sizeof(path))) {
            ag_cls();
            ag_cursor(true);
            ag_color(AG_LGRAY, AG_BLACK);
            return 1;
        }
    }

    if (!load_file(path) && s_count == 0) {
        ag_cls();
        ag_cursor(true);
        ag_color(AG_LGRAY, AG_BLACK);
        ag_print(s_status);
        ag_print("\n");
        return 1;
    }

    s_running = true;
    while (s_running) {
        redraw();

        ag_event_t ev;
        if (!ag_poll_event(&ev, 50)) {
            ag_heartbeat();
            continue;
        }
        if (ev.type == AG_EV_FOCUS_GAINED) {
            continue; /* redraw() at top of loop */
        }
        if (ev.type == AG_EV_QUIT) {
            if (!ag_focused()) {
                continue;
            }
            if (!s_dirty || confirm("Discard unsaved changes?")) {
                break;
            }
            continue;
        }
        if (!ag_focused() || ev.type != AG_EV_KEY_DOWN) {
            continue;
        }
        handle_key(&ev);
    }

    free_buffer();
    ag_cls();
    ag_cursor(true);
    ag_color(AG_LGRAY, AG_BLACK);
    return 0;
}
