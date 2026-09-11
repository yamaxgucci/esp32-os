/*
 * ArgonOS DESKTOP - the folder window.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "dsk_folder.h"

#include <argon/axe.h>

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
    /*
     * Which of this window's own icons is this entry's, or -1 for the one
     * its extension implies.  A program that carries a picture is drawn
     * with it (ag_axe_icon_t); the rest share the table.
     */
    int8_t     art;
    /*
     * Picked out for an operation, as against the cursor, which is only
     * where the keyboard is.  One byte an entry, and it lives on the entry
     * so that re-reading the directory forgets it - which is right: after a
     * refresh the fourth row is not the file that was the fourth row.
     */
    bool       marked;
} entry_t;

/*
 * How many programs in one directory may show their own face.
 *
 * Every one of them costs an open, a read of the header and a read of the
 * picture, at the moment the directory is listed - and 260 bytes to keep
 * it in.  Eight is what fits on a screen at once, which is the only place
 * an icon can be looked at; the ninth program falls back to the table and
 * nobody can tell until they scroll, by which time the read would have
 * been paid for anyway.
 */
#define ART_MAX 8
#define ART_PX  (DSK_ICON_W * DSK_ICON_H)

typedef struct {
    char     path[AG_PATH_MAX];
    entry_t *entries;
    int      cap;   /* entries the allocation actually holds */
    int      n;     /* entries in it */
    int      total; /* entries the directory has, listed or not */
    int      sel;
    int      marked;   /* how many entries carry a mark */
    int      top;      /* the first row on screen */
    bool     truncated;
    bool     used;
    uint8_t  art[ART_MAX][ART_PX];
    int      narts;
    /*
     * The rubber band, in screen coordinates.
     *
     * `anchor` is where the button went down and is kept whether or not a
     * band ever appears - a press that never moves is a click, and the
     * only way to know which it was is to wait and see.  `band` says the
     * press has travelled far enough to mean a selection.
     */
    bool     band;
    int16_t  ax, ay; /* the anchor */
    int16_t  bx, by; /* where the pointer is now */
    bool     pressed;
} folder_t;

/*
 * How far the pointer travels before a click becomes a band.
 *
 * The same reasoning as the icon drag: a hand that moved two pixels while
 * clicking has clicked.  On glass it is not two pixels - a stylus rolls,
 * a finger is eight pixels wide - and a band that appears under every tap
 * would flicker a selection rectangle over the whole shell.
 */
#define BAND_SLOP 5

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

/*
 * Twice the room, or false and the listing says it was cut short.
 *
 * Both blocks exist at once for the length of the copy, which is the cost
 * of not having realloc: doubling from 256 to 512 wants sixty kilobytes for
 * a moment to end up with forty.  That is why it doubles rather than adding
 * a fixed slice - the number of moments like that is the logarithm of the
 * directory, not its size.
 */
static bool grow(folder_t *f)
{
    if (f->cap >= ENTRY_MAX) {
        return false;
    }
    int want = f->cap * 2;
    if (want > ENTRY_MAX) {
        want = ENTRY_MAX;
    }
    entry_t *bigger = (entry_t *)ag_malloc(sizeof(entry_t) * (size_t)want);
    if (bigger == NULL) {
        return false;
    }
    for (int i = 0; i < f->n; i++) {
        bigger[i] = f->entries[i];
    }
    ag_free(f->entries);
    f->entries = bigger;
    f->cap = want;
    return true;
}

/*
 * The picture a program carries, if it carries one.
 *
 * Read straight out of the file with the loader nowhere in sight: the
 * header says where it is, and a shell has no business loading a program
 * in order to find out what it looks like.  Anything unexpected - an older
 * header with no room for the fields, a size that is not one icon, a file
 * that will not open - means "no picture", which is not an error: it is
 * every program built before today.
 */
