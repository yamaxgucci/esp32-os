/*
 * ArgonOS DESKTOP - the folder window.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "dsk_folder.h"

#include <argon/argon.h>
#include <argon/libc.h>

#include "dsk_cursor.h"
#include "dsk_icons.h"
#include "dsk_paint.h"

#define ROW_H     16
#define BAR_W     12  /* the scroll bar, and its arrows are square */
#define NAME_MAX  64
#define ENTRY_MAX 512 /* 40 KB of entries; a bigger directory is truncated */

typedef struct {
    char       name[NAME_MAX];
    uint64_t   size;
    bool       is_dir;
    dsk_icon_t icon;
} entry_t;

typedef struct {
    char     path[AG_PATH_MAX];
    entry_t *entries;
    int      n;
    int      sel;
    int      top;      /* the first row on screen */
    bool     truncated;
    bool     used;
} folder_t;

#define FOLDER_MAX 8
static folder_t s_folders[FOLDER_MAX];

static dsk_metrics_t      s_m;
static dsk_folder_open_fn s_open_file;

/* ---- reading ----------------------------------------------------------- */

static int cmp_entry(const entry_t *a, const entry_t *b)
{
    /* Directories first, then by name, and ".." before every directory. */
    const bool a_up = (a->name[0] == '.' && a->name[1] == '.');
    const bool b_up = (b->name[0] == '.' && b->name[1] == '.');
    if (a_up != b_up) {
        return a_up ? -1 : 1;
    }
    if (a->is_dir != b->is_dir) {
        return a->is_dir ? -1 : 1;
    }
    return ag_stricmp(a->name, b->name);
}

static void sort_entries(entry_t *e, int n)
{
    /* Insertion sort: a directory is a few hundred entries and this is not
     * the slow part - reading them is. */
    for (int i = 1; i < n; i++) {
        entry_t key = e[i];
        int     j = i - 1;
        while (j >= 0 && cmp_entry(&e[j], &key) > 0) {
            e[j + 1] = e[j];
            j--;
        }
        e[j + 1] = key;
    }
}

/* Is this the root of a drive, so that there is no ".." to offer? */
static bool is_root(const char *path)
{
    /* "c:\" and "c:/" and "/" are roots; "c:\x" is not. */
    if (path[0] == '\0') {
        return true;
    }
    if (path[1] == ':' && (path[2] == '\0' ||
                           ((path[2] == '\\' || path[2] == '/') &&
                            path[3] == '\0'))) {
        return true;
    }
    return (path[0] == '/' && path[1] == '\0');
}

static void read_dir(folder_t *f)
{
    /*
     * The hourglass goes up first.  A directory on HostFS is hundreds of
     * milliseconds and the shell is not answering anything while it reads;
     * without this it looks like it has stopped.
     */
    dsk_cursor_shape(DSK_CUR_WAIT);

    f->n = 0;
    f->sel = 0;
    f->top = 0;
    f->truncated = false;
    if (f->entries == NULL) {
        f->entries = (entry_t *)ag_malloc(sizeof(entry_t) * ENTRY_MAX);
        if (f->entries == NULL) {
            dsk_cursor_shape(DSK_CUR_ARROW);
            return;
        }
    }

    if (!is_root(f->path)) {
        entry_t *e = &f->entries[f->n++];
        ag_strlcpy(e->name, "..", sizeof(e->name));
        e->size = 0;
        e->is_dir = true;
        e->icon = DSK_ICON_UP;
    }

    const ag_handle_t d = ag_opendir(f->path);
    if (d >= 0) {
        ag_dirent_t de;
        while (ag_readdir(d, &de) == AG_OK) {
            if (de.name[0] == '\0' || ag_strcmp(de.name, ".") == 0 ||
                ag_strcmp(de.name, "..") == 0) {
                continue;
            }
            if (f->n >= ENTRY_MAX) {
                f->truncated = true;
                break;
            }
            entry_t *e = &f->entries[f->n++];
            ag_strlcpy(e->name, de.name, sizeof(e->name));
            e->size = de.st.size;
            e->is_dir = ((de.st.attr & AG_A_DIR) != 0);
            e->icon = e->is_dir ? DSK_ICON_FOLDER : dsk_icon_for(e->name);
        }
        (void)ag_closedir(d);
    }
    sort_entries(f->entries, f->n);
    dsk_cursor_shape(DSK_CUR_ARROW);
}

/* ---- geometry ---------------------------------------------------------- */

static dsk_rect_t list_rect(dsk_win_t *w)
{
    const dsk_rect_t c = dsk_wm_client(w);
    if (dsk_rect_empty(c)) {
        return c;
    }
    return dsk_rect(c.x, c.y, (int16_t)(c.w - BAR_W), c.h);
}

