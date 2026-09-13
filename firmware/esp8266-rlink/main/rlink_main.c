/*
 * ArgonOS - RLINK coprocessor firmware for the ESP8266.
 *
 * This is the real far end of EXTRADIO.SYS: an ESP8266 (e.g. ESP-07) wired to an
 * ArgonOS board's UART1, running OUR protocol instead of the stock AT firmware.
 * It is the on-metal twin of tools/rlinkd.py (the QEMU fake): same RLINK wire
 * format (apps/common/rlink/ag_rlink.h), same request/reply/push shape - but the
 * sockets are lwIP sockets on a real Wi-Fi join, not the host's network.
 *
 * WIRE: raw RLINK frames over UART0 at 115200 8N1.  UART0 is the ESP8266's only
 * full UART and also its log console, so the logs are silenced (sdkconfig) and we
 * never printf - the boot-ROM banner is the one exception, drained by the guest's
 * magic resync before the first HELLO reply.
 *
 * CREDENTIALS: the guest hands us ssid+passphrase in the START frame (proto v2);
 * we join there and push RL_EV_GOTIP when DHCP lands.  Nothing is stored in flash.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "tcpip_adapter.h"
#include "nvs_flash.h"
#include "driver/uart.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "ag_rlink.h"

/* ag_err_t values we answer with (mirror sdk/include/argon/abi.h, as rlinkd.py
 * does - the codec is shared but the error enum is not). */
#define AG_ENOENT    2
#define AG_EIO       5
#define AG_EBADF     9
#define AG_EAGAIN    11
#define AG_ENFILE    23
#define AG_ETIMEDOUT 60

#define RL_UART       UART_NUM_0
#define RL_UART_BAUD  115200
#define RL_RX_BUF     2048
#define MAX_CHAN      8       /* == EXT_MAX_CHAN in the guest driver */
#define PUMP_STACK    2560
#define PUMP_RECV     512     /* DATA chunk; the guest reassembles into its ring */

/* ---------------------------------------------------------------------- */
/* State                                                                   */
/* ---------------------------------------------------------------------- */

static SemaphoreHandle_t s_wlock; /* serialise frames written to the guest   */
static SemaphoreHandle_t s_clock; /* protects the channel table              */

typedef struct {
    bool         used;
    bool         listener;
    int          fd;
    TaskHandle_t pump;
} chan_t;

static chan_t s_chan[MAX_CHAN];

static volatile bool     s_have_creds;
static volatile bool     s_got_ip;
static volatile uint32_t s_ip_host; /* host-order IPv4 once we have a lease */

/* One request payload at a time (the read loop is single-threaded). */
static uint8_t s_payload[RL_MAX_PAYLOAD];

/* ---------------------------------------------------------------------- */
/* Wire out                                                                */
/* ---------------------------------------------------------------------- */

static void send_frame(uint8_t op, uint8_t flags, uint16_t seq, int32_t status,
                       int32_t ch, uint32_t a0, uint32_t a1,
                       const void *payload, uint32_t plen)
{
    ag_rlink_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.op = op;
    h.flags = flags;
    h.seq = seq;
    h.status = status;
    h.ch = ch;
    h.a0 = a0;
    h.a1 = a1;
    h.len = plen;

    uint8_t hdr[RL_HDR_SIZE];
    ag_rlink_pack(hdr, &h); /* writes the magic for us */

    xSemaphoreTake(s_wlock, portMAX_DELAY);
    uart_write_bytes(RL_UART, (const char *)hdr, RL_HDR_SIZE);
    if (plen > 0 && payload != NULL) {
        uart_write_bytes(RL_UART, (const char *)payload, plen);
    }
    xSemaphoreGive(s_wlock);
}

static void reply(uint16_t seq, uint8_t op, int32_t status, int32_t ch,
                  uint32_t a0, uint32_t a1)
{
    send_frame(op, RL_F_RESPONSE, seq, status, ch, a0, a1, NULL, 0);
}

static void push_data(int ch, const void *p, uint32_t plen, bool eof)
{
    send_frame(RL_OP_DATA, eof ? RL_F_EOF : 0, 0, 0, ch, 0, 0, p, plen);
}

static void push_event(uint32_t ev, uint32_t a1)
{
    send_frame(RL_OP_EVENT, 0, 0, 0, -1, ev, a1, NULL, 0);
}

/* ---------------------------------------------------------------------- */
/* Channels                                                                */
/* ---------------------------------------------------------------------- */

