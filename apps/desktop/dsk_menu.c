/*
 * ArgonOS DESKTOP - the menu bar.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "dsk_menu.h"

#include "dsk_paint.h"

#define PAD_X   6  /* space either side of a title on the bar   */
#define ITEM_H  16
#define SEP_H   6
#define DROP_PAD 3 /* the drop-down's own border                */

static dsk_metrics_t s_m;
static void (*s_damage)(dsk_rect_t r);
static void (*s_chose)(uint16_t id);

static dsk_menu_t *s_menus;
static int         s_n;
static int         s_open = -1; /* which title is dropped down, -1 for none */
static int         s_hi = -1;   /* which item is highlighted                */

static size_t label_len(const char *s)
{
    size_t n = 0;
    if (s != NULL) {
        while (s[n] != '\0') {
            n++;
        }
    }
    return n;
}

static void damage(dsk_rect_t r)
{
    if (s_damage != NULL) {
        s_damage(r);
    }
}

void dsk_menu_init(const dsk_metrics_t *m, void (*damage_fn)(dsk_rect_t r),
                   void (*chose)(uint16_t id))
{
    if (m != NULL) {
        s_m = *m;
    }
    s_damage = damage_fn;
    s_chose = chose;
    s_open = -1;
    s_hi = -1;
}

void dsk_menu_set(dsk_menu_t *menus, int n)
{
    s_menus = menus;
    s_n = (n < 0) ? 0 : ((n > DSK_MENU_MAX) ? DSK_MENU_MAX : n);
}

dsk_menu_t *dsk_menu_get(int which)
{
    if (s_menus == NULL || which < 0 || which >= s_n) {
        return NULL;
    }
    return &s_menus[which];
}

bool dsk_menu_open(void) { return s_open >= 0; }

/* ---- geometry ---------------------------------------------------------- */

dsk_rect_t dsk_menu_title_rect(int which)
{
    if (s_menus == NULL || which < 0 || which >= s_n ||
        dsk_rect_empty(s_m.menubar)) {
        return dsk_rect_none();
    }
    int16_t at = PAD_X;
    for (int i = 0; i < which; i++) {
        at = (int16_t)(at +
                       (int16_t)(DSK_FONT_W * (int16_t)label_len(
                                                  s_menus[i].title)) +
                       2 * PAD_X);
    }
    const int16_t w =
        (int16_t)(DSK_FONT_W * (int16_t)label_len(s_menus[which].title) +
                  2 * PAD_X);
    return dsk_rect(at, s_m.menubar.y, w, (int16_t)(s_m.menubar.h - 1));
}

static int16_t drop_width(const dsk_menu_t *mn)
{
    size_t widest = 0;
    for (uint8_t i = 0; i < mn->n; i++) {
        const size_t n = label_len(mn->items[i].label);
        if (n > widest) {
            widest = n;
        }
    }
    /* Room for a tick to the left and a little air to the right. */
    return (int16_t)(DSK_FONT_W * (int16_t)widest + 12 + 2 * DROP_PAD + 8);
}

static int16_t drop_height(const dsk_menu_t *mn)
{
    int16_t h = 2 * DROP_PAD;
    for (uint8_t i = 0; i < mn->n; i++) {
        h = (int16_t)(h + (mn->items[i].separator ? SEP_H : ITEM_H));
    }
    return h;
}

dsk_rect_t dsk_menu_drop_rect(int which)
{
    const dsk_menu_t *mn = dsk_menu_get(which);
    if (mn == NULL || mn->n == 0) {
        return dsk_rect_none();
    }
    const dsk_rect_t t = dsk_menu_title_rect(which);
    int16_t          w = drop_width(mn);
    const int16_t    h = drop_height(mn);
    int16_t          x = t.x;

    if (w > s_m.screen_w) {
        w = s_m.screen_w;
    }
    /* Slide back onto the screen rather than hang off the right edge. */
    if (x + w > s_m.screen_w) {
        x = (int16_t)(s_m.screen_w - w);
    }
    if (x < 0) {
        x = 0;
    }
    return dsk_rect(x, (int16_t)(s_m.menubar.y + s_m.menubar.h), w, h);
}

dsk_rect_t dsk_menu_item_rect(int which, int item)
{
    const dsk_menu_t *mn = dsk_menu_get(which);
    if (mn == NULL || item < 0 || item >= (int)mn->n) {
        return dsk_rect_none();
    }
    const dsk_rect_t d = dsk_menu_drop_rect(which);
    if (dsk_rect_empty(d)) {
        return dsk_rect_none();
    }
    int16_t y = (int16_t)(d.y + DROP_PAD);
    for (int i = 0; i < item; i++) {
        y = (int16_t)(y + (mn->items[i].separator ? SEP_H : ITEM_H));
    }
    return dsk_rect((int16_t)(d.x + DROP_PAD), y,
                    (int16_t)(d.w - 2 * DROP_PAD),
                    mn->items[item].separator ? SEP_H : ITEM_H);
}

