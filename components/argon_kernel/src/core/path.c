/*
 * ArgonOS - path handling.
 *
 * Canonicalisation is done by pushing components onto the output buffer
 * rather than by concatenating and then cleaning up, which keeps the whole
 * thing to a single output buffer and no temporary of its own.
 *
 * NOTE: `out` must not overlap `in` or `cwd`.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/path.h>

#include <string.h>

static inline bool is_sep(char c) { return c == '/' || c == '\\'; }

static inline char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static inline bool is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

const char *ag_path_drive(char letter)
{
    switch (lower(letter)) {
    case 'a': return "/sd";
    case 'c': return "/sys";
    case 't': return "/tmp";
    case 'd': return "/dev";
    case 'h': return "/host"; /* HostFS — live host folder (QEMU) */
    default:  return NULL;
    }
}

/* Appends one already-delimited component, folding "." and "..". */
static ag_err_t push_comp(char *out, size_t outlen, size_t *len,
                          const char *comp, size_t clen)
{
    if (clen == 0) {
        return AG_OK; /* collapses "//" */
    }
    if (clen == 1 && comp[0] == '.') {
        return AG_OK;
    }
    if (clen == 2 && comp[0] == '.' && comp[1] == '.') {
        while (*len > 0 && out[*len - 1] != '/') {
            (*len)--;
        }
        if (*len > 0) {
            (*len)--; /* drop the separator itself; ".." at root is a no-op */
        }
        out[*len] = '\0';
        return AG_OK;
    }
    if (clen > AG_NAME_MAX) {
        return -AG_ERANGE;
    }
    if (*len + 1 + clen + 1 > outlen) {
        return -AG_ERANGE;
    }
    out[(*len)++] = '/';
    memcpy(out + *len, comp, clen);
    *len += clen;
    out[*len] = '\0';
    return AG_OK;
}

static ag_err_t push_path(char *out, size_t outlen, size_t *len, const char *p)
{
    while (*p != '\0') {
        while (is_sep(*p)) {
            p++;
        }
        const char *start = p;
        while (*p != '\0' && !is_sep(*p)) {
            p++;
        }
        const ag_err_t err = push_comp(out, outlen, len, start,
                                       (size_t)(p - start));
        if (err != AG_OK) {
            return err;
        }
    }
    return AG_OK;
}

ag_err_t ag_path_resolve(const char *in, const char *cwd, char *out,
                         size_t outlen)
{
    if (out == NULL || outlen < 2) {
        return -AG_EINVAL;
    }

    size_t len = 0;
    out[0] = '\0';

    if (in == NULL) {
        in = "";
    }

    const char *rest = in;

    if (is_alpha(in[0]) && in[1] == ':') {
        const char *drive = ag_path_drive(in[0]);
        if (drive == NULL) {
            return -AG_EINVAL;
        }
        const ag_err_t err = push_path(out, outlen, &len, drive);
        if (err != AG_OK) {
            return err;
        }
        rest = in + 2;
    } else if (is_sep(in[0])) {
        /* Already rooted; nothing to seed the buffer with. */
    } else if (cwd != NULL && cwd[0] != '\0') {
        const ag_err_t err = push_path(out, outlen, &len, cwd);
        if (err != AG_OK) {
            return err;
        }
    }

    const ag_err_t err = push_path(out, outlen, &len, rest);
    if (err != AG_OK) {
        return err;
    }

    if (len == 0) {
        out[0] = '/';
        out[1] = '\0';
    }
    return AG_OK;
}

ag_err_t ag_path_join(const char *base, const char *child, char *out,
                      size_t outlen)
{
    if (out == NULL || outlen < 2) {
        return -AG_EINVAL;
    }

    size_t len = 0;
    out[0] = '\0';

    if (child != NULL && (is_sep(child[0]) ||
                          (is_alpha(child[0]) && child[1] == ':'))) {
        /* An absolute child ignores the base, as everywhere else. */
        return ag_path_resolve(child, NULL, out, outlen);
    }

    if (base != NULL) {
        const ag_err_t err = push_path(out, outlen, &len, base);
        if (err != AG_OK) {
            return err;
        }
    }
    if (child != NULL) {
        const ag_err_t err = push_path(out, outlen, &len, child);
        if (err != AG_OK) {
            return err;
        }
    }
    if (len == 0) {
        out[0] = '/';
        out[1] = '\0';
    }
    return AG_OK;
}

const char *ag_path_basename(const char *path)
{
    if (path == NULL) {
        return "";
    }
    const char *last = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (is_sep(*p)) {
            last = p + 1;
        }
    }
    return last;
}

ag_err_t ag_path_dirname(const char *path, char *out, size_t outlen)
{
    if (path == NULL || out == NULL || outlen < 2) {
        return -AG_EINVAL;
    }

    size_t cut = 0;
    bool found = false;
    for (size_t i = 0; path[i] != '\0'; i++) {
        if (is_sep(path[i])) {
            cut = i;
            found = true;
        }
    }

    if (!found) {
        out[0] = '.';
        out[1] = '\0';
        return AG_OK;
    }
    if (cut == 0) {
        cut = 1; /* "/a" -> "/" */
    }
    if (cut + 1 > outlen) {
        return -AG_ERANGE;
    }
    memcpy(out, path, cut);
    out[cut] = '\0';
    return AG_OK;
}

const char *ag_path_ext(const char *path)
{
    const char *base = ag_path_basename(path);
    const char *dot = NULL;
    for (const char *p = base; *p != '\0'; p++) {
        if (*p == '.') {
            dot = p;
        }
    }
    /* A dotfile has no extension: ".config" is a name, not an extension. */
    if (dot == NULL || dot == base) {
        return NULL;
    }
    return dot;
}

int ag_path_icmp(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return (a == b) ? 0 : (a == NULL ? -1 : 1);
    }
    while (*a != '\0' && *b != '\0') {
        const char ca = lower(*a);
        const char cb = lower(*b);
        if (ca != cb) {
            return (int)(unsigned char)ca - (int)(unsigned char)cb;
        }
        a++;
        b++;
    }
    return (int)(unsigned char)lower(*a) - (int)(unsigned char)lower(*b);
}

bool ag_path_match(const char *pattern, const char *name)
{
    if (pattern == NULL || name == NULL) {
        return false;
    }

    const char *star = NULL;
    const char *retry = NULL;

    while (*name != '\0') {
        if (*pattern == '?' || lower(*pattern) == lower(*name)) {
            pattern++;
            name++;
        } else if (*pattern == '*') {
            star = pattern++;
            retry = name;
        } else if (star != NULL) {
            /* Backtrack: let the star swallow one more character. */
            pattern = star + 1;
            name = ++retry;
        } else {
            return false;
        }
    }

    while (*pattern == '*') {
        pattern++;
    }
    return *pattern == '\0';
}

bool ag_path_is_absolute(const char *path)
{
    if (path == NULL || path[0] != '/') {
        return false;
    }
    if (path[1] == '\0') {
        return true; /* root */
    }

    for (size_t i = 0; path[i] != '\0'; i++) {
        if (path[i] == '\\') {
            return false;
        }
        if (path[i] == '/' && path[i + 1] == '/') {
            return false;
        }
        if (path[i] == '/' && path[i + 1] == '.') {
            const char c = path[i + 2];
            if (c == '\0' || c == '/') {
                return false; /* "/." */
            }
            if (c == '.' && (path[i + 3] == '\0' || path[i + 3] == '/')) {
                return false; /* "/.." */
            }
        }
    }

    const size_t n = strlen(path);
    return path[n - 1] != '/';
}
