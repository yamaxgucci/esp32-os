/*
 * ArgonOS DESKTOP - the graphical shell, in the manner of Windows 3.11.
 *
 * Plan, decisions and the list of what is deliberately not here:
 * docs/plans/desktop.md.  This file is the event loop and the painting of the
 * desktop itself; windows, folders and files arrive in the phases after this
 * one.
 *
 * The loop blocks in ag_poll_event with no timeout unless something is
 * animating, which is the difference between a shell that costs nothing while
 * nobody touches it and one that eats a core to show a static picture.
 *
 *   run h:\desktop.axe          Esc or Q leaves
 *   run h:\desktop.axe 30       ... and leaves by itself after 30 seconds,
 *                               which is how the scripted runs stay bounded
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/keys.h>
#include <argon/libc.h>

#include "dsk.h"
#include "dsk_cursor.h"
#include "dsk_paint.h"

/*
 * Eight kilobytes of stack, not the default sixteen.
 *
 * The stack comes out of internal SRAM for the whole life of the process, and
 * on a board showing a 320x240 surface the largest block left is about fifteen
 * kilobytes - so the default is within a whisker of not fitting at all.  This
 * shell keeps its directory listings on the heap and recurses only as deep as
 * a directory tree, which is what the eight is measured against.
 */
AG_APP_SIZED("DESKTOP", "0.1", "argon", AG_AXE_NEEDS_GFX, 8 * 1024, 0);

#define DSK_VERSION "0.1"

static dsk_metrics_t s_m;
static dsk_damage_t  s_damage;

/* What the status strip says, and the counters behind it. */
static uint32_t s_ptr_events;
static uint32_t s_key_events;
static uint8_t  s_buttons;
static uint32_t s_repaints;
static uint32_t s_reflushes;
static bool     s_double_buf;

/* ---- the desktop's own furniture --------------------------------------- */

static const char *const k_menu[] = {"File", "Window", "Help"};
#define MENU_N ((int)(sizeof(k_menu) / sizeof(k_menu[0])))

/* Where each menu title sits, so a later phase can hit-test it. */
static dsk_rect_t menu_item_rect(int i)
{
    int16_t at = 6;
    for (int n = 0; n < MENU_N && n < i; n++) {
        at = (int16_t)(at + (int16_t)(8 * (int16_t)strlen(k_menu[n])) + 12);
    }
    if (i < 0 || i >= MENU_N) {
        return dsk_rect_none();
    }
    return dsk_rect(at, 1, (int16_t)(8 * (int16_t)strlen(k_menu[i])) + 8,
                    (int16_t)(s_m.menubar_h - 2));
}

static void draw_menubar(void)
{
    if (dsk_rect_empty(s_m.menubar) || !dsk_visible(s_m.menubar)) {
        return;
    }
    dsk_fill(s_m.menubar, DSK_LGRAY);
    /* One dark line under it, as the strip's only edge. */
    dsk_hline(s_m.menubar.x, (int16_t)(s_m.menubar.y + s_m.menubar.h - 1),
              s_m.menubar.w, DSK_DGRAY);
    for (int i = 0; i < MENU_N; i++) {
        const dsk_rect_t r = menu_item_rect(i);
        if (dsk_rect_empty(r)) {
            continue;
        }
        dsk_text((int16_t)(r.x + 4), (int16_t)(r.y + 1), k_menu[i], DSK_BLACK,
                 DSK_LGRAY);
    }
}

