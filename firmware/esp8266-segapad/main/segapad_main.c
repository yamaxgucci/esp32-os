/*
 * ArgonOS - an ESP8266 that pretends to be a PCF8574 with a Mega Drive pad on
 * it, so SEGAPAD.SYS can be tried before there is a pad and an expander.
 *
 * The buttons come from a phone: the module serves a pad page on port 80, on
 * its own access point SEGAPAD (open, 192.168.4.1) and, when main/wifi_secret.h
 * names a network, on that network too.  The page sends what is held; the
 * module answers the CYD's I2C reads with what a pad holding it would show
 * (apps/common/segapad - the same model the host tests run).
 *
 * THE WIRES are the UART ones, so the module that the CYD flashes over its
 * UART1 is the module it then reads over I2C, with nothing moved:
 *
 *   CYD IO21 (SDA) - module TX, GPIO1        CYD IO22 (SCL) - module RX, GPIO3
 *
 * After boot both are GPIOs; the ROM's banner at reset still goes out on
 * GPIO1, which the CYD sees as noise on an idle bus.
 *
 * THE BUS is answered in software: the ESP8266 has no I2C slave.  A START
 * (SDA falling while SCL is high) raises an interrupt, and the whole
 * transaction is then run in that interrupt, polling the two lines - about
 * 0.4 ms at 100 kHz.  The radio's NMI cannot be held off, and one landing in
 * the middle of a byte loses a bit; the CYD then sees a NACK or a garbled read,
 * which SEGAPAD.SYS already treats as one bad read among many.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/soc.h"
#include "esp8266/gpio_register.h"
#include "esp8266/pin_mux_register.h"
#include "rom/ets_sys.h"

#include "ag_segapad.h"
#include "page.h"

#if __has_include("wifi_secret.h")
#include "wifi_secret.h" /* SP_SSID, SP_PASS: gitignored, never in the tree */
#endif

#define SDA_PIN 1
#define SCL_PIN 3
#define SDA_BIT (1u << SDA_PIN)
#define SCL_BIT (1u << SCL_PIN)

#define PCF_ADDR 0x20
#define CPU_MHZ  160u
#define WAIT_CYC (CPU_MHZ * 2000u) /* 2 ms: a master that stopped mid-byte */
#define HELD_MS  1000u             /* a page that went quiet lets go        */

/* Each state the page reported plays until the CYD has read it SCANS times
 * whole - eight steps with no lost answer, and the step where a 6-button pad
 * shows Z Y X Mode among them - and for at least STATE_MS so a game
 * sees it for a few frames.  Counting by the clock alone lost taps: while the
 * request that brought a state comes in, this module drops most of its
 * answers on the bus, and at 48 ms 7 of 10 taps reached the CYD's driver, at
 * 100 ms 8 of 10, and at three reads of eight steps 7 of 10 (5 Nov 2026):
 * a step that comes late resets the pad's count like a real one's, and that
 * read has no X Y Z in it though nothing on the bus was lost.  STATE_MAX_MS lets a state go when no CYD
 * reads at all. */
#define STATE_MS     50u
#define STATE_MAX_MS 500u
#define SCANS        3u
#define STATES   16u

static ag_segapad_model_t s_pad;
static volatile uint16_t  s_held;
/* States the page reported, played in order: the httpd task adds at the head,
 * the main task takes from the tail.  While one plays the pad the CYD reads is
 * s_play; once they are all played it is s_held. */
static uint16_t           s_states[STATES];
static volatile unsigned  s_st_head, s_st_tail;
static volatile uint16_t  s_play;
static volatile bool      s_playing;

static inline uint16_t IRAM_ATTR pad_now(void)
{
    return s_playing ? s_play : s_held;
}
static volatile uint32_t  s_heard_ms;
static uint32_t           s_us, s_cc;
static uint8_t            s_latch = 0xff;
static volatile uint32_t  s_reads, s_writes, s_lost;
/* Whole reads of the pad: the CYD writes SELECT high twice in a row (its idle
 * byte, then step 0) to begin one, then reads eight steps; a lost answer
 * spoils the read it was in.  s_scans counts those, s_sixes the ones that
 * also reached the 6-button step. */
