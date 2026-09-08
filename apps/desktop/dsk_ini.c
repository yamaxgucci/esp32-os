/*
 * ArgonOS DESKTOP - reading and writing C:\DESKTOP.INI.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "dsk_ini.h"

#include <argon/argon.h>
#include <argon/libc.h>

/*
 * The whole file is read into one buffer and parsed there.
 *
 * A line-at-a-time reader over ag_read would be a syscall per line for a file
 * that is a few hundred bytes; this is one open, one read, one close.  The
 * bound is what stops a file that is not this file - somebody's log, a
 * truncated write - from being read in full before it is rejected.
 */
#define INI_MAX 2048

/* ---------------------------------------------------------------------- */
/* Parsing                                                               */
/* ---------------------------------------------------------------------- */

static bool is_space(char c) { return c == ' ' || c == '\t'; }

static char *trim(char *s)
{
    while (*s != '\0' && is_space(*s)) {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && (is_space(s[n - 1]) || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
    return s;
}

/* A decimal or hexadecimal number, and how far it got.  Signed, because an
 * icon dragged off the left edge is clamped by the caller, not here. */
static const char *number(const char *s, int32_t *out)
{
    int32_t v = 0;
    bool    neg = false;

    while (*s != '\0' && is_space(*s)) {
        s++;
    }
    if (*s == '-') {
        neg = true;
        s++;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        while (true) {
            int d;
            if (*s >= '0' && *s <= '9') {
                d = *s - '0';
            } else if (*s >= 'a' && *s <= 'f') {
                d = *s - 'a' + 10;
            } else if (*s >= 'A' && *s <= 'F') {
                d = *s - 'A' + 10;
            } else {
                break;
            }
            v = v * 16 + d;
            s++;
        }
    } else {
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            s++;
        }
    }
    *out = neg ? -v : v;
    return s;
}

/* "12,34" and "12,34,56,78,0": as many as are there, the rest untouched. */
static int numbers(const char *s, int32_t *out, int max)
{
    int n = 0;
    while (n < max) {
        const char *next = number(s, &out[n]);
        if (next == s) {
            break;
        }
        n++;
        s = next;
        while (*s != '\0' && (is_space(*s) || *s == ',')) {
            s++;
        }
        if (*s == '\0') {
            break;
        }
    }
    return n;
}

void dsk_ini_defaults(dsk_ini_t *ini)
{
    memset(ini, 0, sizeof(*ini));
    ini->background = DSK_TEAL;
    ini->dblclick_ms = 400u;
}

bool dsk_ini_icon_of(const dsk_ini_t *ini, const char *label, int16_t *x,
                     int16_t *y)
{
    for (int i = 0; i < ini->nicons; i++) {
        if (ag_stricmp(ini->icon[i].label, label) == 0) {
            if (x != NULL) {
                *x = ini->icon[i].x;
            }
            if (y != NULL) {
                *y = ini->icon[i].y;
            }
            return true;
        }
    }
    return false;
}

void dsk_ini_set_icon(dsk_ini_t *ini, const char *label, int16_t x, int16_t y)
{
    for (int i = 0; i < ini->nicons; i++) {
        if (ag_stricmp(ini->icon[i].label, label) == 0) {
            ini->icon[i].x = x;
            ini->icon[i].y = y;
            return;
        }
    }
    if (ini->nicons >= DSK_INI_ICONS) {
        return;
    }
    dsk_ini_icon_t *ic = &ini->icon[ini->nicons++];
    ag_strlcpy(ic->label, label, sizeof(ic->label));
    ic->x = x;
    ic->y = y;
}

/* ---------------------------------------------------------------------- */
/* Loading                                                               */
/* ---------------------------------------------------------------------- */

typedef enum { SEC_NONE = 0, SEC_DESKTOP, SEC_ICONS, SEC_WINDOWS } sec_t;

static sec_t section_of(const char *name)
{
    if (ag_stricmp(name, "desktop") == 0) {
        return SEC_DESKTOP;
    }
    if (ag_stricmp(name, "icons") == 0) {
        return SEC_ICONS;
    }
    if (ag_stricmp(name, "windows") == 0) {
        return SEC_WINDOWS;
    }
    return SEC_NONE;
}

static void apply(dsk_ini_t *ini, sec_t sec, char *key, char *value)
{
    int32_t v[5];

    switch (sec) {
    case SEC_DESKTOP:
        if (ag_stricmp(key, "background") == 0) {
            if (numbers(value, v, 1) == 1) {
                ini->background = (uint32_t)v[0] & 0x00FFFFFFu;
            }
        } else if (ag_stricmp(key, "dblclick") == 0) {
            /*
             * Bounded, because this one can make the shell unusable: zero
             * turns every double click into two single clicks, and ten
             * seconds turns two unrelated clicks a minute apart into one.
             */
            if (numbers(value, v, 1) == 1 && v[0] >= 100 && v[0] <= 2000) {
                ini->dblclick_ms = (uint16_t)v[0];
            }
        }
        break;

    case SEC_ICONS:
        if (numbers(value, v, 2) == 2) {
            dsk_ini_set_icon(ini, key, (int16_t)v[0], (int16_t)v[1]);
        }
        break;

    case SEC_WINDOWS:
        /* The key is a number nobody reads; the path is in the value, because
         * a DOS path contains a backslash and an INI key should not. */
        if (ini->nwins < DSK_INI_WINS) {
            /* "x,y,w,h,state,path" - the path last, so it may contain commas
             * without needing quoting rules nobody would remember. */
            int         got = 0;
            const char *p = value;
            for (; got < 5; got++) {
                const char *next = number(p, &v[got]);
                if (next == p) {
                    break;
                }
                p = next;
                while (*p != '\0' && (is_space(*p) || *p == ',')) {
                    p++;
                }
            }
            if (got == 5 && *p != '\0' && v[2] > 0 && v[3] > 0) {
                dsk_ini_win_t *w = &ini->win[ini->nwins++];
                w->frame = dsk_rect((int16_t)v[0], (int16_t)v[1],
                                    (int16_t)v[2], (int16_t)v[3]);
                w->state = (uint8_t)v[4];
                ag_strlcpy(w->path, p, sizeof(w->path));
            }
        }
        break;

    default:
        break;
    }
}

void dsk_ini_load(dsk_ini_t *ini)
{
    const ag_handle_t h = ag_open(DSK_INI_PATH, AG_O_RDONLY);
    if (h < 0) {
        return; /* no file: the defaults stand, and nothing is wrong */
    }

    char *text = (char *)ag_malloc(INI_MAX + 1);
    if (text == NULL) {
        (void)ag_close(h);
        return;
    }
    const int32_t n = ag_read(h, text, INI_MAX);
    (void)ag_close(h);
    if (n <= 0) {
        ag_free(text);
        return;
    }
    text[n] = '\0';

    sec_t sec = SEC_NONE;
    char *at = text;
    while (*at != '\0') {
        char *eol = at;
        while (*eol != '\0' && *eol != '\n') {
            eol++;
        }
        const bool last = (*eol == '\0');
        *eol = '\0';

        char *line = trim(at);
        if (line[0] != '\0' && line[0] != ';' && line[0] != '#') {
            if (line[0] == '[') {
                char *close = line;
                while (*close != '\0' && *close != ']') {
                    close++;
                }
                *close = '\0';
                sec = section_of(trim(line + 1));
            } else {
                char *eq = line;
                while (*eq != '\0' && *eq != '=') {
                    eq++;
                }
                if (*eq == '=') {
                    *eq = '\0';
                    apply(ini, sec, trim(line), trim(eq + 1));
                }
            }
        }

        if (last) {
            break;
        }
        at = eol + 1;
    }

    ag_free(text);
    ini->loaded = true;
}

/* ---------------------------------------------------------------------- */
/* Saving                                                               */
/* ---------------------------------------------------------------------- */

static void put(char *buf, size_t cap, const char *s) { ag_strlcat(buf, s, cap); }

static void put_num(char *buf, size_t cap, int32_t v)
{
    char num[24];
    if (v < 0) {
        ag_strlcat(buf, "-", cap);
        v = -v;
    }
    ag_strlcat(buf, ag_utoa((uint64_t)v, num, sizeof(num), 0, false), cap);
}

static void put_hex(char *buf, size_t cap, uint32_t v)
{
    static const char k_hex[] = "0123456789ABCDEF";
    char              out[9];
    int               at = 0;
    ag_strlcat(buf, "0x", cap);
    for (int shift = 20; shift >= 0; shift -= 4) {
        out[at++] = k_hex[(v >> shift) & 0xFu];
    }
    out[at] = '\0';
    ag_strlcat(buf, out, cap);
}

ag_err_t dsk_ini_save(const dsk_ini_t *ini)
{
    char *text = (char *)ag_malloc(INI_MAX);
    if (text == NULL) {
        return -AG_ENOMEM;
    }
    text[0] = '\0';

    put(text, INI_MAX, "; ArgonOS desktop.  Written when the shell exits and\n"
                       "; when an icon is moved.  Delete it to start over.\n\n"
                       "[desktop]\n"
                       "background = ");
    put_hex(text, INI_MAX, ini->background);
    put(text, INI_MAX, "\ndblclick   = ");
    put_num(text, INI_MAX, ini->dblclick_ms);
    put(text, INI_MAX, "\n\n; where each drive icon was dragged to\n[icons]\n");

    for (int i = 0; i < ini->nicons; i++) {
        put(text, INI_MAX, ini->icon[i].label);
        put(text, INI_MAX, " = ");
        put_num(text, INI_MAX, ini->icon[i].x);
        put(text, INI_MAX, ",");
        put_num(text, INI_MAX, ini->icon[i].y);
        put(text, INI_MAX, "\n");
    }

    put(text, INI_MAX,
        "\n; x,y,w,h,state,path - the path last, so it may hold commas\n"
        "[windows]\n");
    for (int i = 0; i < ini->nwins; i++) {
        const dsk_ini_win_t *w = &ini->win[i];
        put_num(text, INI_MAX, i + 1);
        put(text, INI_MAX, " = ");
        put_num(text, INI_MAX, w->frame.x);
        put(text, INI_MAX, ",");
        put_num(text, INI_MAX, w->frame.y);
        put(text, INI_MAX, ",");
        put_num(text, INI_MAX, w->frame.w);
        put(text, INI_MAX, ",");
        put_num(text, INI_MAX, w->frame.h);
        put(text, INI_MAX, ",");
        put_num(text, INI_MAX, w->state);
        put(text, INI_MAX, ",");
        put(text, INI_MAX, w->path);
        put(text, INI_MAX, "\n");
    }

    const ag_handle_t h =
        ag_open(DSK_INI_PATH, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (h < 0) {
        ag_free(text);
        return (ag_err_t)h;
    }

    const size_t  len = strlen(text);
    const int32_t wrote = ag_write(h, text, len);
    /*
     * Synced before the handle is closed, and the error kept.  This is written
     * on the way out, and on a board the way out is often followed by the
     * power going off - a file left in a cache is a file that was not saved.
     */
    ag_err_t err = ag_sync(h);
    (void)ag_close(h);
    if (wrote != (int32_t)len && err == AG_OK) {
        err = (wrote < 0) ? (ag_err_t)wrote : -AG_ENOSPC;
    }

    ag_free(text);
    return err;
}