static int alloc_ch(void)
{
    int c = -1;
    xSemaphoreTake(s_clock, portMAX_DELAY);
    for (int i = 0; i < MAX_CHAN; i++) {
        if (!s_chan[i].used) {
            s_chan[i].used = true;
            s_chan[i].listener = false;
            s_chan[i].fd = -1;
            s_chan[i].pump = NULL;
            c = i;
            break;
        }
    }
    xSemaphoreGive(s_clock);
    return c;
}

static void free_ch(int ch)
{
    if (ch < 0 || ch >= MAX_CHAN) {
        return;
    }
    xSemaphoreTake(s_clock, portMAX_DELAY);
    s_chan[ch].used = false;
    s_chan[ch].listener = false;
    s_chan[ch].fd = -1;
    s_chan[ch].pump = NULL;
    xSemaphoreGive(s_clock);
}

/* Received bytes are pushed, not pulled: one task per connected socket copies
 * recv() output into DATA frames until the far side closes (mirrors rlinkd's
 * pump threads). */
static void pump_task(void *arg)
{
    const int ch = (int)arg;
    int fd;
    xSemaphoreTake(s_clock, portMAX_DELAY);
    fd = (ch >= 0 && ch < MAX_CHAN) ? s_chan[ch].fd : -1;
    xSemaphoreGive(s_clock);

    uint8_t buf[PUMP_RECV];
    for (;;) {
        int n = recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            push_data(ch, buf, (uint32_t)n, false);
        } else {
            push_data(ch, NULL, 0, true); /* 0 = peer close, <0 = error */
            break;
        }
    }

    xSemaphoreTake(s_clock, portMAX_DELAY);
    if (ch >= 0 && ch < MAX_CHAN) {
        s_chan[ch].pump = NULL;
    }
    xSemaphoreGive(s_clock);
    vTaskDelete(NULL);
}

static void start_pump(int ch)
{
    xTaskCreate(pump_task, "rlpump", PUMP_STACK, (void *)ch, 5,
                &s_chan[ch].pump);
}

/* ---------------------------------------------------------------------- */
/* Request handlers                                                         */
/* ---------------------------------------------------------------------- */

static void on_hello(const ag_rlink_hdr_t *h)
{
    reply(h->seq, RL_OP_HELLO, 0, -1, RL_PROTO_VERSION, RL_CAP_SOCKETS);
}

static void on_start(const ag_rlink_hdr_t *h, const uint8_t *payload)
{
    if (h->len > 0) {
        uint32_t sl = h->a0;
        if (sl > h->len) {
            sl = h->len;
        }
        uint32_t pl = h->len - sl;
        char ssid[33] = {0};
        char pass[65] = {0};
        if (sl > 32) {
            sl = 32;
        }
        if (pl > 64) {
            pl = 64;
        }
        memcpy(ssid, payload, sl);
        memcpy(pass, payload + h->a0, pl);

        wifi_config_t wc;
        memset(&wc, 0, sizeof(wc));
        strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
        strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));

        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(ESP_IF_WIFI_STA, &wc);
        s_have_creds = true;
        s_got_ip = false;
        esp_wifi_connect();
    }
    reply(h->seq, RL_OP_START, 0, -1, 0, 0);
    /* RL_EV_GOTIP is pushed from the Wi-Fi event handler when DHCP lands. */
}

static void on_ready(const ag_rlink_hdr_t *h)
{
    reply(h->seq, RL_OP_READY, s_got_ip ? 1 : 0, -1, 0, 0);
}

static void on_ifaddr(const ag_rlink_hdr_t *h)
{
    reply(h->seq, RL_OP_IFADDR, 0, -1, s_ip_host, 0);
}

static void on_resolve(const ag_rlink_hdr_t *h, const uint8_t *payload)
{
    char name[128];
    uint32_t l = h->len;
    if (l > sizeof(name) - 1) {
        l = sizeof(name) - 1;
    }
    memcpy(name, payload, l);
    name[l] = '\0';

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(name, NULL, &hints, &res) != 0 || res == NULL) {
        reply(h->seq, RL_OP_RESOLVE, -AG_ENOENT, -1, 0, 0);
        return;
    }
    struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
    uint32_t host_order = lwip_ntohl(sa->sin_addr.s_addr);
    freeaddrinfo(res);
    reply(h->seq, RL_OP_RESOLVE, 0, -1, host_order, 0);
}

