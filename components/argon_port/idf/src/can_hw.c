/*
 * ArgonOS port: ESP-IDF - CAN over the TWAI controller.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdkconfig.h"

#if defined(CONFIG_ARGON_CAN) && CONFIG_ARGON_CAN

#include <string.h>

#include <argon/port/can.h>

#include "freertos/FreeRTOS.h"
#include "driver/twai.h"

static bool s_up;

static bool timing_for(uint32_t kbps, twai_timing_config_t *t)
{
    switch (kbps) {
    case 25:   { twai_timing_config_t x = TWAI_TIMING_CONFIG_25KBITS();   *t = x; return true; }
    case 50:   { twai_timing_config_t x = TWAI_TIMING_CONFIG_50KBITS();   *t = x; return true; }
    case 100:  { twai_timing_config_t x = TWAI_TIMING_CONFIG_100KBITS();  *t = x; return true; }
    case 125:  { twai_timing_config_t x = TWAI_TIMING_CONFIG_125KBITS();  *t = x; return true; }
    case 250:  { twai_timing_config_t x = TWAI_TIMING_CONFIG_250KBITS();  *t = x; return true; }
    case 500:  { twai_timing_config_t x = TWAI_TIMING_CONFIG_500KBITS();  *t = x; return true; }
    case 800:  { twai_timing_config_t x = TWAI_TIMING_CONFIG_800KBITS();  *t = x; return true; }
    case 1000: { twai_timing_config_t x = TWAI_TIMING_CONFIG_1MBITS();    *t = x; return true; }
    default: return false;
    }
}

int ag_port_can_open(int tx, int rx, uint32_t kbps)
{
    if (s_up) {
        ag_port_can_close();
    }
    twai_general_config_t g =
        TWAI_GENERAL_CONFIG_DEFAULT(tx, rx, TWAI_MODE_NORMAL);
    twai_timing_config_t t;
    if (!timing_for(kbps, &t)) {
        return -1;
    }
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    if (twai_driver_install(&g, &t, &f) != ESP_OK) {
        return -1;
    }
    if (twai_start() != ESP_OK) {
        twai_driver_uninstall();
        return -1;
    }
    s_up = true;
    return 0;
}

int ag_port_can_send(uint32_t id, const uint8_t *data, int len, bool extended)
{
    if (!s_up || len < 0 || len > 8) {
        return -1;
    }
    twai_message_t m;
    memset(&m, 0, sizeof(m));
    m.identifier = id;
    m.extd = extended ? 1 : 0;
    m.data_length_code = (uint8_t)len;
    if (data != NULL && len > 0) {
        memcpy(m.data, data, (size_t)len);
    }
    return (twai_transmit(&m, pdMS_TO_TICKS(1000)) == ESP_OK) ? 0 : -1;
}

int ag_port_can_recv(uint32_t *id, uint8_t *data, int *len, bool *extended,
                     uint32_t timeout_ms)
{
    if (!s_up) {
        return -1;
    }
    twai_message_t m;
    const esp_err_t e = twai_receive(&m, pdMS_TO_TICKS(timeout_ms));
    if (e == ESP_ERR_TIMEOUT) {
        return 0;
    }
    if (e != ESP_OK) {
        return -1;
    }
    if (id != NULL) {
        *id = m.identifier;
    }
    if (extended != NULL) {
        *extended = m.extd != 0;
    }
    const int n = m.data_length_code > 8 ? 8 : m.data_length_code;
    if (len != NULL) {
        *len = n;
    }
    if (data != NULL) {
        memcpy(data, m.data, (size_t)n);
    }
    return 1;
}

void ag_port_can_close(void)
{
    if (s_up) {
        twai_stop();
        twai_driver_uninstall();
        s_up = false;
    }
}

#endif /* CONFIG_ARGON_CAN */
