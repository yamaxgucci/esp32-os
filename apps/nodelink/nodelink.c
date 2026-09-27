/*
 * ArgonOS - NODELINK.SYS: the wires to other nodes of the same system, over
 * UARTs.
 *
 *   drv load c:\drv\nodelink.sys    (publishes /dev/node0, node1... one a wire)
 *
 * THE KERNEL'S FRAMED LINK CARRIES IT (25 Sep), as it carries LINKSPI.  This
 * driver was nlink over a byte stream - its own frame assembler, windows per
 * class, sequence numbers, a retransmit timer and a listener task on every
 * wire - and the kernel's framed link over a UART (io->uart_link_open, ABI
 * 0.61) now does all of that for both wires alike: frames checksummed,
 * acknowledged, only what is lost sent again, blocks that say whether they
 * are worth sending again, a session that starts the stream over when the
 * other board restarts, the frame agreed as the smaller of the two boards'.
 *
 * So ONE MESSAGE IS ONE BLOCK: a six-byte head - class, channel, destination,
 * source - and the payload, exactly as LINKSPI.SYS sends it.  CONTROL and
 * BULK go AG_SPILINK_RELIABLE; STREAM does not, so a lost stream message is
 * skipped and never delayed.  The kernel's link reader (linkprov) is the one
 * reader of each wire; back-pressure is the link's own.
 *
 * WHAT A WIRE IS, all of it from its section:
 *
 *   [link]              -> /dev/node0; [link2] -> node1, up to [link4]
 *   uart  = 1           the port; its pins are BOARD.CFG's uart<N>
 *   baud  = 921600
 *   dma   = no          yes: by the UART's DMA (the S3, the C3, the C6) -
 *                       what 16 Mbaud needs; ring: the same, receiving into
 *                       a ring the DMA never stops going round (no bytes
 *                       lost to a receive mounted again after each pause);
 *                       no: through the port's driver, which every chip and
 *                       the emulator have
 *   frame = 1024        this wire's largest frame; [wire] frame if not said
 *
 * `[link]` with no number is the first, because every board that has one wire
 * already spells it that way.  The numbered ones exist on a board in the
 * middle of a star.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_DRV("NODELINK", "0.2", "argon");

#define ND_UART_DEFAULT 1 /* QEMU emulates UART0 (console) and UART1 only */
#define ND_BAUD         921600u
#define ND_WIRES_MAX    4u

/*
 * The largest message, and what the layer above sizes its reads by (a
 * reader's buffer is this plus 64, at most 4096 - see linkprov.c): the same
 * as LINKSPI's, so a message that crosses one wire crosses the next.  A block
 * this long is cut across frames where the frame is smaller, which costs
 * nothing - the link cuts any block anywhere.
 */
#define ND_MTU  2040u
#define ND_HEAD 6u

/* How long a reliable send waits for room before -AG_EAGAIN, which the layer
 * above takes as back-pressure and tries again. */
#define ND_SEND_MS 20u

typedef struct {
    uint32_t index;
    int      uart;
    uint32_t baud;
    bool     link_open;

    /* One writer and one reader a link: the kernel's rule for block mode.
     * The head goes beside the caller's bytes, not in front of a copy of
     * them (send2/recv2): 4 KB a wire of buffers here once did nothing else. */
    ag_mutex_t txlock;
    ag_mutex_t rxlock;

    bool     heard;
    uint32_t sent, received, tx_bytes, rx_bytes;
    uint32_t stream_dropped; /* STREAM messages lost, told or not taken */
    uint32_t malformed;      /* blocks too short to hold a head         */
    uint32_t cut;            /* messages cut by the other end restarting */
} node_t;

static node_t  *s_wire[ND_WIRES_MAX];
static uint32_t s_wires;

static const ag_io_api_t   *IO;
static const ag_task_api_t *TASK;