static volatile uint32_t  s_scans, s_sixes;
/* From a request with states in it to the first whole read of the CYD after
 * the first of them began to play, in ms: the last, the most and the sum. */
static volatile uint32_t  s_rq_ms, s_lag_pick, s_lag, s_lag_max, s_lag_sum, s_lag_n;
static int                s_run = -1;
static bool               s_six_seen;
static uint8_t            s_last_write;
/* What the bus looked like from here: interrupts taken, address bytes read
 * (and the last one), and STARTs that came too late to follow. */
static volatile uint32_t  s_irqs, s_addrs, s_late;
static volatile uint8_t   s_last_addr;

static inline uint32_t lines(void)
{
    return GPIO_REG_READ(GPIO_IN_ADDRESS);
}

/* Open drain: the output latch is 0, and a line is driven by enabling it. */
static inline void sda_low(void)
{
    GPIO_REG_WRITE(GPIO_ENABLE_W1TS_ADDRESS, SDA_BIT);
}

static inline void sda_free(void)
{
    GPIO_REG_WRITE(GPIO_ENABLE_W1TC_ADDRESS, SDA_BIT);
}

/* Microseconds, kept from the cycle counter, which wraps every 26 s. */
static uint32_t IRAM_ATTR now_us(void)
{
    const uint32_t cc = soc_get_ccount();
    const uint32_t d = (cc - s_cc) / CPU_MHZ;
    s_cc += d * CPU_MHZ;
    s_us += d;
    return s_us;
}

/* Wait for SCL to be `high`; false after 2 ms. */
static bool IRAM_ATTR wait_scl(bool high)
{
    const uint32_t t0 = soc_get_ccount();
    while (((lines() & SCL_BIT) != 0u) != high) {
        if (soc_get_ccount() - t0 > WAIT_CYC) {
            return false;
        }
    }
    return true;
}

#define RX_STOP    (-1)
#define RX_RESTART (-2)

/*
 * A byte from the master, SCL low on entry and on return.  SDA moving while
 * SCL is high is a STOP (rising) or a repeated START (falling).
 */
static int IRAM_ATTR rx_byte(void)
{
    int b = 0;
    for (int i = 0; i < 8; i++) {
        if (!wait_scl(true)) {
            return RX_STOP;
        }
        const uint32_t first = lines() & SDA_BIT;
        b = (b << 1) | (first ? 1 : 0);
        const uint32_t t0 = soc_get_ccount();
        for (;;) {
            const uint32_t l = lines();
            if ((l & SCL_BIT) == 0u) {
                break;
            }
            if ((l & SDA_BIT) != first) {
                return first ? RX_RESTART : RX_STOP;
            }
            if (soc_get_ccount() - t0 > WAIT_CYC) {
                return RX_STOP;
            }
        }
    }
    return b;
}

/* The ninth clock with SDA held low. */
static bool IRAM_ATTR ack(void)
{
    sda_low();
    const bool ok = wait_scl(true) && wait_scl(false);
    sda_free();
    return ok;
}

/* A byte to the master; true when it asked for another. */
static bool IRAM_ATTR tx_byte(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        if ((b >> i) & 1u) {
            sda_free();
        } else {
            sda_low();
        }
        if (!wait_scl(true) || !wait_scl(false)) {
            sda_free();
            return false;
        }
    }
    sda_free();
    if (!wait_scl(true)) {
        return false;
    }
    const bool more = (lines() & SDA_BIT) == 0u;
    (void)wait_scl(false);
    return more;
}

