/*
 * ArgonOS - configuration file parser.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/cfg.h>

#include <string.h>

#include <argon/path.h> /* ag_path_icmp */

static inline bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

static char *skip_space(char *p)
{
    while (*p != '\0' && is_space(*p)) {
        p++;
    }
    return p;
}

/* Trims trailing whitespace in place; returns the same pointer. */
static char *rtrim(char *p)
{
    size_t n = strlen(p);
    while (n > 0 && is_space(p[n - 1])) {
        p[--n] = '\0';
    }
    return p;
}

/*
 * Cuts an unquoted trailing comment and strips one layer of quotes.
 * Quoting is what lets a value contain a ';', which matters for things like
 * prompt strings.
 */
static char *clean_value(char *v)
{
    bool in_quotes = false;
    for (char *p = v; *p != '\0'; p++) {
        if (*p == '"') {
            in_quotes = !in_quotes;
        } else if (!in_quotes && (*p == ';' || *p == '#')) {
            *p = '\0';
            break;
        }
    }

    v = rtrim(skip_space(v));

    const size_t n = strlen(v);
    if (n >= 2 && v[0] == '"' && v[n - 1] == '"') {
        v[n - 1] = '\0';
        v++;
    }
    return v;
}

void ag_cfg_reset(ag_cfg_t *cfg)
{
    if (cfg != NULL) {
        memset(cfg, 0, sizeof(*cfg));
    }
}

ag_err_t ag_cfg_parse(char *text, ag_cfg_t *cfg)
{
    if (text == NULL || cfg == NULL) {
        return -AG_EINVAL;
    }

    const char *section = "";
    char       *p = text;
    uint16_t    lineno = 0;

    while (*p != '\0') {
        lineno++;

        char *line = p;
        char *eol = strchr(p, '\n');
        if (eol != NULL) {
            *eol = '\0';
            p = eol + 1;
        } else {
            p = line + strlen(line);
        }

        line = rtrim(skip_space(line));

        if (line[0] == '\0' || line[0] == ';' || line[0] == '#') {
            continue;
        }

        if (line[0] == '[') {
            char *close = strchr(line, ']');
            if (close == NULL) {
                cfg->bad_lines++;
                continue;
            }
            *close = '\0';
            section = rtrim(skip_space(line + 1));
            continue;
        }

        char *eq = strchr(line, '=');
        if (eq == NULL) {
            eq = strchr(line, ':');
        }
        if (eq == NULL) {
            cfg->bad_lines++;
            continue;
        }

        *eq = '\0';
        char *key = rtrim(skip_space(line));
        char *value = clean_value(eq + 1);

        if (key[0] == '\0') {
            cfg->bad_lines++;
            continue;
        }
        if (cfg->count >= AG_CFG_MAX_ENTRIES) {
            cfg->dropped++;
            continue;
        }

        cfg->entries[cfg->count].section = section;
        cfg->entries[cfg->count].key = key;
        cfg->entries[cfg->count].value = value;
        cfg->entries[cfg->count].line = lineno;
        cfg->count++;
    }

    return AG_OK;
}

/* Splits "section.key"; a key with no dot belongs to the unnamed section. */
static void split_key(const char *dotted, const char **section,
                      const char **key)
{
    const char *dot = strchr(dotted, '.');
    if (dot == NULL) {
        *section = "";
        *key = dotted;
    } else {
        *section = dotted;
        *key = dot + 1;
    }
}

static bool entry_matches(const ag_cfg_entry_t *e, const char *section,
                          size_t section_len, const char *key)
{
    if (strlen(e->section) != section_len) {
        return false;
    }
    for (size_t i = 0; i < section_len; i++) {
        const char a = e->section[i];
        const char b = section[i];
        const char la = (a >= 'A' && a <= 'Z') ? (char)(a + 32) : a;
        const char lb = (b >= 'A' && b <= 'Z') ? (char)(b + 32) : b;
        if (la != lb) {
            return false;
        }
    }
    return ag_path_icmp(e->key, key) == 0;
}

const char *ag_cfg_get(const ag_cfg_t *cfg, const char *dotted_key,
                       const char *fallback)
{
    if (cfg == NULL || dotted_key == NULL) {
        return fallback;
    }

    const char *section;
    const char *key;
    split_key(dotted_key, &section, &key);
    const size_t section_len = (key == dotted_key)
                                   ? 0
                                   : (size_t)(key - section - 1);

    /* Search backwards so that a later file overrides an earlier one. */
    for (int i = (int)cfg->count - 1; i >= 0; i--) {
        if (entry_matches(&cfg->entries[i], section, section_len, key)) {
            return cfg->entries[i].value;
        }
    }
    return fallback;
}

