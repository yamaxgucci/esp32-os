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

#include <stdlib.h>

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

/* An update fed piece by piece: esp_ota_begin/write/end on the spare slot. */
typedef struct {
    const esp_partition_t *part;
    esp_ota_handle_t       h;
} ag_ota_push_t;

void *ag_port_ota_begin(uint32_t size)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL || (size != 0 && size > part->size)) {
        return NULL;
    }
    ag_ota_push_t *o = calloc(1, sizeof(*o));
    if (o == NULL) {
        return NULL;
    }
    o->part = part;
    if (esp_ota_begin(part, size ? size : OTA_SIZE_UNKNOWN, &o->h) != ESP_OK) {
        free(o);
        return NULL;
    }
    return o;
}

int ag_port_ota_write(void *h, const void *p, size_t n)
{
    ag_ota_push_t *o = (ag_ota_push_t *)h;
    if (o == NULL) {
        return -1;
    }
    return (esp_ota_write(o->h, p, n) == ESP_OK) ? 0 : -1;
}

int ag_port_ota_end(void *h, bool activate)
{
    ag_ota_push_t *o = (ag_ota_push_t *)h;
    if (o == NULL) {
        return -1;
    }
    esp_err_t err = esp_ota_end(o->h); /* validates the image */
    if (err == ESP_OK && activate) {
        err = esp_ota_set_boot_partition(o->part);
    }
    free(o);
    return (err == ESP_OK) ? 0 : -1;
}

void ag_port_ota_abort(void *h)
{
    ag_ota_push_t *o = (ag_ota_push_t *)h;
    if (o != NULL) {
        (void)esp_ota_abort(o->h);
        free(o);
    }
}

#endif /* CONFIG_ARGON_NET_OTA */
