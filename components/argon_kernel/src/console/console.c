/*
 * ArgonOS - console subsystem.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/console.h>

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/*
 * A lone ESC only becomes the Escape key once nothing follows it.  30 ms is
 * long enough that a real escape sequence has arrived even over a slow link,
 * and short enough that pressing Escape feels immediate.
 */
#define AG_CON_ESC_IDLE_MS 30

#define AG_CON_TICK_MS 10
#define AG_CON_EVENT_QUEUE 32
#define AG_CON_READ_CHUNK 64

typedef struct {
    const ag_con_transport_t *transport;
    void                     *ctx;
    ag_vtout_t                out;
    ag_vtin_t                 in;
    uint32_t                  last_byte_ms;
    bool                      used;
} ag_con_endpoint_t;

static ag_screen_t        s_screen;
static ag_con_endpoint_t  s_endpoints[AG_CON_MAX_ENDPOINTS];
static SemaphoreHandle_t  s_lock;
static QueueHandle_t      s_events;
static TaskHandle_t       s_task;
static bool               s_ready;
static ag_con_sink_fn     s_redirect;
static void              *s_redirect_ctx;

/* ---------------------------------------------------------------------- */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void ag_console_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    }
}

void ag_console_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGiveRecursive(s_lock);
    }
}

ag_screen_t *ag_console_screen(void) { return &s_screen; }

bool ag_console_ready(void) { return s_ready; }

/* ---------------------------------------------------------------------- */
/* Output                                                                 */
/* ---------------------------------------------------------------------- */

static void sink_to_transport(void *ctx, const char *data, size_t len)
{
    ag_con_endpoint_t *ep = (ag_con_endpoint_t *)ctx;
    if (ep->transport->write != NULL) {
        ep->transport->write(ep->ctx, data, len);
    }
}

/* Caller holds the lock. */
static void render_all(void)
{
    for (int i = 0; i < AG_CON_MAX_ENDPOINTS; i++) {
        ag_con_endpoint_t *ep = &s_endpoints[i];
        if (!ep->used) {
            continue;
        }
        ag_vtout_take_dirty(&ep->out, &s_screen);
        ag_vtout_flush(&ep->out, &s_screen, sink_to_transport, ep);
    }
    ag_screen_clear_dirty(&s_screen);
}

void ag_console_redirect(ag_con_sink_fn sink, void *ctx)
{
    ag_console_lock();
    s_redirect = sink;
    s_redirect_ctx = ctx;
    ag_console_unlock();
}

void ag_console_write(const char *buf, size_t len)
{
    if (!s_ready || buf == NULL) {
        return;
    }

    ag_console_lock();
    if (s_redirect != NULL) {
        s_redirect(s_redirect_ctx, buf, len);
    } else {
        ag_screen_write(&s_screen, buf, len);
    }
    ag_console_unlock();
}

void ag_console_puts(const char *s)
{
    if (s != NULL) {
        ag_console_write(s, strlen(s));
    }
}

int ag_console_vprintf(const char *fmt, va_list ap)
{
    char      buf[256];
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);

    if (n <= 0) {
        return n;
    }
    /* vsnprintf reports what it would have written; only what fit is real. */
    const size_t len = ((size_t)n < sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;
    ag_console_write(buf, len);
    return n;
}

int ag_console_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = ag_console_vprintf(fmt, ap);
    va_end(ap);
    return n;
}

void ag_console_sync(void)
{
    if (!s_ready) {
        return;
    }
    ag_console_lock();
    render_all();
    ag_console_unlock();
}

/* ---------------------------------------------------------------------- */
/* Input                                                                  */
/* ---------------------------------------------------------------------- */

static void publish(const ag_event_t *ev)
{
    ag_event_t stamped = *ev;
    stamped.ts = (ag_time_t)esp_timer_get_time();
    /*
     * A full queue means nobody is reading.  Dropping the oldest event keeps
     * the most recent keystrokes, which is what a user expects after the
     * system was briefly busy.
     */
    if (xQueueSend(s_events, &stamped, 0) != pdTRUE) {
        ag_event_t discard;
        (void)xQueueReceive(s_events, &discard, 0);
        (void)xQueueSend(s_events, &stamped, 0);
    }
}

