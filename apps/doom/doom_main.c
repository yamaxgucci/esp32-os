/*
 * ArgonOS - Doom (doomgeneric). Mute milestone.
 *
 *   python apps/doom/build.py
 *   run h:\doom.axe -iwad h:\doom1.wad
 *
 * Core: doomgeneric / Chocolate Doom, GPLv2+.  This file: Apache-2.0.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <string.h>

#include "doomgeneric.h"
#include "doomgeneric_argon.h"

AG_APP_SIZED("DOOM", "0.1", "argon", AG_AXE_NEEDS_GFX, 32 * 1024,
             5 * 1024 * 1024);

static int ends_with_ci(const char *s, const char *ext)
{
    size_t n, m, i;
    if (s == NULL || ext == NULL) {
        return 0;
    }
    n = strlen(s);
    m = strlen(ext);
    if (n < m) {
        return 0;
    }
    s += n - m;
    for (i = 0; i < m; i++) {
        char a = s[i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

static int arg_eq(const char *s, const char *lit)
{
    if (s == NULL) {
        return 0;
    }
    for (; *lit && *s; lit++, s++) {
        char cb = *s;
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (*lit != cb) {
            return 0;
        }
    }
    return *lit == '\0' && *s == '\0';
}

static int has_iwad_flag(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (arg_eq(argv[i], "-iwad")) {
            return 1;
        }
    }
    return 0;
}

/* frames90 or frames=90. Bare numbers clash with -warp / -skill. */
static int parse_frames_arg(const char *s, int *out)
{
    const char *lit = "frames";
    int         n = 0;
    if (s == NULL || out == NULL) {
        return 0;
    }
    while (*lit) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != *lit) {
            return 0;
        }
        lit++;
        s++;
    }
    if (*s == '=') {
        s++;
    }
    if (*s < '0' || *s > '9') {
        return 0;
    }
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        s++;
    }
    if (*s != '\0') {
        return 0;
    }
    *out = n;
    return 1;
}

static const char *first_wad(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (arg_eq(argv[i], "-iwad") && i + 1 < argc) {
            i++;
            continue;
        }
        if (ends_with_ci(argv[i], ".wad")) {
            return argv[i];
        }
    }
    return NULL;
}

static int file_ok(const char *path)
{
    ag_handle_t h = ag_open(path, AG_O_RDONLY);
    if (h < 0) {
        return 0;
    }
    ag_close(h);
    return 1;
}

int ag_main(int argc, char **argv)
{
    char        *own[24];
    int          n = 0;
    const char  *wad;
    int          i;
    int          livepad = 1;
    int          frames = -1;
    int          have_iwad;

    for (i = 1; i < argc; i++) {
        if (arg_eq(argv[i], "nolivepad")) {
            livepad = 0;
        } else {
            (void)parse_frames_arg(argv[i], &frames);
        }
    }
    doom_argon_set_live_pad(livepad);

    have_iwad = has_iwad_flag(argc, argv);
    wad = NULL;
    if (!have_iwad) {
        wad = first_wad(argc, argv);
        if (wad == NULL) {
            static const char *k_try[] = {
                "h:\\doom1.wad", "a:\\doom1.wad", "h:\\DOOM1.WAD",
                "a:\\DOOM1.WAD", NULL};
            int t;
            for (t = 0; k_try[t] != NULL; t++) {
                if (file_ok(k_try[t])) {
                    wad = k_try[t];
                    break;
                }
            }
        }
        if (wad == NULL) {
            ag_printf("doom: need shareware doom1.wad\n");
            ag_printf("doom: run h:\\doom.axe -iwad h:\\doom1.wad\n");
            return 1;
        }
        ag_printf("doom: iwad %s\n", wad);
    }

    own[n++] = argv[0];
    if (!have_iwad) {
        own[n++] = "-iwad";
        own[n++] = (char *)wad;
    }
    for (i = 1; i < argc && n < (int)(sizeof own / sizeof own[0]) - 1; i++) {
        int dummy = 0;
        if (arg_eq(argv[i], "nolivepad") || parse_frames_arg(argv[i], &dummy)) {
            continue;
        }
        if (!have_iwad && wad != NULL && argv[i] == wad) {
            continue;
        }
        own[n++] = argv[i];
    }
    doomgeneric_Create(n, own);

    {
        int ran = 0;
        while (!doom_argon_quit()) {
            if (frames >= 0 && ran >= frames) {
                break;
            }
            if (frames < 0 && !ag_focused()) {
                ag_heartbeat();
                ag_delay(50);
                continue;
            }
            doomgeneric_Tick();
            ran++;
            ag_yield();
        }
    }

    ag_gfx_release();
    return 0;
}