/* A write sets the port; SELECT is the pad's input. */
static void IRAM_ATTR port_write(uint8_t v)
{
    if (v == 0xffu && s_last_write == 0xffu) {
        s_run = 0;
        s_six_seen = false;
    }
    s_last_write = v;
    s_latch = v;
    s_pad.held = pad_now();
    ag_segapad_model_select(&s_pad, (v & AG_SEGAPAD_SELECT) != 0u, now_us());
    s_writes++;
}

/* A read is the pins: the pad's six lines, and the latch where it drives. */
static uint8_t IRAM_ATTR port_read(void)
{
    s_pad.held = pad_now();
    s_reads++;
    if (s_pad.sel && s_pad.falls == 3u) {
        s_six_seen = true;
    }
    if (s_run >= 0 && ++s_run == 8) {
        s_scans++;
        if (s_six_seen) {
            s_sixes++;
        }
        s_run = -1;
    }
    return (uint8_t)(ag_segapad_model_read(&s_pad) & (s_latch | 0x3fu));
}

static void IRAM_ATTR transaction(void)
{
    for (;;) {
        if (!wait_scl(false)) {
            return;
        }
        const int a = rx_byte();
        if (a < 0) {
            if (a == RX_RESTART) {
                continue;
            }
            return;
        }
        s_addrs++;
        s_last_addr = (uint8_t)a;
        if ((a >> 1) != PCF_ADDR) {
            return; /* not ours: let the next START find us */
        }
        if (!ack()) {
            s_lost++;
            s_run = -1;
            return;
        }
        if (a & 1) {
            while (tx_byte(port_read())) {
            }
            return;
        }
        for (;;) {
            const int d = rx_byte();
            if (d == RX_RESTART) {
                break; /* the read half of a write-then-read */
            }
            if (d < 0) {
                s_run = -1;
                return;
            }
            port_write((uint8_t)d);
            if (!ack()) {
                s_lost++;
                s_run = -1;
                return;
            }
        }
    }
}

static void IRAM_ATTR on_gpio(void *arg)
{
    (void)arg;
    const uint32_t st = GPIO_REG_READ(GPIO_STATUS_ADDRESS);
    GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, st);
    s_irqs++;
    /*
     * SDA fell.  With SCL still high this is a START, and the address has not
     * begun.  With SCL already low it may be a START this interrupt reached
     * late - the master holds SCL high only about 4 us after one - so it is
     * followed all the same while SDA is still low: if the first bit has
     * already gone, the address comes out wrong and is ignored, and the
     * count says so.
     */
    const uint32_t l = lines();
    if ((st & SDA_BIT) && !(l & SDA_BIT)) {
        if (!(l & SCL_BIT)) {
            s_late++;
        }
        transaction();
    }
    GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, SDA_BIT);
}

static void bus_init(void)
{
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_GPIO1);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0RXD_U, FUNC_GPIO3);
    PIN_PULLUP_EN(PERIPHS_IO_MUX_U0TXD_U);
    PIN_PULLUP_EN(PERIPHS_IO_MUX_U0RXD_U);
    GPIO_REG_WRITE(GPIO_OUT_W1TC_ADDRESS, SDA_BIT | SCL_BIT);
    GPIO_REG_WRITE(GPIO_ENABLE_W1TC_ADDRESS, SDA_BIT | SCL_BIT);

    gpio_config_t io = {
        .pin_bit_mask = SDA_BIT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);
    gpio_isr_register(on_gpio, NULL, 0, NULL);
    /* In this SDK registering does not enable the interrupt - only
     * gpio_isr_handler_add() does, and this file does not use it.  Without
     * this line the module counted no interrupt at all (4 Nov 2026). */
    _xt_isr_unmask(1u << ETS_GPIO_INUM);
}

/* ---- the page ---------------------------------------------------------- */

static esp_err_t get_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, k_page, strlen(k_page));
}

/*
 * /p?m=HEX[&e=HEX,HEX...]: what is held now, as AG_SP_* bits, and the states
 * the buttons went through since the last request, each played until the CYD
 * has read it (see SCANS) before m holds - a tap too quick for a request of
 * its own still reaches the CYD, with its release.
 */
