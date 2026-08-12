/*
 * ArgonOS SMS - control / video config loader.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "sms_cfg.h"

#include <argon/argon.h>
#include <argon/keys.h>

#include <string.h>

#define CFG_MAX 4096

void sms_cfg_set_defaults(sms_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->key[0][SMS_ACT_UP] = AG_KEY_UP;
    cfg->key[0][SMS_ACT_DOWN] = AG_KEY_DOWN;
    cfg->key[0][SMS_ACT_LEFT] = AG_KEY_LEFT;
    cfg->key[0][SMS_ACT_RIGHT] = AG_KEY_RIGHT;
    cfg->key[0][SMS_ACT_B1] = AG_KEY_Z;
    cfg->key[0][SMS_ACT_B2] = AG_KEY_X;
    cfg->key[0][SMS_ACT_PAUSE] = AG_KEY_ENTER;
    cfg->key[0][SMS_ACT_QUIT] = AG_KEY_ESC;

    cfg->key[1][SMS_ACT_UP] = AG_KEY_W;
    cfg->key[1][SMS_ACT_DOWN] = AG_KEY_S;
    cfg->key[1][SMS_ACT_LEFT] = AG_KEY_A;
    cfg->key[1][SMS_ACT_RIGHT] = AG_KEY_D;
    cfg->key[1][SMS_ACT_B1] = AG_KEY_J;
    cfg->key[1][SMS_ACT_B2] = AG_KEY_K;
    cfg->key[1][SMS_ACT_PAUSE] = AG_KEY_P;
    cfg->key[1][SMS_ACT_QUIT] = AG_KEY_Q;
    cfg->fullscreen = 0;
}

static int truthy(const char *v)
{
    if (v == NULL || v[0] == '\0') {
        return 0;
    }
    if (v[0] == '1' && v[1] == '\0') {
        return 1;
    }
    if ((v[0] == 'y' || v[0] == 'Y') &&
        (v[1] == '\0' || ((v[1] == 'e' || v[1] == 'E') &&
                          (v[2] == 's' || v[2] == 'S') && v[3] == '\0'))) {
        return 1;
    }
    if ((v[0] == 't' || v[0] == 'T') &&
        (v[1] == '\0' ||
         ((v[1] == 'r' || v[1] == 'R') && (v[2] == 'u' || v[2] == 'U') &&
          (v[3] == 'e' || v[3] == 'E') && v[4] == '\0'))) {
        return 1;
    }
    if ((v[0] == 'o' || v[0] == 'O') && (v[1] == 'n' || v[1] == 'N') &&
        v[2] == '\0') {
        return 1;
    }
    return 0;
}

static int eq_ci(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return *a == '\0' && *b == '\0';
}

static uint16_t name_to_hid(const char *name)
{
    static const struct {
        const char *n;
        uint16_t    k;
    } tab[] = {
        {"up", AG_KEY_UP},       {"down", AG_KEY_DOWN},
        {"left", AG_KEY_LEFT},   {"right", AG_KEY_RIGHT},
        {"enter", AG_KEY_ENTER}, {"esc", AG_KEY_ESC},
        {"escape", AG_KEY_ESC},  {"space", AG_KEY_SPACE},
        {"tab", AG_KEY_TAB},     {"a", AG_KEY_A},
        {"b", AG_KEY_B},         {"c", AG_KEY_C},
        {"d", AG_KEY_D},         {"e", AG_KEY_E},
        {"f", AG_KEY_F},         {"g", AG_KEY_G},
        {"h", AG_KEY_H},         {"i", AG_KEY_I},
        {"j", AG_KEY_J},         {"k", AG_KEY_K},
        {"l", AG_KEY_L},         {"m", AG_KEY_M},
        {"n", AG_KEY_N},         {"o", AG_KEY_O},
        {"p", AG_KEY_P},         {"q", AG_KEY_Q},
        {"r", AG_KEY_R},         {"s", AG_KEY_S},
        {"t", AG_KEY_T},         {"u", AG_KEY_U},
        {"v", AG_KEY_V},         {"w", AG_KEY_W},
        {"x", AG_KEY_X},         {"y", AG_KEY_Y},
        {"z", AG_KEY_Z},
    };
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if (eq_ci(name, tab[i].n)) {
            return tab[i].k;
        }
    }
    return 0;
}

static int act_from_name(const char *act)
{
    if (eq_ci(act, "up")) {
        return SMS_ACT_UP;
    }
    if (eq_ci(act, "down")) {
        return SMS_ACT_DOWN;
    }
    if (eq_ci(act, "left")) {
        return SMS_ACT_LEFT;
    }
    if (eq_ci(act, "right")) {
        return SMS_ACT_RIGHT;
    }
    if (eq_ci(act, "b1") || eq_ci(act, "a")) {
        return SMS_ACT_B1;
    }
    if (eq_ci(act, "b2") || eq_ci(act, "b")) {
        return SMS_ACT_B2;
    }
    if (eq_ci(act, "pause")) {
        return SMS_ACT_PAUSE;
    }
    if (eq_ci(act, "quit")) {
        return SMS_ACT_QUIT;
    }
    return -1;
}

static void apply_line(sms_cfg_t *cfg, char *line)
{
    char *hash = strchr(line, '#');
    if (hash != NULL) {
        *hash = '\0';
    }
    /* trim */
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t' ||
                     line[n - 1] == '\r')) {
        line[--n] = '\0';
    }
    if (n == 0) {
        return;
    }
    char *eq = strchr(line, '=');
    if (eq == NULL) {
        return;
    }
    *eq = '\0';
    char *key = line;
    char *val = eq + 1;
    while (*val == ' ' || *val == '\t') {
        val++;
    }
    n = strlen(key);
    while (n > 0 && (key[n - 1] == ' ' || key[n - 1] == '\t')) {
        key[--n] = '\0';
    }

    if (eq_ci(key, "fullscreen") || eq_ci(key, "video.fullscreen")) {
        cfg->fullscreen = truthy(val);
        return;
    }

    /* pad0.up=Z  (c/start/x/… ignored here — hostfsd handles those for live pad) */
    if ((key[0] == 'p' || key[0] == 'P') && (key[1] == 'a' || key[1] == 'A') &&
        (key[2] == 'd' || key[2] == 'D') && (key[3] == '0' || key[3] == '1') &&
        key[4] == '.') {
        int pad = key[3] - '0';
        int act = act_from_name(key + 5);
        uint16_t hid = name_to_hid(val);
        if (act >= 0 && hid != 0) {
            cfg->key[pad][act] = hid;
        }
    }
}

