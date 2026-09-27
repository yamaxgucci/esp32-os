/*
 * ArgonOS - LINKSPI.SYS: the wire to another node of the same system, over a
 * four-wire SPI cable.
 *
 *   drv load c:\drv\linkspi.sys     (publishes /dev/node1 by default)
 *
 * THE KERNEL'S FRAMED LINK CARRIES IT, and this file is what is left above it
 * (24 Sep).  The first shape of this driver was NODELINK over raw SPI blocks:
 * nlink's frame assembler, its windows, its retransmission and a byte ring,
 * with the master clocking 512-byte blocks itself and the slave's kernel
 * moving them.  It worked out at some 20 KB/s where the same wires carry
 * 2.3 MB/s, and on the C3 it would not open at all - its raw receive ring
 * wanted 16 KB in one piece.  The kernel's framed link (io->spi_link_open2,
 * AG_SPILINK_FRAMED | AG_SPILINK_BLOCKS) now does everything that took:
 *
 *   - both ends clock and arm in kernel tasks, the master two frames queued;
 *   - every frame is checksummed, acknowledged, and sent again if lost - only
 *     the one lost, and only what is worth sending again;
 *   - the frame is the smaller of the two boards' [wire] frame;
 *   - a board that restarts starts a new session, and the other end begins
 *     the stream again with it instead of waiting for offsets it will never
 *     see (that is what nodelink's reset frame was for).
 *
 * So ONE MESSAGE IS ONE BLOCK: a six-byte head - class, channel, destination,
 * source - and the payload.  CONTROL and BULK go AG_SPILINK_RELIABLE; STREAM
 * does not, so a lost stream message is skipped and never delayed, which is
 * that class's whole contract.  There are no windows here, no timers, no
 * sequence numbers and no listener task: the kernel's link receives whether
 * anybody reads or not, and the kernel's link reader (linkprov) is the one
 * reader.  Back-pressure is the link's own: a sender waits for room, and a
 * receiver that does not read stops the other end's writer, not its data.
 *
 * WHAT IT GIVES UP.  One stream for all three classes: a control message
 * queued behind bulk waits for what is ahead of it in the kernel's transmit
 * ring - five frames, some 4 ms at 2.3 MB/s.  nlink's separate windows let
 * control overtake; at this rate there is little to overtake.
 *
 * WHICH END HOLDS THE CLOCK is still this wire's one asymmetry, and
 * `[linkspi] role` still says it: both ends run the same image, and the role
 * is only the flag the link is opened with.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_DRV("LINKSPI", "0.2", "argon");

/*
 * The largest message, and it is what the layer above sizes its reads by (a
 * reader's buffer is this plus 64, at most 4096 - see linkprov.c).  2040 is
 * what this wire said before, so nothing above sees a change; a block of it
 * is cut across two 2 KB frames, which costs nothing - the link cuts any
 * block anywhere.
 */
#define LSPI_MTU  2040u
#define LSPI_HEAD 6u

#define LSPI_DEV_DEFAULT "node1"
#define LSPI_BUS_DEFAULT 2
#define LSPI_CS_DEFAULT  10
#define LSPI_KHZ_DEFAULT 20000u

/*
 * How long a reliable send waits for room before it says -AG_EAGAIN, which the
 * layer above takes as back-pressure and tries again.  Long enough that a
 * burst waits for the wire rather than spinning on it, short enough that a
 * caller holding a lock is not held for long.
 */
#define LSPI_SEND_MS 20u

typedef struct {
    int      bus;
    int      cs;
    uint32_t khz;
    bool     master;
    bool     link_open;
    char     dev[AG_DEV_NAME_MAX];

    /* One writer and one reader a link: the kernel's rule for block mode.
     * The head goes beside the caller's bytes, not in front of a copy of
     * them (send2/recv2): 4 KB of buffers here once did nothing else. */
    ag_mutex_t txlock;
    ag_mutex_t rxlock;

    bool     heard;
    uint32_t sent, received, tx_bytes, rx_bytes;
    uint32_t stream_dropped; /* STREAM messages lost, told or not taken */
    uint32_t malformed;      /* blocks too short to hold a head         */
    uint32_t cut;            /* messages cut by the other end restarting */
} node_t;