int dsk_menu_title_at(int16_t x, int16_t y)
{
    for (int i = 0; i < s_n; i++) {
        if (dsk_rect_has(dsk_menu_title_rect(i), x, y)) {
            return i;
        }
    }
    return -1;
}

int dsk_menu_item_at(int16_t x, int16_t y)
{
    const dsk_menu_t *mn = dsk_menu_get(s_open);
    if (mn == NULL) {
        return -1;
    }
    for (int i = 0; i < (int)mn->n; i++) {
        if (dsk_rect_has(dsk_menu_item_rect(s_open, i), x, y)) {
            return i;
        }
    }
    return -1;
}

/* ---- drawing ----------------------------------------------------------- */

void dsk_menu_draw_bar(dsk_rect_t area)
{
    if (dsk_rect_empty(s_m.menubar) || !dsk_visible(s_m.menubar)) {
        return;
    }
    (void)area;
    dsk_fill(s_m.menubar, DSK_LGRAY);
    dsk_hline(s_m.menubar.x, (int16_t)(dsk_rect_y2(s_m.menubar) - 1),
              s_m.menubar.w, DSK_DGRAY);

    for (int i = 0; i < s_n; i++) {
        const dsk_rect_t t = dsk_menu_title_rect(i);
        if (dsk_rect_empty(t)) {
            continue;
        }
        const bool lit = (i == s_open);
        if (lit) {
            dsk_fill(t, DSK_NAVY);
        }
        dsk_text((int16_t)(t.x + PAD_X), (int16_t)(t.y + 1), s_menus[i].title,
                 lit ? DSK_WHITE : DSK_BLACK, lit ? DSK_NAVY : DSK_LGRAY);
    }
}

void dsk_menu_draw_open(dsk_rect_t area)
{
    const dsk_menu_t *mn = dsk_menu_get(s_open);
    if (mn == NULL) {
        return;
    }
    const dsk_rect_t d = dsk_menu_drop_rect(s_open);
    if (dsk_rect_empty(d) || !dsk_rect_overlaps(d, area)) {
        return;
    }
    dsk_clip(dsk_rect_clip(d, area));
    dsk_panel(d, true, DSK_LGRAY);
    /* One black line outside the bevel: a menu has to lift off the desktop. */
    dsk_frame(d, DSK_BLACK);

    for (int i = 0; i < (int)mn->n; i++) {
        const dsk_menu_item_t *it = &mn->items[i];
        const dsk_rect_t       r = dsk_menu_item_rect(s_open, i);
        if (dsk_rect_empty(r)) {
            continue;
        }
        if (it->separator) {
            const int16_t y = (int16_t)(r.y + r.h / 2);
            dsk_hline((int16_t)(r.x + 2), y, (int16_t)(r.w - 4), DSK_DGRAY);
            dsk_hline((int16_t)(r.x + 2), (int16_t)(y + 1), (int16_t)(r.w - 4),
                      DSK_WHITE);
            continue;
        }
        const bool lit = (i == s_hi) && it->enabled;
        if (lit) {
            dsk_fill(r, DSK_NAVY);
        }
        if (it->checked) {
            dsk_text((int16_t)(r.x + 2), r.y, "\xFB",
                     lit ? DSK_WHITE : DSK_BLACK,
                     lit ? DSK_NAVY : DSK_LGRAY);
        }
        /*
         * A disabled item is grey, not absent.  It says the shell can do the
         * thing and cannot do it now, which is information; leaving it out
         * would make the menu change shape between openings.
         */
        const uint32_t fg = !it->enabled ? DSK_DGRAY
                                         : (lit ? DSK_WHITE : DSK_BLACK);
        dsk_text_fit((int16_t)(r.x + 12), r.y, (int16_t)(r.w - 14), it->label,
                     fg, lit ? DSK_NAVY : DSK_LGRAY);
    }
    dsk_clip_reset();
}

/* ---- opening, closing, choosing ---------------------------------------- */

static void open_at(int which)
{
    if (which == s_open) {
        return;
    }
    const dsk_rect_t was = dsk_menu_drop_rect(s_open);
    s_open = which;
    s_hi = -1;
    if (!dsk_rect_empty(was)) {
        damage(was);
    }
    damage(s_m.menubar);
    const dsk_rect_t now = dsk_menu_drop_rect(s_open);
    if (!dsk_rect_empty(now)) {
        damage(now);
    }
}

