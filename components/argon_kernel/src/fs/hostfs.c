/*
 * ArgonOS - HostFS VFS backend (UART1 RPC to host helper).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/hostfs.h>

#include <stdlib.h>
#include <string.h>

#include <argon/hsfs_proto.h>
#include <argon/input.h>
#include <argon/log.h>
#include <argon/proc.h>
#include <argon/vfs.h>

#include <argon/port/mem.h>
#include <argon/port/sync.h>
#include <argon/port/task.h>
#include <argon/port/time.h>
#include <argon/port/uart.h>

#define HSFS_UART           1
#define HSFS_UART_BAUD      115200
#define HSFS_RX_BUF         (64 * 1024)
#define HSFS_TX_BUF         (16 * 1024)
/* Large WRITE + PADPUSH needs headroom: one 4 KiB RPC at 115200 is ~0.4 s. */
#define HSFS_RPC_TIMEOUT_MS 30000
#define HSFS_PING_TIMEOUT_MS 250
/* On the boot path only: a helper that is already listening answers in single
 * digit milliseconds over a local socket, and a board with no helper must not
 * pay more than this.  Slow starts are picked up by the retry task instead. */
#define HSFS_BOOT_PING_MS    100
#define HSFS_HELPER_WAIT_MS  3000
/* Cap each uart_write so pump_rx_during_tx can drain PADPUSH between slices. */
#define HSFS_TX_SLICE       128

typedef struct {
    uint32_t host_h;
    uint64_t pos;
    uint64_t size;
    bool     is_pad;  /* H:\sms.pad — served from push cache, no READ RPC */
    bool     writable;
} hsfs_file_t;

typedef struct {
    uint32_t host_h;
} hsfs_dir_t;

static ag_port_mutex_t s_rpc_mu;
static ag_port_task_t      s_rpc_holder;
static bool              s_uart_up;
static bool              s_mounted;
static uint16_t          s_seq;

static uint8_t  s_pad[AG_PAD_BYTES];
static uint32_t s_pad_ms; /* uptime ms of the last PADPUSH; 0 = never */
static ag_port_task_t s_pad_task;
static ag_port_task_t s_wait_task; /* off-boot retry while the helper appears */

/* One-header lookaside: pad_drain must not eat a real RPC response. */
static hsfs_hdr_t s_held_hdr;
static bool       s_held_hdr_valid;

static bool read_all(void *buf, size_t len, uint32_t timeout_ms);
static bool read_hdr(hsfs_hdr_t *hdr, uint32_t timeout_ms);
static bool drain_bytes(uint32_t len, uint32_t timeout_ms);

static uint32_t now_ms(void)
{
    return (uint32_t)(ag_port_us() / 1000);
}

static bool is_pad_rel(const char *rel)
{
    if (rel == NULL) {
        return false;
    }
    /* "/sms.pad", "sms.pad", "\\sms.pad" */
    while (*rel == '/' || *rel == '\\') {
        rel++;
    }
    return (rel[0] == 's' || rel[0] == 'S') &&
           (rel[1] == 'm' || rel[1] == 'M') &&
           (rel[2] == 's' || rel[2] == 'S') &&
           rel[3] == '.' &&
           (rel[4] == 'p' || rel[4] == 'P') &&
           (rel[5] == 'a' || rel[5] == 'A') &&
           (rel[6] == 'd' || rel[6] == 'D') &&
           rel[7] == '\0';
}

static void pad_store(const uint8_t *blob, uint32_t len)
{
    if (blob == NULL || len < 2u) {
        return;
    }
    memset(s_pad, 0, sizeof(s_pad));
    const uint32_t n = (len > AG_PAD_BYTES) ? AG_PAD_BYTES : len;
    memcpy(s_pad, blob, n);
    s_pad_ms = now_ms();
    /* Input layer is what apps and /dev/joy0 read; we stay a source. */
    ag_input_push_pad(s_pad, AG_PAD_BYTES);
}

