/*
 * ArgonOS DESKTOP - message and input dialogs.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "dsk_dlg.h"

#include "dsk_kbd.h"
#include "dsk_paint.h"
#include "dsk_wm.h"

#define BTN_W    64
#define BTN_H    20
#define BTN_GAP  8
#define PAD      10
#define BLINK_MS 530u /* the interval a VGA text caret used */
#define LINE_MAX 64

typedef struct {
    bool        up;
    bool        input;      /* a text box rather than a message */
    dsk_dlg_kind_t kind;
    /*
     * The message text, one string per line.  An array rather than two named
     * lines because a file's properties are six facts, and the alternative -
     * gluing them into two lines - is a dialog that truncates whichever fact
     * happens to be last.
     */
    char        line[DSK_DLG_LINES_MAX][LINE_MAX];
    int         nlines;
    char        text[DSK_INPUT_MAX];
    uint16_t    len;
    bool        caret_lit;
    uint32_t    caret_at;
    dsk_win_t  *win;
    /*
     * Whether this box carries a keyboard of its own.  Decided when it
     * opens, from what the machine has and what the person asked for, and
     * not re-asked while it is up: a keyboard that appeared halfway through
     * would move the buttons out from under a finger already going for one.
     */
    bool        keys;
    void       *ctx;
    void (*done_msg)(dsk_answer_t a, void *ctx);
    void (*done_txt)(dsk_answer_t a, const char *text, void *ctx);
} dlg_t;

static dsk_metrics_t s_m;
static dlg_t         s_d;
static bool          s_want_keys;

void dsk_dlg_keyboard(bool show) { s_want_keys = show; }

static size_t str_len(const char *s)
{
    size_t n = 0;
    if (s != NULL) {
        while (s[n] != '\0') {
            n++;
        }
    }
    return n;
}

static void str_copy(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    if (src != NULL) {
        while (src[n] != '\0' && n + 1 < cap) {
            dst[n] = src[n];
            n++;
        }
    }
    dst[n] = '\0';
}

/* ---- what the buttons are ---------------------------------------------- */

static const char *btn_label(int which)
{
    switch (s_d.kind) {
    case DSK_DLG_YESNO:
        return (which == 0) ? "Yes" : "No";
    case DSK_DLG_OKCANCEL:
        return (which == 0) ? "Ok" : "Cancel";
    default:
        return "Ok";
    }
}

static int btn_count(void) { return (s_d.kind == DSK_DLG_OK) ? 1 : 2; }

static dsk_answer_t btn_answer(int which)
{
    switch (s_d.kind) {
    case DSK_DLG_YESNO:
        return (which == 0) ? DSK_ANSWER_YES : DSK_ANSWER_NO;
    case DSK_DLG_OKCANCEL:
        return (which == 0) ? DSK_ANSWER_OK : DSK_ANSWER_CANCEL;
    default:
        return DSK_ANSWER_OK;
    }
}

static dsk_rect_t btn_rect(dsk_rect_t client, int which)
{
    const int    n = btn_count();
    const int16_t total = (int16_t)(n * BTN_W + (n - 1) * BTN_GAP);
    const int16_t x0 = (int16_t)(client.x + (client.w - total) / 2);
    return dsk_rect((int16_t)(x0 + which * (BTN_W + BTN_GAP)),
                    (int16_t)(dsk_rect_y2(client) - PAD - BTN_H), BTN_W, BTN_H);
}

static dsk_rect_t edit_rect(dsk_rect_t client);

/* Where the keyboard goes: between the text field and the buttons. */
static dsk_rect_t kbd_rect(dsk_rect_t client)
{
    if (!s_d.keys) {
        return dsk_rect_none();
    }
    /*
     * Between the text field and the buttons, and never over either.
     *
     * The window manager cuts a box down to the work area, so a box that
     * asked for more than the screen comes back shorter than it planned -
     * and a keyboard anchored to the bottom then climbs over the field it
     * is there to fill.  On the CYD that is exactly what happened: the keys
     * could be hit and what they typed could not be seen.  So the space is
     * measured, not assumed, and the keys shrink to fit it.
     */
    const dsk_rect_t e = edit_rect(client);
    const int16_t    top = (int16_t)(dsk_rect_y2(e) + PAD);
    const int16_t    bottom = (int16_t)(dsk_rect_y2(client) - PAD - BTN_H -
                                     PAD);
    const int16_t    avail = (int16_t)(bottom - top);
    const int16_t    h = dsk_kbd_height_for((int16_t)(client.w - 2 * PAD),
                                            avail);
    if (h <= 0) {
        return dsk_rect_none();
    }
    return dsk_rect((int16_t)(client.x + PAD), top,
                    (int16_t)(client.w - 2 * PAD), h);
}

