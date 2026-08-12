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
 * Screen layout, 80x25.  Two panels side by side over four lines of chrome:
 *
 *   rows 0..20   panel boxes: border, 19 entries, border with the totals
 *   row 21       what the cursor is on, in full
 *   row 22       messages, and the line where questions are answered
 *   row 23       the key bar
 *   row 24       free space and the name of this thing
 */
#define FM_COLS 80
#define FM_ROWS 25
#define FM_PANEL_W 40
#define FM_PANEL_ROWS 21
#define FM_VISIBLE (FM_PANEL_ROWS - 2)
#define FM_ROW_DETAIL 21
#define FM_ROW_MESSAGE 22
#define FM_ROW_KEYS 23
#define FM_ROW_STATUS 24

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
void fm_copy(void);   /* F5 */
void fm_move(void);   /* F6 */
void fm_mkdir(void);  /* F7 */
void fm_delete(void); /* F8 */
void fm_help(void);   /* F1 */
void fm_run(const fm_entry_t *entry);

#endif /* ARGON_FM_H */