static bool ingest_padpush_locked(const hsfs_hdr_t *hdr, uint32_t timeout_ms)
{
    if (hdr == NULL || hdr->op != HSFS_OP_PADPUSH) {
        return false;
    }
    if (hdr->path_len != 0 || hdr->data_len > HSFS_MAX_DATA) {
        return false;
    }
    uint8_t  sink[64];
    uint8_t  padbuf[8];
    uint32_t left = hdr->data_len;
    uint32_t got = 0;
    while (left > 0) {
        const uint32_t chunk = left > sizeof(sink) ? (uint32_t)sizeof(sink) : left;
        if (!read_all(sink, chunk, timeout_ms)) {
            return false;
        }
        if (got < sizeof(padbuf)) {
            const uint32_t copy =
                (chunk < (sizeof(padbuf) - got)) ? chunk : (sizeof(padbuf) - got);
            memcpy(padbuf + got, sink, copy);
            got += copy;
        }
        left -= chunk;
    }
    pad_store(padbuf, got);
    return true;
}

static void *hsfs_alloc(size_t n)
{
    void *p = ag_port_alloc(n, AG_MEM_FAST | AG_MEM_BYTE);
    if (p == NULL) {
        p = ag_port_alloc(n, AG_MEM_BYTE);
    }
    return p;
}

static bool uart_open(void)
{
    if (s_uart_up) {
        return true;
    }
    const ag_port_uart_cfg_t cfg = {
        .baud = HSFS_UART_BAUD,
        .data_bits = 8,
        .parity = 0,
        .stop_bits = 1,
    };
    if (ag_port_uart_open(HSFS_UART, &cfg, HSFS_RX_BUF, HSFS_TX_BUF) != AG_OK) {
        return false;
    }
    /* QEMU UART1 needs no GPIOs; on hardware set pins via BOARD.CFG later. */
    if (ag_port_uart_pins(HSFS_UART, AG_PORT_UART_PIN_KEEP,
                          AG_PORT_UART_PIN_KEEP) != AG_OK) {
        return false;
    }
    (void)ag_port_uart_flush(HSFS_UART);
    s_uart_up = true;
    return true;
}

/*
 * While transmitting a large request, the host may still be trying to deliver
 * a PADPUSH.  If our RX fills, host sendall blocks, stops reading our TX, and
 * uart_write_bytes waits forever.  Pump PADPUSH (or hold a non-pad hdr) here.
 *
 * Only start a read when a full header is buffered so a short timeout cannot
 * consume a partial frame and desync the stream.
 */
static void pump_rx_during_tx(void)
{
    for (;;) {
        if (s_held_hdr_valid) {
            return;
        }
        const int32_t avail = ag_port_uart_pending(HSFS_UART);
        if (avail < (int32_t)sizeof(hsfs_hdr_t)) {
            return;
        }
        hsfs_hdr_t hdr;
        if (!read_all(&hdr, sizeof(hdr), 200)) {
            return;
        }
        if (hdr.magic == HSFS_MAGIC && hdr.op == HSFS_OP_PADPUSH) {
            /* Payload is small (3 or 6); give it time to finish arriving. */
            if (!ingest_padpush_locked(&hdr, 200)) {
                return;
            }
            continue;
        }
        s_held_hdr = hdr;
        s_held_hdr_valid = true;
        return;
    }
}

