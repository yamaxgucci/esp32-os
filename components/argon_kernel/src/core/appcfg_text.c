/*
 * ArgonOS - an application's own settings, as text.  See appcfg_text.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "appcfg_text.h"

#include <string.h>

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/* The line `at`..`end` as key and value, or false when it is a comment, blank
 * or not a `key = value`.  Both halves are trimmed. */
static bool split_line(const char *at, const char *end, const char **key,
                       size_t *key_len, const char **val, size_t *val_len)
{
    while (at < end && is_space(*at)) {
        at++;
    }
    if (at == end || *at == ';' || *at == '#' || *at == '[') {
        return false;
    }
    const char *eq = at;
    while (eq < end && *eq != '=') {
        eq++;
    }
    if (eq == end) {
        return false;
    }
    const char *k_end = eq;
    while (k_end > at && is_space(k_end[-1])) {
        k_end--;
    }
    if (k_end == at) {
        return false;
    }
    const char *v_at = eq + 1;
    while (v_at < end && is_space(*v_at)) {
        v_at++;
    }
    const char *v_end = end;
    while (v_end > v_at && is_space(v_end[-1])) {
        v_end--;
    }
    *key = at;
    *key_len = (size_t)(k_end - at);
    *val = v_at;
    *val_len = (size_t)(v_end - v_at);
    return true;
}

/* A value with its quotes taken off and its trailing comment cut. */
static void clean_value(const char *val, size_t val_len, char *out, size_t len)
{
    size_t n = 0;
    bool quoted = (val_len >= 2 && val[0] == '"' && val[val_len - 1] == '"');
    if (quoted) {
        if (val_len - 2u < len) {
            memcpy(out, val + 1, val_len - 2u);
            n = val_len - 2u;
        }
    } else {
        const bool in_quotes_never = true;
        (void)in_quotes_never;
        bool in_quotes = false;
        for (size_t i = 0; i < val_len && n + 1u < len; i++) {
            const char c = val[i];
            if (c == '"') {
                in_quotes = !in_quotes;
                continue;
            }
            if (!in_quotes && (c == ';' || c == '#')) {
                break;
            }
            out[n++] = c;
        }
        while (n > 0 && is_space(out[n - 1])) {
            n--;
        }
    }
    out[n] = '\0';
}

bool ag_appcfg_text_get(const char *text, const char *key, char *out,
                        size_t len)
{
    if (text == NULL || key == NULL || out == NULL || len == 0) {
        return false;
    }
    const size_t key_len = strlen(key);
    for (const char *line = text; line != NULL && *line != '\0';) {
        const char *end = line;
        while (*end != '\0' && *end != '\n') {
            end++;
        }
        const char *k = NULL;
        const char *v = NULL;
        size_t k_len = 0;
        size_t v_len = 0;
        if (split_line(line, end, &k, &k_len, &v, &v_len) && k_len == key_len &&
            memcmp(k, key, key_len) == 0) {
            clean_value(v, v_len, out, len);
            return true;
        }
        line = (*end == '\n') ? end + 1 : end;
    }
    return false;
}

int32_t ag_appcfg_text_get_int(const char *text, const char *key,
                               int32_t fallback)
{
    char value[AG_APPCFG_VALUE_MAX];
    if (!ag_appcfg_text_get(text, key, value, sizeof(value))) {
        return fallback;
    }
    const char *p = value;
    while (is_space(*p)) {
        p++;
    }
    bool neg = false;
    if (*p == '-' || *p == '+') {
        neg = (*p == '-');
        p++;
    }
    uint32_t base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    }
    uint32_t n = 0;
    bool digits = false;
    for (; *p != '\0'; p++) {
        uint32_t d;
        if (*p >= '0' && *p <= '9') {
            d = (uint32_t)(*p - '0');
        } else if (base == 16 && *p >= 'a' && *p <= 'f') {
            d = (uint32_t)(*p - 'a' + 10);
        } else if (base == 16 && *p >= 'A' && *p <= 'F') {
            d = (uint32_t)(*p - 'A' + 10);
        } else {
            break;
        }
        n = n * base + d;
        digits = true;
    }
    if (!digits) {
        return fallback;
    }
    const int32_t v = (int32_t)n;
    return neg ? -v : v;
}

size_t ag_appcfg_text_set(const char *in, const char *key, const char *value,
                          char *out, size_t outlen)
{
    if (key == NULL || value == NULL || out == NULL ||
        strlen(key) >= AG_APPCFG_KEY_MAX ||
        strlen(value) >= AG_APPCFG_VALUE_MAX) {
        return 0;
    }
    const size_t key_len = strlen(key);
    size_t at = 0;
    bool replaced = false;

    if (in != NULL) {
        for (const char *line = in; *line != '\0';) {
            const char *end = line;
            while (*end != '\0' && *end != '\n') {
                end++;
            }
            const char *k = NULL;
            const char *v = NULL;
            size_t k_len = 0;
            size_t v_len = 0;
            if (!replaced && split_line(line, end, &k, &k_len, &v, &v_len) &&
                k_len == key_len && memcmp(k, key, key_len) == 0) {
                /* Keep the column the old line started at, so a hand-aligned
                 * file stays aligned and a diff of it stays readable. */
                const size_t indent = (size_t)(k - line);
                const size_t need = indent + key_len + 3u + strlen(value) + 1u;
                if (at + need >= outlen) {
                    return 0;
                }
                memset(out + at, ' ', indent);
                at += indent;
                memcpy(out + at, key, key_len);
                at += key_len;
                if (indent > 0) {
                    out[at++] = ' ';
                    out[at++] = '=';
                    out[at++] = ' ';
                } else {
                    memcpy(out + at, " = ", 3);
                    at += 3;
                }
                const size_t n = strlen(value);
                memcpy(out + at, value, n);
                at += n;
                out[at++] = '\n';
                replaced = true;
            } else {
                const size_t n = (size_t)(end - line);
                if (at + n + 1u >= outlen) {
                    return 0;
                }
                memcpy(out + at, line, n);
                at += n;
                out[at++] = '\n';
            }
            line = (*end == '\n') ? end + 1 : end;
        }
    }

    if (!replaced) {
        const size_t need = key_len + 3u + strlen(value) + 1u;
        if (at + need >= outlen) {
            return 0;
        }
        memcpy(out + at, key, key_len);
        at += key_len;
        memcpy(out + at, " = ", 3);
        at += 3;
        const size_t n = strlen(value);
        memcpy(out + at, value, n);
        at += n;
        out[at++] = '\n';
    }

    out[at] = '\0';
    return at;
}

void ag_appcfg_file_name(const char *app, char *out, size_t len)
{
    if (out == NULL || len == 0) {
        return;
    }
    size_t n = 0;
    if (app != NULL) {
        for (const char *p = app; *p != '\0' && n + 1u < len; p++) {
            char c = *p;
            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                            c == '_' || c == '-';
            if (ok) {
                out[n++] = c;
            }
        }
    }
    if (n == 0) {
        /* A process with no name still gets a file of its own, and one nobody
         * else's name can collide with. */
        const char *fallback = "APP";
        while (*fallback != '\0' && n + 1u < len) {
            out[n++] = *fallback++;
        }
    }
    out[n] = '\0';
}