static bool read_icon(const char *path, uint8_t *out)
{
    const ag_handle_t h = ag_open(path, AG_O_RDONLY);
    if (h < 0) {
        return false;
    }

    bool             ok = false;
    ag_axe_header_t  hdr;
    ag_axe_icon_t    pic;

    if (ag_read(h, &hdr, sizeof(hdr)) == (int32_t)sizeof(hdr) &&
        hdr.magic[0] == 'A' && hdr.magic[1] == 'X' && hdr.magic[2] == 'E' &&
        hdr.header_size >= (uint16_t)(offsetof(ag_axe_header_t, icon_size) +
                                      sizeof(uint32_t)) &&
        hdr.icon_offset != 0 && hdr.icon_size == sizeof(pic) + ART_PX &&
        ag_seek(h, (int64_t)hdr.icon_offset, AG_SEEK_SET) >= 0 &&
        ag_read(h, &pic, sizeof(pic)) == (int32_t)sizeof(pic) &&
        pic.magic[0] == 'A' && pic.magic[1] == 'X' && pic.magic[2] == 'I' &&
        pic.w == DSK_ICON_W && pic.h == DSK_ICON_H && pic.fmt == 0) {
        ok = ag_read(h, out, ART_PX) == (int32_t)ART_PX;
    }

    (void)ag_close(h);
    return ok;
}

/* dir + name, spelled the way this file spells it further down. */
static void join(const char *dir, const char *name, char *out, size_t len);

/* Does this name end in .AXE or .SYS?  Only those carry one. */
static bool is_program(const char *name)
{
    int n = 0;
    while (name[n] != '\0') {
        n++;
    }
    if (n < 4 || name[n - 4] != '.') {
        return false;
    }
    return ag_stricmp(&name[n - 3], "AXE") == 0 ||
           ag_stricmp(&name[n - 3], "SYS") == 0;
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
    /*
     * As much as this directory needs, and no more.
     *
     * This asked for five hundred and twelve entries - forty kilobytes in
     * one block - and came down by halves until the machine agreed.  It
     * fixed the window that would not open and left a worse thing behind:
     * a directory of eight files held forty kilobytes of nothing, out of a
     * hundred and eleven the whole shell has.  Paste then could not find
     * eight kilobytes for a copy buffer, on a board with plenty free.
     *
     * So it starts small and grows: thirty-two entries, doubling as the
     * directory turns out to be bigger, up to the ceiling.  A directory of
     * eight costs two and a half kilobytes, a directory of five hundred
     * costs what it did before, and neither is decided in advance.
     */
    if (f->entries == NULL) {
        f->entries = (entry_t *)ag_malloc(sizeof(entry_t) * 32u);
        if (f->entries == NULL) {
            dsk_cursor_shape(DSK_CUR_ARROW);
            return;
        }
        f->cap = 32;
    }
    f->total = 0;
    f->marked = 0;
    f->narts = 0;

    if (!is_root(f->path)) {
        /* The way up is the shell's row, not the directory's: it is in `n`
         * because it is on screen, and out of `total` because a count of a
         * directory is a count of what is in it. */
        entry_t *e = &f->entries[f->n++];
        ag_strlcpy(e->name, "..", sizeof(e->name));
        e->size = 0;
        e->is_dir = true;
        e->icon = DSK_ICON_UP;
        e->marked = false;
        e->art = -1;
    }

    const ag_handle_t d = ag_opendir(f->path);
    if (d >= 0) {
        ag_dirent_t de;
        while (ag_readdir(d, &de) == AG_OK) {
            if (de.name[0] == '\0' || ag_strcmp(de.name, ".") == 0 ||
                ag_strcmp(de.name, "..") == 0) {
                continue;
            }
            /*
             * Counted even when there is nowhere to put it: a listing that
             * says "seven objects" about a directory of nine is a listing
             * that lies, and the count is what the window is believed on.
             */
            f->total++;
            if (f->n >= f->cap && !grow(f)) {
                f->truncated = true;
                continue;
            }
            entry_t *e = &f->entries[f->n++];
            ag_strlcpy(e->name, de.name, sizeof(e->name));
            e->size = de.st.size;
            e->is_dir = ((de.st.attr & AG_A_DIR) != 0);
            e->icon = e->is_dir ? DSK_ICON_FOLDER : dsk_icon_for(e->name);
            e->art = -1;
            if (!e->is_dir && f->narts < ART_MAX && is_program(e->name)) {
                char whole[AG_PATH_MAX];
                join(f->path, e->name, whole, sizeof(whole));
                if (read_icon(whole, f->art[f->narts])) {
                    e->art = (int8_t)f->narts;
                    f->narts++;
                }
            }
            /*
             * Every field of a new entry is written here, and this one was
             * not: the array comes from ag_malloc and holds whatever was in
             * that memory, so a directory opened with rubbish in the mark
             * byte showed every row picked out while the count said none
             * were.  A field added to a structure that is filled by hand is
             * a field that has to be filled by hand.
             */
            e->marked = false;
        }
        (void)ag_closedir(d);
    }
    sort_entries(f->entries, f->n);
    dsk_cursor_shape(DSK_CUR_ARROW);
}

