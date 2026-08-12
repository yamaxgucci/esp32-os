/*
 * ArgonOS - PATH search and file associations.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/shell_path.h>

#include <string.h>

#include <argon/path.h>
#include <argon/vfs.h>

bool ag_shell_name_is_path(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    if (name[0] == '/' || name[0] == '\\') {
        return true;
    }
    for (const char *p = name; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            return true;
        }
        if (*p == ':' && p == name + 1) {
            return true; /* A:foo or A:\foo */
        }
    }
    return false;
}

static bool is_regular_file(const char *abs)
{
    ag_stat_t st;
    if (ag_vfs_stat(abs, NULL, &st) != AG_OK) {
        return false;
    }
    return (st.attr & AG_A_DIR) == 0;
}

static ag_err_t try_candidate(const char *dir, const char *name, char *out,
                              size_t outlen)
{
    char joined[AG_PATH_MAX];
    const ag_err_t err = ag_path_join(dir, name, joined, sizeof(joined));
    if (err != AG_OK) {
        return err;
    }
    if (!is_regular_file(joined)) {
        return -AG_ENOENT;
    }
    if (strlen(joined) + 1 > outlen) {
        return -AG_ERANGE;
    }
    memcpy(out, joined, strlen(joined) + 1);
    return AG_OK;
}

bool ag_shell_is_script(const char *path)
{
    const char *ext = ag_path_ext(path);
    if (ext == NULL) {
        return false;
    }
    return ag_path_icmp(ext, ".bat") == 0 || ag_path_icmp(ext, ".cmd") == 0;
}

static bool is_path_cmd_ext(const char *name)
{
    const char *ext = ag_path_ext(name);
    if (ext == NULL) {
        return false;
    }
    return ag_path_icmp(ext, ".axe") == 0 || ag_shell_is_script(name);
}

static ag_err_t try_name_variants(const char *dir, const char *name, char *out,
                                  size_t outlen)
{
    /*
     * PATH finds applications and shell scripts.  A bare `readme.txt` must
     * fall through to associations, not be treated as a command.
     */
    if (ag_path_ext(name) != NULL) {
        if (!is_path_cmd_ext(name)) {
            return -AG_ENOENT;
        }
        return try_candidate(dir, name, out, outlen);
    }

    static const char *const k_exts[] = {".axe", ".AXE", ".bat", ".BAT",
                                         ".cmd", ".CMD"};
    char with_ext[AG_NAME_MAX + 8];
    const size_t nlen = strlen(name);
    for (size_t i = 0; i < sizeof(k_exts) / sizeof(k_exts[0]); i++) {
        const size_t elen = strlen(k_exts[i]);
        if (nlen + elen + 1 > sizeof(with_ext)) {
            return -AG_ERANGE;
        }
        memcpy(with_ext, name, nlen);
        memcpy(with_ext + nlen, k_exts[i], elen + 1);
        if (try_candidate(dir, with_ext, out, outlen) == AG_OK) {
            return AG_OK;
        }
    }
    return -AG_ENOENT;
}

ag_err_t ag_shell_resolve_cmd(const char *name, const char *cwd,
                              const char *path_env, char *out, size_t outlen)
{
    if (name == NULL || name[0] == '\0' || out == NULL || outlen == 0) {
        return -AG_EINVAL;
    }

    if (ag_shell_name_is_path(name)) {
        const ag_err_t err = ag_path_resolve(name, cwd, out, outlen);
        if (err != AG_OK) {
            return err;
        }
        if (!is_regular_file(out)) {
            return -AG_ENOENT;
        }
        /* Explicit paths may be any file; `run` decides if it is an image. */
        return AG_OK;
    }

    /* Bare name in the cwd first — same habit as typing a local file. */
    {
        const ag_err_t err = try_name_variants(cwd != NULL ? cwd : "/", name,
                                              out, outlen);
        if (err == AG_OK) {
            return AG_OK;
        }
    }

    if (path_env == NULL || path_env[0] == '\0') {
        return -AG_ENOENT;
    }

    const char *p = path_env;
    while (*p != '\0') {
        while (*p == ';') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char *end = p;
        while (*end != '\0' && *end != ';') {
            end++;
        }

        char dir_raw[AG_PATH_MAX];
        const size_t dlen = (size_t)(end - p);
        if (dlen == 0 || dlen >= sizeof(dir_raw)) {
            p = (*end == ';') ? end + 1 : end;
            continue;
        }
        memcpy(dir_raw, p, dlen);
        dir_raw[dlen] = '\0';

        /* Trim trailing whitespace and separators. */
        size_t len = dlen;
        while (len > 0) {
            const char c = dir_raw[len - 1];
            if (c != ' ' && c != '\t' && c != '\\' && c != '/') {
                break;
            }
            dir_raw[--len] = '\0';
        }

        char dir_abs[AG_PATH_MAX];
        if (ag_path_resolve(dir_raw, cwd, dir_abs, sizeof(dir_abs)) == AG_OK) {
            if (try_name_variants(dir_abs, name, out, outlen) == AG_OK) {
                return AG_OK;
            }
        }

        p = (*end == ';') ? end + 1 : end;
    }
    return -AG_ENOENT;
}

const char *ag_shell_assoc_lookup(const ag_cfg_t *cfg, const char *filename)
{
    if (cfg == NULL || filename == NULL) {
        return NULL;
    }
    const char *base = ag_path_basename(filename);
    const char *ext = ag_path_ext(base);
    if (ext == NULL || ext[0] == '\0') {
        return NULL;
    }

    /*
     * Assoc keys are stored as the extension including the dot
     * ("assoc..txt" is awkward); we use section "assoc" and key ".txt"
     * which cfg exposes as "assoc..txt" when key is ".txt"... Actually
     * dotted_key is "section.key", so key ".txt" becomes "assoc..txt".
     * Prefer iterating [assoc] entries and matching key to ext.
     */
    for (uint16_t i = 0; i < cfg->count; i++) {
        const ag_cfg_entry_t *e = &cfg->entries[i];
        if (e->section == NULL || e->key == NULL || e->value == NULL) {
            continue;
        }
        if (ag_path_icmp(e->section, "assoc") != 0) {
            continue;
        }
        /* Allow both ".txt" and "txt" in the file. */
        const char *key = e->key;
        if (key[0] != '.' && ext[0] == '.') {
            if (ag_path_icmp(key, ext + 1) == 0) {
                return e->value;
            }
        } else if (ag_path_icmp(key, ext) == 0) {
            return e->value;
        }
    }
    return NULL;
}

bool ag_shell_autoexec_skip_line(const char *line)
{
    if (line == NULL) {
        return true;
    }
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    if (*line == '\0' || *line == ';' || *line == '#') {
        return true;
    }
    if ((line[0] == 'r' || line[0] == 'R') &&
        (line[1] == 'e' || line[1] == 'E') &&
        (line[2] == 'm' || line[2] == 'M') &&
        (line[3] == '\0' || line[3] == ' ' || line[3] == '\t')) {
        return true;
    }
    return false;
}
