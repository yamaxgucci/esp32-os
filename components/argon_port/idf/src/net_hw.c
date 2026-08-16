/*
 * ArgonOS port: ESP-IDF - OpenEth under QEMU, and lwIP sockets.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdkconfig.h"

#if CONFIG_ARGON_ENABLE_NET

#include <argon/port/net.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

static esp_netif_t                *s_netif;
static esp_eth_handle_t            s_eth;
static esp_eth_netif_glue_handle_t s_glue;
static volatile bool               s_got_ip;
static esp_ip4_addr_t              s_ip;
static ag_port_net_ready_fn        s_on_ready;

static ag_err_t map_errno(int e)
{
    switch (e) {
    case 0:
        return AG_OK;
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
        return -AG_EAGAIN;
    case ENOMEM:
    case ENOBUFS:
        return -AG_ENOMEM;
    case EBADF:
        return -AG_EBADF;
    case EINVAL:
        return -AG_EINVAL;
    case ECONNRESET:
    case EPIPE:
        return -AG_EIO;
    case ETIMEDOUT:
        return -AG_ETIMEDOUT;
    case EADDRINUSE:
        return -AG_EBUSY;
    case ENOTCONN:
        return -AG_ENOTSUP;
    default:
        return -AG_EIO;
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
    if (ev == NULL || ev->esp_netif != s_netif) {
        return;
    }
    s_ip = ev->ip_info.ip;
    s_got_ip = true;

    if (s_on_ready != NULL) {
        s_on_ready(ntohl(ev->ip_info.ip.addr), ntohl(ev->ip_info.netmask.addr),
                   ntohl(ev->ip_info.gw.addr));
    }
}

void ag_port_net_on_ready(ag_port_net_ready_fn fn) { s_on_ready = fn; }

ag_err_t ag_port_net_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        err = nvs_flash_init();
    }
    /* Carry on either way: DHCP does not strictly need NVS. */

    if (esp_netif_init() != ESP_OK) {
        return -AG_EIO;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return -AG_EIO;
    }

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&cfg);
    if (s_netif == NULL) {
        return -AG_ENOMEM;
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.autonego_timeout_ms = 100;
    phy_config.reset_gpio_num = -1;

    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_config);
    if (mac == NULL || phy == NULL) {
        return -AG_ENODEV;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    if (esp_eth_driver_install(&eth_config, &s_eth) != ESP_OK) {
        return -AG_EIO;
    }

    s_glue = esp_eth_new_netif_glue(s_eth);
    if (esp_netif_attach(s_netif, s_glue) != ESP_OK) {
        return -AG_EIO;
    }

    (void)esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_got_ip,
                                     NULL);
    /*
     * ESP-IDF also logs GOT_IP under this tag.  The kernel's own line is
     * enough; the duplicate used to glue itself to the shell prompt.
     */
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);

    if (esp_eth_start(s_eth) != ESP_OK) {
        return -AG_EIO;
    }
    return AG_OK;
}

bool ag_port_net_ready(void) { return s_got_ip; }

ag_err_t ag_port_net_ifaddr(uint32_t *addr)
{
    if (addr == NULL) {
        return -AG_EINVAL;
    }
    if (!s_got_ip) {
        return -AG_EAGAIN;
    }
    *addr = ntohl(s_ip.addr); /* host-order IPv4 */
    return AG_OK;
}

int ag_port_net_listen(uint16_t port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return (int)map_errno(errno);
    }

    int yes = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        const ag_err_t e = map_errno(errno);
        close(fd);
        return (int)e;
    }
    if (listen(fd, 4) != 0) {
        const ag_err_t e = map_errno(errno);
        close(fd);
        return (int)e;
    }
    return fd;
}

int ag_port_net_accept(int lfd, uint32_t timeout_ms)
{
    if (lfd < 0) {
        return -AG_EBADF;
    }

    /*
     * 0 = return at once, UINT32_MAX = block forever.  Do NOT use
     * SO_RCVTIMEO = 0 to poll: on lwIP that means "wait forever", which hung a
     * driver's write() until the development machine happened to connect.
     */
    if (timeout_ms != UINT32_MAX) {
        fd_set         rfds;
        struct timeval tv;

        FD_ZERO(&rfds);
        FD_SET(lfd, &rfds);
        tv.tv_sec = (time_t)(timeout_ms / 1000u);
        tv.tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u);

        const int pr = select(lfd + 1, &rfds, NULL, NULL, &tv);
        if (pr < 0) {
            return (int)map_errno(errno);
        }
        if (pr == 0) {
            return (int)(timeout_ms == 0u ? -AG_EAGAIN : -AG_ETIMEDOUT);
        }
    }

    struct sockaddr_in peer;
    socklen_t          plen = sizeof(peer);
    const int          cfd = accept(lfd, (struct sockaddr *)&peer, &plen);
    if (cfd < 0) {
        return (int)map_errno(errno);
    }

    int yes = 1;
    (void)setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    return cfd;
}

int ag_port_net_connect(uint32_t addr, uint16_t port, uint32_t timeout_ms)
{
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return (int)map_errno(errno);
    }

    if (timeout_ms != UINT32_MAX) {
        struct timeval tv;
        tv.tv_sec = (time_t)(timeout_ms / 1000u);
        tv.tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u);
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(addr);
    sa.sin_port = htons(port);

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        const ag_err_t e = map_errno(errno);
        close(fd);
        return (int)e;
    }
    return fd;
}

int32_t ag_port_net_send(int fd, const void *buf, size_t len)
{
    if (fd < 0) {
        return -AG_EBADF;
    }
    const int n = (int)send(fd, buf, len, 0);
    return (n < 0) ? (int32_t)map_errno(errno) : (int32_t)n;
}

int32_t ag_port_net_recv(int fd, void *buf, size_t len)
{
    if (fd < 0) {
        return -AG_EBADF;
    }
    const int n = (int)recv(fd, buf, len, 0);
    return (n < 0) ? (int32_t)map_errno(errno) : (int32_t)n;
}

void ag_port_net_close(int fd)
{
    if (fd >= 0) {
        (void)close(fd);
    }
}

ag_err_t ag_port_net_nonblock(int fd, bool on)
{
    if (fd < 0) {
        return -AG_EBADF;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return map_errno(errno);
    }
    const int next = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (fcntl(fd, F_SETFL, next) < 0) {
        return map_errno(errno);
    }
    return AG_OK;
}

#endif /* CONFIG_ARGON_ENABLE_NET */