static dsk_rect_t edit_rect(dsk_rect_t client)
{
    return dsk_rect((int16_t)(client.x + PAD),
                    (int16_t)(client.y + PAD + dsk_ui_h() + 4),
                    (int16_t)(client.w - 2 * PAD), (int16_t)(dsk_ui_h() + 6));
}

/* ---- answering --------------------------------------------------------- */

static void answer(dsk_answer_t a)
{
    /*
     * Everything is taken out of the state before the callback runs, because a
     * handler is entitled to open the next dialog - and would be refused if
     * this one were still marked up.
     */
    dlg_t d = s_d;
    s_d.up = false;
    s_d.win = NULL;

    if (d.win != NULL) {
        dsk_wm_close(d.win);
    }
    if (d.input) {
        if (d.done_txt != NULL) {
            d.done_txt(a, d.text, d.ctx);
        }
    } else if (d.done_msg != NULL) {
        d.done_msg(a, d.ctx);
    }
}

/* ---- the window ops ---------------------------------------------------- */

static void dlg_draw(dsk_win_t *w, dsk_rect_t client)
{
    (void)w;
    dsk_fill(client, DSK_LGRAY);

    if (s_d.input) {
        dsk_text_fit((int16_t)(client.x + PAD), (int16_t)(client.y + PAD),
                     (int16_t)(client.w - 2 * PAD), s_d.line[0], DSK_BLACK,
                     DSK_LGRAY);
        const dsk_rect_t e = edit_rect(client);
        dsk_panel(e, false, DSK_WHITE);
        /* The tail of a long line, so what is being typed stays in sight. */
        const int16_t room = (int16_t)((e.w - 8) / dsk_ui_w());
        const int16_t from =
            (int16_t)((s_d.len > (uint16_t)room) ? s_d.len - room : 0);
        dsk_text_fit((int16_t)(e.x + 4), (int16_t)(e.y + 3),
                     (int16_t)(e.w - 8), &s_d.text[from], DSK_BLACK,
                     DSK_WHITE);
        if (s_d.caret_lit) {
            const int16_t cx =
                (int16_t)(e.x + 4 + (s_d.len - from) * dsk_ui_w());
            dsk_fill(dsk_rect(cx, (int16_t)(e.y + 3), 2, dsk_ui_h()),
                     DSK_BLACK);
        }
    } else {
        for (int i = 0; i < s_d.nlines; i++) {
            dsk_text_fit((int16_t)(client.x + PAD),
                         (int16_t)(client.y + PAD +
                                   i * (dsk_ui_h() + 2)),
                         (int16_t)(client.w - 2 * PAD), s_d.line[i],
                         DSK_BLACK, DSK_LGRAY);
        }
    }

    const dsk_rect_t kb = kbd_rect(client);
    if (!dsk_rect_empty(kb)) {
        dsk_kbd_draw(kb);
    }

    for (int i = 0; i < btn_count(); i++) {
        const dsk_rect_t b = btn_rect(client, i);
        dsk_panel(b, true, DSK_LGRAY);
        /* The default answer gets the black outline it had in 3.11. */
        if (i == 0) {
            dsk_frame(dsk_rect_inset(b, -1), DSK_BLACK);
        }
        const char   *label = btn_label(i);
        const int16_t tw = (int16_t)(dsk_ui_w() * (int16_t)str_len(label));
        dsk_text((int16_t)(b.x + (b.w - tw) / 2),
                 (int16_t)(b.y + (b.h - dsk_ui_h()) / 2), label, DSK_BLACK,
                 DSK_LGRAY);
    }
}