static void draw_statusbar(void)
{
    if (dsk_rect_empty(s_m.statusbar) || !dsk_visible(s_m.statusbar)) {
        return;
    }
    char        line[80];
    char        num[24];
    const int16_t ty = (int16_t)(s_m.statusbar.y + 2);

    dsk_fill(s_m.statusbar, DSK_LGRAY);
    dsk_hline(s_m.statusbar.x, s_m.statusbar.y, s_m.statusbar.w, DSK_WHITE);

    ag_strlcpy(line, "ptr ", sizeof(line));
    ag_strlcat(line, ag_utoa((uint64_t)(uint32_t)dsk_cursor_x(), num,
                             sizeof(num), 0, false),
               sizeof(line));
    ag_strlcat(line, ",", sizeof(line));
    ag_strlcat(line, ag_utoa((uint64_t)(uint32_t)dsk_cursor_y(), num,
                             sizeof(num), 0, false),
               sizeof(line));
    ag_strlcat(line, "  btn ", sizeof(line));
    ag_strlcat(line,
               ag_utoa((uint64_t)s_buttons, num, sizeof(num), 0, false),
               sizeof(line));
    ag_strlcat(line, "  ptrev ", sizeof(line));
    ag_strlcat(line, ag_utoa((uint64_t)s_ptr_events, num, sizeof(num), 0,
                             false),
               sizeof(line));
    ag_strlcat(line, "  keyev ", sizeof(line));
    ag_strlcat(line, ag_utoa((uint64_t)s_key_events, num, sizeof(num), 0,
                             false),
               sizeof(line));

    dsk_text_small((int16_t)(s_m.statusbar.x + 4), ty, line, DSK_BLACK,
                   DSK_LGRAY);
}

/*
 * A plate in the middle of the desktop saying what this is and what it is
 * drawing into.  It is here because the numbers on it are the ones a scripted
 * run needs to prove: the surface it got, whether that surface is
 * double-buffered, and that both fonts render.
 */
static dsk_rect_t welcome_rect(void)
{
    const int16_t w = (s_m.work.w < 232) ? (int16_t)(s_m.work.w - 16) : 232;
    const int16_t h = 76;
    return dsk_rect((int16_t)(s_m.work.x + (s_m.work.w - w) / 2),
                    (int16_t)(s_m.work.y + (s_m.work.h - h) / 2), w, h);
}

static void draw_welcome(void)
{
    const dsk_rect_t r = welcome_rect();
    if (dsk_rect_empty(r) || r.h < 40 || !dsk_visible(r)) {
        return;
    }
    char num[24];
    char line[64];

    dsk_panel(r, true, DSK_LGRAY);

    /* Caption, drawn the way a window's will be. */
    const dsk_rect_t cap =
        dsk_rect((int16_t)(r.x + 2), (int16_t)(r.y + 2), (int16_t)(r.w - 4),
                 s_m.title_h);
    dsk_fill(cap, DSK_NAVY);
    dsk_text_fit((int16_t)(cap.x + 3), (int16_t)(cap.y + 1),
                 (int16_t)(cap.w - 6), "ArgonOS Desktop", DSK_WHITE, DSK_NAVY);

    int16_t ty = (int16_t)(cap.y + cap.h + 4);

    ag_strlcpy(line, "surface ", sizeof(line));
    ag_strlcat(line, ag_utoa((uint64_t)(uint32_t)s_m.screen_w, num,
                             sizeof(num), 0, false),
               sizeof(line));
    ag_strlcat(line, "x", sizeof(line));
    ag_strlcat(line, ag_utoa((uint64_t)(uint32_t)s_m.screen_h, num,
                             sizeof(num), 0, false),
               sizeof(line));
    ag_strlcat(line, s_double_buf ? " db" : " single", sizeof(line));
    dsk_text_fit((int16_t)(r.x + 6), ty, (int16_t)(r.w - 12), line, DSK_BLACK,
                 DSK_LGRAY);
    ty = (int16_t)(ty + DSK_FONT_H);

    dsk_text_small((int16_t)(r.x + 6), ty,
                   "phase 0  -  Esc or Q to leave", DSK_BLACK, DSK_LGRAY);
}

