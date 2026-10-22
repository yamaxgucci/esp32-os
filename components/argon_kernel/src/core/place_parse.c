/*
 * ArgonOS - the [memory] section's values: "psram, iram", "only iram".
 *
 * Free of kernel and ESP-IDF dependencies; host tested (test_place.c).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#define AG_PLACE_HOST_ONLY
#include <argon/place.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const struct {
    const char     *word;
    ag_place_kind_t kind;
} k_words[] = {
    {"iram", AG_PLACE_IRAM},   {"sram", AG_PLACE_IRAM},
    {"internal", AG_PLACE_IRAM}, {"psram", AG_PLACE_PSRAM},
    {"flash", AG_PLACE_FLASH}, {"rom", AG_PLACE_ROM},
};

const char *ag_place_kind_name(ag_place_kind_t k)
{
    switch (k) {
    case AG_PLACE_IRAM:  return "iram";
    case AG_PLACE_PSRAM: return "psram";
    case AG_PLACE_FLASH: return "flash";
    case AG_PLACE_ROM:   return "rom";
    default:             return "?";
    }
}

static bool word_is(const char *w, size_t n, const char *lit)
{
    if (strlen(lit) != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)w[i]) != lit[i]) {
            return false;
        }
    }
    return true;
}

static void say(char *why, size_t len, const char *fmt, const char *a,
                size_t alen)
{
    if (why != NULL && len > 0) {
        char word[24];
        const size_t n = (alen < sizeof(word) - 1u) ? alen : sizeof(word) - 1u;
        memcpy(word, a, n);
        word[n] = '\0';
        snprintf(why, len, fmt, word);
    }
}

ag_err_t ag_place_parse(const char *text, uint32_t allowed,
                        ag_place_policy_t *out, char *why, size_t why_len)
{
    if (text == NULL || out == NULL) {
        return -AG_EINVAL;
    }
    ag_place_policy_t p;
    memset(&p, 0, sizeof(p));

    const char *s = text;
    bool        first = true;
    while (*s != '\0') {
        while (*s == ',' || isspace((unsigned char)*s)) {
            s++;
        }
        if (*s == '\0') {
            break;
        }
        const char *w = s;
        while (*s != '\0' && *s != ',' && !isspace((unsigned char)*s)) {
            s++;
        }
        const size_t n = (size_t)(s - w);

        if (word_is(w, n, "only")) {
            if (!first) {
                say(why, why_len, "`%s` goes before the list", w, n);
                return -AG_EINVAL;
            }
            p.only = true;
            first = false;
            continue;
        }
        first = false;

        ag_place_kind_t kind = AG_PLACE_NONE;
        for (size_t i = 0; i < sizeof(k_words) / sizeof(k_words[0]); i++) {
            if (word_is(w, n, k_words[i].word)) {
                kind = k_words[i].kind;
                break;
            }
        }
        if (kind == AG_PLACE_NONE) {
            say(why, why_len, "`%s` is not a kind of memory "
                              "(iram, psram, flash, rom)", w, n);
            return -AG_EINVAL;
        }
        if ((allowed & AG_PLACE_BIT(kind)) == 0u) {
            say(why, why_len, "this cannot live in %s", w, n);
            return -AG_EINVAL;
        }
        for (uint8_t i = 0; i < p.count; i++) {
            if (p.kind[i] == (uint8_t)kind) {
                say(why, why_len, "`%s` is named twice", w, n);
                return -AG_EINVAL;
            }
        }
        p.kind[p.count++] = (uint8_t)kind;
    }

    if (p.count == 0) {
        say(why, why_len, "no kind of memory named%s", "", 0);
        return -AG_EINVAL;
    }
    *out = p;
    return AG_OK;
}

size_t ag_place_format(const ag_place_policy_t *p, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    size_t len = 0;
    if (p == NULL) {
        return 0;
    }
    if (p->only) {
        len += (size_t)snprintf(out + len, cap - len, "only ");
    }
    for (uint8_t i = 0; i < p->count && len < cap; i++) {
        len += (size_t)snprintf(out + len, cap - len, "%s%s",
                                (i > 0) ? ", " : "",
                                ag_place_kind_name((ag_place_kind_t)p->kind[i]));
    }
    return (len < cap) ? len : cap - 1u;
}
