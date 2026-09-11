/*
 * ArgonOS DESKTOP - the shell's keyboard.  See dsk_oskbd.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "dsk_oskbd.h"

#include <string.h>

#include "dsk_kbd.h"
#include "dsk_paint.h"
#include "dsk_rect.h"

/* Enter, Esc and the one that puts the keyboard away. */
#define BTNS 3

static const dsk_metrics_t *s_m;
static void (*s_damage)(dsk_rect_t r);
static void (*s_key)(uint16_t keycode, uint32_t unicode);

static bool s_allowed = true;
static bool s_up;

/*
 * Which of the three is held down, or below zero.
 *
 * A key that does not look pressed is a key you cannot tell you have hit,
 * and on a panel there is nothing else to go by: no click, no travel, and a
 * stylus hides the very square it is on.
 */
static int s_btn_down = -1;

/*
 * Whether the row is answering a box or ending a line.
 *
 * The keys are the same two - a box takes Enter for OK and Esc for
 * Cancel, which is what a keyboard has always done - but what they are
 * CALLED matters on a panel, where the label is the only clue there is.
 */
static bool s_for_dialog;

void dsk_oskbd_mode(bool for_dialog) { s_for_dialog = for_dialog; }

static int16_t btn_h(void) { return (int16_t)(dsk_ui_h() + 6); }

/*
 * How tall it is when it is up, whether or not it is.
 *
 * Asked before it is raised: a box that has to be typed into is laid out
 * while the keys are still down, and it has to be put above where they are
 * going to be.  Answering only when visible put the first such box behind
 * the keyboard it had itself asked for.
 */
int16_t dsk_oskbd_height(void)
{
    if (s_m == NULL) {
        return 0;
    }
    const int16_t h = dsk_kbd_height_for(s_m->work.w,
                                         (int16_t)(s_m->work.h / 2));
    return (h > 0) ? (int16_t)(h + btn_h()) : 0;
}

dsk_rect_t dsk_oskbd_rect(void)
{
    if (!s_up || s_m == NULL) {
        return dsk_rect_none();
    }
    const int16_t total = dsk_oskbd_height();
    if (total <= 0) {
        return dsk_rect_none();
    }
    return dsk_rect(s_m->work.x, (int16_t)(dsk_rect_y2(s_m->work) - total),
                    s_m->work.w, total);
}

/* The letters: everything below the row of three. */
static dsk_rect_t keys_rect(void)
{
    const dsk_rect_t r = dsk_oskbd_rect();
    if (dsk_rect_empty(r)) {
        return r;
    }
    return dsk_rect(r.x, (int16_t)(r.y + btn_h()), r.w,
                    (int16_t)(r.h - btn_h()));
}

/*
 * Enter, Esc and Hide, across the top of it.
 *
 * The letters come from dsk_kbd, which was drawn for a dialog box: it has no
 * Enter because a dialog has an OK button, and no Esc because it has Cancel.
 * A prompt has neither, and a line nobody can end is not a prompt - so the
 * three that are missing live here, in front of the keys they belong to,
 * rather than by changing a layout the dialogs are hit-testing against.
 */
static dsk_rect_t btn_rect(int which)
{
    const dsk_rect_t r = dsk_oskbd_rect();
    if (dsk_rect_empty(r) || which < 0 || which >= BTNS) {
        return dsk_rect_none();
    }
    const int16_t w = (int16_t)(r.w / BTNS);
    const int16_t x = (int16_t)(r.x + which * w);
    return dsk_rect(x, r.y,
                    (which == BTNS - 1) ? (int16_t)(dsk_rect_x2(r) - x) : w,
                    btn_h());
}

static const char *btn_label(int which)
{
    switch (which) {
    case 0:  return s_for_dialog ? "OK" : "Enter";
    case 1:  return s_for_dialog ? "Cancel" : "Esc";
    default: return "Hide";
    }
}

void dsk_oskbd_init(const dsk_metrics_t *m, void (*damage)(dsk_rect_t r),
                    void (*key)(uint16_t keycode, uint32_t unicode))
{
    s_m = m;
    s_damage = damage;
    s_key = key;
    s_up = false;
    s_btn_down = -1;
}

void dsk_oskbd_allow(bool allowed)
{
    s_allowed = allowed;
    if (!allowed) {
        dsk_oskbd_show(false);
    }
}

bool dsk_oskbd_allowed(void) { return s_allowed; }
bool dsk_oskbd_visible(void) { return s_up; }

void dsk_oskbd_show(bool on)
{
    if (on && !s_allowed) {
        return;
    }
    if (on == s_up) {
        return;
    }

    /*
     * The rectangle is asked for while it is still there, so that putting
     * the keyboard away repaints what it was covering.  Asking afterwards
     * gives an empty one and leaves the keys on the glass.
     */
    const dsk_rect_t was = dsk_oskbd_rect();
    s_up = on;
    s_btn_down = -1;
    dsk_kbd_hilite(-1);
    if (on) {
        dsk_kbd_reset(); /* back to lower case, like a keyboard just picked up */
    }
    if (s_damage != NULL) {
        const dsk_rect_t now = dsk_oskbd_rect();
        s_damage(dsk_rect_empty(now) ? was : now);
    }
}