static bool dlg_pointer(dsk_win_t *w, dsk_hit_t where, int16_t x, int16_t y,
                        uint8_t buttons, dsk_ptr_t type, bool dbl)
{
    const bool down = (type == DSK_PTR_DOWN);
    (void)buttons;
    (void)dbl;
    if (where != DSK_HIT_CLIENT || !down) {
        return false;
    }
    const dsk_rect_t client = dsk_wm_client(w);
    for (int i = 0; i < btn_count(); i++) {
        if (dsk_rect_has(btn_rect(client, i), x, y)) {
            answer(btn_answer(i));
            return true;
        }
    }

    const dsk_rect_t kb = kbd_rect(client);
    if (!dsk_rect_empty(kb) && dsk_rect_has(kb, x, y)) {
        const int c = dsk_kbd_press(kb, x, y);
        if (c == DSK_KBD_SHIFT) {
            dsk_wm_damage_rect(kb); /* every label changed case */
        } else if (c == DSK_KBD_BACKSPACE) {
            if (s_d.len > 0) {
                s_d.text[--s_d.len] = '\0';
                dsk_wm_damage_rect(edit_rect(client));
            }
        } else if (c >= 0x20 && c < 0x7F && s_d.len + 1 < DSK_INPUT_MAX) {
            s_d.text[s_d.len++] = (char)c;
            s_d.text[s_d.len] = '\0';
            dsk_wm_damage_rect(edit_rect(client));
            /* Shift falls back to lower case after one letter, so the
             * keyboard's own face changed too. */
            dsk_wm_damage_rect(kb);
        }
        return true;
    }
    return true;
}

static bool dlg_key(dsk_win_t *w, uint16_t keycode, uint32_t unicode,
                    uint16_t mods)
{
    (void)w;
    (void)mods;
    if (keycode == DSK_KEY_ENTER) {
        answer(btn_answer(0));
        return true;
    }
    if (keycode == DSK_KEY_ESC) {
        answer(btn_count() > 1 ? btn_answer(1) : btn_answer(0));
        return true;
    }
    if (!s_d.input) {
        return true; /* a message box eats the rest */
    }
    if (keycode == DSK_KEY_BACKSPACE) {
        if (s_d.len > 0) {
            s_d.text[--s_d.len] = '\0';
            dsk_wm_damage_rect(edit_rect(dsk_wm_client(w)));
        }
        return true;
    }
    /*
     * Printable ASCII only, and no cursor keys: this box exists to type a
     * path or a name into, and a full line editor is not what Phase 1 owes
     * anybody (docs/plans/desktop.md §9.13).
     */
    if (unicode >= 0x20u && unicode < 0x7Fu && s_d.len + 1 < DSK_INPUT_MAX) {
        s_d.text[s_d.len++] = (char)unicode;
        s_d.text[s_d.len] = '\0';
        dsk_wm_damage_rect(edit_rect(dsk_wm_client(w)));
    }
    return true;
}

static void dlg_closed(dsk_win_t *w)
{
    (void)w;
    /*
     * The window went away without an answer - the close box, or close_all on
     * the way out.  Treat it as the cancelling answer, exactly as clicking
     * the second button would.
     */
    if (s_d.up) {
        s_d.win = NULL;
        answer(btn_count() > 1 ? btn_answer(1) : btn_answer(0));
    }
}

static const dsk_win_ops_t k_dlg_ops = {
    .draw = dlg_draw,
    .key = dlg_key,
    .pointer = dlg_pointer,
    .closed = dlg_closed,
};

/* ---- opening ----------------------------------------------------------- */

void dsk_dlg_init(const dsk_metrics_t *m)
{
    if (m != NULL) {
        s_m = *m;
    }
    s_d.up = false;
    s_d.win = NULL;
}

bool dsk_dlg_up(void) { return s_d.up; }

static dsk_win_t *open_window(const char *title, int16_t w, int16_t h)
{
    if (w > s_m.work.w) {
        w = s_m.work.w;
    }
    if (h > s_m.work.h) {
        h = s_m.work.h;
    }
    const dsk_rect_t f =
        dsk_rect((int16_t)(s_m.work.x + (s_m.work.w - w) / 2),
                 (int16_t)(s_m.work.y + (s_m.work.h - h) / 3), w, h);
    dsk_win_t *win = dsk_wm_open(title, f, &k_dlg_ops, NULL);
    if (win != NULL) {
        win->modal = true;
        win->resizable = false;
    }
    return win;
}

/* Wide enough for the longest line, within what the work area allows. */
static int16_t width_for_lines(void)
{
    size_t n = 0;
    for (int i = 0; i < s_d.nlines; i++) {
        const size_t m = str_len(s_d.line[i]);
        if (m > n) {
            n = m;
        }
    }
    int16_t w = (int16_t)(dsk_ui_w() * (int16_t)n + 2 * PAD + 8);
    const int16_t least = (int16_t)(2 * BTN_W + BTN_GAP + 2 * PAD);
    if (w < least) {
        w = least;
    }
    return w;
}

