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
 * Not every chip has it in IDF: SOC_UHCI_SUPPORTED is absent on the original
 * ESP32 and on the S2.  On the ESP32 the block is there and this file drives
 * its receive by register (below); on the S2 the port answers -AG_ENOTSUP in
 * so many words.
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
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

/*
 * The receive that is never stopped (ag_port_uart_dma_listen_ring): where
 * this build reaches IDF's channel, and not on the S3.  There the channel's
 * interrupt will not be allocated a second time ("alloc interrupt failed"),
 * and its IRAM and DRAM are one SRAM - code it cannot use put the emulator's
 * build 184 bytes over (25 Sep 2026).
 */
#if AG_UHCI_PRIVATE && !CONFIG_IDF_TARGET_ESP32S3
#define AG_UART_DMA_RING 1
#include "esp_private/gdma_link.h" /* the never-stopped receive's own list */
#else
#define AG_UART_DMA_RING 0
#endif
#if AG_UHCI_PRIVATE
/* IDF's controller, private to its driver: the receive's state (rx_task)
 * and, for the ring, its channel */
#include "hal/uhci_hal.h"
#include "uhci_private.h"
#endif

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
    /* the wire between transfers (`link probe`, 1 Oct 2026): transfers
     * ended (counted in the interrupt) and when the last did; transfers
     * handed over, those handed to a DMA with nothing left, and how long
     * it had had nothing */
    volatile uint32_t        tx_ended;
    volatile int64_t         tx_end_us;
    uint32_t                 tx_subs, tx_idle_subs;
    int64_t                  tx_idle_us;
    uint32_t                 tx_last_quiet_us; /* before the latest transfer */
    /* each transfer's own time: when the one on the wire began, and those
     * slower than a byte a microsecond (16 Mbaud is 1.6) - count, worst */
    volatile int64_t         tx_cur_start;
    volatile uint32_t        tx_slow, tx_slow_max_us, tx_slow_max_len;
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
#if AG_UART_DMA_RING
    /* the ring that is never stopped (ag_port_uart_dma_listen_ring) */
    bool                     ring;
    uint32_t                 ring_nodes;
    uint32_t                 ring_node;
    volatile uint32_t        ring_done; /* descriptors closed so far */
    /* does the DMA give a descriptor's owner bit back: 0 not known yet,
     * 1 yes (ring_done takes every one it gave), 2 no (one a call) */
    uint8_t                  ring_owner;
    gdma_link_list_handle_t  ring_link; /* ours: IDF's has 3 to 5 nodes */
#endif
} dma_port_t;

/*
 * Made at the first question, in internal memory (its interrupts read it):
 * as a static it was DRAM the S3's emulator build has not got - 208 bytes
 * over once the probe's counters were in (1 Oct 2026).
 */
static dma_port_t *s_dma;