dsk_rect_t dsk_oskbd_work(void)
{
    if (s_m == NULL) {
        return dsk_rect_none();
    }
    const dsk_rect_t r = dsk_oskbd_rect();
    if (dsk_rect_empty(r)) {
        return s_m->work;
    }
    return dsk_rect(s_m->work.x, s_m->work.y, s_m->work.w,
                    (int16_t)(r.y - s_m->work.y));
}

void dsk_oskbd_draw(dsk_rect_t clip)
{
    const dsk_rect_t r = dsk_oskbd_rect();
    if (dsk_rect_empty(r) || !dsk_rect_overlaps(r, clip)) {
        return;
    }

    dsk_clip(clip);
    for (int i = 0; i < BTNS; i++) {
        const dsk_rect_t b = btn_rect(i);
        dsk_fill(b, DSK_LGRAY);
        dsk_bevel(b, s_btn_down != i);
        const char   *label = btn_label(i);
        const int16_t tw = (int16_t)(dsk_ui_w() * (int16_t)strlen(label));
        dsk_text((int16_t)(b.x + (b.w - tw) / 2),
                 (int16_t)(b.y + (b.h - dsk_ui_h()) / 2), label, DSK_BLACK,
                 DSK_LGRAY);
    }
    dsk_kbd_draw(keys_rect());
    dsk_clip_reset();
}

/* Redraw only the keys, which is what a highlight changes. */
static void damage_keys(void)
{
    if (s_damage != NULL) {
        s_damage(keys_rect());
    }
}

bool dsk_oskbd_pointer(dsk_ptr_t type, int16_t x, int16_t y)
{
    const dsk_rect_t r = dsk_oskbd_rect();
    if (dsk_rect_empty(r)) {
        return false;
    }

    /*
     * A release is taken wherever it happens once something is held, so the
     * highlight cannot be left on by dragging off the key.
     */
    if (type != DSK_PTR_DOWN) {
        const bool held = (s_btn_down >= 0);
        if (held) {
            const dsk_rect_t b = btn_rect(s_btn_down);
            s_btn_down = -1;
            if (s_damage != NULL) {
                s_damage(b);
            }
        }
        if (dsk_kbd_key_at(keys_rect(), x, y) >= 0 || held) {
            dsk_kbd_hilite(-1);
            damage_keys();
        }
        return dsk_rect_has(r, x, y) || held;
    }

    if (!dsk_rect_has(r, x, y)) {
        return false;
    }

    for (int i = 0; i < BTNS; i++) {
        const dsk_rect_t b = btn_rect(i);
        if (!dsk_rect_has(b, x, y)) {
            continue;
        }
        s_btn_down = i;
        if (s_damage != NULL) {
            s_damage(b);
        }
        if (i == 2) {
            dsk_oskbd_show(false);
        } else if (s_key != NULL) {
            if (i == 0) {
                s_key(DSK_KEY_ENTER, 0x0Du);
            } else {
                s_key(DSK_KEY_ESC, 0x1Bu);
            }
        }
        return true;
    }

    const dsk_rect_t keys = keys_rect();
    if (!dsk_rect_has(keys, x, y)) {
        return true; /* the gap between the rows is still ours */
    }

    dsk_kbd_hilite(dsk_kbd_key_at(keys, x, y));
    const int c = dsk_kbd_press(keys, x, y);
    if (c == DSK_KBD_SHIFT) {
        dsk_kbd_hilite(-1); /* every label changed case; nothing stays down */
    } else if (c == DSK_KBD_BACKSPACE && s_key != NULL) {
        s_key(DSK_KEY_BACKSPACE, 0x08u);
    } else if (c >= 0x20 && c < 0x7F && s_key != NULL) {
        s_key(0, (uint32_t)c);
    }
    damage_keys();
    return true;
}

void dsk_oskbd_probe(dsk_rect_t *keys, dsk_rect_t *row)
{
    /*
     * Where the keys are, whether or not they are up.
     *
     * For the test in the emulator, which has to hit them with a virtual
     * mouse.  It is asked of the program rather than worked out in the
     * script on purpose: the script that counted arrow keys into a menu was
     * wrong the moment the menu grew an item, and it took three runs and a
     * false accusation of the kernel to find out.
     */
    const bool was = s_up;
    s_up = true;
    if (keys != NULL) {
        *keys = keys_rect();
    }
    if (row != NULL) {
        *row = dsk_rect(dsk_oskbd_rect().x, dsk_oskbd_rect().y,
                        dsk_oskbd_rect().w, btn_h());
    }
    s_up = was;
}
