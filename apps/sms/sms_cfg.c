/*
 * ArgonOS SMS - control config loader.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "sms_cfg.h"

#include <argon/argon.h>
#include <argon/keys.h>

#include <string.h>

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
}

int sms_cfg_load(sms_cfg_t *cfg, const char *rom_path)
{
    (void)rom_path;
    /*
     * Defaults only in-guest.  Opening sms.cfg from A:/T: (and relative paths)
     * has triple-faulted this process; HostFS live pad reads the same file on
     * the Windows side via tools/sms_pad.py.
     */
    sms_cfg_set_defaults(cfg);
    return 0;
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
