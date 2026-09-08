/*
 * ArgonOS DESKTOP - the file operations, and the box that says how far.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "dsk_ops.h"

#include "dsk_dlg.h"
#include "dsk_paint.h"

#include "../common/fsops/fsops.h"

#include <argon/argon.h>
#include <argon/keys.h>
#include <argon/libc.h>

/*
 * Eight kilobytes, out of the arena rather than off the stack.
 *
 * The size decides how many system calls a copy costs and nothing else; the
 * arena is a megabyte, and eight kilobytes on a sixteen kilobyte stack is not
 * a thing to do.  fsops allocates nothing itself, so this is the one place the
 * decision is made.
 */
#define OPS_CHUNK (8u * 1024u)

/* How often the box may be redrawn.  A copy off the RAM disk would otherwise
 * spend its time drawing, and a flush is whole rows of the panel. */
#define OPS_UI_MS 250u

#define BOX_W 260
#define BOX_H 92
#define BOX_PAD 10
#define BAR_H 14

static dsk_metrics_t s_m;
static void (*s_repaint)(void);
static bool s_changed;

/* ---------------------------------------------------------------------- */
/* The box                                                                */
/* ---------------------------------------------------------------------- */

static dsk_rect_t box_rect(void)
{
    const int16_t w = (BOX_W < s_m.screen_w) ? BOX_W : s_m.screen_w;
    const int16_t h = (BOX_H < s_m.screen_h) ? BOX_H : s_m.screen_h;
    return dsk_rect((int16_t)((s_m.screen_w - w) / 2),
                    (int16_t)((s_m.screen_h - h) / 2), w, h);
}

/* The last component: a whole path does not fit and the name is the part
 * that says what is happening. */
static const char *base_name(const char *path)
{
    const char *at = (path != NULL) ? path : "";
    for (const char *p = at; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            at = p + 1;
        }
    }
    return at;
}

static void num(char *out, size_t len, uint64_t v)
{
    (void)ag_utoa(v, out, len, 0, true);
}

/*
 * Draws the whole box every time rather than only the bar.
 *
 * The bar is the only thing that moves, but the box is 260x92 and the flush
 * behind it takes whole rows of the panel anyway - so drawing the frame again
 * costs nothing measurable and saves keeping track of which parts are already
 * on the screen, which is the sort of bookkeeping that leaves one stale pixel
 * behind and nothing to explain it.
 */
static void draw_box(const char *verb, const char *what, uint64_t done,
                     uint64_t total, uint32_t files_done, uint32_t files_total)
{
    const dsk_rect_t r = box_rect();
    char             line[80];
    char             number[24];

    dsk_clip_reset();
    dsk_panel(r, true, DSK_LGRAY);

    /* A caption strip, so it reads as a window rather than as a smear. */
    const dsk_rect_t cap =
        dsk_rect((int16_t)(r.x + 2), (int16_t)(r.y + 2), (int16_t)(r.w - 4),
                 s_m.title_h);
    dsk_fill(cap, DSK_NAVY);
    dsk_text((int16_t)(cap.x + 3),
             (int16_t)(cap.y + (cap.h - DSK_FONT_H) / 2), verb, DSK_WHITE,
             DSK_NAVY);

    const int16_t x = (int16_t)(r.x + BOX_PAD);
    const int16_t w = (int16_t)(r.w - 2 * BOX_PAD);
    int16_t       y = (int16_t)(cap.y + cap.h + 6);

    dsk_text_fit(x, y, w, base_name(what), DSK_BLACK, DSK_LGRAY);
    y = (int16_t)(y + DSK_FONT_H + 4);

    /* The bar: sunken track, navy fill, nothing when there is no total. */
    const dsk_rect_t track = dsk_rect(x, y, w, BAR_H);
    dsk_panel(track, false, DSK_WHITE);
    if (total > 0) {
        uint64_t part = (done > total) ? total : done;
        const int16_t inner = (int16_t)(track.w - 4);
        const int16_t fill = (int16_t)((part * (uint64_t)inner) / total);
        if (fill > 0) {
            dsk_fill(dsk_rect((int16_t)(track.x + 2), (int16_t)(track.y + 2),
                              fill, (int16_t)(track.h - 4)),
                     DSK_NAVY);
        }
    }
    y = (int16_t)(y + BAR_H + 5);

    line[0] = '\0';
    if (total > 0) {
        const uint32_t pct = (uint32_t)((done * 100u) / total);
        ag_strlcat(line, ag_utoa(pct, number, sizeof(number), 0, false),
                   sizeof(line));
        ag_strlcat(line, "%   ", sizeof(line));
    }
    num(number, sizeof(number), done / 1024u);
    ag_strlcat(line, number, sizeof(line));
    if (total > 0) {
        ag_strlcat(line, " / ", sizeof(line));
        num(number, sizeof(number), total / 1024u);
        ag_strlcat(line, number, sizeof(line));
    }
    ag_strlcat(line, " KB", sizeof(line));
    if (files_total > 1u) {
        ag_strlcat(line, ",  file ", sizeof(line));
        ag_strlcat(line,
                   ag_utoa(files_done + 1u, number, sizeof(number), 0, false),
                   sizeof(line));
        ag_strlcat(line, " of ", sizeof(line));
        ag_strlcat(line, ag_utoa(files_total, number, sizeof(number), 0, false),
                   sizeof(line));
    }
    dsk_text_fit(x, y, w, line, DSK_BLACK, DSK_LGRAY);
    y = (int16_t)(y + DSK_FONT_H + 3);

    dsk_text_fit(x, y, w, "Esc to stop", DSK_DGRAY, DSK_LGRAY);

    dsk_flush(r);
}