static void on_connect(const ag_rlink_hdr_t *h)
{
    const uint32_t addr = h->a0; /* host order */
    const uint16_t port = (uint16_t)h->ch;

    int c = alloc_ch();
    if (c < 0) {
        reply(h->seq, RL_OP_CONNECT, -AG_ENFILE, -1, 0, 0);
        return;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        free_ch(c);
        reply(h->seq, RL_OP_CONNECT, -AG_EIO, -1, 0, 0);
        return;
    }

    struct sockaddr_in d;
    memset(&d, 0, sizeof(d));
    d.sin_family = AF_INET;
    d.sin_port = lwip_htons(port);
    d.sin_addr.s_addr = lwip_htonl(addr);

    if (connect(fd, (struct sockaddr *)&d, sizeof(d)) != 0) {
        close(fd);
        free_ch(c);
        reply(h->seq, RL_OP_CONNECT, -AG_EIO, -1, 0, 0);
        return;
    }

    xSemaphoreTake(s_clock, portMAX_DELAY);
    s_chan[c].fd = fd;
    xSemaphoreGive(s_clock);
    reply(h->seq, RL_OP_CONNECT, 0, c, 0, 0);
    start_pump(c);
}

static void on_listen(const ag_rlink_hdr_t *h)
{
    const uint16_t port = (uint16_t)h->a0;

    int c = alloc_ch();
    if (c < 0) {
        reply(h->seq, RL_OP_LISTEN, -AG_ENFILE, -1, 0, 0);
        return;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        free_ch(c);
        reply(h->seq, RL_OP_LISTEN, -AG_EIO, -1, 0, 0);
        return;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in b;
    memset(&b, 0, sizeof(b));
    b.sin_family = AF_INET;
    b.sin_port = lwip_htons(port);
    b.sin_addr.s_addr = lwip_htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&b, sizeof(b)) != 0 || listen(fd, 4) != 0) {
        close(fd);
        free_ch(c);
        reply(h->seq, RL_OP_LISTEN, -AG_EIO, -1, 0, 0);
        return;
    }

    xSemaphoreTake(s_clock, portMAX_DELAY);
    s_chan[c].fd = fd;
    s_chan[c].listener = true;
    xSemaphoreGive(s_clock);
    reply(h->seq, RL_OP_LISTEN, 0, c, 0, 0);
}