static esp_err_t get_press(httpd_req_t *req)
{
    char q[96], v[8], e[72];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "m", v, sizeof(v)) == ESP_OK) {
        const uint16_t m = (uint16_t)(strtoul(v, NULL, 16) & AG_SP_ALL);
        if (httpd_query_key_value(q, "e", e, sizeof(e)) == ESP_OK) {
            for (char *p = e; *p != '\0';) {
                char *end = p;
                const uint16_t t = (uint16_t)(strtoul(p, &end, 16) & AG_SP_ALL);
                if (end == p) {
                    break;
                }
                const unsigned h = s_st_head;
                if (h - s_st_tail < STATES) {
                    s_states[h % STATES] = t;
                    s_st_head = h + 1u;
                }
                p = (*end == ',') ? end + 1 : end;
            }
            s_rq_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        }
        s_held = m;
        s_heard_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    }
    char out[220];
    const int n = snprintf(out, sizeof(out),
                           "%x %u %u %u irq %u addr %u last %02x late %u scans %u six %u"
                           " pick %u lag %u max %u sum %u n %u",
                           (unsigned)s_held, (unsigned)s_writes,
                           (unsigned)s_reads, (unsigned)s_lost,
                           (unsigned)s_irqs, (unsigned)s_addrs,
                           (unsigned)s_last_addr, (unsigned)s_late,
                           (unsigned)s_scans, (unsigned)s_sixes,
                           (unsigned)s_lag_pick, (unsigned)s_lag,
                           (unsigned)s_lag_max, (unsigned)s_lag_sum,
                           (unsigned)s_lag_n);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, out, n);
}

/*
 * POST /ota with the application image as the body: written to the other
 * slot, made the one to boot, and the module restarts into it.  A module in
 * the CYD's CN1 has no GPIO0 within reach, so this is how it is updated.
 */
/* A 500 that says which step failed, how, and how far it got: a bare one
 * twice sent the layout hunting blind. */
static esp_err_t ota_fail(httpd_req_t *req, const char *step, int err,
                          int done)
{
    char msg[96];
    const int n = snprintf(msg, sizeof(msg), "ota: %s failed (%d / 0x%x) at %d bytes\n",
                           step, err, (unsigned)err, done);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, msg, n);
    return ESP_FAIL;
}

static esp_err_t post_ota(httpd_req_t *req)
{
    const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t       h = 0;
    if (p == NULL) {
        return ota_fail(req, "no other slot", 0, 0);
    }
    esp_err_t e = esp_ota_begin(p, OTA_SIZE_UNKNOWN, &h);
    if (e != ESP_OK) {
        return ota_fail(req, "begin", e, 0);
    }
    static char buf[1024];
    int         left = (int)req->content_len;
    int         done = 0;
    while (left > 0) {
        const int n = httpd_req_recv(req, buf,
                                     left < (int)sizeof(buf) ? left
                                                             : (int)sizeof(buf));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            esp_ota_end(h);
            return ota_fail(req, "receive", n, done);
        }
        e = esp_ota_write(h, buf, (size_t)n);
        if (e != ESP_OK) {
            esp_ota_end(h);
            return ota_fail(req, "write", e, done);
        }
        left -= n;
        done += n;
    }
    e = esp_ota_end(h);
    if (e != ESP_OK) {
        return ota_fail(req, "end (image check)", e, done);
    }
    e = esp_ota_set_boot_partition(p);
    if (e != ESP_OK) {
        return ota_fail(req, "set boot slot", e, done);
    }
    httpd_resp_send(req, "ok, restarting\n", 15);
    vTaskDelay(500 / portTICK_PERIOD_MS);
    esp_restart();
    return ESP_OK;
}

/* GET /ota: the slot running, "slot 0" or "slot 1" - ota.ps1 sends the image
 * linked for the other one. */