/* Paint everything that falls inside r.  The only painting path there is. */
static void draw_region(dsk_rect_t r)
{
    if (dsk_rect_empty(r)) {
        return;
    }
    dsk_clip(r);

    dsk_fill(s_m.work, DSK_TEAL);
    draw_welcome();
    draw_menubar();
    draw_statusbar();

    dsk_clip_reset();
}

/*
 * Repaint what is damaged, then show it.
 *
 * The order is the whole of the software-cursor contract: the pointer comes off
 * the screen first, so that nothing is drawn under a stale copy of the pixels
 * it saved, and goes back on last.
 */
static void commit(void)
{
    if (s_damage.n == 0) {
        return;
    }
    dsk_damage_clip(&s_damage, s_m.screen);

    dsk_cursor_hide();
    for (uint8_t i = 0; i < s_damage.n; i++) {
        draw_region(s_damage.r[i]);
    }
    dsk_cursor_show();

    /* The pointer's square needs a flush even though it needed no repaint. */
    dsk_damage_add(&s_damage, dsk_cursor_rect());
    for (uint8_t i = 0; i < s_damage.n; i++) {
        dsk_flush(s_damage.r[i]);
    }
    dsk_damage_clear(&s_damage);
}

static void damage(dsk_rect_t r) { dsk_damage_add(&s_damage, r); }

/*
 * The status strip is rate limited, and the reason is the wire to the CYD.
 *
 * The pointer's own cost is two squares of sixteen pixels; repainting a strip
 * the whole width of the screen next to it multiplies that by twenty, for text
 * nobody can read while the mouse is moving anyway.  So a move marks the strip
 * dirty and it is redrawn at most ten times a second - which also means the
 * position it finally settles at is the position it displays.
 */
#define DSK_STATUS_MS 100u

static bool     s_status_dirty;
static uint32_t s_status_at;

static uint32_t status_due_in(uint32_t now)
{
    if (!s_status_dirty) {
        return UINT32_MAX;
    }
    const uint32_t since = now - s_status_at;
    return (since >= DSK_STATUS_MS) ? 0u : (DSK_STATUS_MS - since);
}

static void status_settle(uint32_t now)
{
    if (status_due_in(now) != 0u) {
        return;
    }
    s_status_dirty = false;
    s_status_at = now;
    damage(s_m.statusbar);
}

/* ---- main -------------------------------------------------------------- */

static uint32_t parse_seconds(int argc, char **argv)
{
    if (argc < 2 || argv[1] == NULL) {
        return 0;
    }
    uint32_t v = 0;
    for (const char *p = argv[1]; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
        v = v * 10u + (uint32_t)(*p - '0');
        if (v > 86400u) {
            return 86400u;
        }
    }
    return v;
}

