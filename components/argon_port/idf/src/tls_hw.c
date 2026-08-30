/*
 * ArgonOS port: ESP-IDF - TLS client over esp_tls.
 *
 * esp_tls does the handshake and the record crypto; this wraps it in the same
 * recv_now / send / wait_readable shape the plain socket port has, so the
 * kernel's buffered reader drives http and https through one branch.  The peer
 * is verified against the certificate bundle esp_crt_bundle compiles in.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdkconfig.h"

#if defined(CONFIG_ARGON_NET_TLS) && CONFIG_ARGON_NET_TLS

#include <stdlib.h>
#include <string.h>

#include <argon/port/tls.h>
#include <argon/port/net.h> /* reuse nonblock + wait_readable on the raw fd */

#include "esp_crt_bundle.h"
#include "esp_tls.h"

struct ag_port_tls {
    esp_tls_t *tls;
    int        sockfd;
};

ag_port_tls_t ag_port_tls_connect(const char *host, uint16_t port,
                                  uint32_t timeout_ms)
{
    if (host == NULL || host[0] == '\0') {
        return NULL;
    }
    if (timeout_ms == 0) {
        timeout_ms = 15000;
    }

    struct ag_port_tls *h = calloc(1, sizeof(*h));
    if (h == NULL) {
        return NULL;
    }
    h->sockfd = -1;

    h->tls = esp_tls_init();
    if (h->tls == NULL) {
        free(h);
        return NULL;
    }

    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach, /* verify against the bundle */
        .timeout_ms = (int)timeout_ms,
    };

    /* Blocking connect + handshake, bounded by timeout_ms.  1 = up. */
    if (esp_tls_conn_new_sync(host, (int)strlen(host), (int)port, &cfg,
                              h->tls) != 1) {
        esp_tls_conn_destroy(h->tls);
        free(h);
        return NULL;
    }

    /* Past the handshake, run non-blocking so recv_now never waits inside the
     * TLS library - the kernel's reader does the waiting and the Ctrl+C check. */
    if (esp_tls_get_conn_sockfd(h->tls, &h->sockfd) == ESP_OK && h->sockfd >= 0) {
        (void)ag_port_net_nonblock(h->sockfd, true);
    }
    return h;
}

int32_t ag_port_tls_recv_now(ag_port_tls_t h, void *buf, size_t len)
{
    if (h == NULL || h->tls == NULL) {
        return -AG_EBADF;
    }
    const ssize_t n = esp_tls_conn_read(h->tls, buf, len);
    if (n > 0) {
        return (int32_t)n;
    }
    if (n == 0) {
        return 0; /* peer closed */
    }
    if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
        return -AG_EAGAIN; /* nothing decrypted yet, come back later */
    }
    return -AG_EIO;
}

int32_t ag_port_tls_send(ag_port_tls_t h, const void *buf, size_t len)
{
    if (h == NULL || h->tls == NULL) {
        return -AG_EBADF;
    }
    const ssize_t n = esp_tls_conn_write(h->tls, buf, len);
    if (n > 0) {
        return (int32_t)n;
    }
    if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
        return -AG_EAGAIN;
    }
    return -AG_EIO;
}

int ag_port_tls_wait_readable(ag_port_tls_t h, uint32_t timeout_ms)
{
    if (h == NULL || h->sockfd < 0) {
        return -AG_EBADF;
    }
    return ag_port_net_wait_readable(h->sockfd, timeout_ms);
}

size_t ag_port_tls_pending(ag_port_tls_t h)
{
    if (h == NULL || h->tls == NULL) {
        return 0;
    }
    const ssize_t avail = esp_tls_get_bytes_avail(h->tls);
    return (avail > 0) ? (size_t)avail : 0;
}

void ag_port_tls_close(ag_port_tls_t h)
{
    if (h == NULL) {
        return;
    }
    if (h->tls != NULL) {
        esp_tls_conn_destroy(h->tls);
    }
    free(h);
}

#endif /* CONFIG_ARGON_NET_TLS */