/* ---------------------------------------------------------------------- */
/* Running one                                                            */
/* ---------------------------------------------------------------------- */

typedef struct {
    const char *verb;
    uint32_t    last_ms;
    bool        stop;
    bool        drawn;
} run_t;

/*
 * Escape, and nothing else.
 *
 * The events are taken rather than looked at, because whatever arrives while
 * the box is up is aimed at the box: a click meant for a window behind it, or
 * a key meant for a list, would otherwise be delivered afterwards to a shell
 * that has moved on.  AG_EV_QUIT is Ctrl+C from the supervisor and means the
 * same thing here as Escape.
 */
static bool asked_to_stop(void)
{
    ag_event_t ev;
    bool       stop = ag_interrupted();

    while (ag_poll_event(&ev, 0)) {
        if (ev.type == AG_EV_QUIT) {
            stop = true;
        } else if (ev.type == AG_EV_KEY_DOWN &&
                   ev.key.keycode == AG_KEY_ESC) {
            stop = true;
        }
    }
    return stop;
}

static bool tick(void *ctx, const fsops_progress_t *p)
{
    run_t *r = (run_t *)ctx;

    if (asked_to_stop()) {
        r->stop = true;
        return false;
    }

    /*
     * The stop check above is every tick and the drawing is not: Escape has to
     * be noticed on the tick it arrives, and the box does not have to move
     * more than four times a second to look alive.
     */
    const uint32_t now = ag_millis();
    const bool     due = (now - r->last_ms) >= OPS_UI_MS;
    if (r->drawn && !due) {
        return true;
    }
    r->last_ms = now;
    r->drawn = true;

    /* A measured whole shows the whole; a single file shows itself. */
    if (p->total_bytes > 0) {
        draw_box(r->verb, p->path, p->total_done, p->total_bytes,
                 p->files_done, p->files_total);
    } else {
        draw_box(r->verb, p->path, p->file_done, p->file_total, p->files_done,
                 p->files_total);
    }
    return true;
}

static void say_error(const char *label, ag_err_t err)
{
    char line[80];

    ag_strlcpy(line, (label != NULL) ? label : "the file", sizeof(line));
    ag_strlcat(line, ": ", sizeof(line));
    ag_strlcat(line, ag_strerror(err), sizeof(line));
    (void)dsk_dlg_message("Error", line, NULL, DSK_DLG_OK, NULL, NULL);
}

/*
 * Counting first, and only for a directory.
 *
 * A single file already knows its size from its own stat, so its bar is real
 * for free.  A tree does not, and the only route to a bar that means anything
 * is one extra walk - which over HostFS is a wait of its own, so it says
 * "Counting" while it happens and Escape stops it.  A count that was stopped
 * or failed is not an error: the operation goes ahead with no total, and the
 * box shows the file it is on instead of a percentage.
 */
