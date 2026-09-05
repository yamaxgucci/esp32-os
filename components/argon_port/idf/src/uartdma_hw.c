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

typedef struct {
    uhci_controller_handle_t ctrl;
    SemaphoreHandle_t        done;
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

    d->done = xSemaphoreCreateBinary();
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
        .tx_trans_queue_depth = 2,
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

int32_t ag_port_uart_dma_write(int port, const void *buf, size_t len)
{
    if (buf == NULL || len == 0u) {
        return -AG_EINVAL;
    }
    dma_port_t *d = dma_for(port);
    if (d == NULL) {
        return -AG_ENOTSUP;
    }

    /* Anything still in flight from a previous call is finished first: the
     * semaphore counts one, and a stale give would let this one return early. */
    (void)xSemaphoreTake(d->done, 0);

    const esp_err_t rc = uhci_transmit(d->ctrl, (uint8_t *)buf, len);
    if (rc != ESP_OK) {
        /* Alignment is the usual reason, and the caller can do something about
         * it - so say so rather than copying behind its back. */
        return (rc == ESP_ERR_INVALID_ARG) ? -AG_EINVAL : -AG_EIO;
    }

    /*
     * A frame at four megabaud is under half a second; a minute of patience
     * here is not a timeout anybody should ever reach, and reaching it means
     * the peripheral has stopped rather than that the wire is slow.
     */
    if (xSemaphoreTake(d->done, pdMS_TO_TICKS(60000)) != pdTRUE) {
        return -AG_ETIMEDOUT;
    }
    return (int32_t)len;
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

#endif /* SOC_UHCI_SUPPORTED */