static node_t *s_nd;

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
    if (!nd->heard && AG_HAS(IO, spi_link_stats)) {
        ag_spilink_stats_t st;
        memset(&st, 0, sizeof(st));
        if (IO->spi_link_stats(nd->bus, &st) == AG_OK && st.frame != 0u) {
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
    if (len > LSPI_MTU) {
        /* Refused rather than split: only the caller knows where its
         * message boundaries are. */
        return -AG_ERANGE;
    }
    const bool stream = (cls == AG_LINK_STREAM);

    uint8_t head[LSPI_HEAD];
    head[0] = cls;
    head[1] = chan;
    put16(head + 2, dst);
    put16(head + 4, src);
    TASK->mutex_lock(nd->txlock, UINT32_MAX);
    const int32_t w = IO->spi_link_send2(
        nd->bus, head, LSPI_HEAD, buf, len,
        stream ? 0u
               : (cls == AG_LINK_BACKGROUND)
                   ? (AG_SPILINK_RELIABLE | AG_SPILINK_WHOLE | AG_SPILINK_YIELD)
                   : AG_SPILINK_RELIABLE,
        stream ? 1u : LSPI_SEND_MS);
    int32_t r;
    if (w > 0) {
        nd->sent++;
        nd->tx_bytes += (uint32_t)len;
        r = (int32_t)len;
    } else if (stream && w == -AG_EAGAIN) {
        /* STREAM drops rather than blocks, here as on the wire: a caller
         * told to try again would send an audio frame late, which is the
         * thing this class exists to not do. */
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
    /* The deadline is read off the clock: a notice of lost blocks is taken
     * and the wait goes on for the rest of the time asked for. */
    const uint32_t deadline = now_ms() + timeout_ms;
    if (!TASK->mutex_lock(nd->rxlock, timeout_ms)) {
        return -AG_EAGAIN;
    }
    int32_t r = -AG_EAGAIN;
    for (;;) {
        const int32_t left = (int32_t)(deadline - now_ms());
        ag_spilink_block_t bi;
        memset(&bi, 0, sizeof(bi));
        uint8_t       head[LSPI_HEAD];
        const int32_t n = IO->spi_link_recv2(nd->bus, head, LSPI_HEAD, buf, max,
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
        if ((uint32_t)n < LSPI_HEAD || head[0] >= AG_LINK_CLASSES) {
            nd->malformed++;
            continue;
        }
        /* the bytes are in buf already, cut at max by the link */
        const uint32_t take = (uint32_t)n - LSPI_HEAD;
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
    if (AG_HAS(IO, spi_link_stats)) {
        ag_spilink_stats_t st;
        memset(&st, 0, sizeof(st));
        if (IO->spi_link_stats(nd->bus, &st) == AG_OK) {
            out->retransmits = st.rewinds;
            out->gaps = st.seq_gaps;
            out->bad += st.frames_bad;
            out->rto_ms = 50u; /* the link's fixed retransmit timer */
        }
    }
    return AG_OK;
}

/*
 * The payload rate: the clock's bytes, less what measured 24 Sep on the
 * Zero-C3 pair with 2 KB frames - 96% at 20 MHz, 93% at 40 - taken as 92%,
 * and the six-byte head and eight-byte block record of every message.
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
    out->mtu = LSPI_MTU;
    uint64_t bytes = (uint64_t)nd->khz * 1000u / 8u;
    bytes = bytes * 92u / 100u;
    bytes = bytes * LSPI_MTU / (LSPI_MTU + LSPI_HEAD + 8u);
    out->bytes_per_sec = (uint32_t)bytes;
    out->rtt_us = 0;
    ag_strlcpy(out->wire, "spi", sizeof(out->wire));
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

/*
 * The configuration, all of it from one `[linkspi]` section:
 *
 *   [linkspi]
 *   role = master   ; or slave - who holds the clock
 *   bus  = 2        ; the chip's own SPI number
 *   cs   = 10       ; the select: our output as master, the peer's as slave
 *   khz  = 20000    ; the clock: set on the bus as master; what info()
 *                   ;   computes the rate from on either end
 *   dev  = node1    ; the /dev name this wire publishes
 *
 * The frame is not here: it is the board's, `[wire] frame`, and the two ends
 * run at the smaller.  The bus's pins are BOARD.CFG's spi<bus>.sck/mosi/miso.
 */
static void cfg_str(const char *key, char *buf, size_t len, const char *dflt)
{
    ag_strlcpy(buf, dflt, len);
    if (ag_api()->cfg != NULL && AG_HAS(ag_api()->cfg, get_str)) {
        (void)ag_api()->cfg->get_str(key, buf, len);
    }
}

static int32_t cfg_int(const char *key, int32_t dflt)
{
    if (ag_api()->cfg != NULL && AG_HAS(ag_api()->cfg, get_int)) {
        return ag_api()->cfg->get_int(key, dflt);
    }
    return dflt;
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

static ag_err_t wire_up(void)
{
    node_t *nd = (node_t *)ag_malloc_caps(sizeof(node_t), AG_MEM_DEFAULT);
    if (nd == NULL) {
        nd = (node_t *)ag_malloc_caps(sizeof(node_t), AG_MEM_FAST);
    }
    if (nd == NULL) {
        return -AG_ENOMEM;
    }
    memset(nd, 0, sizeof(*nd));
    nd->bus = cfg_int("linkspi.bus", LSPI_BUS_DEFAULT);
    nd->cs = cfg_int("linkspi.cs", LSPI_CS_DEFAULT);
    nd->khz = (uint32_t)cfg_int("linkspi.khz", (int32_t)LSPI_KHZ_DEFAULT);
    if (nd->khz == 0u) {
        nd->khz = LSPI_KHZ_DEFAULT;
    }
    cfg_str("linkspi.dev", nd->dev, sizeof(nd->dev), LSPI_DEV_DEFAULT);

    /*
     * Which end holds the clock, refused rather than guessed: two masters on
     * a four-wire cable are two chips driving sck and mosi into each other.
     */
    char role[16];
    cfg_str("linkspi.role", role, sizeof(role), "master");
    if (ag_strcmp(role, "master") == 0) {
        nd->master = true;
    } else if (ag_strcmp(role, "slave") == 0) {
        nd->master = false;
    } else {
        ag_log(AG_LOG_ERROR, "linkspi",
               "role = \"%s\" is neither master nor slave: the clock has to be "
               "one end's and only one",
               role);
        ag_free(nd);
        return -AG_EINVAL;
    }

    nd->txlock = TASK->mutex_create();
    nd->rxlock = TASK->mutex_create();
    if (nd->txlock == NULL || nd->rxlock == NULL) {
        node_free(nd);
        return -AG_ENOMEM;
    }

    const uint32_t flags = AG_SPILINK_FRAMED | AG_SPILINK_BLOCKS |
                           (nd->master ? AG_SPILINK_MASTER : 0u);
    const ag_err_t se = IO->spi_link_open2(nd->bus, nd->cs, nd->khz, 0u, flags);
    if (se != AG_OK) {
        ag_log(AG_LOG_ERROR, "linkspi",
               "spi%d cs%d would not open as a %s link at %u kHz: %d", nd->bus,
               nd->cs, nd->master ? "master" : "slave", (unsigned)nd->khz,
               (int)se);
        node_free(nd);
        return se;
    }
    nd->link_open = true;

    const ag_dev_add_t desc = {
        .name = nd->dev,
        .driver = "LINKSPI",
        .cls = AG_DEV_LINK,
        .flags = 0,
        .ops = &k_dev_ops,        /* the registry requires one; empty = ENOTSUP */
        .class_ops = &k_link_ops, /* the vtable the kernel reads                */
        .priv = nd,
    };
    const ag_err_t err = ag_dev_add(&desc);
    if (err != AG_OK) {
        (void)IO->spi_link_close(nd->bus);
        node_free(nd);
        return err;
    }

    s_nd = nd;
    ag_log(AG_LOG_INFO, "linkspi",
           "/dev/%s on spi%d cs%d as %s at %u kHz - bind with `link use %s`",
           nd->dev, nd->bus, nd->cs, nd->master ? "master" : "slave",
           (unsigned)nd->khz, nd->dev);
    return AG_OK;
}

/* The link is closed on unload: its kernel tasks stop and the bus is freed. */
static void linkspi_unload(void)
{
    node_t *nd = s_nd;
    if (nd == NULL || !nd->link_open) {
        return;
    }
    const ag_err_t e = IO->spi_link_close(nd->bus);
    nd->link_open = false;
    if (e != AG_OK) {
        ag_log(AG_LOG_WARN, "linkspi", "spi%d did not close cleanly: %d",
               nd->bus, (int)e);
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
    /* The framed link in block mode is ABI 0.60; there is no fallback to the
     * raw path this driver used to be - on the C3 it did not even open. */
    if (!AG_HAS(IO, spi_link_open2) || !AG_HAS(IO, spi_link_send2) ||
        !AG_HAS(IO, spi_link_recv2) || !AG_HAS(IO, spi_link_close)) {
        ag_log(AG_LOG_ERROR, "linkspi",
               "this kernel has no framed SPI link in block mode (ABI 0.60)");
        return -AG_ENOSYS;
    }
    const ag_err_t e = wire_up();
    if (e != AG_OK) {
        ag_log(AG_LOG_ERROR, "linkspi", "not loaded: %d", (int)e);
        return e;
    }
    ag_module_on_unload(linkspi_unload);
    return AG_OK;
}