void dsk_menu_close(void)
{
    if (s_open < 0) {
        return;
    }
    const dsk_rect_t was = dsk_menu_drop_rect(s_open);
    s_open = -1;
    s_hi = -1;
    if (!dsk_rect_empty(was)) {
        damage(was);
    }
    damage(s_m.menubar);
}

static void highlight(int item)
{
    if (item == s_hi) {
        return;
    }
    const dsk_rect_t a = dsk_menu_item_rect(s_open, s_hi);
    const dsk_rect_t b = dsk_menu_item_rect(s_open, item);
    s_hi = item;
    if (!dsk_rect_empty(a)) {
        damage(a);
    }
    if (!dsk_rect_empty(b)) {
        damage(b);
    }
}

static void choose(int item)
{
    const dsk_menu_t *mn = dsk_menu_get(s_open);
    if (mn == NULL || item < 0 || item >= (int)mn->n) {
        return;
    }
    const dsk_menu_item_t it = mn->items[item];
    if (it.separator || !it.enabled || it.id == 0) {
        return;
    }
    /*
     * Closed first, then told.  A handler that opens a window would otherwise
     * be drawing under a menu that is still on the screen, and the damage the
     * close leaves behind would then wipe it.
     */
    dsk_menu_close();
    if (s_chose != NULL) {
        s_chose(it.id);
    }
}

/* Step to the next item that can actually be picked, skipping separators. */
static int next_pickable(int from, int step)
{
    const dsk_menu_t *mn = dsk_menu_get(s_open);
    if (mn == NULL || mn->n == 0) {
        return -1;
    }
    int at = from;
    for (int guard = 0; guard < (int)mn->n; guard++) {
        at += step;
        if (at < 0) {
            at = (int)mn->n - 1;
        }
        if (at >= (int)mn->n) {
            at = 0;
        }
        if (!mn->items[at].separator && mn->items[at].enabled) {
            return at;
        }
    }
    return -1;
}

bool dsk_menu_pointer(dsk_ptr_t type, int16_t x, int16_t y)
{
    const int on_title = dsk_menu_title_at(x, y);

    if (s_open < 0) {
        if (type == DSK_PTR_DOWN && on_title >= 0) {
            open_at(on_title);
            return true;
        }
        return false;
    }

    /* Open: the bar and the drop-down belong to the menu until it closes. */
    if (on_title >= 0) {
        if (type == DSK_PTR_DOWN) {
            /* A second press on the same title puts it away. */
            if (on_title == s_open) {
                dsk_menu_close();
            } else {
                open_at(on_title);
            }
        } else if (type == DSK_PTR_MOVE) {
            /* Sliding along the bar with the menu open walks the menus. */
            open_at(on_title);
        }
        return true;
    }

    const int item = dsk_menu_item_at(x, y);
    if (item >= 0) {
        highlight(item);
        if (type == DSK_PTR_UP || type == DSK_PTR_DOWN) {
            choose(item);
        }
        return true;
    }

    /* Off the menu entirely: a press elsewhere closes it and is swallowed. */
    if (type == DSK_PTR_DOWN) {
        dsk_menu_close();
        return true;
    }
    if (type == DSK_PTR_MOVE) {
        highlight(-1);
        return true;
    }
    return true;
}

bool dsk_menu_key(uint16_t keycode, uint32_t unicode, uint16_t mods)
{
    (void)unicode;
    if (s_open < 0) {
        /* F10 puts the shell on the bar, which is what it did in 3.11. */
        if (keycode == DSK_KEY_F10 && (mods & DSK_MOD_ALT) == 0) {
            open_at(0);
            return true;
        }
        return false;
    }
    switch (keycode) {
    case DSK_KEY_ESC:
        dsk_menu_close();
        return true;
    case DSK_KEY_DOWN:
        highlight(next_pickable(s_hi, 1));
        return true;
    case DSK_KEY_UP:
        highlight(next_pickable(s_hi < 0 ? 0 : s_hi, -1));
        return true;
    case DSK_KEY_LEFT:
        open_at((s_open + s_n - 1) % s_n);
        return true;
    case DSK_KEY_RIGHT:
        open_at((s_open + 1) % s_n);
        return true;
    case DSK_KEY_ENTER:
    case DSK_KEY_SPACE:
        if (s_hi >= 0) {
            choose(s_hi);
        } else {
            dsk_menu_close();
        }
        return true;
    default:
        return true; /* the menu has the keyboard while it is open */
    }
}