/* ---- geometry ---------------------------------------------------------- */

/*
 * The strip along the bottom that says how much is in here.
 *
 * It costs a row of the list, so it is dropped rather than shrinking the list
 * to nothing: on a 320x240 panel a small window has four rows in it, and three
 * rows with a total under them is worth having while one row with a total
 * under it is not.
 */
#define STATUS_H (DSK_SMALL_H + 3)
#define STATUS_MIN_ROWS 3

static bool has_status(dsk_win_t *w)
{
    const dsk_rect_t c = dsk_wm_client(w);
    return !dsk_rect_empty(c) && c.h >= (STATUS_H + STATUS_MIN_ROWS * ROW_H);
}

static dsk_rect_t status_rect(dsk_win_t *w)
{
    if (!has_status(w)) {
        return dsk_rect_none();
    }
    const dsk_rect_t c = dsk_wm_client(w);
    return dsk_rect(c.x, (int16_t)(dsk_rect_y2(c) - STATUS_H), c.w, STATUS_H);
}

static dsk_rect_t list_rect(dsk_win_t *w)
{
    const dsk_rect_t c = dsk_wm_client(w);
    if (dsk_rect_empty(c)) {
        return c;
    }
    const int16_t h = (int16_t)(has_status(w) ? c.h - STATUS_H : c.h);
    return dsk_rect(c.x, c.y, (int16_t)(c.w - BAR_W), h);
}