static dsk_rect_t bar_rect(dsk_win_t *w)
{
    const dsk_rect_t c = dsk_wm_client(w);
    if (dsk_rect_empty(c) || c.w <= BAR_W) {
        return dsk_rect_none();
    }
    return dsk_rect((int16_t)(dsk_rect_x2(c) - BAR_W), c.y, BAR_W, c.h);
}

static int rows_visible(dsk_win_t *w)
{
    const dsk_rect_t l = list_rect(w);
    return dsk_rect_empty(l) ? 0 : (l.h / ROW_H);
}

static void clamp_scroll(dsk_win_t *w, folder_t *f)
{
    const int rows = rows_visible(w);
    int       max_top = f->n - rows;
    if (max_top < 0) {
        max_top = 0;
    }
    if (f->top > max_top) {
        f->top = max_top;
    }
    if (f->top < 0) {
        f->top = 0;
    }
    /* Keep the selection on screen: that is what an arrow key means. */
    if (f->sel < f->top) {
        f->top = f->sel;
    }
    if (rows > 0 && f->sel >= f->top + rows) {
        f->top = f->sel - rows + 1;
    }
}

/* ---- drawing ----------------------------------------------------------- */

static void draw_bar(dsk_win_t *w, folder_t *f)
{
    const dsk_rect_t b = bar_rect(w);
    if (dsk_rect_empty(b) || b.h < 3 * BAR_W) {
        return;
    }
    dsk_fill(b, DSK_LGRAY);
    const dsk_rect_t up = dsk_rect(b.x, b.y, BAR_W, BAR_W);
    const dsk_rect_t dn =
        dsk_rect(b.x, (int16_t)(dsk_rect_y2(b) - BAR_W), BAR_W, BAR_W);
    dsk_panel(up, true, DSK_LGRAY);
    dsk_panel(dn, true, DSK_LGRAY);
    /* Two little triangles, drawn as stacked lines. */
    for (int i = 0; i < 4; i++) {
        dsk_hline((int16_t)(up.x + 6 - i), (int16_t)(up.y + 4 + i),
                  (int16_t)(1 + 2 * i), DSK_BLACK);
        dsk_hline((int16_t)(dn.x + 3 + i), (int16_t)(dn.y + 4 + i),
                  (int16_t)(7 - 2 * i), DSK_BLACK);
    }

    /* The thumb: where the visible rows are in the whole list. */
    const int16_t track_y = (int16_t)(b.y + BAR_W);
    const int16_t track_h = (int16_t)(b.h - 2 * BAR_W);
    const int     rows = rows_visible(w);
    if (track_h <= 0 || f->n <= rows) {
        return;
    }
    int16_t th = (int16_t)((int32_t)track_h * rows / f->n);
    if (th < 8) {
        th = 8;
    }
    if (th > track_h) {
        th = track_h;
    }
    const int     span = f->n - rows;
    const int16_t ty =
        (int16_t)(track_y + (int32_t)(track_h - th) * f->top / (span > 0 ? span : 1));
    dsk_panel(dsk_rect(b.x, ty, BAR_W, th), true, DSK_LGRAY);
}

static void draw_folder(dsk_win_t *w, dsk_rect_t client)
{
    folder_t *f = (folder_t *)w->user;
    const dsk_rect_t l = list_rect(w);
    char             num[24];

    dsk_fill(client, DSK_WHITE);
    if (f == NULL || f->entries == NULL) {
        dsk_text((int16_t)(client.x + 4), (int16_t)(client.y + 4),
                 "no memory for this directory", DSK_BLACK, DSK_WHITE);
        return;
    }

    const int rows = rows_visible(w);
    for (int i = 0; i < rows; i++) {
        const int at = f->top + i;
        if (at >= f->n) {
            break;
        }
        const entry_t   *e = &f->entries[at];
        const bool       lit = (at == f->sel);
        const dsk_rect_t r =
            dsk_rect(l.x, (int16_t)(l.y + i * ROW_H), l.w, ROW_H);
        const uint32_t bg = lit ? DSK_NAVY : DSK_WHITE;
        const uint32_t fg = lit ? DSK_WHITE : DSK_BLACK;

        if (lit) {
            dsk_fill(r, DSK_NAVY);
        }
        dsk_icon_draw(e->icon, r.x, r.y, 1, bg);
        /* The size column is only worth its room when there is room. */
        const int16_t size_w = (r.w > 200) ? 72 : 0;
        dsk_text_fit((int16_t)(r.x + DSK_ICON_W + 2), r.y,
                     (int16_t)(r.w - DSK_ICON_W - 6 - size_w), e->name, fg, bg);
        if (size_w > 0 && !e->is_dir) {
            const char *s = ag_utoa(e->size, num, sizeof(num), 0, true);
            int16_t     n = 0;
            while (s[n] != '\0') {
                n++;
            }
            dsk_text_fit((int16_t)(dsk_rect_x2(r) - 4 - n * DSK_FONT_W), r.y,
                         (int16_t)(n * DSK_FONT_W), s, fg, bg);
        }
    }

    if (f->n == 0) {
        dsk_text((int16_t)(l.x + 4), (int16_t)(l.y + 4), "(empty)", DSK_DGRAY,
                 DSK_WHITE);
    }
    if (f->truncated) {
        dsk_text_small((int16_t)(l.x + 4), (int16_t)(dsk_rect_y2(l) - 9),
                       "too many files to list", DSK_MAROON, DSK_WHITE);
    }
    draw_bar(w, f);
}

