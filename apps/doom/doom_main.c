/*
 * ArgonOS - Doom (doomgeneric). SFX+music via /dev/pcmvirt.
 *
 *   python apps/doom/build.py
 *   run a:\doom.axe -iwad a:\doom1.wad pcmvirt
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

static int is_sound_cli(const char *s)
{
    return arg_eq(s, "nosound") || arg_eq(s, "pcmnull") || arg_eq(s, "pcmvirt") ||
           arg_eq(s, "mock") || arg_eq(s, "net") || arg_eq(s, "tcp") ||
           arg_eq(s, "mix") || arg_eq(s, "pcmmix") || arg_eq(s, "audio") ||
           arg_eq(s, "i2s") || arg_eq(s, "pcm0");
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
    int          livepad = 0;
    int          frames = -1;
    int          have_iwad;
    const char  *sound_path = "pcmvirt";

    for (i = 1; i < argc; i++) {
        if (arg_eq(argv[i], "livepad")) {
            livepad = 1;
        } else if (arg_eq(argv[i], "nolivepad")) {
            livepad = 0;
        } else if (arg_eq(argv[i], "nosound") || arg_eq(argv[i], "mock")) {
            sound_path = "nosound";
        } else if (is_sound_cli(argv[i])) {
            sound_path = argv[i];
        } else {
            (void)parse_frames_arg(argv[i], &frames);
        }
    }
    doom_argon_set_live_pad(livepad);
    doom_argon_set_sound_path(sound_path);

    have_iwad = has_iwad_flag(argc, argv);
    wad = NULL;
    if (!have_iwad) {
        wad = first_wad(argc, argv);
        if (wad == NULL) {
            static const char *k_try[] = {
                "a:\\doom1.wad", "a:\\DOOM1.WAD", "h:\\doom1.wad",
                "h:\\DOOM1.WAD", NULL};
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
            ag_printf("doom: run a:\\doom.axe -iwad a:\\doom1.wad\n");
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
        if (arg_eq(argv[i], "nolivepad") || arg_eq(argv[i], "livepad") ||
            is_sound_cli(argv[i]) || parse_frames_arg(argv[i], &dummy)) {
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