static dsk_rect_t bar_rect(dsk_win_t *w)
{
    const dsk_rect_t c = dsk_wm_client(w);
    if (dsk_rect_empty(c) || c.w <= BAR_W) {
        return dsk_rect_none();
    }
    const int16_t h = (int16_t)(has_status(w) ? c.h - STATUS_H : c.h);
    return dsk_rect((int16_t)(dsk_rect_x2(c) - BAR_W), c.y, BAR_W, h);
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

/*
 * How many things are in here and how many bytes they come to.
 *
 * The count is of everything, the byte total only of files: a directory's own
 * size is either zero or an implementation detail, and adding it to a total
 * would make the number mean nothing.  A truncated listing says so here
 * instead of over the list, because a strip is where a caveat belongs.
 */
static void draw_status(dsk_win_t *w, folder_t *f)
{
    const dsk_rect_t r = status_rect(w);
    if (dsk_rect_empty(r) || !dsk_visible(r)) {
        return;
    }

    dsk_fill(r, DSK_LGRAY);
    dsk_hline(r.x, r.y, r.w, DSK_WHITE);

    char     line[64];
    char     num[24];
    uint64_t bytes = 0;
    for (int i = 0; i < f->n; i++) {
        if (!f->entries[i].is_dir) {
            bytes += f->entries[i].size;
        }
    }

    ag_strlcpy(line, ag_utoa((uint64_t)f->n, num, sizeof(num), 0, false),
               sizeof(line));
    if (f->truncated) {
        /* "7 of 9 objects": the second number is the directory, the first is
         * what fitted in the memory this machine had. */
        ag_strlcat(line, " of ", sizeof(line));
        ag_strlcat(line, ag_utoa((uint64_t)f->total, num, sizeof(num), 0,
                                 false),
                   sizeof(line));
    }
    ag_strlcat(line, (f->n == 1) ? " object" : " objects", sizeof(line));
    if (f->marked > 0) {
        /* What an operation will act on, in the one place a person is
         * already looking to find out what is in this window. */
        ag_strlcat(line, ", ", sizeof(line));
        ag_strlcat(line, ag_utoa((uint64_t)f->marked, num, sizeof(num), 0,
                                 false),
                   sizeof(line));
        ag_strlcat(line, " picked", sizeof(line));
    }
    dsk_text_small((int16_t)(r.x + 3), (int16_t)(r.y + 2), line, DSK_BLACK,
                   DSK_LGRAY);

    ag_strlcpy(line, ag_utoa(bytes, num, sizeof(num), 0, true), sizeof(line));
    ag_strlcat(line, " bytes", sizeof(line));
    const int16_t tw = dsk_text_small_width(line);
    const int16_t tx = (int16_t)(dsk_rect_x2(r) - 3 - tw);
    /* Only when the two do not collide: a narrow window keeps the count. */
    if (tx > r.x + 3 + dsk_text_small_width("999 objects")) {
        dsk_text_small(tx, (int16_t)(r.y + 2), line, DSK_BLACK, DSK_LGRAY);
    }
}

bool dsk_folder_in_list(const dsk_win_t *w, int16_t x, int16_t y)
{
    if (w == NULL || !dsk_folder_is(w)) {
        return false;
    }
    const dsk_rect_t l = list_rect((dsk_win_t *)w);
    const dsk_rect_t b = bar_rect((dsk_win_t *)w);
    return dsk_rect_has(l, x, y) && !dsk_rect_has(b, x, y);
}

bool dsk_folder_on_sel(const dsk_win_t *w, int16_t x, int16_t y)
{
    if (w == NULL || !dsk_folder_is(w)) {
        return false;
    }
    const folder_t *f = (const folder_t *)w->user;
    if (f == NULL || f->sel < 0 || f->sel >= f->n) {
        return false;
    }
    const dsk_rect_t l = list_rect((dsk_win_t *)w);
    if (!dsk_rect_has(l, x, y)) {
        return false;
    }
    return (f->top + (y - l.y) / ROW_H) == f->sel;
}

static bool folder_pointer(dsk_win_t *w, dsk_hit_t where, int16_t x,
                           int16_t y, uint8_t buttons, dsk_ptr_t type,
                           bool dbl);

dsk_win_t *dsk_folder_banding(void)
{
    for (int i = 0; i < FOLDER_MAX; i++) {
        /*
         * Drawn, not merely armed.
         *
         * Every press in a list arms a possible band, and most of them
         * turn out to be clicks.  Owning the pointer from the press would
         * mean the folder eating the release of every click in it - the
         * context menu's among them - and, worse, one stale armed flag
         * would take the pointer away from everything else in the shell:
         * a window dragged by its caption stopped being redrawn at the
         * end of the drag, because end_track never got the release.
         */
        if (s_folders[i].used && s_folders[i].band) {
            /*
             * The folder knows its own state but not which window wears
             * it, so the windows are asked instead - there are at most a
             * handful and this runs once per pointer event.
             */
            for (int z = 0; z < dsk_wm_count(); z++) {
                dsk_win_t *w = dsk_wm_at(z);
                if (dsk_folder_is(w) && w->user == &s_folders[i]) {
                    return w;
                }
            }
        }
    }
    return NULL;
}

bool dsk_folder_band_event(dsk_win_t *w, dsk_ptr_t type, int16_t x, int16_t y,
                           uint8_t buttons)
{
    return folder_pointer(w, DSK_HIT_CLIENT, x, y, buttons, type, false);
}

bool dsk_folder_band_armed(const dsk_win_t *w)
{
    if (w == NULL || !dsk_folder_is(w)) {
        return false;
    }
    const folder_t *f = (const folder_t *)w->user;
    return (f != NULL) && f->pressed;
}

/* The band as a rectangle, however it was dragged. */
static dsk_rect_t band_rect(const folder_t *f)
{
    const int16_t x0 = (f->ax < f->bx) ? f->ax : f->bx;
    const int16_t y0 = (f->ay < f->by) ? f->ay : f->by;
    const int16_t x1 = (f->ax > f->bx) ? f->ax : f->bx;
    const int16_t y1 = (f->ay > f->by) ? f->ay : f->by;
    return dsk_rect(x0, y0, (int16_t)(x1 - x0 + 1), (int16_t)(y1 - y0 + 1));
}

/*
 * Mark every visible row the band touches, and unmark the rest.
 *
 * Recomputed from scratch on every move rather than accumulated, so that
 * dragging back up unmarks what dragging down marked - a band that only
 * ever added would make a slip unfixable without starting again.
 *
 * Only what is on screen: a band is a thing you draw around what you can
 * see, and a list that scrolled under it would mark rows nobody pointed
 * at.
 */
static void band_marks(dsk_win_t *w, folder_t *f)
{
    const dsk_rect_t l = list_rect(w);
    const dsk_rect_t b = band_rect(f);
    const int        rows = rows_visible(w);

    for (int i = 0; i < f->n; i++) {
        f->entries[i].marked = false;
    }
    f->marked = 0;

    for (int i = 0; i < rows; i++) {
        const int at = f->top + i;
        if (at >= f->n) {
            break;
        }
        const int16_t y0 = (int16_t)(l.y + i * ROW_H);
        if (dsk_rect_y2(b) <= y0 || b.y >= y0 + ROW_H) {
            continue; /* the band is entirely above or below this row */
        }
        f->entries[at].marked = true;
        f->marked++;
    }
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
        /*
         * Two different things, drawn as one where they coincide: a mark is
         * what an operation will act on, and the cursor is where the
         * keyboard is.  While nothing is marked the cursor row IS the
         * selection - which is what every operation in this shell already
         * assumed, and why marking could be added without touching any of
         * them.
         */
        const bool       lit = e->marked || (f->marked == 0 && at == f->sel);
        const bool       cursor = (at == f->sel);
        const dsk_rect_t r =
            dsk_rect(l.x, (int16_t)(l.y + i * ROW_H), l.w, ROW_H);
        const uint32_t bg = lit ? DSK_NAVY : DSK_WHITE;
        const uint32_t fg = lit ? DSK_WHITE : DSK_BLACK;

        if (lit) {
            dsk_fill(r, DSK_NAVY);
        }
        if (cursor && f->marked != 0) {
            /* Where the two differ, the cursor is a frame and the mark is a
             * fill, so a marked row under the cursor is still both. */
            dsk_frame(r, DSK_BLACK);
        }
        if (e->art >= 0) {
            dsk_icon_draw_px(f->art[e->art], r.x, r.y, 1, bg);
        } else {
            dsk_icon_draw(e->icon, r.x, r.y, 1, bg);
        }
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
            dsk_text_fit((int16_t)(dsk_rect_x2(r) - 4 - n * dsk_ui_w()), r.y,
                         (int16_t)(n * dsk_ui_w()), s, fg, bg);
        }
    }

    if (f->n == 0) {
        dsk_text((int16_t)(l.x + 4), (int16_t)(l.y + 4), "(empty)", DSK_DGRAY,
                 DSK_WHITE);
    }
    if (f->truncated && !has_status(w)) {
        /* Where there is no strip to say it in, it is said over the list -
         * ugly, and better than a listing that is quietly short. */
        dsk_text_small((int16_t)(l.x + 4), (int16_t)(dsk_rect_y2(l) - 9),
                       "too many files to list", DSK_MAROON, DSK_WHITE);
    }
    draw_bar(w, f);
    draw_status(w, f);

    /*
     * The band last, over everything, and as part of the client paint
     * rather than as a thing that saves and restores what it covers: on a
     * board with no framebuffer there is nothing to save from, and the
     * strip being drawn is the only pixels that exist.
     */
    if (f->band) {
        dsk_frame(band_rect(f), DSK_BLACK);
    }
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
                           uint8_t buttons, dsk_ptr_t type, bool dbl)
{
    (void)buttons;
    const bool down = (type == DSK_PTR_DOWN);
    folder_t *f = (folder_t *)w->user;
    if (f == NULL) {
        return false;
    }

    /*
     * Everything that is not a press belongs to the band.
     *
     * The manager hands a window its moves and its release with `down`
     * false, and until now this handler threw them away.  A band needs all
     * three, and it needs the release most: the marks are the answer and
     * the rectangle has to come off the screen.
     */
    if (!down) {
        if (!f->pressed) {
            return false;
        }
        if (type != DSK_PTR_MOVE) {
            /*
             * Let go.  Whether the marks stay is already decided - they
             * were made as the band moved - so this only takes the
             * rectangle away.
             */
            const bool was = f->band;
            f->pressed = false;
            f->band = false;
            if (was) {
                dsk_wm_damage_rect(dsk_wm_client(w));
                /*
                 * Said out loud, because a scripted run cannot see a
                 * selection: the rows are lit on the glass and nowhere
                 * else.  One line per completed gesture, which is as
                 * often as a person draws one.
                 */
                ag_printf("desktop: band marked %d\n", f->marked);
            }
            return was;
        }

        const dsk_rect_t l = list_rect(w);
        if (!f->band) {
            const int16_t dx = (int16_t)((x > f->ax) ? x - f->ax : f->ax - x);
            const int16_t dy = (int16_t)((y > f->ay) ? y - f->ay : f->ay - y);
            if (dx <= BAND_SLOP && dy <= BAND_SLOP) {
                return false; /* still a click, as far as anyone knows */
            }
            f->band = true;
        }

        /*
         * Kept inside the list.  A band dragged over the scroll bar or the
         * status strip would paint on them and be rubbed out by their next
         * repaint, and the rows it selects are in here anyway.
         */
        const dsk_rect_t before = band_rect(f);
        f->bx = x;
        f->by = y;
        if (f->bx < l.x) {
            f->bx = l.x;
        }
        if (f->bx >= dsk_rect_x2(l)) {
            f->bx = (int16_t)(dsk_rect_x2(l) - 1);
        }
        if (f->by < l.y) {
            f->by = l.y;
        }
        if (f->by >= dsk_rect_y2(l)) {
            f->by = (int16_t)(dsk_rect_y2(l) - 1);
        }
        band_marks(w, f);

        /*
         * Repainted across the whole width of the list, not just where the
         * rectangle is.
         *
         * A highlight is a full-width thing and the band is whatever shape
         * the hand drew.  Dragged straight down, the band is a sliver two
         * pixels wide: the rows inside it were duly marked and only those
         * two pixels of each were repainted, so on the glass nothing
         * appeared to be selected at all.  Maxim saw it once and it is the
         * same rectangle either way - the strips that carry it are as tall
         * as the band, and their width costs nothing next to their number.
         */
        dsk_rect_t hurt = dsk_rect_union(before, band_rect(f));
        hurt.x = l.x;
        hurt.w = l.w;
        dsk_wm_damage_rect(hurt);
        return true;
    }

    if (where != DSK_HIT_CLIENT) {
        f->pressed = false;
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

    /*
     * Every press in the list is a possible band EXCEPT one on the row
     * that is already picked, which is where dragging a file out of the
     * window begins.  Both cannot be armed at once: the file drag takes
     * the release for its drop, so a band armed beside it would never be
     * told to stop and would stay drawn on the glass.
     *
     * Requiring empty space instead would have been simpler and wrong - a
     * full window has none, and the gesture has to work in the windows
     * people actually have.
     */
    const int row = (y - l.y) / ROW_H;
    f->pressed = !dsk_folder_on_sel(w, x, y);
    f->band = false;
    f->ax = x;
    f->ay = y;
    f->bx = x;
    f->by = y;
    const int at = f->top + row;
    if (at < 0 || at >= f->n) {
        /*
         * Below the last row: no row to select, but the press still arms
         * the band, which is where the gesture usually starts.
         */
        if (f->marked != 0) {
            dsk_folder_mark(w, -1, DSK_MARK_NONE);
        }
        return true;
    }
    /*
     * A plain click starts again from nothing, and that matters more than it
     * looks: without it a mark made a minute ago is still there, invisible
     * below the fold of a scrolled list, and Delete means more than it looks
     * like it means.
     *
     * There is no Ctrl+click, and not for want of trying: a pointer event
     * carries buttons and no modifiers (ABI 0.42), so this window cannot
     * know whether Ctrl was down.  Marking by hand is Space, by finger the
     * context menu, and neither of those needs two devices at once - which
     * on the board is just as well, since it has no keyboard at all.
     */
    if (f->marked != 0 && !dbl) {
        dsk_folder_mark(w, -1, DSK_MARK_NONE);
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

int dsk_folder_marked(const dsk_win_t *w)
{
    const folder_t *f = (w != NULL && dsk_folder_is(w))
                            ? (const folder_t *)w->user
                            : NULL;
    return (f != NULL) ? f->marked : 0;
}

bool dsk_folder_marked_at(const dsk_win_t *w, int which, char *path,
                          size_t len, char *name, size_t name_len,
                          bool *is_dir)
{
    const folder_t *f = (w != NULL && dsk_folder_is(w))
                            ? (const folder_t *)w->user
                            : NULL;
    if (f == NULL || which < 0) {
        return false;
    }
    for (int i = 0; i < f->n; i++) {
        if (!f->entries[i].marked) {
            continue;
        }
        if (which-- != 0) {
            continue;
        }
        const entry_t *e = &f->entries[i];
        if (path != NULL) {
            join(f->path, e->name, path, len);
        }
        if (name != NULL) {
            ag_strlcpy(name, e->name, name_len);
        }
        if (is_dir != NULL) {
            *is_dir = e->is_dir;
        }
        return true;
    }
    return false;
}

/*
 * Marking, the three ways a person asks for it.
 *
 * `row` of -1 means the cursor, which is what a keyboard and a context menu
 * both mean by "this one".  ".." is never marked: it is a place, not a file,
 * and an operation aimed at it is aimed at the parent directory.
 */
void dsk_folder_mark(dsk_win_t *w, int row, dsk_mark_t how)
{
    folder_t *f = (w != NULL && dsk_folder_is(w)) ? (folder_t *)w->user : NULL;
    if (f == NULL || f->entries == NULL) {
        return;
    }
    const bool up = !is_root(f->path); /* row 0 is ".." when there is one */

    switch (how) {
    case DSK_MARK_TOGGLE: {
        const int at = (row >= 0) ? row : f->sel;
        if (at < 0 || at >= f->n || (up && at == 0)) {
            return;
        }
        f->entries[at].marked = !f->entries[at].marked;
        f->marked += f->entries[at].marked ? 1 : -1;
        break;
    }
    case DSK_MARK_ALL:
    case DSK_MARK_NONE:
    case DSK_MARK_INVERT:
        f->marked = 0;
        for (int i = 0; i < f->n; i++) {
            if (up && i == 0) {
                f->entries[i].marked = false;
                continue;
            }
            f->entries[i].marked = (how == DSK_MARK_ALL)    ? true
                                   : (how == DSK_MARK_NONE) ? false
                                                            : !f->entries[i].marked;
            if (f->entries[i].marked) {
                f->marked++;
            }
        }
        break;
    }
    dsk_wm_damage_rect(dsk_wm_client(w));
}

bool dsk_folder_open_sel(dsk_win_t *w)
{
    folder_t *f = (w != NULL) ? (folder_t *)w->user : NULL;
    if (f == NULL || !dsk_folder_is(w) || f->sel < 0 || f->sel >= f->n) {
        return false;
    }
    activate(w, f->sel);
    return true;
}

static bool folder_key(dsk_win_t *w, uint16_t keycode, uint32_t unicode,
                       uint16_t mods)
{
    folder_t *f = (folder_t *)w->user;
    if (f == NULL) {
        return false;
    }
    const int rows = rows_visible(w);
    const int was_sel = f->sel;
    const int was_top = f->top;

    /*
     * Space is the mark key, as it is in every file manager since Norton -
     * and Ctrl+A is the one every graphical shell since has agreed on.  Both
     * are here rather than in the desktop's key table because the marks
     * belong to this window and nothing else can say which row is under its
     * cursor.
     */
    if (keycode == DSK_KEY_SPACE && (mods & DSK_MOD_CTRL) == 0) {
        dsk_folder_mark(w, -1, DSK_MARK_TOGGLE);
        if (f->sel + 1 < f->n) {
            f->sel++; /* marking a run of files should not need two hands */
            clamp_scroll(w, f);
            dsk_wm_damage_rect(dsk_wm_client(w));
        }
        return true;
    }
    if ((mods & DSK_MOD_CTRL) != 0 && (unicode == 'a' || unicode == 'A')) {
        dsk_folder_mark(w, -1, (f->marked == f->n - (is_root(f->path) ? 0 : 1))
                                   ? DSK_MARK_NONE
                                   : DSK_MARK_ALL);
        return true;
    }

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

const char *dsk_folder_path(const dsk_win_t *w)
{
    if (!dsk_folder_is(w)) {
        return NULL;
    }
    const folder_t *f = (const folder_t *)w->user;
    return (f != NULL) ? f->path : NULL;
}

bool dsk_folder_selected(const dsk_win_t *w, char *path, size_t len,
                         char *name, size_t name_len, bool *is_dir)
{
    if (!dsk_folder_is(w)) {
        return false;
    }
    const folder_t *f = (const folder_t *)w->user;
    if (f == NULL || f->sel < 0 || f->sel >= f->n) {
        return false;
    }
    const entry_t *e = &f->entries[f->sel];

    /* ".." is a place, not a file: an operation aimed at it would be aimed at
     * the parent directory, which is never what the click meant. */
    if (e->name[0] == '.' && e->name[1] == '.' && e->name[2] == '\0') {
        return false;
    }

    if (path != NULL) {
        join(f->path, e->name, path, len);
    }
    if (name != NULL) {
        ag_strlcpy(name, e->name, name_len);
    }
    if (is_dir != NULL) {
        *is_dir = e->is_dir;
    }
    return true;
}

void dsk_folder_select_name(dsk_win_t *w, const char *name)
{
    if (!dsk_folder_is(w) || name == NULL) {
        return;
    }
    folder_t *f = (folder_t *)w->user;
    if (f == NULL) {
        return;
    }
    for (int i = 0; i < f->n; i++) {
        if (ag_stricmp(f->entries[i].name, name) == 0) {
            f->sel = i;
            clamp_scroll(w, f);
            dsk_wm_damage_rect(dsk_wm_client(w));
            return;
        }
    }
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
    /*
     * A slot is reused, so the gesture it was left in is not this
     * window's.  The marks live on the entries and go with them; these
     * three do not belong to anything that gets freed.
     */
    f->band = false;
    f->pressed = false;
    f->ax = f->ay = f->bx = f->by = 0;
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