static int read_cfg_file(const char *path, char *buf, size_t cap)
{
    const ag_handle_t h = ag_open(path, AG_O_RDONLY);
    if (h < 0) {
        return -1;
    }
    size_t n = 0;
    for (;;) {
        if (n + 1 >= cap) {
            break;
        }
        const int32_t got = ag_read(h, buf + n, cap - 1u - n);
        if (got < 0) {
            ag_close(h);
            return -1;
        }
        if (got == 0) {
            break;
        }
        n += (size_t)got;
    }
    ag_close(h);
    buf[n] = '\0';
    return (int)n;
}

static void parse_cfg_text(sms_cfg_t *cfg, char *text)
{
    char *p = text;
    while (*p != '\0') {
        char *eol = p;
        while (*eol != '\0' && *eol != '\n') {
            eol++;
        }
        const int at_end = (*eol == '\0');
        *eol = '\0';
        apply_line(cfg, p);
        if (at_end) {
            break;
        }
        p = eol + 1;
    }
}

int sms_cfg_load(sms_cfg_t *cfg, const char *rom_path, char *loaded_path,
                 size_t loaded_len)
{
    sms_cfg_set_defaults(cfg);
    if (loaded_path != NULL && loaded_len > 0) {
        loaded_path[0] = '\0';
    }

    char beside[AG_PATH_MAX];
    beside[0] = '\0';
    if (rom_path != NULL && rom_path[0] != '\0') {
        size_t cut = 0;
        for (size_t i = 0; rom_path[i] != '\0' && i + 1 < sizeof(beside); i++) {
            beside[i] = rom_path[i];
            if (rom_path[i] == '\\' || rom_path[i] == '/' ||
                rom_path[i] == ':') {
                cut = i + 1;
            }
        }
        if (cut > 0 && cut + 8 < sizeof(beside)) {
            memcpy(beside + cut, "sms.cfg", 8);
        } else {
            beside[0] = '\0';
        }
    }

    /* More specific first: beside the ROM wins over the HostFS default. */
    const char *cands[] = {
        beside[0] != '\0' ? beside : NULL,
        "sms.cfg",
        "h:\\sms.cfg",
        "a:\\sms.cfg",
        NULL,
    };

    char *buf = (char *)ag_malloc(CFG_MAX);
    if (buf == NULL) {
        return 0;
    }

    int loaded = 0;
    for (int i = 0; cands[i] != NULL; i++) {
        if (cands[i][0] == '\0') {
            continue;
        }
        if (read_cfg_file(cands[i], buf, CFG_MAX) >= 0) {
            parse_cfg_text(cfg, buf);
            loaded = 1;
            if (loaded_path != NULL && loaded_len > 0) {
                strncpy(loaded_path, cands[i], loaded_len - 1);
                loaded_path[loaded_len - 1] = '\0';
            }
            break;
        }
    }
    ag_free(buf);
    return loaded;
}

int sms_cfg_lookup(const sms_cfg_t *cfg, uint16_t keycode, int *pad_out,
                   int *act_out)
{
    if (cfg == NULL || keycode == 0) {
        return 0;
    }
    for (int pad = 0; pad < 2; pad++) {
        for (int act = 0; act < SMS_ACT_COUNT; act++) {
            if (cfg->key[pad][act] == keycode) {
                if (pad_out != NULL) {
                    *pad_out = pad;
                }
                if (act_out != NULL) {
                    *act_out = act;
                }
                return 1;
            }
        }
    }
    return 0;
}
