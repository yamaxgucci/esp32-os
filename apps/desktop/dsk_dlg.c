/*
 * ArgonOS DESKTOP - message and input dialogs.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "dsk_dlg.h"

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
    char        line1[LINE_MAX];
    char        line2[LINE_MAX];
    char        text[DSK_INPUT_MAX];
    uint16_t    len;
    bool        caret_lit;
    uint32_t    caret_at;
    dsk_win_t  *win;
    void       *ctx;
    void (*done_msg)(dsk_answer_t a, void *ctx);
    void (*done_txt)(dsk_answer_t a, const char *text, void *ctx);
} dlg_t;

static dsk_metrics_t s_m;
static dlg_t         s_d;

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

static dsk_rect_t edit_rect(dsk_rect_t client)
{
    return dsk_rect((int16_t)(client.x + PAD),
                    (int16_t)(client.y + PAD + DSK_FONT_H + 4),
                    (int16_t)(client.w - 2 * PAD), (int16_t)(DSK_FONT_H + 6));
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
                     (int16_t)(client.w - 2 * PAD), s_d.line1, DSK_BLACK,
                     DSK_LGRAY);
        const dsk_rect_t e = edit_rect(client);
        dsk_panel(e, false, DSK_WHITE);
        /* The tail of a long line, so what is being typed stays in sight. */
        const int16_t room = (int16_t)((e.w - 8) / DSK_FONT_W);
        const int16_t from =
            (int16_t)((s_d.len > (uint16_t)room) ? s_d.len - room : 0);
        dsk_text_fit((int16_t)(e.x + 4), (int16_t)(e.y + 3),
                     (int16_t)(e.w - 8), &s_d.text[from], DSK_BLACK,
                     DSK_WHITE);
        if (s_d.caret_lit) {
            const int16_t cx =
                (int16_t)(e.x + 4 + (s_d.len - from) * DSK_FONT_W);
            dsk_fill(dsk_rect(cx, (int16_t)(e.y + 3), 2, DSK_FONT_H),
                     DSK_BLACK);
        }
    } else {
        dsk_text_fit((int16_t)(client.x + PAD), (int16_t)(client.y + PAD),
                     (int16_t)(client.w - 2 * PAD), s_d.line1, DSK_BLACK,
                     DSK_LGRAY);
        if (s_d.line2[0] != '\0') {
            dsk_text_fit((int16_t)(client.x + PAD),
                         (int16_t)(client.y + PAD + DSK_FONT_H + 2),
                         (int16_t)(client.w - 2 * PAD), s_d.line2, DSK_BLACK,
                         DSK_LGRAY);
        }
    }

    for (int i = 0; i < btn_count(); i++) {
        const dsk_rect_t b = btn_rect(client, i);
        dsk_panel(b, true, DSK_LGRAY);
        /* The default answer gets the black outline it had in 3.11. */
        if (i == 0) {
            dsk_frame(dsk_rect_inset(b, -1), DSK_BLACK);
        }
        const char   *label = btn_label(i);
        const int16_t tw = (int16_t)(DSK_FONT_W * (int16_t)str_len(label));
        dsk_text((int16_t)(b.x + (b.w - tw) / 2),
                 (int16_t)(b.y + (b.h - DSK_FONT_H) / 2), label, DSK_BLACK,
                 DSK_LGRAY);
    }
}

static bool dlg_pointer(dsk_win_t *w, dsk_hit_t where, int16_t x, int16_t y,
                        uint8_t buttons, bool down, bool dbl)
{
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
static int16_t width_for(const char *a, const char *b)
{
    size_t n = str_len(a);
    const size_t m = str_len(b);
    if (m > n) {
        n = m;
    }
    int16_t w = (int16_t)(DSK_FONT_W * (int16_t)n + 2 * PAD + 8);
    const int16_t least = (int16_t)(2 * BTN_W + BTN_GAP + 2 * PAD);
    if (w < least) {
        w = least;
    }
    return w;
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
    s_d.kind = kind;
    s_d.input = false;
    str_copy(s_d.line1, sizeof(s_d.line1), line1);
    str_copy(s_d.line2, sizeof(s_d.line2), line2);
    s_d.done_msg = done;
    s_d.ctx = ctx;
    s_d.up = true;

    const int16_t lines = (line2 != NULL && line2[0] != '\0') ? 2 : 1;
    const int16_t h = (int16_t)(2 * s_m.border + s_m.title_h + PAD +
                                lines * (DSK_FONT_H + 2) + PAD + BTN_H + PAD);
    s_d.win = open_window(title, width_for(line1, line2), h);
    if (s_d.win == NULL) {
        s_d.up = false;
        return false;
    }
    return true;
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
    str_copy(s_d.line1, sizeof(s_d.line1), prompt);
    str_copy(s_d.text, sizeof(s_d.text), initial);
    s_d.len = (uint16_t)str_len(s_d.text);
    s_d.done_txt = done;
    s_d.ctx = ctx;
    s_d.caret_lit = true;
    s_d.up = true;

    const int16_t h =
        (int16_t)(2 * s_m.border + s_m.title_h + PAD + DSK_FONT_H + 4 +
                  DSK_FONT_H + 6 + PAD + BTN_H + PAD);
    int16_t w = width_for(prompt, NULL);
    if (w < 240) {
        w = 240;
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