static uint32_t now_ms(void)
{
    return (uint32_t)(g_ag_api->time->us() / 1000u);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* ---------------------------------------------------------------------- */
/* The link vtable                                                          */
/* ---------------------------------------------------------------------- */

/* Nothing to bring up: the link is open from load, and a restart on either
 * end is the link's own business (a new session). */
static ag_err_t nd_start(ag_device_t *dev)
{
    return (ag_dev_priv(dev) != NULL) ? AG_OK : -AG_ENODEV;
}

/* Heard means both ends have agreed a frame: the other end is on the wire. */
static bool nd_up(ag_device_t *dev)
{
    node_t *nd = (node_t *)ag_dev_priv(dev);
    if (nd == NULL) {
        return false;
    }
    if (!nd->heard) {
        ag_spilink_stats_t st;
        memset(&st, 0, sizeof(st));
        if (IO->uart_link_stats(nd->uart, &st) == AG_OK && st.frame != 0u) {
            nd->heard = true;
        }
    }
    return nd->heard;
}

static int32_t send_common(ag_device_t *dev, uint8_t chan, uint8_t cls,
                           const void *buf, size_t len, uint16_t dst,
                           uint16_t src)
{
    node_t *nd = (node_t *)ag_dev_priv(dev);
    if (nd == NULL) {
        return -AG_ENODEV;
    }
    if ((buf == NULL && len > 0) || cls >= AG_LINK_CLASSES) {
        return -AG_EINVAL;
    }
    if (len > ND_MTU) {
        /* Refused rather than split: only the caller knows where its
         * message boundaries are. */
        return -AG_ERANGE;
    }
    const bool stream = (cls == AG_LINK_STREAM);

    uint8_t head[ND_HEAD];
    head[0] = cls;
    head[1] = chan;
    put16(head + 2, dst);
    put16(head + 4, src);
    /*
     * WHOLE, AND WAITED FOR WITHOUT THE LOCK.  The lock is held only while a
     * message is copied in, never while room is awaited, so a sender of
     * sound is not stood behind a picture that is waiting for the wire to
     * drain (see AG_SPILINK_WHOLE).  Bulk yields: it goes only when room for
     * somebody else's message is left behind it.  Stream is one attempt, as
     * it always was: late sound is worse than none.
     */
    const uint32_t how = stream ? AG_SPILINK_WHOLE
                         : (cls == AG_LINK_BULK || cls == AG_LINK_BACKGROUND)
                             ? (AG_SPILINK_RELIABLE | AG_SPILINK_WHOLE |
                                AG_SPILINK_YIELD)
                             : (AG_SPILINK_RELIABLE | AG_SPILINK_WHOLE);
    const uint32_t until = now_ms() + (stream ? 0u : ND_SEND_MS);
    int32_t        w;
    for (;;) {
        TASK->mutex_lock(nd->txlock, UINT32_MAX);
        w = IO->uart_link_send2(nd->uart, head, ND_HEAD, buf, len, how, 0u);
        TASK->mutex_unlock(nd->txlock);
        if (w != -AG_EAGAIN || stream || (int32_t)(now_ms() - until) >= 0) {
            break;
        }
        TASK->sleep_ms(1);
    }
    TASK->mutex_lock(nd->txlock, UINT32_MAX);
    int32_t r;
    if (w > 0) {
        nd->sent++;
        nd->tx_bytes += (uint32_t)len;
        r = (int32_t)len;
    } else if (stream && w == -AG_EAGAIN) {
        /* STREAM drops rather than blocks: a caller told to try again would
         * send an audio frame late, which this class exists not to do. */
        nd->stream_dropped++;
        r = (int32_t)len;
    } else if (w == -AG_EIO) {
        nd->cut++; /* the other end restarted under it */
        r = -AG_EIO;
    } else {
        r = w;
    }
    TASK->mutex_unlock(nd->txlock);
    return r;
}

static int32_t nd_send(ag_device_t *dev, uint8_t chan, uint8_t cls,
                       const void *buf, size_t len)
{
    return send_common(dev, chan, cls, buf, len, 0u, 0u);
}

static int32_t nd_send_a(ag_device_t *dev, uint8_t chan, const void *buf,
                         size_t len, const ag_link_addr_t *to)
{
    if (to == NULL) {
        return -AG_EINVAL;
    }
    return send_common(dev, chan, to->cls, buf, len, to->dst, to->src);
}

static int32_t recv_common(ag_device_t *dev, uint8_t *chan, void *buf,
                           size_t max, uint32_t timeout_ms,
                           ag_link_addr_t *from)
{
    node_t *nd = (node_t *)ag_dev_priv(dev);
    if (nd == NULL) {
        return -AG_ENODEV;
    }
    if (buf == NULL || chan == NULL) {
        return -AG_EINVAL;
    }
    const uint32_t deadline = now_ms() + timeout_ms;
    if (!TASK->mutex_lock(nd->rxlock, timeout_ms)) {
        return -AG_EAGAIN;
    }
    int32_t r = -AG_EAGAIN;
    for (;;) {
        const int32_t left = (int32_t)(deadline - now_ms());
        ag_spilink_block_t bi;
        memset(&bi, 0, sizeof(bi));
        uint8_t       head[ND_HEAD];
        const int32_t n = IO->uart_link_recv2(nd->uart, head, ND_HEAD, buf, max,
                                              &bi,
                                              (left > 0) ? (uint32_t)left : 0u);
        if (n == 0 && (bi.flags & AG_SPILINK_LOST) != 0u) {
            /* stream messages the wire lost, and the link says which */
            nd->stream_dropped += (uint32_t)(uint16_t)(bi.last - bi.no) + 1u;
            continue;
        }
        if (n == -AG_EIO) {
            nd->cut++;
            continue;
        }
        if (n < 0) {
            r = n;
            break;
        }
        if ((uint32_t)n < ND_HEAD || head[0] >= AG_LINK_CLASSES) {
            nd->malformed++;
            continue;
        }
        /* the bytes are in buf already, cut at max by the link */
        const uint32_t take = (uint32_t)n - ND_HEAD;
        *chan = head[1];
        if (from != NULL) {
            from->cls = head[0];
            from->dst = get16(head + 2);
            from->src = get16(head + 4);
        }
        nd->heard = true;
        nd->received++;
        nd->rx_bytes += take;
        r = (int32_t)take;
        break;
    }
    TASK->mutex_unlock(nd->rxlock);
    return r;
}

static int32_t nd_recv(ag_device_t *dev, uint8_t *chan, void *buf, size_t max,
                       uint32_t timeout_ms)
{
    return recv_common(dev, chan, buf, max, timeout_ms, NULL);
}

static int32_t nd_recv_a(ag_device_t *dev, uint8_t *chan, void *buf, size_t max,
                         uint32_t timeout_ms, ag_link_addr_t *from)
{
    return recv_common(dev, chan, buf, max, timeout_ms, from);
}

/* Nothing to pump: the kernel's link moves the wire on its own. */
static void nd_poll(ag_device_t *dev, uint32_t ms)
{
    (void)dev;
    if (ms > 0u) {
        TASK->sleep_ms(ms);
    }
}

static ag_err_t nd_stats(ag_device_t *dev, ag_link_stats_t *out)
{
    node_t *nd = (node_t *)ag_dev_priv(dev);
    if (nd == NULL) {
        return -AG_ENODEV;
    }
    if (out == NULL) {
        return -AG_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    out->sent = nd->sent;
    out->received = nd->received;
    out->tx_bytes = nd->tx_bytes;
    out->rx_bytes = nd->rx_bytes;
    out->rx_dropped = nd->stream_dropped;
    out->bad = nd->malformed;
    /* The wire's own counts are the kernel link's: frames sent again, holes
     * the other end asked about, frames that failed their checksum. */
    ag_spilink_stats_t st;
    memset(&st, 0, sizeof(st));
    if (IO->uart_link_stats(nd->uart, &st) == AG_OK) {
        out->retransmits = st.rewinds;
        out->gaps = st.seq_gaps;
        out->bad += st.frames_bad;
        out->rto_ms = 50u; /* the link's fixed retransmit timer */
    }
    return AG_OK;
}

/*
 * The payload rate: ten bits a byte on the line (a start and a stop bit),
 * less the six-byte head and eight-byte block record of every message, and
 * less a tenth for the link's own headers, acknowledgements and what it sends
 * again - measured 24-25 Sep at 16 Mbaud between the Zero and the C3, 90-99%
 * of the line depending on which way.
 */
static ag_err_t nd_info(ag_device_t *dev, ag_link_info_t *out)
{
    node_t *nd = (node_t *)ag_dev_priv(dev);
    if (nd == NULL) {
        return -AG_ENODEV;
    }
    if (out == NULL) {
        return -AG_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    out->mtu = ND_MTU;
    uint64_t bytes = (uint64_t)nd->baud / 10u;
    bytes = bytes * 90u / 100u;
    bytes = bytes * ND_MTU / (ND_MTU + ND_HEAD + 8u);
    out->bytes_per_sec = (uint32_t)bytes;
    out->rtt_us = 0;
    ag_strlcpy(out->wire, "uart", sizeof(out->wire));
    return AG_OK;
}

static const ag_dev_ops_t k_dev_ops = {0};

static const ag_link_ops_t k_link_ops = {
    .size = sizeof(ag_link_ops_t),
    .start = nd_start,
    .up = nd_up,
    .send = nd_send,
    .recv = nd_recv,
    .poll = nd_poll,
    .stats = nd_stats,
    .info = nd_info,
    .send_a = nd_send_a,
    .recv_a = nd_recv_a,
};

/* ---------------------------------------------------------------------- */
/* Lifecycle                                                               */
/* ---------------------------------------------------------------------- */

/* Spelled out rather than formatted: a .SYS has no stdio, the set is four,
 * and a table cannot get the order wrong the way a loop building strings can. */
static const char *const k_dev_name[ND_WIRES_MAX] = {"node0", "node1", "node2",
                                                     "node3"};
static const char *const k_uart_key[ND_WIRES_MAX] = {
    "link.uart", "link2.uart", "link3.uart", "link4.uart"};
static const char *const k_baud_key[ND_WIRES_MAX] = {
    "link.baud", "link2.baud", "link3.baud", "link4.baud"};
static const char *const k_dma_key[ND_WIRES_MAX] = {
    "link.dma", "link2.dma", "link3.dma", "link4.dma"};
static const char *const k_frame_key[ND_WIRES_MAX] = {
    "link.frame", "link2.frame", "link3.frame", "link4.frame"};

static int32_t cfg_int(const char *key, int32_t dflt)
{
    return AG_HAS(ag_api()->cfg, get_int) ? ag_api()->cfg->get_int(key, dflt)
                                          : dflt;
}

static void node_free(node_t *nd)
{
    if (nd->txlock != NULL) {
        TASK->mutex_delete(nd->txlock);
    }
    if (nd->rxlock != NULL) {
        TASK->mutex_delete(nd->rxlock);
    }
    ag_free(nd);
}

static ag_err_t wire_up(uint32_t i)
{
    const int uart = cfg_int(k_uart_key[i], (i == 0) ? ND_UART_DEFAULT : -1);
    if (uart < 0) {
        return -AG_ENOENT; /* nobody asked for this one */
    }
    const uint32_t baud = (uint32_t)cfg_int(k_baud_key[i], (int32_t)ND_BAUD);
    char how[8] = "no";
    if (AG_HAS(ag_api()->cfg, get_str)) {
        (void)ag_api()->cfg->get_str(k_dma_key[i], how, sizeof(how));
    }
    const bool ring = (ag_strcmp(how, "ring") == 0);
    const bool dma = ring || (ag_strcmp(how, "yes") == 0);
    const int32_t  frame = cfg_int(k_frame_key[i], 0);

    node_t *nd = (node_t *)ag_malloc_caps(sizeof(node_t), AG_MEM_DEFAULT);
    if (nd == NULL) {
        nd = (node_t *)ag_malloc_caps(sizeof(node_t), AG_MEM_FAST);
    }
    if (nd == NULL) {
        return -AG_ENOMEM;
    }
    memset(nd, 0, sizeof(*nd));
    nd->index = i;
    nd->uart = uart;
    nd->baud = baud;
    nd->txlock = TASK->mutex_create();
    nd->rxlock = TASK->mutex_create();
    if (nd->txlock == NULL || nd->rxlock == NULL) {
        node_free(nd);
        return -AG_ENOMEM;
    }

    const ag_err_t le = IO->uart_link_open(
        uart, baud, (frame > 0) ? (uint32_t)frame : 0u,
        AG_SPILINK_BLOCKS | (dma ? AG_UARTLINK_DMA : 0u) |
            (ring ? AG_UARTLINK_RING : 0u));
    if (le != AG_OK) {
        ag_log(AG_LOG_ERROR, "nodelink",
               "uart%d would not open as a link at %u baud%s: %d (its pins "
               "are uart%d.tx/rx in BOARD.CFG)",
               uart, (unsigned)baud, dma ? " by DMA" : "", (int)le, uart);
        node_free(nd);
        return le;
    }
    nd->link_open = true;

    const char *const name = k_dev_name[i];
    const ag_dev_add_t desc = {
        .name = name,
        .driver = "NODELINK",
        .cls = AG_DEV_LINK,
        .flags = 0,
        .ops = &k_dev_ops,        /* the registry requires one; empty = ENOTSUP */
        .class_ops = &k_link_ops, /* the vtable the kernel reads                */
        .priv = nd,
    };
    const ag_err_t err = ag_dev_add(&desc);
    if (err != AG_OK) {
        (void)IO->uart_link_close(uart);
        node_free(nd);
        return err;
    }

    s_wire[i] = nd;
    s_wires++;
    ag_log(AG_LOG_INFO, "nodelink",
           "/dev/%s on uart%d at %u baud%s - bind with `link use %s`", name,
           uart, (unsigned)baud, ring ? " by DMA, ring" : dma ? " by DMA" : "",
           name);
    return AG_OK;
}

/* Every link closed on unload: their kernel tasks stop, the ports are free. */
static void nodelink_unload(void)
{
    for (uint32_t i = 0; i < ND_WIRES_MAX; i++) {
        node_t *nd = s_wire[i];
        if (nd != NULL && nd->link_open) {
            (void)IO->uart_link_close(nd->uart);
            nd->link_open = false;
        }
    }
}

ag_err_t ag_driver_init(void)
{
    IO = ag_api()->io;
    TASK = ag_api()->task;
    if (IO == NULL || TASK == NULL || !AG_HAS(ag_api()->dev, add) ||
        !AG_HAS(TASK, mutex_create)) {
        return -AG_ENOSYS;
    }
    if (!AG_HAS(IO, uart_link_recv2)) {
        ag_log(AG_LOG_ERROR, "nodelink",
               "this kernel has no framed UART link in two parts (ABI 0.62)");
        return -AG_ENOSYS;
    }

    /*
     * `[link]`, then `[link2]`, `[link3]`, `[link4]` for as long as they are
     * there.  Stopping at the first absent one rather than checking all four:
     * a board with a gap in that numbering has a typo, and carrying on past it
     * would bind the wires it found and say nothing about the one it did not.
     */
    for (uint32_t i = 0; i < ND_WIRES_MAX; i++) {
        const ag_err_t e = wire_up(i);
        if (e == -AG_ENOENT) {
            break;
        }
        if (e != AG_OK) {
            ag_log(AG_LOG_ERROR, "nodelink", "%s: %d", k_uart_key[i], (int)e);
            break;
        }
    }
    if (s_wires == 0) {
        return -AG_ENODEV;
    }
    ag_module_on_unload(nodelink_unload);
    if (s_wires > 1) {
        ag_log(AG_LOG_INFO, "nodelink",
               "%u wires: this board is the middle of the cluster",
               (unsigned)s_wires);
    }
    return AG_OK;
}
