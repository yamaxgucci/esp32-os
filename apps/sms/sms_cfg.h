/*
 * ArgonOS SMS - control / video config (two pads).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef SMS_CFG_H
#define SMS_CFG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum sms_cfg_action {
    SMS_ACT_UP = 0,
    SMS_ACT_DOWN,
    SMS_ACT_LEFT,
    SMS_ACT_RIGHT,
    SMS_ACT_B1,
    SMS_ACT_B2,
    SMS_ACT_PAUSE,
    SMS_ACT_QUIT,
    SMS_ACT_COUNT
};

typedef struct {
    /* HID usage id (AG_KEY_*), 0 = unbound */
    uint16_t key[2][SMS_ACT_COUNT];
    /* 1: stretch SMS framebuffer to the full soft display (nearest). */
    int      fullscreen;
} sms_cfg_t;

void sms_cfg_set_defaults(sms_cfg_t *cfg);

/*
 * Loads the first readable candidate: sms.cfg beside the ROM, beside the
 * .AXE, cwd, then H:/A:.  Always leaves defaults filled in; returns 1 if a
 * file was parsed.  When `loaded_path` is non-NULL, copies the path that won.
 */
int sms_cfg_load(sms_cfg_t *cfg, const char *rom_path, const char *exe_path,
                 char *loaded_path, size_t loaded_len);

/* Map HID keycode → pad index (-1 none), action. */
int sms_cfg_lookup(const sms_cfg_t *cfg, uint16_t keycode, int *pad_out,
                   int *act_out);

#ifdef __cplusplus
}
#endif

#endif /* SMS_CFG_H */