static bool write_all(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t         left = len;
    while (left > 0) {
        pump_rx_during_tx();
        const size_t chunk = left > HSFS_TX_SLICE ? HSFS_TX_SLICE : left;
        const int32_t n = ag_port_uart_write(HSFS_UART, p, chunk);
        if (n <= 0) {
            return false;
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
    return true;
}

static bool read_all(void *buf, size_t len, uint32_t timeout_ms)
{
    uint8_t       *p = (uint8_t *)buf;
    size_t         left = len;
    const uint32_t deadline = now_ms() + timeout_ms;
    while (left > 0) {
        if (ag_proc_stopping()) {
            return false;
        }
        const uint32_t now = now_ms();
        if (now >= deadline) {
            return false;
        }
        uint32_t slice = deadline - now;
        /* Cap slices so kill/stop is noticed within ~50 ms. */
        if (slice > 50u) {
            slice = 50u;
        }
        const int32_t n = ag_port_uart_read(HSFS_UART, p, left, slice);
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            continue;
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
    return true;
}

static bool drain_bytes(uint32_t len, uint32_t timeout_ms)
{
    uint8_t sink[64];
    while (len > 0) {
        const uint32_t chunk =
            len > sizeof(sink) ? (uint32_t)sizeof(sink) : len;
        if (!read_all(sink, chunk, timeout_ms)) {
            return false;
        }
        len -= chunk;
    }
    return true;
}

static bool read_hdr(hsfs_hdr_t *hdr, uint32_t timeout_ms)
{
    if (hdr == NULL) {
        return false;
    }
    if (s_held_hdr_valid) {
        *hdr = s_held_hdr;
        s_held_hdr_valid = false;
        return true;
    }
    return read_all(hdr, sizeof(*hdr), timeout_ms);
}

static ag_err_t rpc(uint16_t op, uint32_t a0, uint32_t a1, const char *path,
                    const void *data_in, uint32_t data_in_len, uint32_t *out_a0,
                    uint32_t *out_a1, void *data_out, uint32_t data_out_cap,
                    uint32_t *data_out_len, uint32_t timeout_ms)
{
    if (!s_uart_up) {
        return -AG_ENODEV;
    }

    const uint32_t path_len =
        (path != NULL) ? (uint32_t)strlen(path) : 0u;
    if (path_len > HSFS_MAX_PATH || data_in_len > HSFS_MAX_DATA) {
        return -AG_ERANGE;
    }

    ag_port_mutex_take(s_rpc_mu, AG_PORT_FOREVER);
    s_rpc_holder = ag_port_task_self();

    const uint16_t seq = ++s_seq;
    hsfs_hdr_t     req = {
            .magic = HSFS_MAGIC,
            .op = op,
            .seq = seq,
            .status = 0,
            .a0 = a0,
            .a1 = a1,
            .path_len = path_len,
            .data_len = data_in_len,
    };

    ag_err_t err = -AG_EIO;
    if (!write_all(&req, sizeof(req))) {
        goto done;
    }
    if (path_len > 0 && !write_all(path, path_len)) {
        goto done;
    }
    /* READ: data_len in the header is the max byte count; no payload follows. */
    if (data_in_len > 0 && data_in != NULL &&
        !write_all(data_in, data_in_len)) {
        goto done;
    }

    hsfs_hdr_t resp;
    for (;;) {
        if (!read_hdr(&resp, timeout_ms)) {
            goto done;
        }
        if (resp.magic != HSFS_MAGIC) {
            err = -AG_EIO;
            goto done;
        }
        /* Host may push pad snapshots between our request and its reply. */
        if (resp.op == HSFS_OP_PADPUSH) {
            if (!ingest_padpush_locked(&resp, timeout_ms)) {
                err = -AG_EIO;
                goto done;
            }
            continue;
        }
        if (resp.path_len != 0 || resp.data_len > HSFS_MAX_DATA) {
            (void)drain_bytes(resp.path_len + resp.data_len, timeout_ms);
            err = -AG_EIO;
            goto done;
        }
        if (resp.seq != seq || resp.op != op) {
            /* Stale/late frame — discard payload and keep waiting. */
            if (!drain_bytes(resp.data_len, timeout_ms)) {
                err = -AG_EIO;
                goto done;
            }
            continue;
        }
        break;
    }
    if (resp.data_len > 0) {
        if (data_out == NULL || resp.data_len > data_out_cap) {
            (void)drain_bytes(resp.data_len, timeout_ms);
            err = -AG_ERANGE;
            goto done;
        }
        if (!read_all(data_out, resp.data_len, timeout_ms)) {
            goto done;
        }
    }

    if (out_a0 != NULL) {
        *out_a0 = resp.a0;
    }
    if (out_a1 != NULL) {
        *out_a1 = resp.a1;
    }
    if (data_out_len != NULL) {
        *data_out_len = resp.data_len;
    }
    err = (ag_err_t)resp.status;

done:
    if (ag_proc_stopping()) {
        err = -AG_EINTR;
    }
    s_rpc_holder = NULL;
    ag_port_mutex_give(s_rpc_mu);
    return err;
}

void *ag_hostfs_rpc_holder(void)
{
    return (void *)s_rpc_holder;
}

/* Drain unsolicited PADPUSH while no RPC is in flight (SMS play loop). */
static void pad_drain_once(void)
{
    if (!s_uart_up || s_rpc_mu == NULL) {
        return;
    }
    if (!ag_port_mutex_take(s_rpc_mu, 0)) {
        return;
    }
    for (;;) {
        if (!s_held_hdr_valid) {
            if (ag_port_uart_pending(HSFS_UART) < (int32_t)sizeof(hsfs_hdr_t)) {
                break;
            }
        }
        hsfs_hdr_t hdr;
        if (!read_hdr(&hdr, 50)) {
            break;
        }
        if (hdr.magic != HSFS_MAGIC || hdr.op != HSFS_OP_PADPUSH) {
            /* Put it back for the next RPC — do not drop WRITE/READ replies. */
            s_held_hdr = hdr;
            s_held_hdr_valid = true;
            break;
        }
        if (!ingest_padpush_locked(&hdr, 50)) {
            break;
        }
    }
    ag_port_mutex_give(s_rpc_mu);
}

static void pad_drain_task(void *arg)
{
    (void)arg;
    while (s_mounted) {
        pad_drain_once();
        ag_port_task_delay(ag_port_ms_to_ticks(4));
    }
    s_pad_task = NULL;
    ag_port_task_delete(NULL);
}

/* ---- VFS ops ----------------------------------------------------------- */

static ag_err_t hs_stat(void *ctx, const char *rel, ag_stat_t *out)
{
    (void)ctx;
    if (out == NULL) {
        return -AG_EINVAL;
    }
    if (is_pad_rel(rel)) {
        pad_drain_once();
        if (s_pad_ms == 0u) {
            return -AG_ENOENT; /* host not pushing (no --pad-cfg) */
        }
        memset(out, 0, sizeof(*out));
        out->size = AG_PAD_BYTES;
        out->attr = AG_A_READONLY;
        return AG_OK;
    }
    uint32_t mode = 0, size32 = 0;
    const ag_err_t err =
        rpc(HSFS_OP_STAT, 0, 0, rel, NULL, 0, &mode, &size32, NULL, 0, NULL,
            HSFS_RPC_TIMEOUT_MS);
    if (err != AG_OK) {
        return err;
    }
    memset(out, 0, sizeof(*out));
    out->size = size32;
    out->attr = (mode & HSFS_MODE_DIR) != 0 ? AG_A_DIR : 0;
    return AG_OK;
}

static ag_err_t hs_opendir(void *ctx, const char *rel, void **dir)
{
    (void)ctx;
    uint32_t host_h = 0;
    const ag_err_t err =
        rpc(HSFS_OP_OPENDIR, 0, 0, rel, NULL, 0, &host_h, NULL, NULL, 0, NULL,
            HSFS_RPC_TIMEOUT_MS);
    if (err != AG_OK) {
        return err;
    }
    hsfs_dir_t *d = (hsfs_dir_t *)hsfs_alloc(sizeof(*d));
    if (d == NULL) {
        (void)rpc(HSFS_OP_CLOSEDIR, host_h, 0, NULL, NULL, 0, NULL, NULL, NULL,
                  0, NULL, HSFS_RPC_TIMEOUT_MS);
        return -AG_ENOMEM;
    }
    d->host_h = host_h;
    *dir = d;
    return AG_OK;
}

static ag_err_t hs_readdir(void *ctx, void *dir, ag_dirent_t *out)
{
    (void)ctx;
    hsfs_dir_t *d = (hsfs_dir_t *)dir;
    if (d == NULL || out == NULL) {
        return -AG_EINVAL;
    }
    char     name[HSFS_MAX_NAME + 1];
    uint32_t mode = 0;
    uint32_t size32 = 0;
    uint32_t nlen = 0;
    /* a1 carries the size: without it `dir h:\` printed 0 bytes for every
     * file on the host share.  An older helper leaves it zero, as before. */
    const ag_err_t err =
        rpc(HSFS_OP_READDIR, d->host_h, 0, NULL, NULL, 0, &mode, &size32, name,
            sizeof(name) - 1, &nlen, HSFS_RPC_TIMEOUT_MS);
    if (err != AG_OK) {
        return err;
    }
    if (nlen >= sizeof(name)) {
        nlen = sizeof(name) - 1;
    }
    name[nlen] = '\0';
    memset(out, 0, sizeof(*out));
    if (nlen >= sizeof(out->name)) {
        nlen = sizeof(out->name) - 1;
    }
    memcpy(out->name, name, nlen);
    out->name[nlen] = '\0';
    out->st.attr = (mode & HSFS_MODE_DIR) != 0 ? AG_A_DIR : 0;
    out->st.size = (mode & HSFS_MODE_DIR) != 0 ? 0u : size32;
    return AG_OK;
}

static ag_err_t hs_closedir(void *ctx, void *dir)
{
    (void)ctx;
    hsfs_dir_t *d = (hsfs_dir_t *)dir;
    if (d == NULL) {
        return -AG_EINVAL;
    }
    const ag_err_t err =
        rpc(HSFS_OP_CLOSEDIR, d->host_h, 0, NULL, NULL, 0, NULL, NULL, NULL, 0,
            NULL, HSFS_RPC_TIMEOUT_MS);
    free(d);
    return err;
}

static ag_err_t hs_open(void *ctx, const char *rel, uint32_t flags, void **file)
{
    (void)ctx;
    const bool want_write =
        (flags & (AG_O_WRONLY | AG_O_RDWR | AG_O_CREATE | AG_O_TRUNC |
                  AG_O_APPEND)) != 0;
    if (is_pad_rel(rel)) {
        if (want_write) {
            return -AG_EROFS;
        }
        pad_drain_once();
        if (s_pad_ms == 0u) {
            return -AG_ENOENT;
        }
        hsfs_file_t *pf = (hsfs_file_t *)hsfs_alloc(sizeof(*pf));
        if (pf == NULL) {
            return -AG_ENOMEM;
        }
        pf->host_h = 0;
        pf->pos = 0;
        pf->size = AG_PAD_BYTES;
        pf->is_pad = true;
        pf->writable = false;
        *file = pf;
        return AG_OK;
    }
    uint32_t host_h = 0, size32 = 0;
    const ag_err_t err =
        rpc(HSFS_OP_OPEN, flags, 0, rel, NULL, 0, &host_h, &size32, NULL, 0,
            NULL, HSFS_RPC_TIMEOUT_MS);
    if (err != AG_OK) {
        return err;
    }
    hsfs_file_t *f = (hsfs_file_t *)hsfs_alloc(sizeof(*f));
    if (f == NULL) {
        (void)rpc(HSFS_OP_CLOSE, host_h, 0, NULL, NULL, 0, NULL, NULL, NULL, 0,
                  NULL, HSFS_RPC_TIMEOUT_MS);
        return -AG_ENOMEM;
    }
    f->host_h = host_h;
    f->pos = 0;
    f->size = size32;
    f->is_pad = false;
    f->writable = want_write;
    *file = f;
    return AG_OK;
}

static ag_err_t hs_close(void *ctx, void *file)
{
    (void)ctx;
    hsfs_file_t *f = (hsfs_file_t *)file;
    if (f == NULL) {
        return -AG_EINVAL;
    }
    ag_err_t err = AG_OK;
    if (!f->is_pad) {
        err = rpc(HSFS_OP_CLOSE, f->host_h, 0, NULL, NULL, 0, NULL, NULL, NULL, 0,
                  NULL, HSFS_RPC_TIMEOUT_MS);
    }
    free(f);
    return err;
}

static int32_t hs_read(void *ctx, void *file, void *buf, size_t len)
{
    (void)ctx;
    hsfs_file_t *f = (hsfs_file_t *)file;
    if (f == NULL || buf == NULL) {
        return -AG_EINVAL;
    }
    if (len == 0) {
        return 0;
    }
    if (f->is_pad) {
        /* Live snapshot from host PADPUSH; rewind is local (see hs_seek). */
        pad_drain_once();
        uint8_t snap[AG_PAD_BYTES];
        if (!ag_hostfs_pad_peek(snap, 250)) {
            /* No recent push — zeros (keys up), not a hard error. */
            memset(snap, 0, sizeof(snap));
        }
        if (f->pos >= AG_PAD_BYTES) {
            return 0;
        }
        const size_t avail = AG_PAD_BYTES - (size_t)f->pos;
        const size_t n = (len < avail) ? len : avail;
        memcpy(buf, snap + (size_t)f->pos, n);
        f->pos += n;
        return (int32_t)n;
    }
    if (f->pos >= f->size) {
        return 0;
    }
    uint32_t want = (uint32_t)len;
    if (want > HSFS_MAX_DATA) {
        want = HSFS_MAX_DATA;
    }
    const uint64_t remain = f->size - f->pos;
    if ((uint64_t)want > remain) {
        want = (uint32_t)remain;
    }

    uint32_t got = 0;
    /* Header data_len carries `want`; no request payload (data_in NULL). */
    const ag_err_t err =
        rpc(HSFS_OP_READ, f->host_h, (uint32_t)f->pos, NULL, NULL, want, &got,
            NULL, buf, want, &got, HSFS_RPC_TIMEOUT_MS);
    if (err != AG_OK) {
        return (int32_t)err;
    }
    f->pos += got;
    return (int32_t)got;
}

static int32_t hs_write(void *ctx, void *file, const void *buf, size_t len)
{
    (void)ctx;
    hsfs_file_t *f = (hsfs_file_t *)file;
    if (f == NULL || buf == NULL) {
        return -AG_EINVAL;
    }
    if (f->is_pad || !f->writable) {
        return -AG_EROFS;
    }
    if (len == 0) {
        return 0;
    }
    uint32_t want = (uint32_t)len;
    if (want > HSFS_MAX_DATA) {
        want = HSFS_MAX_DATA;
    }
    uint32_t wrote = 0;
    const ag_err_t err =
        rpc(HSFS_OP_WRITE, f->host_h, (uint32_t)f->pos, NULL, buf, want, &wrote,
            NULL, NULL, 0, NULL, HSFS_RPC_TIMEOUT_MS);
    if (err != AG_OK) {
        return (int32_t)err;
    }
    if (wrote == 0u) {
        return -AG_EIO;
    }
    if (wrote > want) {
        wrote = want;
    }
    f->pos += wrote;
    if (f->pos > f->size) {
        f->size = f->pos;
    }
    return (int32_t)wrote;
}

static int64_t hs_seek(void *ctx, void *file, int64_t off, int whence)
{
    (void)ctx;
    hsfs_file_t *f = (hsfs_file_t *)file;
    if (f == NULL) {
        return -AG_EINVAL;
    }
    int64_t next = (int64_t)f->pos;
    switch (whence) {
    case AG_SEEK_SET: next = off; break;
    case AG_SEEK_CUR: next = (int64_t)f->pos + off; break;
    case AG_SEEK_END: next = (int64_t)f->size + off; break;
    default: return -AG_EINVAL;
    }
    if (next < 0) {
        return -AG_EINVAL;
    }
    /* Writers may seek past EOF; readers stay within the known size. */
    if (!f->writable && !f->is_pad && (uint64_t)next > f->size) {
        next = (int64_t)f->size;
    }
    f->pos = (uint64_t)next;
    return next;
}

static ag_err_t hs_unlink(void *ctx, const char *rel)
{
    (void)ctx;
    if (rel == NULL) {
        return -AG_EINVAL;
    }
    if (is_pad_rel(rel)) {
        return -AG_EROFS;
    }
    return rpc(HSFS_OP_UNLINK, 0, 0, rel, NULL, 0, NULL, NULL, NULL, 0, NULL,
               HSFS_RPC_TIMEOUT_MS);
}

static ag_err_t hs_info(void *ctx, ag_fsinfo_t *out)
{
    (void)ctx;
    if (out == NULL) {
        return -AG_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    strncpy(out->fs, "host", sizeof(out->fs) - 1);
    /* Host disk space is unknown; advertise "plenty" so dir/copy are not misled. */
    out->total = 1ull << 40;
    out->free = 1ull << 40;
    out->read_only = false;
    return AG_OK;
}

static const ag_fs_ops_t k_ops = {
    .name = "host",
    .open = hs_open,
    .close = hs_close,
    .read = hs_read,
    .write = hs_write,
    .seek = hs_seek,
    .sync = NULL,
    .truncate = NULL,
    .stat = hs_stat,
    .unlink = hs_unlink,
    .rename = NULL,
    .mkdir = NULL,
    .rmdir = NULL,
    .opendir = hs_opendir,
    .readdir = hs_readdir,
    .closedir = hs_closedir,
    .info = hs_info,
};

/* One PING; mounts /host and starts the pad drain when the helper answers. */
static ag_err_t hostfs_probe_once(uint32_t timeout_ms)
{
    uint32_t pong = 0;
    ag_err_t err;

    if (s_mounted) {
        return AG_OK;
    }
    err = rpc(HSFS_OP_PING, 0, 0, NULL, NULL, 0, &pong, NULL, NULL, 0, NULL,
              timeout_ms);
    if (err != AG_OK) {
        return -AG_ENODEV;
    }

    err = ag_vfs_mount("/host", &k_ops, NULL, 0);
    if (err != AG_OK) {
        ag_log(AG_LOG_ERROR, "hostfs", "mount /host failed (%d)", (int)err);
        return err;
    }
    s_mounted = true;
    if (s_pad_task == NULL) {
        (void)ag_port_task_create(pad_drain_task, "ag_hpad", 2048, NULL, 5, 0,
                                  0, &s_pad_task);
    }
    ag_log(AG_LOG_INFO, "hostfs", "H: → /host (live host folder)");
    return AG_OK;
}

/*
 * After guest `reboot`, QEMU may drop and reopen the UART1 TCP link, so a
 * reconnecting hostfsd can miss the first PING.  Keep asking - off the boot
 * path, because a host helper is optional and a board that has none must not
 * pay for the wait: on real hardware there is never one.
 */
static void hostfs_wait_task(void *arg)
{
    const int64_t deadline =
        ag_port_us() + (int64_t)HSFS_HELPER_WAIT_MS * 1000;

    (void)arg;
    while (!s_mounted && ag_port_us() < deadline) {
        if (hostfs_probe_once(HSFS_PING_TIMEOUT_MS) == AG_OK) {
            break;
        }
        ag_port_task_delay(ag_port_ms_to_ticks(50));
    }
    if (!s_mounted) {
        ag_log(AG_LOG_INFO, "hostfs", "no host helper on UART1 (skip H:)");
    }
    s_wait_task = NULL;
    ag_port_task_delete(NULL);
}

ag_err_t ag_hostfs_try_mount(void)
{
    if (s_mounted) {
        return AG_OK;
    }
    if (s_rpc_mu == NULL) {
        s_rpc_mu = ag_port_mutex_new();
        if (s_rpc_mu == NULL) {
            return -AG_ENOMEM;
        }
    }
    if (!uart_open()) {
        ag_log(AG_LOG_WARN, "hostfs", "UART1 unavailable");
        return -AG_ENODEV;
    }

    /* Answered straight away: mount now, so H: is there for AUTOEXEC. */
    if (hostfs_probe_once(HSFS_BOOT_PING_MS) == AG_OK) {
        return AG_OK;
    }

    /* Nobody yet.  Keep trying in the background and let boot carry on. */
    if (s_wait_task == NULL) {
        (void)ag_port_task_create(hostfs_wait_task, "ag_hwait", 3072, NULL, 4,
                                  0, 0, &s_wait_task);
    }
    return -AG_ENODEV;
}

bool ag_hostfs_mounted(void) { return s_mounted; }

bool ag_hostfs_pad_peek(uint8_t out[6], uint32_t max_age_ms)
{
    if (out == NULL || s_pad_ms == 0u) {
        return false;
    }
    if (max_age_ms != 0u) {
        const uint32_t age = now_ms() - s_pad_ms;
        if (age > max_age_ms) {
            return false;
        }
    }
    memcpy(out, s_pad, AG_PAD_BYTES);
    return true;
}
