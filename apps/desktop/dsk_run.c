/*
 * ArgonOS DESKTOP - running a program.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "dsk_run.h"

#include <argon/argon.h>
#include <argon/axe.h>
#include <argon/keys.h>
#include <argon/libc.h>

#include "dsk_cursor.h"
#include "dsk_paint.h"

static void (*s_repaint_all)(void);
static char s_note[64];

void dsk_run_init(void (*repaint_all)(void)) { s_repaint_all = repaint_all; }

static bool ends_with_i(const char *s, const char *suffix)
{
    return ag_ends_with_i(s, suffix);
}

/*
 * Does this image want the screen?
 *
 * Read out of the header rather than guessed from the name, because the answer
 * decides whether the shell waits for a key afterwards - and getting it wrong
 * either loses a console program's output or leaves the desktop hidden behind
 * nothing for no reason.
 */
static bool needs_gfx(const char *path, bool *known)
{
    *known = false;
    const ag_handle_t h = ag_open(path, AG_O_RDONLY);
    if (h < 0) {
        return false;
    }
    ag_axe_header_t hdr;
    const int32_t   n = ag_read(h, &hdr, sizeof(hdr));
    (void)ag_close(h);
    if (n != (int32_t)sizeof(hdr)) {
        return false;
    }
    /* "AXE1", four bytes, not a number: see ag_axe_header_t. */
    if (hdr.magic[0] != 'A' || hdr.magic[1] != 'X' || hdr.magic[2] != 'E' ||
        hdr.magic[3] != '1') {
        return false;
    }
    *known = true;
    return (hdr.flags & AG_AXE_NEEDS_GFX) != 0;
}

/* Wait for a key or a click, with the console still showing what was printed. */
static void wait_for_a_key(void)
{
    ag_print("\n-- press a key to go back to the desktop --\n");
    for (;;) {
        ag_event_t ev;
        if (!ag_poll_event(&ev, 30000u)) {
            return; /* nobody there; do not sit here for ever */
        }
        if (ev.type == AG_EV_KEY_DOWN || ev.type == AG_EV_CHAR ||
            ev.type == AG_EV_POINTER_DOWN || ev.type == AG_EV_QUIT) {
            return;
        }
    }
}

/*
 * Hand over the machine, then take it back.
 *
 * The console line before ag_exec is not decoration.  Loading an image off
 * HostFS or through flash XIP takes long enough to look like a hang, and by
 * then the desktop is gone and the screen is blank - `fm` prints the same
 * thing for the same reason.
 */
static void exec_and_return(const char *path, int argc, const char **argv,
                            bool console)
{
    dsk_cursor_hide();
    ag_gfx_release();
    ag_color(AG_LGRAY, AG_BLACK);
    ag_cls();
    ag_cursor(true);
    ag_print("Loading ");
    ag_print(path);
    ag_print(" ...\n");

    /*
     * Spawned and waited for rather than exec'd, and the reason is Ctrl+C: a
     * wait with a timeout can notice that the interrupt was aimed at the
     * child and kill it, where exec would leave the desktop with no say.
     *
     * Nothing here hands the foreground over any more.  It used to, and it
     * did not work: the session decides where input goes and whose console is
     * on the screen, and the session's notion of who is in a slot is not the
     * foreground pid.  A child now goes on top of its parent's slot in the
     * kernel (ag_session_push_to), which is where that belongs - the shell
     * never had to do this either.
     */
    int32_t        status = -1;
    const ag_pid_t child = ag_spawn(path, argc, argv, AG_SPAWN_FOREGROUND);
    if (child < 0) {
        status = (int32_t)child;
    } else {
        while (ag_wait(child, &status, 200u) != AG_OK) {
            if (ag_interrupted()) {
                (void)ag_kill(child);
            }
        }
    }

    /*
     * Always say how it ended; only stop for a key when there is something to
     * read.  A graphical program's screen is gone the moment it returns, so
     * holding the desktop back for a key would be holding it back over an
     * empty screen - but the line still belongs in the console behind, which
     * is where anyone looking for what happened will look, and it is the only
     * trace a program that prints nothing leaves at all.
     */
    char num[24];
    ag_print("\n");
    ag_print(path);
    ag_print(" finished with ");
    ag_print(ag_utoa((uint64_t)(uint32_t)status, num, sizeof(num), 0, false));
    ag_print("\n");
    if (console || status != 0) {
        wait_for_a_key();
    }

    /*
     * The display has to be taken again, and the events the program left
     * behind thrown away: a key pressed to dismiss its output must not also
     * arrive at the desktop, and a pointer position from before is stale.
     */
    ag_gfxinfo_t info;
    if (ag_gfx_acquire(&info) != AG_OK) {
        return;
    }
    ag_cursor(false);
    ag_flush_input();
    if (s_repaint_all != NULL) {
        s_repaint_all();
    }
}