int ag_main(int argc, char **argv)
{
    ag_gfxinfo_t info;

    if (ag_api()->gfx == NULL) {
        ag_printf("desktop: this machine has no display\n");
        return 1;
    }
    const ag_err_t err = ag_gfx_acquire(&info);
    if (err != AG_OK) {
        ag_printf("desktop: cannot take the display (%d)\n", (int)err);
        return 1;
    }
    if (info.fb == NULL) {
        /*
         * A board with no system surface at all - the pixels would have to be
         * the shell's own and go out in bands through gfx->present.  That
         * backend is planned (docs/plans/desktop.md §9.2) and is not here, and
         * drawing anyway would paint into nothing and say nothing about it.
         */
        ag_gfx_release();
        ag_printf("desktop: this display has no surface (%ux%u); "
                  "the band renderer is not built yet\n",
                  (unsigned)info.width, (unsigned)info.height);
        return 1;
    }

    s_double_buf = info.double_buf;
    /*
     * Said on the console as well as drawn on the plate, because a script
     * driving this cannot read the screen: the surface it got is the first
     * thing that has to be checkable from the transcript.
     */
    ag_printf("desktop: surface %ux%u %s\n", (unsigned)info.width,
              (unsigned)info.height, info.double_buf ? "double" : "single");

    dsk_metrics_init(&s_m, (int16_t)info.width, (int16_t)info.height);
    dsk_paint_bind_surface(info.fb, info.stride, s_m.screen_w, s_m.screen_h);
    dsk_cursor_init(s_m.screen_w, s_m.screen_h);

    /* First paint: everything, once. */
    dsk_damage_clear(&s_damage);
    damage(s_m.screen);
    commit();

    const uint32_t deadline_s = parse_seconds(argc, argv);
    const uint32_t started = ag_millis();
    bool           running = true;

    s_status_at = started;
    s_status_dirty = false;

    while (running) {
        /*
         * With nothing pending this blocks for ever, which is the point: a
         * shell showing a static picture must cost nothing at all.  A dirty
         * status strip or an armed deadline shortens the wait to whichever
         * comes first.
         */
        uint32_t   now = ag_millis();
        uint32_t   wait = status_due_in(now);
        ag_event_t ev;

        if (deadline_s != 0u) {
            const uint32_t elapsed = now - started;
            const uint32_t total = deadline_s * 1000u;
            const uint32_t left = (elapsed >= total) ? 0u : total - elapsed;
            if (left < wait) {
                wait = left;
            }
        }

        if (ag_poll_event(&ev, wait)) {
            switch (ev.type) {
            case AG_EV_POINTER_MOVE:
                s_ptr_events++;
                dsk_cursor_move(ev.ptr.x, ev.ptr.y);
                s_status_dirty = true;
                break;
            case AG_EV_POINTER_DOWN:
            case AG_EV_POINTER_UP:
                s_ptr_events++;
                s_buttons = ev.ptr.buttons;
                dsk_cursor_move(ev.ptr.x, ev.ptr.y);
                s_status_dirty = true;
                break;
            case AG_EV_WHEEL:
                s_ptr_events++;
                s_status_dirty = true;
                break;
            case AG_EV_KEY_DOWN:
                s_key_events++;
                if (ev.key.keycode == AG_KEY_ESC ||
                    ev.key.keycode == AG_KEY_Q) {
                    running = false;
                    break;
                }
                /*
                 * Repaint the lot.  F5 because that is what it will mean when
                 * there are folders to refresh, and useful before then for the
                 * question a screenshot cannot answer on its own: whether
                 * something is left on the glass because this shell never
                 * painted it, or because something else painted over it.
                 */
                if (ev.key.keycode == AG_KEY_F5) {
                    s_repaints++;
                    damage(s_m.screen);
                }
                /*
                 * Send the frame again without drawing a thing.
                 *
                 * This exists to separate two failures that look identical in
                 * a photograph: pixels this shell never wrote, and pixels it
                 * wrote that never reached the panel.  If F6 alone cleans the
                 * screen, the drawing was right and the sending was cut short.
                 */
                if (ev.key.keycode == AG_KEY_F6) {
                    s_reflushes++;
                    dsk_flush(s_m.screen);
                }
                s_status_dirty = true;
                break;
            case AG_EV_CHAR:
                if (ev.key.unicode == 'q' || ev.key.unicode == 'Q') {
                    running = false;
                }
                break;
            case AG_EV_QUIT:
                running = false;
                break;
            default:
                break;
            }
        }

        if (ag_interrupted()) {
            running = false;
        }
        now = ag_millis();
        if (deadline_s != 0u && (now - started) >= deadline_s * 1000u) {
            running = false;
        }
        status_settle(now);
        commit();
    }

    dsk_cursor_hide();
    ag_gfx_release();
    ag_color(AG_LGRAY, AG_BLACK);
    ag_cls();
    ag_cursor(true);
    ag_printf("desktop: %u pointer events, %u key events, %u repaints, "
              "%u reflushes\n",
              (unsigned)s_ptr_events, (unsigned)s_key_events,
              (unsigned)s_repaints, (unsigned)s_reflushes);
    return 0;
}
