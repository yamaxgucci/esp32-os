/*
 * ArgonOS port: ESP-IDF - SNTP time sync.
 *
 * A one-shot: bring up the SNTP client with the given server, wait for it to
 * set the system clock, tear it down.  esp_netif_sntp does the settimeofday
 * itself, so nothing above here has to; the kernel's time(NULL) returns real
 * time the moment this returns AG_OK.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdkconfig.h"

#if defined(CONFIG_ARGON_NET_SNTP) && CONFIG_ARGON_NET_SNTP

#include <argon/port/sntp.h>

#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"

ag_err_t ag_port_sntp_sync(const char *server, uint32_t timeout_ms)
{
    if (server == NULL || server[0] == '\0') {
        server = "pool.ntp.org";
    }
    if (timeout_ms == 0) {
        timeout_ms = 10000;
    }

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
    cfg.start = true; /* begin querying at once, not on a later start() call */

    /*
     * esp_netif_sntp_init refuses a second init without a deinit.  A previous
     * sync always deinits on its way out, but a caller that was interrupted, or
     * some other component that stood SNTP up, could leave it initialised; deinit
     * first so a re-init cannot fail on that account.  Harmless when nothing is
     * up (it just clears an already-clear client).
     */
    esp_netif_sntp_deinit();

    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        return -AG_EIO;
    }

    const esp_err_t rc = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    esp_netif_sntp_deinit();

    if (rc == ESP_ERR_TIMEOUT) {
        return -AG_ETIMEDOUT;
    }
    return (rc == ESP_OK) ? AG_OK : -AG_EIO;
}

#endif /* CONFIG_ARGON_NET_SNTP */
