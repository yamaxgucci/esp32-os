/*
 * ArgonOS SMS - control config (two pads).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef SMS_CFG_H
#define SMS_CFG_H

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
} sms_cfg_t;

void sms_cfg_set_defaults(sms_cfg_t *cfg);

/* Loads first readable path among the usual candidates.  Always leaves
 * defaults filled in; returns 1 if a file was parsed. */
int sms_cfg_load(sms_cfg_t *cfg, const char *rom_path);

/* Map HID keycode → pad index (-1 none), action. */
int sms_cfg_lookup(const sms_cfg_t *cfg, uint16_t keycode, int *pad_out,
                   int *act_out);

#ifdef __cplusplus
}
#endif

#endif /* SMS_CFG_H */
