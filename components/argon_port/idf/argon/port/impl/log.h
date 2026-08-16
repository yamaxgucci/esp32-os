/*
 * ArgonOS port: ESP-IDF - the log underneath.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_LOG_H
#define ARGON_PORT_IMPL_LOG_H

#include "esp_log.h"

#define AG_PORT_LOG_NONE    ESP_LOG_NONE
#define AG_PORT_LOG_ERROR   ESP_LOG_ERROR
#define AG_PORT_LOG_WARN    ESP_LOG_WARN
#define AG_PORT_LOG_INFO    ESP_LOG_INFO
#define AG_PORT_LOG_DEBUG   ESP_LOG_DEBUG
#define AG_PORT_LOG_VERBOSE ESP_LOG_VERBOSE

static inline void ag_port_log_redirect(int (*sink)(const char *, va_list))
{
    esp_log_set_vprintf((vprintf_like_t)sink);
}

static inline void ag_port_log_level(const char *tag, int level)
{
    esp_log_level_set(tag, (esp_log_level_t)level);
}

static inline int ag_port_log_level_get(const char *tag)
{
    return (int)esp_log_level_get(tag);
}

#endif /* ARGON_PORT_IMPL_LOG_H */
