/*
 * ArgonOS - QEMU OpenEth + thin TCP sockets for api->net.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "sdkconfig.h"

#if CONFIG_ARGON_ENABLE_NET

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include <argon/log.h>
#include <argon/net.h>

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

#define AG_NET_MAX_SOCK 8
#define AG_NET_HANDLE_BASE 0x71000000

static esp_netif_t                   *s_netif;
static esp_eth_handle_t               s_eth;
static esp_eth_netif_glue_handle_t    s_glue;
static volatile bool                  s_got_ip;
static esp_ip4_addr_t                 s_ip;
static int                            s_fds[AG_NET_MAX_SOCK];
static bool                           s_in_use[AG_NET_MAX_SOCK];
static SemaphoreHandle_t              s_lock;

static void lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

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

static int slot_of(ag_handle_t h)
{
    if (h < AG_NET_HANDLE_BASE ||
        h >= AG_NET_HANDLE_BASE + AG_NET_MAX_SOCK) {
        return -1;
    }
    return (int)(h - AG_NET_HANDLE_BASE);
}

static ag_handle_t adopt_fd(int fd)
{
    if (fd < 0) {
        return (ag_handle_t)map_errno(errno);
    }
    lock();
    for (int i = 0; i < AG_NET_MAX_SOCK; i++) {
        if (!s_in_use[i]) {
            s_in_use[i] = true;
            s_fds[i] = fd;
            unlock();
            return (ag_handle_t)(AG_NET_HANDLE_BASE + i);
        }
    }
    unlock();
    close(fd);
    return (ag_handle_t)(-AG_ENFILE);
}

static int fd_of(ag_handle_t h)
{
    const int slot = slot_of(h);
    if (slot < 0 || !s_in_use[slot]) {
        return -1;
    }
    return s_fds[slot];
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id,
                      void *data)
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
    ag_log(AG_LOG_INFO, "net", "ip " IPSTR " mask " IPSTR " gw " IPSTR,
           IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.netmask),
           IP2STR(&ev->ip_info.gw));
}

ag_err_t ag_net_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return -AG_ENOMEM;
    }
    for (int i = 0; i < AG_NET_MAX_SOCK; i++) {
        s_fds[i] = -1;
        s_in_use[i] = false;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ag_log(AG_LOG_WARN, "net", "nvs_flash_init: %s", esp_err_to_name(err));
        /* Continue: DHCP does not strictly need NVS. */
    }

    err = esp_netif_init();
    if (err != ESP_OK) {
        ag_log(AG_LOG_ERROR, "net", "esp_netif_init: %s", esp_err_to_name(err));
        return -AG_EIO;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ag_log(AG_LOG_ERROR, "net", "event loop: %s", esp_err_to_name(err));
        return -AG_EIO;
    }

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&cfg);
    if (s_netif == NULL) {
        ag_log(AG_LOG_ERROR, "net", "esp_netif_new failed");
        return -AG_ENOMEM;
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.autonego_timeout_ms = 100;
    phy_config.reset_gpio_num = -1;

    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_config);
    if (mac == NULL || phy == NULL) {
        ag_log(AG_LOG_ERROR, "net", "OpenEth MAC/PHY create failed");
        return -AG_ENODEV;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&eth_config, &s_eth);
    if (err != ESP_OK) {
        ag_log(AG_LOG_ERROR, "net", "eth install: %s", esp_err_to_name(err));
        return -AG_EIO;
    }

    s_glue = esp_eth_new_netif_glue(s_eth);
    err = esp_netif_attach(s_netif, s_glue);
    if (err != ESP_OK) {
        ag_log(AG_LOG_ERROR, "net", "netif attach: %s", esp_err_to_name(err));
        return -AG_EIO;
    }

    (void)esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_got_ip,
                                     NULL);
    /*
     * ESP-IDF also logs GOT_IP under this tag.  Ours in on_got_ip is enough;
     * the duplicate used to glue itself to the shell prompt.
     */
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);

    err = esp_eth_start(s_eth);
    if (err != ESP_OK) {
        ag_log(AG_LOG_ERROR, "net", "eth start: %s", esp_err_to_name(err));
        return -AG_EIO;
    }

    ag_log(AG_LOG_INFO, "net", "OpenEth started (waiting for DHCP)");
    return AG_OK;
}

bool ag_net_ready(void) { return s_got_ip; }

static bool api_ready(void) { return s_got_ip; }

static ag_err_t api_ifaddr(uint32_t *addr_out)
{
    if (addr_out == NULL) {
        return -AG_EINVAL;
    }
    if (!s_got_ip) {
        return -AG_EAGAIN;
    }
    /* Host-order IPv4. */
    *addr_out = ntohl(s_ip.addr);
    return AG_OK;
}