const char *ag_cfg_next(const ag_cfg_t *cfg, const char *dotted_key,
                        size_t *iter)
{
    if (cfg == NULL || dotted_key == NULL || iter == NULL) {
        return NULL;
    }

    const char *section;
    const char *key;
    split_key(dotted_key, &section, &key);
    const size_t section_len = (key == dotted_key)
                                   ? 0
                                   : (size_t)(key - section - 1);

    while (*iter < cfg->count) {
        const ag_cfg_entry_t *e = &cfg->entries[(*iter)++];
        if (entry_matches(e, section, section_len, key)) {
            return e->value;
        }
    }
    return NULL;
}

/* Parses an optionally 0x-prefixed, optionally negative integer. */
static bool parse_int(const char *s, int32_t *out)
{
    if (s == NULL) {
        return false;
    }
    while (is_space(*s)) {
        s++;
    }

    bool negative = false;
    if (*s == '-') {
        negative = true;
        s++;
    } else if (*s == '+') {
        s++;
    }

    int base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }

    bool any = false;
    int64_t acc = 0;
    for (; *s != '\0'; s++) {
        int digit;
        if (*s >= '0' && *s <= '9') {
            digit = *s - '0';
        } else if (base == 16 && *s >= 'a' && *s <= 'f') {
            digit = *s - 'a' + 10;
        } else if (base == 16 && *s >= 'A' && *s <= 'F') {
            digit = *s - 'A' + 10;
        } else {
            break;
        }
        acc = acc * base + digit;
        any = true;
        if (acc > 0x7fffffffLL) {
            return false;
        }
    }
    if (!any) {
        return false;
    }

    /* Anything but trailing whitespace after the number is a malformed value. */
    while (is_space(*s)) {
        s++;
    }
    if (*s != '\0') {
        return false;
    }

    *out = (int32_t)(negative ? -acc : acc);
    return true;
}

int32_t ag_cfg_get_int(const ag_cfg_t *cfg, const char *dotted_key,
                       int32_t fallback)
{
    const char *v = ag_cfg_get(cfg, dotted_key, NULL);
    int32_t out;
    return (v != NULL && parse_int(v, &out)) ? out : fallback;
}

bool ag_cfg_get_bool(const ag_cfg_t *cfg, const char *dotted_key, bool fallback)
{
    const char *v = ag_cfg_get(cfg, dotted_key, NULL);
    if (v == NULL || v[0] == '\0') {
        return fallback;
    }

    static const char *const yes[] = {"1", "yes", "y", "true",  "on",  "enabled"};
    static const char *const no[] = {"0", "no", "n", "false", "off", "disabled"};

    for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++) {
        if (ag_path_icmp(v, yes[i]) == 0) {
            return true;
        }
    }
    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++) {
        if (ag_path_icmp(v, no[i]) == 0) {
            return false;
        }
    }
    return fallback;
}

int32_t ag_cfg_parse_size(const char *text, int32_t fallback)
{
    if (text == NULL) {
        return fallback;
    }

    /* Split the numeric part from a K/M/G suffix, then reuse parse_int. */
    char   number[24];
    size_t n = 0;
    const char *s = text;

    while (is_space(*s)) {
        s++;
    }
    while (*s != '\0' && !is_space(*s) && n + 1 < sizeof(number)) {
        const char c = *s;
        const bool is_suffix = (c == 'k' || c == 'K' || c == 'm' || c == 'M' ||
                                c == 'g' || c == 'G' || c == 'b' || c == 'B');
        /* 'b' and hex digits overlap; only treat a suffix as such at the end. */
        if (is_suffix && !(n >= 2 && number[0] == '0' &&
                           (number[1] == 'x' || number[1] == 'X'))) {
            break;
        }
        number[n++] = c;
        s++;
    }
    number[n] = '\0';

    int32_t value;
    if (!parse_int(number, &value)) {
        return fallback;
    }

    int64_t scaled = value;
    switch (*s) {
    case 'k': case 'K': scaled = (int64_t)value * 1024; break;
    case 'm': case 'M': scaled = (int64_t)value * 1024 * 1024; break;
    case 'g': case 'G': scaled = (int64_t)value * 1024 * 1024 * 1024; break;
    case 'b': case 'B': case '\0': break;
    default: return fallback;
    }

    if (scaled > 0x7fffffffLL || scaled < -0x80000000LL) {
        return fallback;
    }
    return (int32_t)scaled;
}
