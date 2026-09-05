/*
 * ArgonOS port: ESP-IDF - the transmitter, driven by hardware.
 *
 * ag_port_uart_write is a thread copying into a ring and waking whenever the
 * peripheral has room.  For a console that is exactly right; for a hundred and
 * fifty kilobytes of picture a frame it is the wrong shape, and the board says
 * so - with a game running, the wire carried 172 KB/s of a possible 400 while
 * the sender waited for a core.
 *
 * UHCI is the UART's own DMA.  A transfer is handed over once and the thread
 * sleeps until it has gone; nothing wakes eighty times a frame, and nothing
 * has to be scheduled for the wire to stay full.  It reads external RAM too,
 * which matters here: the picture is in PSRAM and copying it into internal
 * memory first would put back the pass this exists to remove.
 *
 * Not every chip has it: SOC_UHCI_SUPPORTED is absent on the original ESP32
 * and on the S2, and there this file compiles to a port that answers
 * -AG_ENOTSUP in so many words.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/uart.h>

#include "soc/soc_caps.h"

#if SOC_UHCI_SUPPORTED

#include "driver/uhci.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/*
 * One controller per port, made on first use and never taken down: a display
 * driver is loaded for the length of a session, and tearing this down while a
 * transfer is in the air is a fault somewhere else entirely.
 */
#ifndef AG_PORT_UART_MAX
#define AG_PORT_UART_MAX 3
#endif

/*
 * Four in the queue, because one at a time leaves the wire idle.
 *
 * A transfer that is only started after the last one finished costs a
 * wake-up between every pair - the interrupt gives the semaphore, the task has
 * to be scheduled to see it, and the transmitter has nothing to send until it
 * is.  Measured over thirty slices that was about a hundred milliseconds of a
 * four-hundred millisecond frame.  With a queue the next slice is already
 * waiting when the last byte of this one leaves.
 */
#define AG_DMA_QUEUE 4

typedef struct {
    uhci_controller_handle_t ctrl;
    SemaphoreHandle_t        done;   /* counts finished transfers */
    uint32_t                 out;    /* handed over, not yet finished */
    bool                     tried;
} dma_port_t;

static dma_port_t s_dma[AG_PORT_UART_MAX];

static bool IRAM_ATTR on_tx_done(uhci_controller_handle_t ctrl,
                                 const uhci_tx_done_event_data_t *edata,
                                 void *user_ctx)
{
    (void)ctrl;
    (void)edata;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx, &woken);
    return woken == pdTRUE;
}

static dma_port_t *dma_for(int port)
{
    if (port < 0 || port >= AG_PORT_UART_MAX) {
        return NULL;
    }
    dma_port_t *d = &s_dma[port];
    if (d->ctrl != NULL) {
        return d;
    }
    if (d->tried) {
        return NULL; /* asked once, refused once; do not ask every frame */
    }
    d->tried = true;

    d->done = xSemaphoreCreateCounting(AG_DMA_QUEUE, 0);
    if (d->done == NULL) {
        return NULL;
    }

    /*
     * Sized for a picture: one transfer in the air at a time and up to a
     * slice's worth of it, which is what the display driver hands over.  The
     * receive side is not used here - the far end says very little and the
     * ordinary driver still reads it - so its memory is kept small.
     */
    const uhci_controller_config_t cfg = {
        .uart_port = (uart_port_t)port,
        .tx_trans_queue_depth = AG_DMA_QUEUE,
        .max_transmit_size = 8192,
        .max_receive_internal_mem = 512,
        .dma_burst_size = 32,
        .max_packet_receive = 0,
        .rx_eof_flags = {
            .rx_brk_eof = 0,
            .idle_eof = 0,
            .length_eof = 0,
        },
    };
    if (uhci_new_controller(&cfg, &d->ctrl) != ESP_OK) {
        vSemaphoreDelete(d->done);
        d->done = NULL;
        d->ctrl = NULL;
        return NULL;
    }

    const uhci_event_callbacks_t cbs = {
        .on_rx_trans_event = NULL,
        .on_tx_trans_done = on_tx_done,
    };
    if (uhci_register_event_callbacks(d->ctrl, &cbs, d->done) != ESP_OK) {
        (void)uhci_del_controller(d->ctrl);
        vSemaphoreDelete(d->done);
        d->done = NULL;
        d->ctrl = NULL;
        return NULL;
    }
    return d;
}

bool ag_port_uart_dma_ready(int port)
{
    return dma_for(port) != NULL;
}

/* One finished transfer, waited for.  A minute of patience is not a timeout
 * anybody should reach: reaching it means the peripheral has stopped. */
static bool wait_one(dma_port_t *d)
{
    if (xSemaphoreTake(d->done, pdMS_TO_TICKS(60000)) != pdTRUE) {
        return false;
    }
    if (d->out > 0u) {
        d->out--;
    }
    return true;
}

int32_t ag_port_uart_dma_submit(int port, const void *buf, size_t len)
{
    if (buf == NULL || len == 0u) {
        return -AG_EINVAL;
    }
    dma_port_t *d = dma_for(port);
    if (d == NULL) {
        return -AG_ENOTSUP;
    }

    /* Room in the queue first; only then is the buffer the hardware's. */
    while (d->out >= AG_DMA_QUEUE) {
        if (!wait_one(d)) {
            return -AG_ETIMEDOUT;
        }
    }

    const esp_err_t rc = uhci_transmit(d->ctrl, (uint8_t *)buf, len);
    if (rc != ESP_OK) {
        /* Alignment is the usual reason, and the caller can do something about
         * it - so say so rather than copying behind its back. */
        return (rc == ESP_ERR_INVALID_ARG) ? -AG_EINVAL : -AG_EIO;
    }
    d->out++;
    return (int32_t)len;
}

ag_err_t ag_port_uart_dma_drain(int port)
{
    dma_port_t *d = dma_for(port);
    if (d == NULL) {
        return -AG_ENOTSUP;
    }
    while (d->out > 0u) {
        if (!wait_one(d)) {
            return -AG_ETIMEDOUT;
        }
    }
    return AG_OK;
}

int32_t ag_port_uart_dma_write(int port, const void *buf, size_t len)
{
    const int32_t n = ag_port_uart_dma_submit(port, buf, len);
    if (n < 0) {
        return n;
    }
    const ag_err_t err = ag_port_uart_dma_drain(port);
    return (err == AG_OK) ? n : (int32_t)err;
}

#else /* !SOC_UHCI_SUPPORTED */

bool ag_port_uart_dma_ready(int port)
{
    (void)port;
    return false;
}

int32_t ag_port_uart_dma_write(int port, const void *buf, size_t len)
{
    (void)port;
    (void)buf;
    (void)len;
    return -AG_ENOTSUP;
}

int32_t ag_port_uart_dma_submit(int port, const void *buf, size_t len)
{
    (void)port;
    (void)buf;
    (void)len;
    return -AG_ENOTSUP;
}

ag_err_t ag_port_uart_dma_drain(int port)
{
    (void)port;
    return -AG_ENOTSUP;
}

#endif /* SOC_UHCI_SUPPORTED */
