/*
 * ArgonOS file manager - what the two files share.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_FM_H
#define ARGON_FM_H

#include <argon/argon.h>
#include <argon/keys.h>
#include <argon/libc.h>

/*
 * The same panels/ops build three ways.
 *
 * As a text .AXE: AG_APP in fm.c, cell drawing via fm_ui_text.c.
 * With AG_BUILTIN: linked into the kernel; `fm` spawns it as a process in a slot.
 * As GFXFM.AXE: FM_GFX_BUILD + apps/gfxfm (soft gfx cell backend, same ops).
 *
 * Drawing goes through fm_ui.h so ops never call the console or gfx directly.
 */
#ifdef AG_BUILTIN
#define FM_ENTRY ag_fm_main
#else
#define FM_ENTRY ag_main
#endif

int FM_ENTRY(int argc, char **argv);

/*
 * The layout is measured, not declared.
 *
 * Wide: two panels side by side over four lines of chrome - the boxes, then
 * what the cursor is on, then messages, then the key bar and the status line.
 * Narrow: the same two panels one above the other, and the detail line goes.
 *
 * Eighty by twenty-five is what a console has been since 1981 and what this
 * was written for, but a board whose only screen is 320 pixels wide runs a
 * forty column console (docs/09-esp32-cyd.md), and two forty-wide panels do
 * not go into forty columns.  They do go one above the other.
 *
 * So the numbers below are fields rather than constants, and the names that
 * used to be constants are now expressions over them - which is why almost
 * none of the drawing code changed.  Buffers use FM_LINE_MAX, because a
 * variable length array on a 16 KB stack is a different kind of trouble.
 */
#define FM_LINE_MAX 128

typedef struct {
    int  cols, rows;
    int  panel_x[2], panel_y[2];
    int  panel_w, panel_h;
    int  visible; /* entries in a panel: panel_h - 2 */
    int  row_detail, row_message, row_keys, row_status;
    bool stacked; /* one panel above the other, rather than side by side */
} fm_layout_t;

extern fm_layout_t g_lay;

/* Works out the above from the console's own size.  Call once, first. */
void fm_layout_init(void);

#define FM_COLS (g_lay.cols)
#define FM_ROWS (g_lay.rows)
#define FM_PANEL_W (g_lay.panel_w)
#define FM_PANEL_ROWS (g_lay.panel_h)
#define FM_VISIBLE (g_lay.visible)
#define FM_ROW_DETAIL (g_lay.row_detail)
#define FM_ROW_MESSAGE (g_lay.row_message)
#define FM_ROW_KEYS (g_lay.row_keys)
#define FM_ROW_STATUS (g_lay.row_status)

/*
 * The panel colours are the ones everyone who has seen a two-panel manager
 * expects, which is the whole point of looking like one.
 */
#define FM_ATTR_PANEL AG_ATTR(AG_LGRAY, AG_BLUE)
#define FM_ATTR_FRAME AG_ATTR(AG_WHITE, AG_BLUE)
#define FM_ATTR_DIR AG_ATTR(AG_WHITE, AG_BLUE)
#define FM_ATTR_EXEC AG_ATTR(AG_LGREEN, AG_BLUE)
#define FM_ATTR_CURSOR AG_ATTR(AG_BLACK, AG_CYAN)
#define FM_ATTR_KEYS AG_ATTR(AG_BLACK, AG_CYAN)
#define FM_ATTR_STATUS AG_ATTR(AG_LGRAY, AG_BLACK)
#define FM_ATTR_MESSAGE AG_ATTR(AG_YELLOW, AG_BLACK)
#define FM_ATTR_ERROR AG_ATTR(AG_LRED, AG_BLACK)
#define FM_ATTR_DIALOG AG_ATTR(AG_BLACK, AG_LGRAY)

/*
 * Entries are held in the process arena, and the count is bounded: a directory
 * with more than this shows the first FM_MAX_ENTRIES and says so, which is
 * better than failing to open it at all.
 */
#define FM_MAX_ENTRIES 512
#define FM_NAME_MAX 64

typedef struct {
    char     name[FM_NAME_MAX];
    uint64_t size;
    uint64_t mtime;
    bool     is_dir;
    bool     is_up; /* the ".." entry, which is not a real name           */
} fm_entry_t;

typedef struct {
    char        path[AG_PATH_MAX];
    fm_entry_t *entries;
    int         count;
    int         capacity; /* entries the arena would give, see FM_MAX_ENTRIES */
    int         truncated; /* entries the directory had beyond our bound   */
    int         cursor;
    int         top;
    uint64_t    total_bytes;
} fm_panel_t;

extern fm_panel_t g_panel[2];
extern int        g_active;

/* ---- drawing (fm.c) ----------------------------------------------------- */

void fm_put(int x, int y, const char *s, uint8_t attr);
void fm_put_clipped(int x, int y, int width, const char *s, uint8_t attr);
void fm_clear_row(int y, uint8_t attr);
void fm_frame(int x, int y, int w, int h, uint8_t attr);
void fm_draw_all(void);
/* Only what moving the cursor changes: the active panel and the two status rows. */
void fm_draw_light(void);
void fm_draw_panel(int which);
void fm_draw_detail(void);
void fm_message(const char *text);
void fm_error(const char *what, ag_err_t err);

/* ---- panels (fm.c) ----------------------------------------------------- */

ag_err_t fm_read_panel(fm_panel_t *p, const char *path);
ag_err_t fm_reload(fm_panel_t *p);
/* Joins a directory and a name into a path the kernel will accept. */
void fm_join(const char *dir, const char *name, char *out, size_t len);
/* "/sd/apps" as "A:\APPS", for the eyes only - the kernel deals in the first. */
void fm_dos_path(const char *posix, char *out, size_t len);
fm_entry_t *fm_current(void);
fm_panel_t *fm_active(void);
fm_panel_t *fm_other(void);

/* ---- dialogs and operations (fmops.c) ---------------------------------- */

/* An answered question, or false when Esc said no. */
bool fm_ask(const char *prompt, char *buf, size_t len);
bool fm_confirm(const char *question);
/* Waits for one key, for after something has printed over the screen. */
void fm_pause(const char *note);

void fm_view(void);   /* F3 */
void fm_edit(void);   /* F4 */
void fm_copy(void);   /* F5 */
void fm_move(void);   /* F6 */
void fm_mkdir(void);  /* F7 */
void fm_delete(void); /* F8 */
void fm_help(void);   /* F1 */
void fm_run(const fm_entry_t *entry);

#endif /* ARGON_FM_H */