/* Shared by both message forms: the lines are already in s_d. */
static bool open_message(const char *title, dsk_dlg_kind_t kind,
                         void (*done)(dsk_answer_t a, void *ctx), void *ctx)
{
    s_d.kind = kind;
    s_d.input = false;
    s_d.done_msg = done;
    s_d.ctx = ctx;
    s_d.up = true;

    const int16_t h =
        (int16_t)(2 * s_m.border + s_m.title_h + PAD +
                  s_d.nlines * (dsk_ui_h() + 2) + PAD + BTN_H + PAD);
    s_d.win = open_window(title, width_for_lines(), h);
    if (s_d.win == NULL) {
        s_d.up = false;
        return false;
    }
    return true;
}

bool dsk_dlg_message(const char *title, const char *line1, const char *line2,
                     dsk_dlg_kind_t kind,
                     void (*done)(dsk_answer_t a, void *ctx), void *ctx)
{
    if (s_d.up) {
        return false;
    }
    dlg_t fresh = {0};
    s_d = fresh;
    str_copy(s_d.line[0], LINE_MAX, line1);
    s_d.nlines = 1;
    if (line2 != NULL && line2[0] != '\0') {
        str_copy(s_d.line[1], LINE_MAX, line2);
        s_d.nlines = 2;
    }
    return open_message(title, kind, done, ctx);
}

bool dsk_dlg_lines(const char *title, const char *const *lines, int n,
                   void (*done)(dsk_answer_t a, void *ctx), void *ctx)
{
    if (s_d.up || lines == NULL || n <= 0) {
        return false;
    }
    dlg_t fresh = {0};
    s_d = fresh;
    if (n > DSK_DLG_LINES_MAX) {
        n = DSK_DLG_LINES_MAX;
    }
    for (int i = 0; i < n; i++) {
        str_copy(s_d.line[i], LINE_MAX, lines[i]);
    }
    s_d.nlines = n;
    return open_message(title, DSK_DLG_OK, done, ctx);
}

bool dsk_dlg_input(const char *title, const char *prompt, const char *initial,
                   void (*done)(dsk_answer_t a, const char *text, void *ctx),
                   void *ctx)
{
    if (s_d.up) {
        return false;
    }
    dlg_t fresh = {0};
    s_d = fresh;
    s_d.kind = DSK_DLG_OKCANCEL;
    s_d.input = true;
    str_copy(s_d.line[0], LINE_MAX, prompt);
    s_d.nlines = 1;
    str_copy(s_d.text, sizeof(s_d.text), initial);
    s_d.len = (uint16_t)str_len(s_d.text);
    s_d.done_txt = done;
    s_d.ctx = ctx;
    s_d.caret_lit = true;
    s_d.up = true;

    s_d.keys = s_want_keys;
    dsk_kbd_reset();

    int16_t h =
        (int16_t)(2 * s_m.border + s_m.title_h + PAD + dsk_ui_h() + 4 +
                  dsk_ui_h() + 6 + PAD + BTN_H + PAD);
    int16_t w = width_for_lines();
    if (w < 240) {
        w = 240;
    }
    if (s_d.keys) {
        /*
         * As wide as there is room for, because the keys divide that width
         * by ten: on a 320-pixel panel a box sized to its prompt would give
         * keys of eighteen pixels, and a fingertip is wider than that.
         */
        w = s_m.work.w;
        /* What is left of the screen once the box's own furniture has had
         * its share; asking for more than this gets the box cut down. */
        const int16_t room = (int16_t)(s_m.work.h - h - PAD);
        const int16_t kh = dsk_kbd_height_for(
            (int16_t)(w - 2 * s_m.border - 2 * PAD), room);
        if (kh > 0) {
            h = (int16_t)(h + kh + PAD);
        } else {
            s_d.keys = false;
        }
    }
    s_d.win = open_window(title, w, h);
    if (s_d.win == NULL) {
        s_d.up = false;
        return false;
    }
    return true;
}

/* ---- the caret --------------------------------------------------------- */

uint32_t dsk_dlg_wait_ms(uint32_t now_ms)
{
    if (!s_d.up || !s_d.input) {
        return UINT32_MAX;
    }
    const uint32_t since = now_ms - s_d.caret_at;
    return (since >= BLINK_MS) ? 0u : (BLINK_MS - since);
}

void dsk_dlg_tick(uint32_t now_ms)
{
    if (dsk_dlg_wait_ms(now_ms) != 0u) {
        return;
    }
    s_d.caret_at = now_ms;
    s_d.caret_lit = !s_d.caret_lit;
    if (s_d.win != NULL) {
        /*
         * The edit box, not the window: a caret that damaged the whole dialog
         * twice a second would repaint two hundred lines of text for two
         * pixels of change.
         */
        dsk_wm_damage_rect(edit_rect(dsk_wm_client(s_d.win)));
    }
}