static void on_accept(const ag_rlink_hdr_t *h)
{
    const int lch = h->ch;
    int lfd = -1;
    bool ok = false;
    if (lch >= 0 && lch < MAX_CHAN) {
        xSemaphoreTake(s_clock, portMAX_DELAY);
        ok = s_chan[lch].used && s_chan[lch].listener;
        lfd = s_chan[lch].fd;
        xSemaphoreGive(s_clock);
    }
    if (!ok) {
        reply(h->seq, RL_OP_ACCEPT, -AG_EBADF, -1, 0, 0);
        return;
    }

    /* a1: 0 = poll (non-blocking), 0xffffffff/absent = block, else ms. */
    int flags = fcntl(lfd, F_GETFL, 0);
    if (h->a1 == 0) {
        fcntl(lfd, F_SETFL, flags | O_NONBLOCK);
    } else if (h->a1 != 0xffffffffu) {
        struct timeval tv;
        tv.tv_sec = h->a1 / 1000;
        tv.tv_usec = (h->a1 % 1000) * 1000;
        setsockopt(lfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    int fd = accept(lfd, NULL, NULL);

    if (h->a1 == 0) {
        fcntl(lfd, F_SETFL, flags); /* restore blocking */
    } else if (h->a1 != 0xffffffffu) {
        struct timeval z = {0, 0};
        setsockopt(lfd, SOL_SOCKET, SO_RCVTIMEO, &z, sizeof(z));
    }

    if (fd < 0) {
        reply(h->seq, RL_OP_ACCEPT, -AG_EAGAIN, -1, 0, 0);
        return;
    }
    int c = alloc_ch();
    if (c < 0) {
        close(fd);
        reply(h->seq, RL_OP_ACCEPT, -AG_ENFILE, -1, 0, 0);
        return;
    }
    xSemaphoreTake(s_clock, portMAX_DELAY);
    s_chan[c].fd = fd;
    xSemaphoreGive(s_clock);
    reply(h->seq, RL_OP_ACCEPT, 0, c, 0, 0);
    start_pump(c);
}

static void on_send(const ag_rlink_hdr_t *h, const uint8_t *payload)
{
    const int ch = h->ch;
    int fd = -1;
    if (ch >= 0 && ch < MAX_CHAN) {
        xSemaphoreTake(s_clock, portMAX_DELAY);
        if (s_chan[ch].used && !s_chan[ch].listener) {
            fd = s_chan[ch].fd;
        }
        xSemaphoreGive(s_clock);
    }
    if (fd < 0) {
        reply(h->seq, RL_OP_SEND, -AG_EBADF, -1, 0, 0);
        return;
    }
    int sent = send(fd, payload, h->len, 0);
    reply(h->seq, RL_OP_SEND, sent < 0 ? -AG_EIO : sent, -1, 0, 0);
}

static void on_close(const ag_rlink_hdr_t *h)
{
    const int ch = h->ch;
    int fd = -1;
    if (ch >= 0 && ch < MAX_CHAN) {
        xSemaphoreTake(s_clock, portMAX_DELAY);
        if (s_chan[ch].used) {
            fd = s_chan[ch].fd;
        }
        xSemaphoreGive(s_clock);
    }
    if (fd >= 0) {
        close(fd); /* wakes the pump task, which sees the error and exits */
    }
    free_ch(ch);
    reply(h->seq, RL_OP_CLOSE, 0, -1, 0, 0);
}

static void on_nonblock(const ag_rlink_hdr_t *h)
{
    /* The guest tracks O_NONBLOCK itself (recv_now vs recv); just acknowledge. */
    reply(h->seq, RL_OP_NONBLOCK, 0, -1, 0, 0);
}

static void dispatch(const ag_rlink_hdr_t *h, const uint8_t *payload)
{
    switch (h->op) {
    case RL_OP_HELLO:    on_hello(h);              break;
    case RL_OP_START:    on_start(h, payload);     break;
    case RL_OP_READY:    on_ready(h);              break;
    case RL_OP_IFADDR:   on_ifaddr(h);             break;
    case RL_OP_RESOLVE:  on_resolve(h, payload);   break;
    case RL_OP_CONNECT:  on_connect(h);            break;
    case RL_OP_LISTEN:   on_listen(h);             break;
    case RL_OP_ACCEPT:   on_accept(h);             break;
    case RL_OP_SEND:     on_send(h, payload);      break;
    case RL_OP_CLOSE:    on_close(h);              break;
    case RL_OP_NONBLOCK: on_nonblock(h);           break;
    default:             /* unknown op: ignore, stay in sync */ break;
    }
}

/* ---------------------------------------------------------------------- */
/* Wire in                                                                 */
/* ---------------------------------------------------------------------- */

static void read_exact(uint8_t *buf, uint32_t n)
{
    uint32_t have = 0;
    while (have < n) {
        int r = uart_read_bytes(RL_UART, buf + have, n - have, portMAX_DELAY);
        if (r > 0) {
            have += (uint32_t)r;
        }
    }
}

/* Fill a header, then slide byte-by-byte until the magic is in place - this is
 * what swallows the boot-ROM banner before the guest's first HELLO. */
static void read_header(uint8_t hdr[RL_HDR_SIZE])
{
    read_exact(hdr, RL_HDR_SIZE);
    for (;;) {
        uint32_t magic = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                         ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
        if (magic == RL_MAGIC) {
            return;
        }
        memmove(hdr, hdr + 1, RL_HDR_SIZE - 1);
        read_exact(hdr + RL_HDR_SIZE - 1, 1);
    }
}

static void rlink_loop(void)
{
    for (;;) {
        uint8_t hdr[RL_HDR_SIZE];
        read_header(hdr);

        ag_rlink_hdr_t h;
        if (!ag_rlink_unpack(&h, hdr)) {
            continue; /* len out of range: resync on the next magic */
        }
        if (h.len > 0) {
            read_exact(s_payload, h.len);
        }
        dispatch(&h, s_payload);
    }
}

/* ---------------------------------------------------------------------- */
/* Wi-Fi                                                                    */
/* ---------------------------------------------------------------------- */

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_got_ip = false;
        if (s_have_creds) {
            esp_wifi_connect(); /* keep retrying until START's network is up */
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_ip_host = lwip_ntohl(e->ip_info.ip.addr);
        s_got_ip = true;
        push_event(RL_EV_GOTIP, s_ip_host);
    }
    /* WIFI_EVENT_STA_START: do nothing - we connect only once START gives us a
     * network, not on bring-up. */
}

/* ---------------------------------------------------------------------- */
/* Boot                                                                     */
/* ---------------------------------------------------------------------- */

void app_main(void)
{
    nvs_flash_init();
    tcpip_adapter_init();
    esp_event_loop_create_default();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start(); /* idles in STA until START hands us credentials */

    esp_log_level_set("*", ESP_LOG_NONE); /* keep UART0 clean for RLINK */

    uart_config_t uc = {
        .baud_rate = RL_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(RL_UART, &uc);
    uart_driver_install(RL_UART, RL_RX_BUF, 0, 0, NULL, 0);

    s_wlock = xSemaphoreCreateMutex();
    s_clock = xSemaphoreCreateMutex();

    rlink_loop();
}
