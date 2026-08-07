/*
 * ArgonOS port helpers for SMS Plus GX.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "shared.h"

#include <string.h>

t_config option;
uint16_t *sms_bitmap;

void system_manage_sram(uint8_t *sram, uint8_t slot_number, uint8_t mode)
{
    (void)sram;
    (void)slot_number;
    (void)mode;
}

void smsp_state(uint8_t slot_number, uint8_t mode)
{
    (void)slot_number;
    (void)mode;
}
