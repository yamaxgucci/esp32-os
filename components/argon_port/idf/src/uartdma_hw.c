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
#include "sdkconfig.h"

#if SOC_UHCI_SUPPORTED

#include "driver/uhci.h"
#include "driver/uart.h"
#include "hal/uart_ll.h"
#include <string.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

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
    /* receiving: one ring the DMA writes round and round, and the pieces
     * of it that are full, in order, for the reader */
    QueueHandle_t            rx_q;
    SemaphoreHandle_t        rx_rearm;  /* given when a receive has ENDED */
    /*
     * Two rings, used in turn: each receive that ends on idle is followed by
     * one on the OTHER ring, so the tail it left is not under the next one.
     * Per ring: how many times it has been mounted, and how far its latest
     * mount has written, in bytes from that mount's start.
     */
    uint8_t                 *rx_ring[2];
    volatile uint32_t        rx_gen[2];
    volatile uint32_t        rx_mv[2];
    volatile uint8_t         rx_now;     /* the ring mounted, or last mounted */
    volatile bool            rx_active;  /* a receive is mounted on rx_now */
    int                      port;
    volatile uint32_t        rx_overrun;

} dma_port_t;

static dma_port_t s_dma[AG_PORT_UART_MAX];

static bool IRAM_ATTR on_tx_done(uhci_controller_handle_t ctrl,
                                 const uhci_tx_done_event_data_t *edata,
                                 void *user_ctx)
{
    (void)ctrl;
    (void)edata;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(((dma_port_t *)user_ctx)->done, &woken);
    return woken == pdTRUE;
}

/*
 * RECEIVING BY DMA, and what it replaces is the one thing the UART could not
 * survive at 16 Mbaud: a 128-byte FIFO emptied by an interrupt.  The driver
 * asks for that interrupt only when 120 bytes are in, which at 16 Mbaud leaves
 * eight bytes - five microseconds - for it to be taken.
 *
 * ONE RECEIVE, MOUNTED ONCE, THAT DOES NOT END.  The driver's descriptor list
 * for receiving is a circle: the DMA fills node after node of one buffer and
 * goes round again, raising an event per full node, and it stops only on an
 * end-of-frame.  The first two versions asked for an end every 4096 bytes
 * (length_eof) and mounted the next buffer after it - and lost bytes at every
 * one of those seams, at 4 Mbaud as much as at 16: on the Zero-C3 pair, 490
 * blocks gone in five seconds whatever the block size, one per 4 KB received.
 * So there is no length end here at all.  The end that is left is IDLE: the
 * line fell quiet, the tail of what was sent is delivered, and the receive is
 * mounted again - which is a gap, and what arrives in it is lost (see
 * rx_task for how rare that is made, and dma_for for the bursts that turned
 * each end into corruption as well).
 *
 * A READER THAT FALLS BEHIND LOSES BYTES; IT MUST NEVER GET WRONG ONES.  There
 * is no flow control on these wires, so a reader slower than the line loses
 * - and it did, on a board computing beside it.  What it must not do is what
 * it did: take a piece of the ring the DMA had already gone round and
 * written again, and hand back a block half old and half new - 1741 damaged
 * blocks where there should have been missing ones.  So every piece carries
 * the number of the node it was, and after copying it out the reader checks
 * that the DMA has not since come round to that node: if it has, what was
 * copied is thrown away and counted as an overrun.  A gap, never a mix.
 *
 * TWO RINGS IN TURN, because a receive that ends on idle is mounted again at
 * the START of its ring - over the node the tail it just delivered is in.
 * On a board where the sender pauses often (one computing beside it), every
 * short burst's tail sat in node 0 of the ring about to be written, and a
 * check that said so threw nearly everything away.  With the next receive on
 * the other ring, a tail is safe until that ring comes round again.
 *
 * Each ring is sized by what the chip can spare: 16 KB each on the S3 (10 ms
 * of 16 Mbaud per ring), 8 KB each on the C3, which is short of memory.
 */