static void count_first(fsops_req_t *req, run_t *r, const char *path)
{
    ag_stat_t st;
    if (ag_stat(path, &st) != AG_OK || (st.attr & AG_A_DIR) == 0) {
        return;
    }

    const char *was = r->verb;
    r->verb = "Counting";
    uint64_t bytes = 0;
    uint32_t files = 0;
    if (fsops_measure(req, path, &bytes, &files) == AG_OK) {
        req->total_bytes = bytes;
        req->files_total = files;
    }
    r->verb = was;
    r->last_ms = 0;
    r->drawn = false;
}

static bool begin(fsops_req_t *req, run_t *r, const char *verb)
{
    memset(req, 0, sizeof(*req));
    memset(r, 0, sizeof(*r));
    r->verb = verb;

    req->chunk = (char *)ag_malloc(OPS_CHUNK);
    if (req->chunk == NULL) {
        return false;
    }
    req->chunk_len = OPS_CHUNK;
    req->fs = fsops_argon_fs();
    req->tick = tick;
    req->tick_ctx = r;
    return true;
}

/*
 * Whatever happened, the desktop goes back on the screen and the keyboard is
 * emptied: the Escape that stopped a copy must not also close a window, and
 * the clicks someone got impatient with must not arrive now.
 */
static void finish(fsops_req_t *req, run_t *r, const char *label, ag_err_t err)
{
    ag_free(req->chunk);
    req->chunk = NULL;

    if (r->drawn && s_repaint != NULL) {
        s_repaint();
    }
    ag_flush_input();
    (void)ag_interrupted();

    if (err == -AG_EKILLED || r->stop) {
        /* Not an error: it is what Escape does, and it already happened. */
        return;
    }
    if (err != AG_OK) {
        say_error(label, err);
    }
}

/* ---------------------------------------------------------------------- */
/* What the shell calls                                                   */
/* ---------------------------------------------------------------------- */

void dsk_ops_init(const dsk_metrics_t *m, void (*repaint)(void))
{
    s_m = *m;
    s_repaint = repaint;
}

bool dsk_ops_changed(void) { return s_changed; }

void dsk_ops_copy(const char *from, const char *to, const char *label)
{
    fsops_req_t req;
    run_t       r;

    s_changed = false;
    if (!begin(&req, &r, "Copying")) {
        say_error(label, -AG_ENOMEM);
        return;
    }
    count_first(&req, &r, from);
    const ag_err_t err = fsops_copy(&req, from, to);
    /* Even a stopped copy has written something, so the windows have to
     * re-read either way. */
    s_changed = true;
    finish(&req, &r, label, err);
}

void dsk_ops_move(const char *from, const char *to, const char *label)
{
    fsops_req_t req;
    run_t       r;

    s_changed = false;
    if (!begin(&req, &r, "Moving")) {
        say_error(label, -AG_ENOMEM);
        return;
    }
    /*
     * Counted before the rename is tried, which is one walk wasted whenever
     * the rename works.  Not worth avoiding: a rename that works takes no
     * time, and the alternative is counting from inside fsops_move, where the
     * box does not exist.
     */
    count_first(&req, &r, from);
    const ag_err_t err = fsops_move(&req, from, to);
    s_changed = true;
    finish(&req, &r, label, err);
}

void dsk_ops_delete(const char *path, const char *label)
{
    fsops_req_t req;
    run_t       r;

    s_changed = false;
    if (!begin(&req, &r, "Deleting")) {
        say_error(label, -AG_ENOMEM);
        return;
    }
    count_first(&req, &r, path);
    const ag_err_t err = fsops_delete(&req, path);
    s_changed = true;
    finish(&req, &r, label, err);
}

bool dsk_ops_mkdir(const char *path, const char *label)
{
    s_changed = false;
    const ag_err_t err = ag_mkdir(path);
    if (err != AG_OK) {
        say_error(label, err);
        return false;
    }
    s_changed = true;
    return true;
}

bool dsk_ops_rename(const char *from, const char *to, const char *label)
{
    s_changed = false;

    /*
     * Rename and not move: within one directory these are the same call, and
     * the desktop only ever renames in place - a name typed into the rename
     * box that pointed somewhere else would move the file without saying so.
     */
    const ag_err_t err = ag_rename(from, to);
    if (err != AG_OK) {
        say_error(label, err);
        return false;
    }
    s_changed = true;
    return true;
}