static esp_err_t get_ota(httpd_req_t *req)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    char msg[32];
    const int n = snprintf(msg, sizeof(msg), "slot %d at 0x%x\n",
                           p != NULL && p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1,
                           p != NULL ? (unsigned)p->address : 0u);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, msg, n);
}

/*
 * An answer goes out as two writes, the headers and then the body, and with
 * Nagle on the body waited for the browser to acknowledge the headers, which
 * it puts off: the first byte came 20 ms after the request and the last 65 to
 * 130 (6 Nov 2026) - a press, and a release, later by that much.
 */
static esp_err_t on_open(httpd_handle_t hd, int fd)
{
    (void)hd;
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return ESP_OK;
}

static void http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 3;
    cfg.lru_purge_enable = true;
    cfg.open_fn = on_open;
    httpd_handle_t h = NULL;
    if (httpd_start(&h, &cfg) != ESP_OK) {
        return;
    }
    static const httpd_uri_t page = {"/", HTTP_GET, get_page, NULL};
    static const httpd_uri_t press = {"/p", HTTP_GET, get_press, NULL};
    static const httpd_uri_t ota = {"/ota", HTTP_POST, post_ota, NULL};
    static const httpd_uri_t slot = {"/ota", HTTP_GET, get_ota, NULL};
    httpd_register_uri_handler(h, &page);
    httpd_register_uri_handler(h, &press);
    httpd_register_uri_handler(h, &ota);
    httpd_register_uri_handler(h, &slot);
}

/* ---- the radio --------------------------------------------------------- */

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    }
}

static void wifi_start(void)
{
    tcpip_adapter_init();
    esp_event_loop_create_default();
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&ic);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);

    wifi_config_t ap = {0};
    strcpy((char *)ap.ap.ssid, "SEGAPAD");
    ap.ap.ssid_len = 7;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_OPEN;
#ifdef SP_SSID
    wifi_config_t sta = {0};
    strncpy((char *)sta.sta.ssid, SP_SSID, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, SP_PASS, sizeof(sta.sta.password) - 1);
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &sta);
#else
    esp_wifi_set_mode(WIFI_MODE_AP);
#endif
    esp_wifi_set_config(ESP_IF_WIFI_AP, &ap);
    esp_wifi_start();
    /* The radio dozes between the router's beacons by default, and a press
     * waited for the next one: a ping took 7 to 115 ms, 47 on average, and a
     * request of the page's about 100 (6 Nov 2026). */
    esp_wifi_set_ps(WIFI_PS_NONE);
#ifdef SP_SSID
    tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, "segapad");
    esp_wifi_connect();
#endif
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_NONE); /* GPIO1 is the bus, not a log */
    nvs_flash_init();
    ag_segapad_model_init(&s_pad, 1);
    s_cc = soc_get_ccount();
    bus_init();
    wifi_start();
    http_start();
    uint32_t play_at = 0, scans_at = 0, scans_pick = 0, rq_seen = 0;
    bool pending = false;
    for (;;) {
        vTaskDelay(1); /* a tick, 10 ms */
        const uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (pending && s_scans != scans_pick) {
            pending = false;
            s_lag = now - rq_seen;
            s_lag_max = s_lag > s_lag_max ? s_lag : s_lag_max;
            s_lag_sum += s_lag;
            s_lag_n++;
        }
        if (s_held != 0u && now - s_heard_ms > HELD_MS) {
            s_held = 0;
        }
        if (s_playing && now - play_at < STATE_MAX_MS &&
            (now - play_at < STATE_MS || s_sixes - scans_at < SCANS)) {
            continue;
        }
        if (s_st_tail != s_st_head) {
            s_play = s_states[s_st_tail % STATES];
            s_st_tail = s_st_tail + 1u;
            scans_at = s_sixes;
            if (s_rq_ms != rq_seen) {
                rq_seen = s_rq_ms;
                s_lag_pick = now - rq_seen;
                scans_pick = s_scans;
                pending = true;
            }
            s_playing = true;
            play_at = now;
        } else {
            s_playing = false;
        }
    }
}