#if CONFIG_IDF_TARGET_ESP32C3
#define AG_DMA_RX_RING 8192u
#else
#define AG_DMA_RX_RING 16384u
#endif
/* the DMA may be anywhere inside the node after the last one it finished,
 * and a descriptor holds at most 4095 bytes */
#define AG_DMA_RX_NODE_MAX 4096u
#define AG_DMA_RX_EVQ      (2u * (AG_DMA_RX_RING / 4095u + 1u) + 4u)

typedef struct {
    const uint8_t *p;
    uint32_t       n;
    uint32_t       off;  /* bytes from its mount's start to where it begins */
    uint32_t       gen;  /* which mount of its ring */
    uint8_t        ring;
} rx_piece_t;

static bool IRAM_ATTR on_rx(uhci_controller_handle_t ctrl,
                            const uhci_rx_event_data_t *edata, void *user_ctx)
{
    (void)ctrl;
    dma_port_t *d = (dma_port_t *)user_ctx;
    BaseType_t  woken = pdFALSE;
    const uint8_t r = d->rx_now;
    if (edata->recv_size > 0u) {
        const rx_piece_t piece = {edata->data, (uint32_t)edata->recv_size,
                                  d->rx_mv[r], d->rx_gen[r], r};
        d->rx_mv[r] += (uint32_t)edata->recv_size;
        if (xQueueSendFromISR(d->rx_q, &piece, &woken) != pdTRUE) {
            /* the oldest waiting piece is long overwritten by now */
            rx_piece_t old;
            (void)xQueueReceiveFromISR(d->rx_q, &old, &woken);
            (void)xQueueSendFromISR(d->rx_q, &piece, &woken);
            d->rx_overrun++;
        }
    }
    if (edata->flags.totally_received) {
        d->rx_active = false;
        xSemaphoreGiveFromISR(d->rx_rearm, &woken);
    }
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
     * Sized for a picture: up to a slice's worth per transfer, which is what
     * the display driver hands over.  The receive side is one ring of
     * AG_DMA_RX_RING, and it ends only when the line goes idle (see on_rx).
     */
    const uhci_controller_config_t cfg = {
        .uart_port = (uart_port_t)port,
        .tx_trans_queue_depth = AG_DMA_QUEUE,
        .max_transmit_size = 8192,
        .max_receive_internal_mem = AG_DMA_RX_RING,
        /*
         * NO BURSTS, and this is the line that stopped the corruption.  With
         * 32-byte bursts every idle end left the last 32 or 64 bytes before
         * the pause replaced by the first 32 or 64 after it - byte for byte
         * the next block's beginning, 126 damaged 8 KB blocks in 965 at
         * 16 Mbaud - and with bursts off not one block was damaged over the
         * same run.  At 1.6 MB/s single transfers cost the bus nothing.
         */
        .dma_burst_size = 0,
        .max_packet_receive = AG_DMA_RX_RING,
        .rx_eof_flags = {
            .rx_brk_eof = 0,
            .idle_eof = 1,
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
        .on_rx_trans_event = on_rx,
        .on_tx_trans_done = on_tx_done,
    };
    if (uhci_register_event_callbacks(d->ctrl, &cbs, d) != ESP_OK) {
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

/*
 * Are a piece's bytes still the ones that arrived?  Its ring has been written
 * since by (a) the rest of its own mount, which goes round the ring and
 * reaches the piece again a whole ring later, or (b) the next mount of that
 * ring, which starts from the ring's first byte.  Either is known to the byte
 * except for the node the DMA is inside now, which is counted as written.
 */
static bool piece_ok(const dma_port_t *d, const rx_piece_t *pc)
{
    const uint8_t  r = pc->ring;
    const uint32_t g = d->rx_gen[r];
    const bool     busy = d->rx_active && d->rx_now == r;
    const uint32_t ext = d->rx_mv[r] + (busy ? AG_DMA_RX_NODE_MAX : 0u);
    if (g == pc->gen) {
        return ext < pc->off + AG_DMA_RX_RING;
    }
    if (g == pc->gen + 1u) {
        return ext <= (pc->off % AG_DMA_RX_RING);
    }
    return false;
}

/*
 * The task that keeps a receive mounted: once at the start, and again after
 * each idle end, on the other ring.  It does nothing else, so the only gap is
 * after the line has gone quiet.
 */
static void rx_task(void *arg)
{
    dma_port_t *d = (dma_port_t *)arg;
    uint8_t     r = 0;
    for (;;) {
        /*
         * WHAT COUNTS AS IDLE, set every time because a reconfiguration of
         * the port may put it back.  The chip's default is 256 bit times -
         * 16 us at 16 Mbaud - and a sender that stops to build its next block
         * pauses longer than that, so every such pause ended the receive.  An
         * end costs a re-mount, and bytes that arrive while the receive is
         * being mounted again are lost: the C3 sending 8 KB blocks, which it
         * builds in about a millisecond each, still loses one block in thirty
         * that way.  1023 bit times (64 us at 16 Mbaud) is the most the
         * register holds, so only a sender that has really stopped ends one.
         */
        uart_ll_set_rx_idle_thr(UART_LL_GET_HW(d->port), 1023);
        /* the new mount's bookkeeping first: its first node may complete
         * before uhci_receive has even returned */
        d->rx_mv[r] = 0;
        d->rx_gen[r]++;
        d->rx_now = r;
        d->rx_active = true;
        if (uhci_receive(d->ctrl, d->rx_ring[r], AG_DMA_RX_RING) != ESP_OK) {
            d->rx_active = false;
            vTaskDelay(1);
            continue;
        }
        (void)xSemaphoreTake(d->rx_rearm, portMAX_DELAY);
        r ^= 1u;
    }
}

typedef struct {
    rx_piece_t cur;
    uint32_t   at;
} rx_reader_t;

static rx_reader_t s_rd[AG_PORT_UART_MAX];

static ag_err_t rx_start(int port, dma_port_t *d)
{
    d->port = port;
    d->rx_q = xQueueCreate(AG_DMA_RX_EVQ, sizeof(rx_piece_t));
    d->rx_rearm = xSemaphoreCreateBinary();
    for (int k = 0; k < 2; k++) {
        d->rx_ring[k] = heap_caps_malloc(AG_DMA_RX_RING,
                                         MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    if (d->rx_q == NULL || d->rx_rearm == NULL || d->rx_ring[0] == NULL ||
        d->rx_ring[1] == NULL) {
        return -AG_ENOMEM;
    }
    /* two readers of one FIFO would each get half the bytes */
    uart_disable_rx_intr((uart_port_t)port);
    TaskHandle_t t = NULL;
    if (xTaskCreate(rx_task, "uartrx", 3072, d, configMAX_PRIORITIES - 3, &t) !=
        pdPASS) {
        return -AG_ENOMEM;
    }
    return AG_OK;
}

int32_t ag_port_uart_dma_read(int port, void *buf, size_t len,
                              uint32_t timeout_ms)
{
    if (buf == NULL || len == 0u || port < 0 || port >= AG_PORT_UART_MAX) {
        return -AG_EINVAL;
    }
    dma_port_t *d = dma_for(port);
    if (d == NULL) {
        return -AG_ENOTSUP;
    }
    if (d->rx_q == NULL) {
        const ag_err_t err = rx_start(port, d);
        if (err != AG_OK) {
            return err;
        }
    }
    rx_reader_t *r = &s_rd[port];
    size_t       got = 0;
    while (got < len) {
        if (r->at >= r->cur.n) {
            /* wait only for the first piece: after that, take what is there */
            const TickType_t wait = (got == 0u) ? pdMS_TO_TICKS(timeout_ms) : 0;
            if (xQueueReceive(d->rx_q, &r->cur, wait) != pdTRUE) {
                break;
            }
            r->at = 0;
        }
        size_t n = r->cur.n - r->at;
        if (n > len - got) {
            n = len - got;
        }
        memcpy((uint8_t *)buf + got, r->cur.p + r->at, n);
        /* still the bytes that arrived?  if not, drop the piece whole */
        if (!piece_ok(d, &r->cur)) {
            d->rx_overrun++;
            r->at = r->cur.n;
            continue;
        }
        r->at += (uint32_t)n;
        got += n;
    }
    return (int32_t)got;
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