static void pump_endpoint(ag_con_endpoint_t *ep)
{
    uint8_t chunk[AG_CON_READ_CHUNK];

    if (ep->transport->read == NULL) {
        return;
    }

    const int32_t n = ep->transport->read(ep->ctx, chunk, sizeof(chunk));
    for (int32_t i = 0; i < n; i++) {
        ag_event_t ev;
        if (ag_vtin_feed(&ep->in, chunk[i], &ev)) {
            publish(&ev);
        }
    }

    if (n > 0) {
        ep->last_byte_ms = now_ms();
    } else if (ag_vtin_busy(&ep->in) &&
               (now_ms() - ep->last_byte_ms) >= AG_CON_ESC_IDLE_MS) {
        ag_event_t ev;
        if (ag_vtin_idle(&ep->in, &ev)) {
            publish(&ev);
        }
    }
}

bool ag_console_read_event(ag_event_t *ev, uint32_t timeout_ms)
{
    if (!s_ready || ev == NULL) {
        return false;
    }
    const TickType_t ticks = (timeout_ms == UINT32_MAX)
                                 ? portMAX_DELAY
                                 : pdMS_TO_TICKS(timeout_ms);
    return xQueueReceive(s_events, ev, ticks) == pdTRUE;
}

int32_t ag_console_getch(uint32_t timeout_ms)
{
    const uint32_t deadline = now_ms() + timeout_ms;

    for (;;) {
        ag_event_t ev;
        const uint32_t remaining =
            (timeout_ms == UINT32_MAX)
                ? UINT32_MAX
                : ((now_ms() < deadline) ? (deadline - now_ms()) : 0);

        if (!ag_console_read_event(&ev, remaining)) {
            return -1;
        }
        if (ev.type == AG_EV_KEY_DOWN && ev.key.unicode != 0) {
            return (int32_t)ev.key.unicode;
        }
        if (timeout_ms != UINT32_MAX && now_ms() >= deadline) {
            return -1;
        }
    }
}

/* ---------------------------------------------------------------------- */

static void console_task(void *arg)
{
    (void)arg;

    for (;;) {
        for (int i = 0; i < AG_CON_MAX_ENDPOINTS; i++) {
            if (s_endpoints[i].used) {
                pump_endpoint(&s_endpoints[i]);
            }
        }

        /*
         * Unconditional: a flush with nothing pending emits nothing, and the
         * cursor can move without any cell changing.
         */
        ag_console_lock();
        render_all();
        ag_console_unlock();

        vTaskDelay(pdMS_TO_TICKS(AG_CON_TICK_MS));
    }
}

ag_err_t ag_console_init(uint16_t cols, uint16_t rows)
{
    if (s_ready) {
        return AG_OK;
    }

    const size_t need = ag_screen_memsize(cols, rows);
    void *mem = heap_caps_malloc(need, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (mem == NULL) {
        return -AG_ENOMEM;
    }

    const ag_err_t err = ag_screen_init(&s_screen, mem, need, cols, rows);
    if (err != AG_OK) {
        heap_caps_free(mem);
        return err;
    }

    s_lock = xSemaphoreCreateRecursiveMutex();
    s_events = xQueueCreate(AG_CON_EVENT_QUEUE, sizeof(ag_event_t));
    if (s_lock == NULL || s_events == NULL) {
        heap_caps_free(mem);
        return -AG_ENOMEM;
    }

    s_ready = true;

    /*
     * The console task lives on the system core: the application core is
     * meant to stay free for the application.
     */
    if (xTaskCreatePinnedToCore(console_task, "ag_con", 3072, NULL, 10, &s_task,
                                0) != pdPASS) {
        s_ready = false;
        heap_caps_free(mem);
        return -AG_ENOMEM;
    }

    return AG_OK;
}

ag_err_t ag_console_attach(const ag_con_transport_t *transport, void *ctx)
{
    if (transport == NULL) {
        return -AG_EINVAL;
    }

    ag_console_lock();
    for (int i = 0; i < AG_CON_MAX_ENDPOINTS; i++) {
        ag_con_endpoint_t *ep = &s_endpoints[i];
        if (ep->used) {
            continue;
        }

        memset(ep, 0, sizeof(*ep));
        ep->transport = transport;
        ep->ctx = ctx;
        ag_vtin_init(&ep->in);
        ag_vtout_init(&ep->out);
        ag_vtout_hello(&ep->out, sink_to_transport, ep);
        ep->last_byte_ms = now_ms();
        ep->used = true;

        ag_console_unlock();
        return AG_OK;
    }
    ag_console_unlock();

    return -AG_ENFILE;
}
