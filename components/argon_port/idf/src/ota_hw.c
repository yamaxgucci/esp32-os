/*
 * ArgonOS port: ESP-IDF - over-the-air update over esp_https_ota.
 *
 * esp_https_ota does the whole job in one call: open the URL, stream the body
 * into the next OTA partition, validate the image and set it to boot.  http and
 * https both work; https is verified against the certificate bundle.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdkconfig.h"

#if defined(CONFIG_ARGON_NET_OTA) && CONFIG_ARGON_NET_OTA

#include <argon/port/ota.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"

bool ag_port_ota_capable(void)
{
    return esp_ota_get_next_update_partition(NULL) != NULL;
}

const char *ag_port_ota_running(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    return (p != NULL) ? p->label : "?";
}

int ag_port_ota_pull(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return -1;
    }
    esp_http_client_config_t http = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach, /* used only for https */
        .keep_alive_enable = true,
        .timeout_ms = 20000,
    };
    esp_https_ota_config_t cfg = {
        .http_config = &http,
    };
    const esp_err_t err = esp_https_ota(&cfg);
    return (err == ESP_OK) ? 0 : -1;
}

#endif /* CONFIG_ARGON_NET_OTA */