/* ---- opening what is selected ------------------------------------------ */

/* dir + name, in the DOS spelling the shell uses everywhere else. */
static void join(const char *dir, const char *name, char *out, size_t len)
{
    ag_strlcpy(out, dir, len);
    size_t n = 0;
    while (out[n] != '\0') {
        n++;
    }
    if (n > 0 && out[n - 1] != '\\' && out[n - 1] != '/') {
        ag_strlcat(out, "\\", len);
    }
    ag_strlcat(out, name, len);
}

/* One directory up, in place.  False when there is nowhere to go. */
static bool parent_of(const char *path, char *out, size_t len)
{
    ag_strlcpy(out, path, len);
    int n = 0;
    while (out[n] != '\0') {
        n++;
    }
    /* Drop a trailing separator, then everything after the last one. */
    while (n > 0 && (out[n - 1] == '\\' || out[n - 1] == '/')) {
        out[--n] = '\0';
    }
    while (n > 0 && out[n - 1] != '\\' && out[n - 1] != '/') {
        out[--n] = '\0';
    }
    if (n == 0) {
        return false;
    }
    /* "c:\" stays "c:\"; anything deeper loses its trailing separator. */
    if (n > 3) {
        out[n - 1] = '\0';
    }
    return true;
}

static void activate(dsk_win_t *w, int which)
{
    folder_t *f = (folder_t *)w->user;
    if (f == NULL || which < 0 || which >= f->n) {
        return;
    }
    const entry_t *e = &f->entries[which];
    char           path[AG_PATH_MAX];

    if (e->is_dir && e->name[0] == '.' && e->name[1] == '.') {
        if (parent_of(f->path, path, sizeof(path))) {
            dsk_folder_go(w, path);
        }
        return;
    }
    join(f->path, e->name, path, sizeof(path));
    if (e->is_dir) {
        dsk_folder_go(w, path);
        return;
    }
    if (s_open_file != NULL) {
        s_open_file(path, false);
    }
}

/* ---- input ------------------------------------------------------------- */

static bool folder_pointer(dsk_win_t *w, dsk_hit_t where, int16_t x, int16_t y,
                           uint8_t buttons, bool down, bool dbl)
{
    (void)buttons;
    folder_t *f = (folder_t *)w->user;
    if (f == NULL || where != DSK_HIT_CLIENT || !down) {
        return false;
    }
    const dsk_rect_t b = bar_rect(w);
    if (dsk_rect_has(b, x, y)) {
        const int rows = rows_visible(w);
        if (y < b.y + BAR_W) {
            f->top--;
        } else if (y >= dsk_rect_y2(b) - BAR_W) {
            f->top++;
        } else {
            /* On the track: a page towards where it was pressed. */
            const int16_t mid = (int16_t)(b.y + b.h / 2);
            f->top += (y < mid) ? -rows : rows;
        }
        clamp_scroll(w, f);
        dsk_wm_damage_rect(dsk_wm_client(w));
        return true;
    }

    const dsk_rect_t l = list_rect(w);
    if (!dsk_rect_has(l, x, y)) {
        return true;
    }
    const int row = (y - l.y) / ROW_H;
    const int at = f->top + row;
    if (at < 0 || at >= f->n) {
        return true;
    }
    if (f->sel != at) {
        f->sel = at;
        dsk_wm_damage_rect(dsk_wm_client(w));
    }
    if (dbl) {
        activate(w, at);
    }
    return true;
}