/* The handler [assoc] names for this extension, or NULL. */
static const char *assoc_for(const char *path, char *buf, size_t len)
{
    const char *dot = NULL;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '.') {
            dot = p;
        }
        if (*p == '\\' || *p == '/') {
            dot = NULL;
        }
    }
    if (dot == NULL) {
        return NULL;
    }
    char key[32];
    ag_strlcpy(key, "assoc", sizeof(key));
    ag_strlcat(key, dot, sizeof(key)); /* `dot` still carries its own '.' */
    if (g_ag_api->cfg == NULL ||
        g_ag_api->cfg->get_str(key, buf, len) != AG_OK || buf[0] == '\0') {
        return NULL;
    }
    return buf;
}

void dsk_run_open(const char *path, const char **note)
{
    if (path == NULL || path[0] == '\0') {
        return;
    }
    const char *argv[2] = {path, NULL};

    if (ends_with_i(path, ".AXE")) {
        bool       known = false;
        const bool gfx = needs_gfx(path, &known);
        if (!known) {
            ag_strlcpy(s_note, "not an ArgonOS program", sizeof(s_note));
            *note = s_note;
            return;
        }
        exec_and_return(path, 1, argv, !gfx);
        return;
    }
    if (ends_with_i(path, ".SYS")) {
        /*
         * A driver is not an application and the loader refuses it, so say so
         * here rather than letting `run` say it on a console nobody is
         * looking at.
         */
        ag_strlcpy(s_note, "a driver: use drv install", sizeof(s_note));
        *note = s_note;
        return;
    }

    char handler[AG_PATH_MAX];
    if (assoc_for(path, handler, sizeof(handler)) != NULL) {
        /*
         * The handler may be a builtin command - `edit` is - and those are
         * the shell's, not ours.  An .AXE we can exec; anything else is a
         * command line, and there is no way to run one of those from an
         * application, so it is named and refused rather than half-done.
         */
        if (ends_with_i(handler, ".AXE")) {
            const char *hargv[2] = {handler, path};
            exec_and_return(handler, 2, hargv, false);
            return;
        }
        ag_strlcpy(s_note, "opens with the ", sizeof(s_note));
        ag_strlcat(s_note, handler, sizeof(s_note));
        ag_strlcat(s_note, " command", sizeof(s_note));
        *note = s_note;
        return;
    }

    ag_strlcpy(s_note, "no program for this kind of file", sizeof(s_note));
    *note = s_note;
}

void dsk_run_command(const char *line, const char **note)
{
    if (line == NULL || line[0] == '\0') {
        return;
    }
    /* One word: the image.  Anything after it is its arguments, unsplit. */
    char path[AG_PATH_MAX];
    int  n = 0;
    while (line[n] != '\0' && line[n] != ' ' && n + 1 < (int)sizeof(path)) {
        path[n] = line[n];
        n++;
    }
    path[n] = '\0';
    const char *rest = (line[n] == ' ') ? &line[n + 1] : NULL;

    ag_stat_t st;
    if (ag_stat(path, &st) != AG_OK) {
        ag_strlcpy(s_note, "no such file: ", sizeof(s_note));
        ag_strlcat(s_note, path, sizeof(s_note));
        *note = s_note;
        return;
    }
    if (!ends_with_i(path, ".AXE")) {
        dsk_run_open(path, note);
        return;
    }
    bool       known = false;
    const bool gfx = needs_gfx(path, &known);
    if (!known) {
        ag_strlcpy(s_note, "not an ArgonOS program", sizeof(s_note));
        *note = s_note;
        return;
    }
    if (rest != NULL && rest[0] != '\0') {
        const char *argv[3] = {path, rest, NULL};
        exec_and_return(path, 2, argv, !gfx);
    } else {
        const char *argv[2] = {path, NULL};
        exec_and_return(path, 1, argv, !gfx);
    }
}
