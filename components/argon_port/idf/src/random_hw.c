/*
 * ArgonOS port: ESP-IDF - random bytes from the hardware RNG.
 *
 * esp_fill_random draws on the RNG, which is fed by the RF/ADC noise sources;
 * it is the CSPRNG the WPA supplicant and TLS already trust on this chip.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/random.h>

#include "esp_random.h"

void ag_port_random(void *buf, size_t len)
{
    if (buf != NULL && len > 0) {
        esp_fill_random(buf, len);
    }
}