static bool folder_key(dsk_win_t *w, uint16_t keycode, uint32_t unicode,
                       uint16_t mods)
{
    (void)unicode;
    (void)mods;
    folder_t *f = (folder_t *)w->user;
    if (f == NULL) {
        return false;
    }
    const int rows = rows_visible(w);
    const int was_sel = f->sel;
    const int was_top = f->top;

    switch (keycode) {
    case DSK_KEY_UP:
        f->sel--;
        break;
    case DSK_KEY_DOWN:
        f->sel++;
        break;
    case 0x4Bu: /* PageUp */
        f->sel -= (rows > 1) ? rows - 1 : 1;
        break;
    case 0x4Eu: /* PageDown */
        f->sel += (rows > 1) ? rows - 1 : 1;
        break;
    case 0x4Au: /* Home */
        f->sel = 0;
        break;
    case 0x4Du: /* End */
        f->sel = f->n - 1;
        break;
    case DSK_KEY_ENTER:
        activate(w, f->sel);
        return true;
    case DSK_KEY_BACKSPACE: {
        char up[AG_PATH_MAX];
        if (!is_root(f->path) && parent_of(f->path, up, sizeof(up))) {
            dsk_folder_go(w, up);
        }
        return true;
    }
    case DSK_KEY_F5:
        read_dir(f);
        dsk_wm_damage_rect(dsk_wm_client(w));
        return true;
    default:
        return false;
    }

    if (f->sel < 0) {
        f->sel = 0;
    }
    if (f->sel >= f->n) {
        f->sel = (f->n > 0) ? f->n - 1 : 0;
    }
    clamp_scroll(w, f);
    if (f->sel != was_sel || f->top != was_top) {
        dsk_wm_damage_rect(dsk_wm_client(w));
    }
    return true;
}

static void folder_closed(dsk_win_t *w)
{
    folder_t *f = (folder_t *)w->user;
    if (f == NULL) {
        return;
    }
    if (f->entries != NULL) {
        ag_free(f->entries);
        f->entries = NULL;
    }
    f->used = false;
}

static const dsk_win_ops_t k_folder_ops = {
    .draw = draw_folder,
    .key = folder_key,
    .pointer = folder_pointer,
    .closed = folder_closed,
};

/* ---- the shell's side -------------------------------------------------- */

void dsk_folder_init(const dsk_metrics_t *m, dsk_folder_open_fn open_file)
{
    if (m != NULL) {
        s_m = *m;
    }
    s_open_file = open_file;
    for (int i = 0; i < FOLDER_MAX; i++) {
        s_folders[i].used = false;
        s_folders[i].entries = NULL;
    }
}

bool dsk_folder_is(const dsk_win_t *w)
{
    return w != NULL && w->ops == &k_folder_ops;
}

void dsk_folder_go(dsk_win_t *w, const char *path)
{
    folder_t *f = (folder_t *)w->user;
    if (f == NULL || path == NULL) {
        return;
    }
    ag_strlcpy(f->path, path, sizeof(f->path));
    read_dir(f);
    dsk_wm_set_title(w, f->path);
    dsk_wm_damage_rect(dsk_wm_client(w));
}

dsk_win_t *dsk_folder_open(const char *path)
{
    folder_t *f = NULL;
    for (int i = 0; i < FOLDER_MAX; i++) {
        if (!s_folders[i].used) {
            f = &s_folders[i];
            break;
        }
    }
    if (f == NULL || path == NULL) {
        return NULL;
    }
    f->used = true;
    ag_strlcpy(f->path, path, sizeof(f->path));

    /* Cascaded, and wide enough for a name and a size when the screen allows. */
    const int16_t step = (int16_t)(s_m.title_h + s_m.border);
    const int16_t w = (s_m.work.w < 280) ? (int16_t)(s_m.work.w - 8) : 280;
    const int16_t h = (s_m.work.h < 170) ? (int16_t)(s_m.work.h - 8) : 170;
    const int16_t at = (int16_t)(dsk_wm_count() % 5);

    dsk_win_t *win = dsk_wm_open(path,
                                 dsk_rect((int16_t)(s_m.work.x + 8 + at * step),
                                          (int16_t)(s_m.work.y + 8 + at * step),
                                          w, h),
                                 &k_folder_ops, f);
    if (win == NULL) {
        f->used = false;
        return NULL;
    }
    read_dir(f);
    dsk_wm_damage_rect(win->frame);
    return win;
}

void dsk_folder_refresh(const char *path)
{
    for (int z = 0; z < dsk_wm_count(); z++) {
        dsk_win_t *w = dsk_wm_at(z);
        if (!dsk_folder_is(w)) {
            continue;
        }
        folder_t *f = (folder_t *)w->user;
        if (path == NULL || ag_stricmp(f->path, path) == 0) {
            read_dir(f);
            dsk_wm_damage_rect(dsk_wm_client(w));
        }
    }
}