static bool dma_table(void)
{
    if (s_dma == NULL) {
        s_dma = heap_caps_calloc(AG_PORT_UART_MAX, sizeof(dma_port_t),
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return s_dma != NULL;
}

static bool IRAM_ATTR on_tx_done(uhci_controller_handle_t ctrl,
                                 const uhci_tx_done_event_data_t *edata,
                                 void *user_ctx)
{
    (void)ctrl;
    (void)edata;
    BaseType_t        woken = pdFALSE;
    dma_port_t *const d = (dma_port_t *)user_ctx;
    const int64_t     now = esp_timer_get_time();
    const uint32_t    took = (uint32_t)(now - d->tx_cur_start);
    if (edata != NULL && took > edata->sent_size) {
        d->tx_slow++;
        if (took > d->tx_slow_max_us) {
            d->tx_slow_max_us = took;
            d->tx_slow_max_len = (uint32_t)edata->sent_size;
        }
    }
    d->tx_cur_start = now; /* the next, if there is one, starts now */
    d->tx_end_us = now;
    d->tx_ended++;
    xSemaphoreGiveFromISR(d->done, &woken);
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
/*
 * The pieces waiting to be read: a ring's worth of whole descriptors, and 32
 * more for short ones.  A framed link's acknowledgements are 24 bytes each
 * with a pause either side, so every one is a piece of its own - some 1 800 a
 * second from the C3 - and with room for 14 the Zero threw hundreds away in
 * five seconds (reports of holes among them).  20 bytes a place.
 */
#define AG_DMA_RX_EVQ      (2u * (AG_DMA_RX_RING / 4095u + 1u) + 36u)

typedef struct {
    const uint8_t *p;
    uint32_t       n;
    uint32_t       off;  /* bytes from its mount's start to where it begins;
                          * in a ring: the number of its descriptor */
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
    if (port < 0 || port >= AG_PORT_UART_MAX || !dma_table()) {
        return NULL;
    }
    dma_port_t *d = &s_dma[port];
    if (d->ctrl != NULL) {
        return d;
    }
    if (d->tried) {
        return NULL; /* asked once, refused once; do not ask every frame */
    }
    /*
     * NOT A SECOND CONTROLLER WHERE THERE IS ONE.  The C3 and the S3 have a
     * single UHCI; IDF refuses a second ("no free controller") - after
     * touching the one that is working.  Asking was enough to do it: `link`
     * on the C3's driver-path wire to the CYD asked whether uart1 could DMA,
     * and from that moment the C3 heard nothing more on uart0 from the Zero
     * (26 Sep 2026: 0 retransmissions before, 351 twenty seconds after).
     */
#if defined(SOC_UHCI_NUM)
    uint32_t in_use = 0;
    for (int i = 0; i < AG_PORT_UART_MAX; i++) {
        in_use += (s_dma[i].ctrl != NULL) ? 1u : 0u;
    }
    if (in_use >= (uint32_t)SOC_UHCI_NUM) {
        return NULL; /* not "tried": this port may have it once that one is let go */
    }
#endif
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

bool ag_port_uart_dma_can_send(void)
{
    return true;
}

bool ag_port_uart_dma_can_receive(void)
{
    return AG_UART_DMA_RING != 0;
}

void ag_port_uart_dma_diag(int port, uint32_t out[8])
{
    (void)port;
    for (int i = 0; i < 8; i++) {
        out[i] = 0u;
    }
}

void ag_port_uart_dma_tx_gaps(int port, uint32_t *subs, uint32_t *idle,
                              uint32_t *idle_us)
{
    *subs = *idle = *idle_us = 0u;
    dma_port_t *const d =
        (port >= 0 && port < AG_PORT_UART_MAX && s_dma != NULL) ? &s_dma[port]
                                                           : NULL;
    if (d == NULL || d->ctrl == NULL) {
        return;
    }
    /* the gaps since the last look, then started again; the transfers
     * handed over since the start (the interrupt counts against them) */
    *subs = d->tx_subs;
    *idle = d->tx_idle_subs;
    *idle_us = (uint32_t)d->tx_idle_us;
    d->tx_idle_subs = 0u;
    d->tx_idle_us = 0;
}

uint32_t ag_port_uart_dma_last_quiet(int port)
{
    const dma_port_t *const d =
        (port >= 0 && port < AG_PORT_UART_MAX && s_dma != NULL) ? &s_dma[port]
                                                           : NULL;
    return (d != NULL && d->ctrl != NULL) ? d->tx_last_quiet_us : 0u;
}

void ag_port_uart_dma_tx_slow(int port, uint32_t *slow, uint32_t *max_us,
                              uint32_t *max_len)
{
    *slow = *max_us = *max_len = 0u;
    dma_port_t *const d =
        (port >= 0 && port < AG_PORT_UART_MAX && s_dma != NULL) ? &s_dma[port]
                                                           : NULL;
    if (d == NULL || d->ctrl == NULL) {
        return;
    }
    *slow = d->tx_slow;
    *max_us = d->tx_slow_max_us;
    *max_len = d->tx_slow_max_len;
    d->tx_slow = d->tx_slow_max_us = d->tx_slow_max_len = 0u;
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

    d->tx_last_quiet_us = 0u;
    if (d->tx_subs == d->tx_ended) {
        d->tx_cur_start = esp_timer_get_time(); /* it goes out at once */
    }
    if (d->tx_subs == d->tx_ended && d->tx_subs != 0u) {
        const int64_t q = esp_timer_get_time() - d->tx_end_us;
        d->tx_idle_subs++; /* everything handed over has ended: a gap */
        d->tx_idle_us += q;
        d->tx_last_quiet_us = (uint32_t)q;
    }
    const esp_err_t rc = uhci_transmit(d->ctrl, (uint8_t *)buf, len);
    if (rc != ESP_OK) {
        /* Alignment is the usual reason, and the caller can do something about
         * it - so say so rather than copying behind its back. */
        return (rc == ESP_ERR_INVALID_ARG) ? -AG_EINVAL : -AG_EIO;
    }
    d->out++;
    d->tx_subs++;
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

ag_err_t ag_port_uart_dma_settle(int port, uint32_t max_out)
{
    dma_port_t *d = dma_for(port);
    if (d == NULL) {
        return -AG_ENOTSUP;
    }
    while (d->out > max_out) {
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
#if AG_UART_DMA_RING
    if (d->ring) {
        /* its descriptor comes round again after `nodes` more: until then
         * nothing has written over it */
        return (uint32_t)(d->ring_done - pc->off) < d->ring_nodes;
    }
#endif
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
    uint32_t    refused = 0; /* mounts refused in a row */
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
#if AG_UHCI_PRIVATE
        /*
         * NOT BEFORE IDF IS DONE WITH THE LAST ONE.  Its handler tells us the
         * receive ended and only then stops the channel and marks the
         * controller ready; on the S3 this task wakes on the other core in
         * between, and the mount failed ("controller not in enable state",
         * every pause of the line) and waited a tick - 1.5 KB of 16 Mbaud
         * with nothing receiving it.  The handler is microseconds from done.
         */
        for (int k = 0; k < 200 && atomic_load(&d->ctrl->rx_dir.rx_fsm) !=
                                       UHCI_RX_FSM_ENABLE;
             k++) {
            esp_rom_delay_us(1);
        }
#endif
        if (uhci_receive(d->ctrl, d->rx_ring[r], AG_DMA_RX_RING) != ESP_OK) {
            d->rx_active = false;
            /*
             * A CONTROLLER THAT NEVER COMES BACK.  Once on the Zero, after
             * resets on both ends of a 16 Mbaud line, the receive state stayed
             * off "enable" for good: this loop asked a thousand times a second,
             * IDF said so in the log each time, and the console drowned in it -
             * a board that answered nothing, with nothing wrong but the one
             * receive (26 Sep 2026).  So: the complaint once, not per tick, and
             * after a fifth of a second of refusals the state is put back,
             * which is all that stands between the channel and the next mount.
             */
            refused++;
            if (refused == 10u) {
                esp_log_level_set("uhci", ESP_LOG_NONE);
            }
#if AG_UHCI_PRIVATE
            if (refused % 200u == 0u) {
                atomic_store(&d->ctrl->rx_dir.rx_fsm, UHCI_RX_FSM_ENABLE);
                ESP_EARLY_LOGW("uartdma", "uart%d: receive stuck for %u tries; "
                                          "state put back to enable",
                               d->port, (unsigned)refused);
            }
#endif
            vTaskDelay(1);
            continue;
        }
        if (refused >= 10u) {
            esp_log_level_set("uhci", ESP_LOG_WARN);
        }
        refused = 0;
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

#if AG_UART_DMA_RING

/* A closed descriptor's bytes to the reader. */
static void IRAM_ATTR ring_put(dma_port_t *d, uint32_t k, uint32_t n,
                               BaseType_t *woken)
{
    const rx_piece_t piece = {d->rx_ring[0] + (size_t)k * d->ring_node, n,
                              d->ring_done, 0, 0};
    d->ring_done++;
    if (n > 0u && xQueueSendFromISR(d->rx_q, &piece, woken) != pdTRUE) {
        rx_piece_t old;
        (void)xQueueReceiveFromISR(d->rx_q, &old, woken);
        (void)xQueueSendFromISR(d->rx_q, &piece, woken);
        d->rx_overrun++;
    }
}

/*
 * Descriptors of the ring closed - full, or cut short by an idle line - and
 * the DMA already in the next.  Nothing is stopped or mounted here.
 *
 * EVERY ONE THE DMA HAS GIVEN BACK, NOT ONE A CALL (29 Sep 2026).  This took
 * a call for one descriptor, and the channel's interrupt is a status bit:
 * two descriptors closed before the handler ran are one call.  One of them
 * was never handed to the reader, and nothing counted it - the C3 lost 4.6%
 * of the bytes the Zero sent, 6 735 frames short of bytes and none wrong,
 * with "fell behind" at 0.  Short descriptors make it likely: every ack and
 * every marker of the picture is a burst an idle line ends, a few tens of
 * microseconds apart.  And the length of one ended by an idle line was
 * counted onwards to the next descriptor marked EOF, which after a missed
 * call is a stale one from the lap before.
 *
 * So each descriptor is taken by its owner bit, which the DMA gives to the
 * CPU when it has written it: every one that is the CPU's, in turn, with the
 * length the DMA wrote there, and handed back.  The DMA does not check the
 * bit on this ring (it goes round regardless), so handing back costs it
 * nothing.  If this chip's DMA did not give the bit back, the first call
 * would find its own descriptor still the DMA's: then it is one a call, as
 * before.
 */
static bool IRAM_ATTR ring_done(gdma_channel_handle_t chan,
                                gdma_event_data_t *ev, void *user)
{
    (void)chan;
    dma_port_t *d = (dma_port_t *)user;
    BaseType_t  woken = pdFALSE;
    if (d->ring_owner != 2u) {
        uint32_t taken = 0;
        for (; taken < d->ring_nodes; taken++) {
            const uint32_t   k = d->ring_done % d->ring_nodes;
            gdma_lli_owner_t own = GDMA_LLI_OWNER_DMA;
            (void)gdma_link_get_owner(d->ring_link, (int)k, &own);
            if (own != GDMA_LLI_OWNER_CPU) {
                break;
            }
            const uint32_t n =
                (uint32_t)gdma_link_get_length(d->ring_link, (int)k);
            (void)gdma_link_set_owner(d->ring_link, (int)k,
                                      GDMA_LLI_OWNER_DMA);
            ring_put(d, k, n, &woken);
        }
        if (taken != 0u) {
            d->ring_owner = 1u; /* the DMA gives the bit back */
            return woken == pdTRUE;
        }
        if (d->ring_owner == 1u) {
            return woken == pdTRUE; /* taken by the call before */
        }
        d->ring_owner = 2u; /* never given back: one a call */
    }
    const uint32_t k = d->ring_done % d->ring_nodes;
    const uint32_t n = ev->flags.normal_eof
                           ? (uint32_t)gdma_link_count_buffer_size_till_eof(
                                 d->ring_link, k)
                           : d->ring_node;
    ring_put(d, k, n, &woken);
    return woken == pdTRUE;
}

ag_err_t ag_port_uart_dma_listen_ring(int port, uint32_t bytes)
{
    if (port < 0 || port >= AG_PORT_UART_MAX) {
        return -AG_EINVAL;
    }
    dma_port_t *d = dma_for(port);
    if (d == NULL) {
        return -AG_ENOTSUP;
    }
    if (d->rx_q != NULL) {
        return d->ring ? AG_OK : -AG_EBUSY; /* the other kind is running */
    }
    uhci_controller_t *const c = d->ctrl;
    /*
     * One ring of 1 KB descriptors - a list of our own, since IDF's has only
     * as many nodes as its one receive needs (3 on the C3, each near the
     * 4095-byte limit).  1 KB is 640 us at 16 Mbaud, and a reader has all but
     * one of them before one is written over.  As big as the caller says: a
     * framed link needs only what its peer may have in flight, where both of
     * the other receive's rings (16 KB on the C3) are for a reader nothing
     * holds back.
     */
    const uint32_t node = 1024u;
    uint32_t       nodes = (bytes != 0u ? bytes : 2u * AG_DMA_RX_RING) / node;
    if (nodes < 4u) {
        nodes = 4u;
    }
    const gdma_link_list_config_t lc = {.num_items = nodes,
                                        .item_alignment = 4};
    if (gdma_new_link_list(&lc, &d->ring_link) != ESP_OK) {
        return -AG_ENOMEM;
    }
    d->port = port;
    d->rx_q = xQueueCreate(AG_DMA_RX_EVQ, sizeof(rx_piece_t));
    d->rx_ring[0] = heap_caps_malloc((size_t)node * nodes,
                                     MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (d->rx_q == NULL || d->rx_ring[0] == NULL) {
        return -AG_ENOMEM;
    }
    d->ring = true;
    d->ring_nodes = nodes;
    d->ring_node = node;
    d->ring_done = 0;
    d->ring_owner = 0;
    gdma_buffer_mount_config_t mc[nodes];
    memset(mc, 0, sizeof(mc));
    for (uint32_t k = 0; k < nodes; k++) {
        mc[k].buffer = d->rx_ring[0] + (size_t)k * node;
        mc[k].buffer_alignment = 4;
        mc[k].length = node;
        mc[k].flags.mark_final = GDMA_FINAL_LINK_TO_DEFAULT; /* round */
    }
    const gdma_rx_event_callbacks_t cb = {.on_recv_done = ring_done};
    esp_err_t e = gdma_register_rx_event_callbacks(
        c->rx_dir.dma_chan, (gdma_rx_event_callbacks_t *)&cb, d);
    if (e == ESP_OK) {
        e = gdma_link_mount_buffers(d->ring_link, 0, mc, nodes, NULL);
        if (e != ESP_OK) {
            ESP_LOGW("uartdma", "uart%d ring: mount: %s", port,
                     esp_err_to_name(e));
        }
    } else {
        ESP_LOGW("uartdma", "uart%d ring: callbacks: %s", port,
                 esp_err_to_name(e));
    }
    if (e != ESP_OK) {
        return -AG_EIO;
    }
    /* two readers of one FIFO would each get half the bytes */
    uart_disable_rx_intr((uart_port_t)port);
    gdma_reset(c->rx_dir.dma_chan);
    gdma_start(c->rx_dir.dma_chan,
               gdma_link_get_head_addr(d->ring_link));
    return AG_OK;
}
#else
ag_err_t ag_port_uart_dma_listen_ring(int port, uint32_t bytes)
{
    (void)port;
    (void)bytes;
    return -AG_ENOTSUP;
}
#endif

uint32_t ag_port_uart_dma_overruns(int port)
{
    dma_port_t *d = (port >= 0 && port < AG_PORT_UART_MAX) ? dma_for(port)
                                                           : NULL;
    return (d != NULL) ? d->rx_overrun : 0u;
}

ag_err_t ag_port_uart_dma_listen(int port)
{
    if (port < 0 || port >= AG_PORT_UART_MAX) {
        return -AG_EINVAL;
    }
    dma_port_t *d = dma_for(port);
    if (d == NULL) {
        return -AG_ENOTSUP;
    }
    return (d->rx_q == NULL) ? rx_start(port, d) : AG_OK;
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

#elif CONFIG_IDF_TARGET_ESP32

/*
 * THE ORIGINAL ESP32: A RECEIVE BY ITS OWN UHCI, BY REGISTER (29 Sep 2026).
 *
 * IDF has no driver for it here - SOC_UHCI_SUPPORTED is absent - but the chip
 * has two UHCI blocks with a DMA of their own (not a GDMA: the older engine,
 * lldesc_t descriptors), and the registers are in soc/uhci_struct.h.  The CYD
 * took its wire through the driver's interrupt, which at 8 Mbaud came every
 * 120 bytes: some 300 KB/s was all it could receive, 62% of a core, and a
 * picture that changes everywhere is 150 KB a frame - 1.2 frames a second.
 *
 * The same shape as the C3's never-stopped receive (above): one ring of 1 KB
 * descriptors, going round for as long as the port is open; an idle line ends
 * the descriptor it is in and the DMA goes on into the next; the handler hands
 * the reader every descriptor the DMA has given back to the CPU by its owner
 * bit, with the length the DMA wrote, and gives it back.  The reader checks
 * after copying that the DMA has not been round to that descriptor since.
 *
 * Only the receive.  What goes out still goes through the driver, which does
 * it well enough: ag_port_uart_dma_ready says no, and the link sends by the
 * driver (uartlink.c).  One port: UHCI0 serves it.
 */
#include "driver/uart.h"
#include "hal/uart_ll.h"
#include "hal/uhci_ll.h"
#include "soc/uhci_struct.h"
#include "soc/interrupts.h"
#include "esp_rom_lldesc.h"
#include "esp_intr_alloc.h"
#include "esp_private/periph_ctrl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>

#define E32_NODE 1024u
#define E32_EVQ  48u

typedef struct {
    const uint8_t *p;
    uint32_t       n;
    uint32_t       no; /* which descriptor it was, counted from the start */
} e32_piece_t;

static struct {
    int                port;       /* -1: none yet */
    lldesc_t          *d;
    uint8_t           *buf;
    uint32_t           nodes;
    volatile uint32_t  done;       /* descriptors given to the reader */
    volatile uint32_t  overrun;
    QueueHandle_t      q;
    intr_handle_t      ih;
    e32_piece_t        cur;
    uint32_t           at;
    volatile uint32_t  pos;        /* where the DMA is: see e32_isr */
    uint32_t           last_cur;   /* the register's descriptor, last read */
    volatile uint32_t  calls, empty_calls, passed, ended;
    /* what a closed descriptor held when taken: passed by with a length
     * written / with none; ended with the owner given / with only its end */
    volatile uint32_t  passed_len, passed_nolen, ended_owner, ended_eof;
} s_e32 = {.port = -1};

/* The descriptor the DMA is in, by its buffer; UINT32_MAX if that does not
 * read as one of ours. */
static uint32_t IRAM_ATTR e32_current(void)
{
    const uint32_t a = UHCI0.dma_in_dscr_bf1; /* its second word: the buffer */
    const uint32_t base = (uint32_t)(uintptr_t)s_e32.buf;
    if (a < base || a >= base + s_e32.nodes * E32_NODE) {
        return UINT32_MAX;
    }
    return (a - base) / E32_NODE;
}

/*
 * WHERE THE DMA IS, KEPT HERE (29-30 Sep 2026).  What the DMA writes into a
 * descriptor it has closed, counted on the CYD: one an idle line ended comes
 * back with its owner bit given and its length (19 534 of 19 534); one it
 * filled whole and went on from comes back with nothing at all - no bit, no
 * length (12 of 12).  Taking them by the bit alone stopped at the first full
 * one for good.  Taking them up to the descriptor its register says it is in
 * went wrong both ways: after an idle end the register still names the one
 * just handed over, which read as a lap ahead and sent the reader the whole
 * ring of old bytes, and the register is not always up to date, which sent
 * each burst a burst late.
 *
 * So the handler keeps where the DMA is itself, `pos`, a count that only
 * goes forward: by how far the register has moved since it last looked -
 * never a lap: a lap between two interrupts is lost bytes anyway - and past
 * any descriptor ahead of it the DMA has given back.  Everything before `pos`
 * is closed, with its length if given and whole if not; the one at `pos`
 * only once given; nothing after.
 */
static void IRAM_ATTR e32_isr(void *arg)
{
    (void)arg;
    const uint32_t st = UHCI0.int_st.val;
    UHCI0.int_clr.val = st;
    BaseType_t     woken = pdFALSE;
    uint32_t       taken = 0;
    const uint32_t n = s_e32.nodes;
    const uint32_t cur = e32_current();
    s_e32.calls++;
    if (cur != UINT32_MAX) {
        s_e32.pos += (cur + n - s_e32.last_cur) % n;
        s_e32.last_cur = cur;
    }
    /* a descriptor given back further on: the DMA has been there */
    for (uint32_t i = s_e32.pos + 1u; i < s_e32.done + n; i++) {
        if (s_e32.d[i % n].owner == 0u) {
            s_e32.pos = i;
        }
    }
    while (s_e32.done < s_e32.pos ||
           (s_e32.done == s_e32.pos && s_e32.d[s_e32.done % n].owner == 0u)) {
        lldesc_t *const d = &s_e32.d[s_e32.done % n];
        uint32_t        len = d->length;
        if (d->owner != 0u) {
            if (len != 0u) {
                s_e32.passed_len++;
            } else {
                s_e32.passed_nolen++;
            }
            len = E32_NODE; /* passed by, filled whole */
            s_e32.passed++;
        } else {
            if (d->eof != 0u) {
                s_e32.ended_eof++;
            }
            s_e32.ended_owner++;
            s_e32.ended++;
        }
        const e32_piece_t pc = {(const uint8_t *)d->buf, len, s_e32.done};
        d->length = 0;
        d->eof = 0;
        d->owner = 1;
        s_e32.done++;
        taken++;
        if (pc.n > 0u && xQueueSendFromISR(s_e32.q, &pc, &woken) != pdTRUE) {
            e32_piece_t old;
            (void)xQueueReceiveFromISR(s_e32.q, &old, &woken);
            (void)xQueueSendFromISR(s_e32.q, &pc, &woken);
            s_e32.overrun++;
        }
    }
    if (taken == 0u) {
        s_e32.empty_calls++;
    }
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

void ag_port_uart_dma_tx_gaps(int port, uint32_t *subs, uint32_t *idle,
                              uint32_t *idle_us)
{
    (void)port;
    *subs = *idle = *idle_us = 0u;
}

uint32_t ag_port_uart_dma_last_quiet(int port)
{
    (void)port;
    return 0u;
}

void ag_port_uart_dma_tx_slow(int port, uint32_t *slow, uint32_t *max_us,
                              uint32_t *max_len)
{
    (void)port;
    *slow = *max_us = *max_len = 0u;
}

void ag_port_uart_dma_diag(int port, uint32_t out[8])
{
    const bool mine = (port == s_e32.port);
    out[0] = mine ? s_e32.calls : 0u;
    out[1] = mine ? s_e32.empty_calls : 0u;
    out[2] = mine ? s_e32.passed : 0u;
    out[3] = mine ? s_e32.ended : 0u;
    out[4] = mine ? s_e32.passed_len : 0u;
    out[5] = mine ? s_e32.passed_nolen : 0u;
    out[6] = mine ? s_e32.ended_owner : 0u;
    out[7] = mine ? s_e32.ended_eof : 0u;
}

bool ag_port_uart_dma_ready(int port)
{
    (void)port;
    return false; /* nothing goes out by DMA here: the driver sends */
}

bool ag_port_uart_dma_can_send(void)
{
    return false;
}

bool ag_port_uart_dma_can_receive(void)
{
    return true; /* the ring of descriptors the DMA goes round */
}

ag_err_t ag_port_uart_dma_listen(int port)
{
    (void)port;
    return -AG_ENOTSUP;
}

ag_err_t ag_port_uart_dma_listen_ring(int port, uint32_t bytes)
{
    if (port < 0 || port > 2) {
        return -AG_EINVAL;
    }
    if (s_e32.port >= 0) {
        return (s_e32.port == port) ? AG_OK : -AG_EBUSY;
    }
    /* at least 8 KB, 10 ms of 8 Mbaud: 4 KB was 5 ms, and a reader on a
     * board drawing a full-screen picture is sometimes later than that */
    uint32_t nodes = (bytes != 0u ? bytes : 8192u) / E32_NODE;
    if (nodes < 8u) {
        nodes = 8u;
    }
    s_e32.d = heap_caps_calloc(nodes, sizeof(lldesc_t),
                               MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_e32.buf = heap_caps_malloc((size_t)nodes * E32_NODE,
                                 MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_e32.q = xQueueCreate(E32_EVQ, sizeof(e32_piece_t));
    if (s_e32.d == NULL || s_e32.buf == NULL || s_e32.q == NULL) {
        heap_caps_free(s_e32.d);
        heap_caps_free(s_e32.buf);
        if (s_e32.q != NULL) {
            vQueueDelete(s_e32.q);
        }
        s_e32.d = NULL;
        s_e32.buf = NULL;
        s_e32.q = NULL;
        return -AG_ENOMEM;
    }
    s_e32.nodes = nodes;
    s_e32.done = 0;
    s_e32.pos = 0;
    s_e32.last_cur = 0;
    for (uint32_t k = 0; k < nodes; k++) {
        lldesc_t *const d = &s_e32.d[k];
        d->size = E32_NODE;
        d->length = 0;
        d->owner = 1;
        d->eof = 0;
        d->buf = s_e32.buf + (size_t)k * E32_NODE;
        d->qe.stqe_next = &s_e32.d[(k + 1u) % nodes]; /* round */
    }

    PERIPH_RCC_ATOMIC() {
        uhci_ll_enable_bus_clock(0, true);
        uhci_ll_reset_register(0);
    }
    uhci_dev_t *const h = &UHCI0;
    h->int_ena.val = 0;
    h->int_clr.val = UINT32_MAX;
    h->conf0.val = 0;
    h->conf0.in_rst = 1;
    h->conf0.in_rst = 0;
    h->conf0.ahbm_fifo_rst = 1;
    h->conf0.ahbm_fifo_rst = 0;
    h->conf0.ahbm_rst = 1;
    h->conf0.ahbm_rst = 0;
    /* the bytes as they came: no SLIP separators, headers, CRCs or escapes,
     * which are the block's reason for being and would each change a byte */
    h->conf1.val = 0;
    h->conf1.crc_disable = 1;
    h->escape_conf.val = 0;
    h->hung_conf.val = 0;
    h->conf0.clk_en = 1;
    h->conf0.uart_idle_eof_en = 1; /* an idle line ends a descriptor */
    h->conf0.indscr_burst_en = 1;
    h->conf0.uart0_ce = (port == 0);
    h->conf0.uart1_ce = (port == 1);
    h->conf0.uart2_ce = (port == 2);

    /* the longest the line may pause inside what one frame is: 1023 bit
     * times, 128 us at 8 Mbaud, the most the register holds */
    uart_ll_set_rx_idle_thr(UART_LL_GET_HW(port), 1023);
    /* two readers of one FIFO would each get half the bytes */
    uart_disable_rx_intr((uart_port_t)port);

    if (esp_intr_alloc(ETS_UHCI0_INTR_SOURCE, 0, e32_isr, NULL, &s_e32.ih) !=
        ESP_OK) {
        return -AG_EIO;
    }
    h->int_ena.in_done = 1;
    h->int_ena.in_suc_eof = 1;
    h->dma_in_link.addr = (uint32_t)(uintptr_t)&s_e32.d[0] & 0xFFFFFu;
    h->dma_in_link.start = 1;
    s_e32.port = port;
    ESP_LOGI("uartdma", "uart%d: receiving by UHCI0, %u descriptors of %u",
             port, (unsigned)nodes, (unsigned)E32_NODE);
    return AG_OK;
}

uint32_t ag_port_uart_dma_overruns(int port)
{
    return (port == s_e32.port) ? s_e32.overrun : 0u;
}

int32_t ag_port_uart_dma_read(int port, void *buf, size_t len,
                              uint32_t timeout_ms)
{
    if (buf == NULL || len == 0u) {
        return -AG_EINVAL;
    }
    if (port != s_e32.port) {
        return -AG_ENOTSUP;
    }
    size_t got = 0;
    while (got < len) {
        if (s_e32.at >= s_e32.cur.n) {
            const TickType_t wait = (got == 0u) ? pdMS_TO_TICKS(timeout_ms) : 0;
            if (xQueueReceive(s_e32.q, &s_e32.cur, wait) != pdTRUE) {
                break;
            }
            s_e32.at = 0;
        }
        size_t n = s_e32.cur.n - s_e32.at;
        if (n > len - got) {
            n = len - got;
        }
        memcpy((uint8_t *)buf + got, s_e32.cur.p + s_e32.at, n);
        /*
         * ITS DESCRIPTOR COMES ROUND AGAIN AFTER `nodes` MORE - counted from
         * where the DMA is (e32_isr's `pos`), not from what the handler has
         * taken (30 Sep 2026): a DMA a lap ahead of a late handler writes
         * over a piece while it is being copied, and counted from the
         * handler's count the piece went on as if whole.
         */
        const uint32_t pos = s_e32.pos;
        if ((uint32_t)(pos - s_e32.cur.no) >= s_e32.nodes) {
            s_e32.overrun++;
            s_e32.at = s_e32.cur.n;
            continue;
        }
        s_e32.at += (uint32_t)n;
        got += n;
    }
    return (int32_t)got;
}

/* Nothing goes out by DMA here (ag_port_uart_dma_ready): the link sends
 * through the driver, and these are never called. */
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

ag_err_t ag_port_uart_dma_settle(int port, uint32_t max_out)
{
    (void)port;
    (void)max_out;
    return -AG_ENOTSUP;
}

#else /* !SOC_UHCI_SUPPORTED */

bool ag_port_uart_dma_ready(int port)
{
    (void)port;
    return false;
}

void ag_port_uart_dma_tx_gaps(int port, uint32_t *subs, uint32_t *idle,
                              uint32_t *idle_us)
{
    (void)port;
    *subs = *idle = *idle_us = 0u;
}

uint32_t ag_port_uart_dma_last_quiet(int port)
{
    (void)port;
    return 0u;
}

void ag_port_uart_dma_tx_slow(int port, uint32_t *slow, uint32_t *max_us,
                              uint32_t *max_len)
{
    (void)port;
    *slow = *max_us = *max_len = 0u;
}

void ag_port_uart_dma_diag(int port, uint32_t out[8])
{
    (void)port;
    for (int i = 0; i < 8; i++) {
        out[i] = 0u;
    }
}

bool ag_port_uart_dma_can_send(void)
{
    return false;
}

bool ag_port_uart_dma_can_receive(void)
{
    return false;
}

ag_err_t ag_port_uart_dma_listen(int port)
{
    (void)port;
    return -AG_ENOTSUP;
}

ag_err_t ag_port_uart_dma_listen_ring(int port, uint32_t bytes)
{
    (void)port;
    (void)bytes;
    return -AG_ENOTSUP;
}

uint32_t ag_port_uart_dma_overruns(int port)
{
    (void)port;
    return 0u;
}

int32_t ag_port_uart_dma_read(int port, void *buf, size_t len,
                              uint32_t timeout_ms)
{
    (void)port;
    (void)buf;
    (void)len;
    (void)timeout_ms;
    return -AG_ENOTSUP;
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

ag_err_t ag_port_uart_dma_settle(int port, uint32_t max_out)
{
    (void)port;
    (void)max_out;
    return -AG_ENOTSUP;
}

#endif /* SOC_UHCI_SUPPORTED */