static ag_err_t api_wait_ready(uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t budget =
        (timeout_ms == UINT32_MAX) ? portMAX_DELAY
                                  : pdMS_TO_TICKS(timeout_ms);
    while (!s_got_ip) {
        if (budget != portMAX_DELAY &&
            (xTaskGetTickCount() - start) >= budget) {
            return -AG_ETIMEDOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return AG_OK;
}

static ag_handle_t api_tcp_listen(uint16_t port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return (ag_handle_t)map_errno(errno);
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
        return (ag_handle_t)e;
    }
    if (listen(fd, 4) != 0) {
        const ag_err_t e = map_errno(errno);
        close(fd);
        return (ag_handle_t)e;
    }
    return adopt_fd(fd);
}

static ag_handle_t api_tcp_accept(ag_handle_t listen_h, uint32_t timeout_ms)
{
    const int lfd = fd_of(listen_h);
    if (lfd < 0) {
        return (ag_handle_t)(-AG_EBADF);
    }

    /*
     * ABI: 0 = return at once, UINT32_MAX = block forever.
     * Do NOT use SO_RCVTIMEO=0 for poll — on lwIP that means "wait forever",
     * which hung pcmvirt write() until Windows connected.
     */
    if (timeout_ms != UINT32_MAX) {
        fd_set         rfds;
        struct timeval tv;

        FD_ZERO(&rfds);
        FD_SET(lfd, &rfds);
        tv.tv_sec = (time_t)(timeout_ms / 1000u);
        tv.tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u);
        {
            const int pr = select(lfd + 1, &rfds, NULL, NULL, &tv);
            if (pr < 0) {
                return (ag_handle_t)map_errno(errno);
            }
            if (pr == 0) {
                return (ag_handle_t)(timeout_ms == 0u ? -AG_EAGAIN
                                                     : -AG_ETIMEDOUT);
            }
        }
    }

    struct sockaddr_in peer;
    socklen_t          plen = sizeof(peer);
    const int          cfd = accept(lfd, (struct sockaddr *)&peer, &plen);
    if (cfd < 0) {
        return (ag_handle_t)map_errno(errno);
    }
    {
        int yes = 1;
        (void)setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    }
    return adopt_fd(cfd);
}

static ag_handle_t api_tcp_connect(uint32_t addr, uint16_t port,
                                   uint32_t timeout_ms)
{
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return (ag_handle_t)map_errno(errno);
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
        return (ag_handle_t)e;
    }
    return adopt_fd(fd);
}

static int32_t api_send(ag_handle_t h, const void *buf, size_t len)
{
    const int fd = fd_of(h);
    if (fd < 0) {
        return -AG_EBADF;
    }
    if (buf == NULL && len > 0) {
        return -AG_EINVAL;
    }
    const int n = (int)send(fd, buf, len, 0);
    if (n < 0) {
        return (int32_t)map_errno(errno);
    }
    return (int32_t)n;
}

static int32_t api_recv(ag_handle_t h, void *buf, size_t len)
{
    const int fd = fd_of(h);
    if (fd < 0) {
        return -AG_EBADF;
    }
    if (buf == NULL && len > 0) {
        return -AG_EINVAL;
    }
    const int n = (int)recv(fd, buf, len, 0);
    if (n < 0) {
        return (int32_t)map_errno(errno);
    }
    return (int32_t)n;
}

static ag_err_t api_close(ag_handle_t h)
{
    const int slot = slot_of(h);
    if (slot < 0) {
        return -AG_EBADF;
    }
    lock();
    if (!s_in_use[slot]) {
        unlock();
        return -AG_EBADF;
    }
    const int fd = s_fds[slot];
    s_in_use[slot] = false;
    s_fds[slot] = -1;
    unlock();
    if (fd >= 0) {
        (void)close(fd);
    }
    return AG_OK;
}

static ag_err_t api_set_nonblock(ag_handle_t h, bool on)
{
    const int fd = fd_of(h);
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

const ag_net_api_t ag_net_api_impl = {
    .size = sizeof(ag_net_api_t),
    .ready = api_ready,
    .wait_ready = api_wait_ready,
    .ifaddr = api_ifaddr,
    .tcp_listen = api_tcp_listen,
    .tcp_accept = api_tcp_accept,
    .tcp_connect = api_tcp_connect,
    .send = api_send,
    .recv = api_recv,
    .close = api_close,
    .set_nonblock = api_set_nonblock,
};

const ag_net_api_t *ag_net_api_table(void) { return &ag_net_api_impl; }

#endif /* CONFIG_ARGON_ENABLE_NET */
