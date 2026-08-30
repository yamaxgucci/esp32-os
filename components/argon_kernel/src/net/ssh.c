/*
 * ArgonOS - SSH server.
 *
 * Milestone 1 was the transport frame: version banners and SSH_MSG_KEXINIT.
 * Milestone 2 is the key exchange and the cipher it turns on:
 *
 *   - curve25519-sha256 : an ephemeral X25519 pair each side, one shared secret.
 *   - ecdsa-sha2-nistp256 : the host key, generated once and kept in /sys, whose
 *     signature over the exchange hash proves this is the same server next time.
 *   - aes256-ctr + hmac-sha2-256 : after NEWKEYS every packet is encrypted and
 *     authenticated (encrypt-and-MAC, the MAC over the sequence number and the
 *     cleartext packet, RFC 4253 sec 6.4).
 *
 * Milestone 3 adds password authentication (RFC 4252) and milestone 4 the
 * session channel (RFC 4254): once a login succeeds the client opens a session,
 * asks for a shell, and that channel is wired straight to the shared console -
 * so `ssh root@board` lands at the same prompt as the UART or telnet.
 *
 * All the maths is behind argon/port/crypto.h - the kernel does the protocol,
 * the port does the primitives on mbedTLS.
 *
 * Flow control is deliberately simple: we advertise a large receive window and
 * top it up as the client spends it, and we honour the client's window on our
 * output but never block the console waiting for it - a shell's traffic never
 * approaches a megabyte between the client's window updates, so nothing is lost
 * in practice; a pathological flood of output could drop the overflow.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/ssh.h>

#if defined(CONFIG_ARGON_NET_SSH) && CONFIG_ARGON_NET_SSH

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <argon/cfg.h>
#include <argon/console.h>
#include <argon/log.h>
#include <argon/shell.h>
#include <argon/vfs.h>

#include <argon/port/crypto.h>
#include <argon/port/mem.h>
#include <argon/port/net.h>
#include <argon/port/random.h>
#include <argon/port/sync.h>
#include <argon/port/task.h>
#include <argon/port/time.h>

#include "core/sysconfig.h"
#include "net/netio.h"

#define SSH_DEFAULT_PORT 22
#define SSH_ACCEPT_MS    400
#define SSH_VERSION      "SSH-2.0-ArgonOS_0.1"

#define SSH_MSG_DISCONNECT       1
#define SSH_MSG_SERVICE_REQUEST  5
#define SSH_MSG_SERVICE_ACCEPT   6
#define SSH_MSG_KEXINIT          20
#define SSH_MSG_NEWKEYS          21
#define SSH_MSG_KEX_ECDH_INIT    30
#define SSH_MSG_KEX_ECDH_REPLY   31
#define SSH_MSG_USERAUTH_REQUEST 50
#define SSH_MSG_USERAUTH_FAILURE 51
#define SSH_MSG_USERAUTH_SUCCESS 52
#define SSH_MSG_GLOBAL_REQUEST   80
#define SSH_MSG_REQUEST_SUCCESS  81
#define SSH_MSG_REQUEST_FAILURE  82
#define SSH_MSG_CHANNEL_OPEN              90
#define SSH_MSG_CHANNEL_OPEN_CONFIRMATION 91
#define SSH_MSG_CHANNEL_OPEN_FAILURE      92
#define SSH_MSG_CHANNEL_WINDOW_ADJUST     93
#define SSH_MSG_CHANNEL_DATA              94
#define SSH_MSG_CHANNEL_EOF               96
#define SSH_MSG_CHANNEL_CLOSE             97
#define SSH_MSG_CHANNEL_REQUEST           98
#define SSH_MSG_CHANNEL_SUCCESS           99
#define SSH_MSG_CHANNEL_FAILURE          100

#define SSH_MAX_AUTH_TRIES 6 /* before the connection is dropped */

/* Channel flow control.  We advertise a large receive window and top it back up
 * as the client spends it; our data chunks stay well under any client's maximum
 * packet.  A shell's traffic is tiny, so this is generous rather than tuned. */
#define SSH_WINDOW_INITIAL 0x100000u /* 1 MiB advertised to the client */
#define SSH_WINDOW_LOW     0x080000u /* top up when it falls below here */
#define SSH_OUR_MAX_PACKET 8192u
#define SSH_DATA_CHUNK     1024u     /* bytes of console output per DATA packet */
#define SSH_IN_RING        2048u     /* keystrokes waiting for the console */

#define SSH_MAX_PACKET   4096 /* handshake and control packets are small */
#define SSH_HOSTKEY_PATH "/sys/SSH_HOST.KEY"

static const char SSH_HOSTKEY_ID[] = "ecdsa-sha2-nistp256";

static volatile bool  s_running;
static volatile bool  s_stop;
static int            s_listen_fd = -1;
static uint16_t       s_port;
static ag_port_task_t s_task;

/* Live credentials set with `ssh user` - they take effect at once, whereas
 * ssh.user / ssh.pass in SYSTEM.CFG are only read at boot.  Empty means "fall
 * back to the config"; a configured password that is also empty means no login
 * is possible, which is the safe default for a freshly flashed board. */
static char s_user[33];
static char s_pass[65];

/* The algorithms this server speaks - now all implemented below. */
static const char *const NL_KEX = "curve25519-sha256";
static const char *const NL_HOSTKEY = "ecdsa-sha2-nistp256";
static const char *const NL_ENC = "aes256-ctr";
static const char *const NL_MAC = "hmac-sha2-256";
static const char *const NL_COMP = "none";

/* Everything one connection needs, off the task stack (~13 KB of buffers). */
typedef struct {
    int         fd;
    ag_netio_t  rd;

    /* transcript for the exchange hash */
    char        v_c[256];
    size_t      i_c_len;
    size_t      i_s_len;
    uint8_t     i_c[SSH_MAX_PACKET];
    uint8_t     i_s[512];

    /* record state */
    uint32_t    seq_in;
    uint32_t    seq_out;
    bool        enc_in;
    bool        enc_out;
    ag_aes_ctr_t *c_in;   /* client -> server */
    ag_aes_ctr_t *c_out;  /* server -> client */
    uint8_t     mac_in[32];
    uint8_t     mac_out[32];
    uint8_t     session_id[32];

    /* nbuf is the raw socket buffer netio reads into; msg is one decoded payload
     * at a time - both may live in PSRAM with the rest of this struct.
     *
     * rx/tx are the assembled packet the cipher works in place on, and they must
     * NOT be in PSRAM: the ESP32-S3's AES accelerator (which mbedTLS uses) reads
     * and writes its buffers by DMA, and the DMA engine cannot reach PSRAM - an
     * AES call on a PSRAM buffer simply hangs.  So they are allocated separately
     * from internal, DMA-reachable memory (see conn_alloc).  Layout: [0..3] is
     * the sequence number for the MAC, the packet follows at +4, so the HMAC
     * covers seq||packet in one contiguous call. */
    uint8_t     nbuf[SSH_MAX_PACKET];
    uint8_t     msg[SSH_MAX_PACKET];
    uint8_t    *rx; /* internal DMA RAM, 4 + SSH_MAX_PACKET + 16 */
    uint8_t    *tx; /* internal DMA RAM, 4 + SSH_MAX_PACKET + 16 */

    /* Session channel (M4).  The ssh_task drives the packet loop; the console
     * task calls the transport write/read below.  send_lock serialises every
     * write_packet across the two tasks; io_lock guards the input ring. */
    bool            ch_open;
    volatile bool   ch_gone; /* the channel has ended; the console must detach */
    bool            exec_pending; /* an exec request is waiting to run */
    char            exec_cmd[256]; /* the command from an exec request */
    uint32_t        ch_peer; /* the client's channel number (our recipient)    */
    uint32_t        send_window; /* bytes we may still send to the client      */
    uint32_t        recv_window; /* bytes the client may still send us         */
    ag_port_mutex_t send_lock;
    ag_port_mutex_t io_lock;
    size_t          in_head;
    size_t          in_tail;
    uint8_t         in_ring[SSH_IN_RING];
    uint8_t         wpay[16 + SSH_DATA_CHUNK]; /* build one DATA payload (send_lock) */
} ssh_conn_t;

#define SSH_RECORD_BUF (4 + SSH_MAX_PACKET + 16)

/* ---- big-endian scalars ------------------------------------------------ */

static uint32_t rd_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* ---- a bounds-checked buffer writer ------------------------------------ */

typedef struct {
    uint8_t *p;
    size_t   cap;
    size_t   len;
    bool     ovf;
} wbuf_t;

static void wb_init(wbuf_t *w, uint8_t *p, size_t cap)
{
    w->p = p;
    w->cap = cap;
    w->len = 0;
    w->ovf = false;
}

static void wb_bytes(wbuf_t *w, const void *data, size_t n)
{
    if (w->len + n > w->cap) {
        w->ovf = true;
        return;
    }
    memcpy(w->p + w->len, data, n);
    w->len += n;
}

static void wb_byte(wbuf_t *w, uint8_t b) { wb_bytes(w, &b, 1); }

static void wb_u32(wbuf_t *w, uint32_t v)
{
    uint8_t t[4];
    wr_u32(t, v);
    wb_bytes(w, t, 4);
}

/* An SSH string: uint32 length then the bytes. */
static void wb_string(wbuf_t *w, const void *data, size_t n)
{
    wb_u32(w, (uint32_t)n);
    wb_bytes(w, data, n);
}

static void wb_cstr(wbuf_t *w, const char *s) { wb_string(w, s, strlen(s)); }

/* An SSH mpint from a big-endian magnitude: leading zeros dropped, a 0x00 added
 * when the top bit would otherwise read as a sign (RFC 4251 sec 5). */
static void wb_mpint(wbuf_t *w, const uint8_t *mag, size_t n)
{
    size_t i = 0;
    while (i < n && mag[i] == 0) {
        i++;
    }
    const size_t m = n - i;
    if (m == 0) {
        wb_u32(w, 0);
        return;
    }
    const bool pad = (mag[i] & 0x80) != 0;
    wb_u32(w, (uint32_t)(m + (pad ? 1 : 0)));
    if (pad) {
        wb_byte(w, 0x00);
    }
    wb_bytes(w, mag + i, m);
}

/* ---- a bounds-checked buffer reader ------------------------------------ */

typedef struct {
    const uint8_t *p;
    size_t         len;
    size_t         pos;
    bool           err;
} rbuf_t;

static void rb_init(rbuf_t *r, const uint8_t *p, size_t len)
{
    r->p = p;
    r->len = len;
    r->pos = 0;
    r->err = false;
}

static uint8_t rb_byte(rbuf_t *r)
{
    if (r->pos + 1 > r->len) {
        r->err = true;
        return 0;
    }
    return r->p[r->pos++];
}

static uint32_t rb_u32_read(rbuf_t *r)
{
    if (r->pos + 4 > r->len) {
        r->err = true;
        return 0;
    }
    const uint32_t v = rd_u32(r->p + r->pos);
    r->pos += 4;
    return v;
}

/* An SSH string: returns a pointer into the buffer and its length; advances. */
static const uint8_t *rb_string(rbuf_t *r, uint32_t *out_len)
{
    if (r->pos + 4 > r->len) {
        r->err = true;
        return NULL;
    }
    const uint32_t n = rd_u32(r->p + r->pos);
    r->pos += 4;
    if (n > r->len - r->pos) {
        r->err = true;
        return NULL;
    }
    const uint8_t *s = r->p + r->pos;
    r->pos += n;
    *out_len = n;
    return s;
}

/* Constant-time equality of an n-byte field against a C string of the same n. */
static bool ct_eq(const uint8_t *a, const char *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) {
        d |= (uint8_t)(a[i] ^ (uint8_t)b[i]);
    }
    return d == 0;
}

/* ---- socket helpers ---------------------------------------------------- */

static bool read_full(ag_netio_t *r, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        const int32_t k = ag_netio_read(r, buf + got, n - got);
        if (k <= 0) {
            return false;
        }
        got += (size_t)k;
    }
    return true;
}

/* ---- version exchange -------------------------------------------------- */

static bool read_client_version(ssh_conn_t *c)
{
    char line[256];
    for (int tries = 0; tries < 8; tries++) {
        if (ag_netio_line(&c->rd, line, sizeof(line)) != AG_OK) {
            return false;
        }
        if (strncmp(line, "SSH-2.0-", 8) == 0 ||
            strncmp(line, "SSH-1.99-", 9) == 0) {
            /* V_C for the hash is the line without its CR LF - which netio
             * already stripped.  A trailing CR can survive on some clients. */
            size_t n = strlen(line);
            if (n > 0 && line[n - 1] == '\r') {
                line[--n] = '\0';
            }
            if (n >= sizeof(c->v_c)) {
                n = sizeof(c->v_c) - 1;
            }
            memcpy(c->v_c, line, n);
            c->v_c[n] = '\0';
            ag_log(AG_LOG_INFO, "ssh", "client is %s", c->v_c);
            return true;
        }
    }
    return false;
}

/* ---- binary packet protocol (cleartext + encrypted) -------------------- */

/* Read one packet's payload into out.  Returns payload length or negative. */
static int32_t read_packet(ssh_conn_t *c, uint8_t *out, size_t cap)
{
    if (!c->enc_in) {
        uint8_t hdr[5];
        if (!read_full(&c->rd, hdr, 5)) {
            return -AG_EIO;
        }
        const uint32_t pkt_len = rd_u32(hdr);
        const uint8_t  pad_len = hdr[4];
        if (pkt_len < 2 || pkt_len > SSH_MAX_PACKET) {
            return -AG_EFORMAT;
        }
        if ((uint32_t)pad_len + 1u > pkt_len) {
            return -AG_EFORMAT;
        }
        const uint32_t payload_len = pkt_len - pad_len - 1;
        if (payload_len > cap) {
            return -AG_ERANGE;
        }
        if (!read_full(&c->rd, out, payload_len)) {
            return -AG_EIO;
        }
        uint8_t pad[256];
        if (pad_len > 0 && !read_full(&c->rd, pad, pad_len)) {
            return -AG_EIO;
        }
        c->seq_in++;
        return (int32_t)payload_len;
    }

    /* Encrypted: decrypt the first block to learn the length, then the rest. */
    uint8_t *P = c->rx + 4;
    if (!read_full(&c->rd, P, 16)) {
        return -AG_EIO;
    }
    ag_crypto_aes_ctr_xcrypt(c->c_in, P, P, 16);
    const uint32_t pkt_len = rd_u32(P);
    if (pkt_len < 2 || pkt_len > SSH_MAX_PACKET) {
        return -AG_EFORMAT;
    }
    const uint32_t total = 4 + pkt_len;
    if ((total % 16) != 0 || total > SSH_MAX_PACKET + 16) {
        return -AG_EFORMAT;
    }
    if (total > 16) {
        if (!read_full(&c->rd, P + 16, total - 16)) {
            return -AG_EIO;
        }
        ag_crypto_aes_ctr_xcrypt(c->c_in, P + 16, P + 16, total - 16);
    }
    uint8_t mac_rx[32];
    if (!read_full(&c->rd, mac_rx, sizeof(mac_rx))) {
        return -AG_EIO;
    }
    wr_u32(c->rx, c->seq_in); /* seq in front of the packet for the MAC */
    uint8_t mac_calc[32];
    ag_crypto_hmac_sha256(c->mac_in, sizeof(c->mac_in), c->rx, 4 + total,
                          mac_calc);
    if (memcmp(mac_calc, mac_rx, sizeof(mac_calc)) != 0) {
        return -AG_EFORMAT; /* bad MAC: give up on this connection */
    }
    const uint8_t pad_len = P[4];
    if ((uint32_t)pad_len + 1u > pkt_len) {
        return -AG_EFORMAT;
    }
    const uint32_t payload_len = pkt_len - pad_len - 1;
    if (payload_len > cap) {
        return -AG_ERANGE;
    }
    memcpy(out, P + 5, payload_len);
    c->seq_in++;
    return (int32_t)payload_len;
}

/* Send one packet.  Block size 8 in the clear, 16 once the cipher is on. */
static ag_err_t write_packet_raw(ssh_conn_t *c, const uint8_t *payload,
                                 size_t len)
{
    const size_t block = c->enc_out ? 16 : 8;
    size_t       need = 4 + 1 + len;
    size_t       pad = block - (need % block);
    if (pad < 4) {
        pad += block;
    }
    const uint32_t pkt_len = (uint32_t)(1 + len + pad);
    const size_t   total = 4 + (size_t)pkt_len;
    if (total > SSH_MAX_PACKET + 16) {
        return -AG_ERANGE;
    }

    uint8_t *P = c->tx + 4;
    wr_u32(P, pkt_len);
    P[4] = (uint8_t)pad;
    memcpy(P + 5, payload, len);
    ag_port_random(P + 5 + len, pad);

    if (!c->enc_out) {
        const ag_err_t e = ag_netio_send_all(c->fd, P, total);
        if (e == AG_OK) {
            c->seq_out++;
        }
        return e;
    }

    wr_u32(c->tx, c->seq_out);
    uint8_t mac[32];
    ag_crypto_hmac_sha256(c->mac_out, sizeof(c->mac_out), c->tx, 4 + total, mac);
    ag_crypto_aes_ctr_xcrypt(c->c_out, P, P, total);
    ag_err_t e = ag_netio_send_all(c->fd, P, total);
    if (e == AG_OK) {
        e = ag_netio_send_all(c->fd, mac, sizeof(mac));
    }
    if (e == AG_OK) {
        c->seq_out++;
    }
    return e;
}

/*
 * The public sender.  Once the session channel is up, the console task sends
 * channel data while the ssh_task sends control packets, so every send is
 * serialised here - the cipher counter and sequence number are single-writer.
 * Before then send_lock is NULL and this is a straight call.
 */
static ag_err_t write_packet(ssh_conn_t *c, const uint8_t *payload, size_t len)
{
    if (c->send_lock != NULL) {
        ag_port_mutex_take(c->send_lock, AG_PORT_FOREVER);
    }
    const ag_err_t e = write_packet_raw(c, payload, len);
    if (c->send_lock != NULL) {
        ag_port_mutex_give(c->send_lock);
    }
    return e;
}

/* ---- KEXINIT ----------------------------------------------------------- */

static ag_err_t send_kexinit(ssh_conn_t *c)
{
    wbuf_t w;
    wb_init(&w, c->i_s, sizeof(c->i_s));
    wb_byte(&w, SSH_MSG_KEXINIT);
    uint8_t cookie[16];
    ag_port_random(cookie, sizeof(cookie));
    wb_bytes(&w, cookie, sizeof(cookie));
    wb_cstr(&w, NL_KEX);
    wb_cstr(&w, NL_HOSTKEY);
    wb_cstr(&w, NL_ENC); /* enc c2s */
    wb_cstr(&w, NL_ENC); /* enc s2c */
    wb_cstr(&w, NL_MAC); /* mac c2s */
    wb_cstr(&w, NL_MAC); /* mac s2c */
    wb_cstr(&w, NL_COMP); /* comp c2s */
    wb_cstr(&w, NL_COMP); /* comp s2c */
    wb_cstr(&w, "");      /* lang c2s */
    wb_cstr(&w, "");      /* lang s2c */
    wb_byte(&w, 0);       /* first_kex_packet_follows */
    wb_u32(&w, 0);        /* reserved */
    if (w.ovf) {
        return -AG_ERANGE;
    }
    c->i_s_len = w.len;
    return write_packet(c, c->i_s, c->i_s_len);
}

/* ---- host key ---------------------------------------------------------- */

/* Load the persistent host key, or make one and store it.  priv is the secret
 * scalar (32), pub is the public point X||Y (64); the file holds both. */
static bool host_key(uint8_t priv[32], uint8_t pub[64])
{
    uint8_t blob[96];

    ag_handle_t h = ag_vfs_open(SSH_HOSTKEY_PATH, NULL, AG_O_RDONLY);
    if (h >= 0) {
        size_t got = 0;
        while (got < sizeof(blob)) {
            const int32_t n = ag_vfs_read(h, blob + got, sizeof(blob) - got);
            if (n <= 0) {
                break;
            }
            got += (size_t)n;
        }
        ag_vfs_close(h);
        if (got == sizeof(blob)) {
            memcpy(priv, blob, 32);
            memcpy(pub, blob + 32, 64);
            return true;
        }
        ag_log(AG_LOG_WARN, "ssh", "host key file short (%u); regenerating",
               (unsigned)got);
    }

    if (ag_crypto_ecdsa_p256_genkey(priv, pub) != 0) {
        ag_log(AG_LOG_ERROR, "ssh", "host key generation failed");
        return false;
    }
    memcpy(blob, priv, 32);
    memcpy(blob + 32, pub, 64);
    h = ag_vfs_open(SSH_HOSTKEY_PATH, NULL, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (h >= 0) {
        size_t put = 0;
        while (put < sizeof(blob)) {
            const int32_t n = ag_vfs_write(h, blob + put, sizeof(blob) - put);
            if (n <= 0) {
                break;
            }
            put += (size_t)n;
        }
        ag_vfs_close(h);
        if (put == sizeof(blob)) {
            ag_log(AG_LOG_INFO, "ssh", "new host key stored in %s",
                   SSH_HOSTKEY_PATH);
        } else {
            ag_log(AG_LOG_WARN, "ssh", "host key not persisted (write short)");
        }
    } else {
        ag_log(AG_LOG_WARN, "ssh", "host key not persisted (/sys not writable)");
    }
    return true;
}

/* The host key blob as it appears on the wire and in the exchange hash:
 * string "ecdsa-sha2-nistp256", string "nistp256", string Q (0x04||X||Y). */
static size_t host_key_blob(uint8_t *out, size_t cap, const uint8_t pub[64])
{
    wbuf_t w;
    wb_init(&w, out, cap);
    wb_cstr(&w, SSH_HOSTKEY_ID);
    wb_cstr(&w, "nistp256");
    uint8_t q[65];
    q[0] = 0x04;
    memcpy(q + 1, pub, 64);
    wb_string(&w, q, sizeof(q));
    return w.ovf ? 0 : w.len;
}

/* ---- key exchange ------------------------------------------------------ */

/* One KDF output: HASH(K || H || X || session_id), truncated to outlen (<=32,
 * which is all this cipher suite needs). */
static void kdf(const uint8_t *kmp, size_t kmp_len, const uint8_t H[32],
                char letter, const uint8_t session_id[32], uint8_t *out,
                size_t outlen)
{
    uint8_t  buf[64 + 32 + 1 + 32];
    wbuf_t   w;
    wb_init(&w, buf, sizeof(buf));
    wb_bytes(&w, kmp, kmp_len);
    wb_bytes(&w, H, 32);
    wb_byte(&w, (uint8_t)letter);
    wb_bytes(&w, session_id, 32);
    uint8_t hash[32];
    ag_crypto_sha256(buf, w.len, hash);
    memcpy(out, hash, outlen);
}

/* Handle KEX_ECDH_INIT -> derive keys, send KEX_ECDH_REPLY and NEWKEYS.  On
 * success the cipher is armed both ways.  Returns AG_OK or negative. */
static ag_err_t do_kex(ssh_conn_t *c, const uint8_t *init_pl, size_t init_len)
{
    /* KEX_ECDH_INIT: byte, string Q_C (32 bytes for curve25519). */
    if (init_len < 1 + 4 || init_pl[0] != SSH_MSG_KEX_ECDH_INIT) {
        ag_log(AG_LOG_WARN, "ssh", "kex: not ECDH_INIT (msg %u, %u bytes)",
               init_len ? (unsigned)init_pl[0] : 0u, (unsigned)init_len);
        return -AG_EFORMAT;
    }
    const uint32_t qc_len = rd_u32(init_pl + 1);
    if (qc_len != 32 || init_len < 1 + 4 + 32) {
        ag_log(AG_LOG_WARN, "ssh", "kex: bad Q_C length %u", (unsigned)qc_len);
        return -AG_EFORMAT;
    }
    const uint8_t *q_c = init_pl + 5;

    /* Our ephemeral pair and the shared secret. */
    uint8_t s_priv[32], q_s[32], K[32];
    if (ag_crypto_x25519_keypair(s_priv, q_s) != 0) {
        ag_log(AG_LOG_WARN, "ssh", "kex: x25519 keypair failed");
        return -AG_EIO;
    }
    if (ag_crypto_x25519(K, s_priv, q_c) != 0) {
        ag_log(AG_LOG_WARN, "ssh", "kex: x25519 shared failed");
        return -AG_EIO;
    }

    /* Host key and its wire blob. */
    uint8_t hk_priv[32], hk_pub[64];
    if (!host_key(hk_priv, hk_pub)) {
        return -AG_EIO;
    }
    uint8_t ks[160];
    const size_t ks_len = host_key_blob(ks, sizeof(ks), hk_pub);
    if (ks_len == 0) {
        return -AG_ERANGE;
    }

    /* K as an mpint, reused for the hash and the KDF.  Worst case is 4 (length)
     * + 1 (sign pad) + 32 (magnitude) = 37 bytes. */
    uint8_t kmp_buf[40];
    wbuf_t  kmp;
    wb_init(&kmp, kmp_buf, sizeof(kmp_buf));
    wb_mpint(&kmp, K, 32);
    if (kmp.ovf) {
        return -AG_ERANGE;
    }

    /* Exchange hash H over the whole transcript. */
    uint8_t *hb = malloc(SSH_MAX_PACKET + 1024);
    if (hb == NULL) {
        return -AG_ENOMEM;
    }
    wbuf_t hw;
    wb_init(&hw, hb, SSH_MAX_PACKET + 1024);
    wb_string(&hw, c->v_c, strlen(c->v_c));
    wb_string(&hw, SSH_VERSION, strlen(SSH_VERSION));
    wb_string(&hw, c->i_c, c->i_c_len);
    wb_string(&hw, c->i_s, c->i_s_len);
    wb_string(&hw, ks, ks_len);
    wb_string(&hw, q_c, 32);
    wb_string(&hw, q_s, 32);
    wb_bytes(&hw, kmp_buf, kmp.len); /* mpint(K), already encoded */
    if (hw.ovf) {
        free(hb);
        return -AG_ERANGE;
    }
    uint8_t H[32];
    ag_crypto_sha256(hb, hw.len, H);
    free(hb);

    /* Sign SHA-256(H) with the host key. */
    uint8_t digest[32];
    ag_crypto_sha256(H, sizeof(H), digest);
    uint8_t sig_r[32], sig_s[32];
    if (ag_crypto_ecdsa_p256_sign(hk_priv, digest, sig_r, sig_s) != 0) {
        ag_log(AG_LOG_WARN, "ssh", "kex: host-key sign failed");
        return -AG_EIO;
    }

    /* Signature blob: string "ecdsa-sha2-nistp256", string (mpint r, mpint s). */
    uint8_t ecsig[80];
    wbuf_t  es;
    wb_init(&es, ecsig, sizeof(ecsig));
    wb_mpint(&es, sig_r, 32);
    wb_mpint(&es, sig_s, 32);
    uint8_t sig[160];
    wbuf_t  sw;
    wb_init(&sw, sig, sizeof(sig));
    wb_cstr(&sw, SSH_HOSTKEY_ID);
    wb_string(&sw, ecsig, es.len);
    if (es.ovf || sw.ovf) {
        return -AG_ERANGE;
    }

    /* KEX_ECDH_REPLY. */
    uint8_t reply[512];
    wbuf_t  rw;
    wb_init(&rw, reply, sizeof(reply));
    wb_byte(&rw, SSH_MSG_KEX_ECDH_REPLY);
    wb_string(&rw, ks, ks_len);
    wb_string(&rw, q_s, 32);
    wb_string(&rw, sig, sw.len);
    if (rw.ovf) {
        return -AG_ERANGE;
    }
    ag_err_t e = write_packet(c, reply, rw.len);
    if (e != AG_OK) {
        return e;
    }

    /* This is the first exchange, so H is the session id. */
    memcpy(c->session_id, H, sizeof(H));

    /* Derive the six keys.  aes256-ctr wants a 32-byte key and 16-byte IV;
     * hmac-sha2-256 a 32-byte key - each exactly one hash. */
    uint8_t iv_c2s[16], iv_s2c[16], k_c2s[32], k_s2c[32];
    kdf(kmp_buf, kmp.len, H, 'A', c->session_id, iv_c2s, sizeof(iv_c2s));
    kdf(kmp_buf, kmp.len, H, 'B', c->session_id, iv_s2c, sizeof(iv_s2c));
    kdf(kmp_buf, kmp.len, H, 'C', c->session_id, k_c2s, sizeof(k_c2s));
    kdf(kmp_buf, kmp.len, H, 'D', c->session_id, k_s2c, sizeof(k_s2c));
    kdf(kmp_buf, kmp.len, H, 'E', c->session_id, c->mac_in, sizeof(c->mac_in));
    kdf(kmp_buf, kmp.len, H, 'F', c->session_id, c->mac_out, sizeof(c->mac_out));

    c->c_in = ag_crypto_aes_ctr_new(k_c2s, iv_c2s);
    c->c_out = ag_crypto_aes_ctr_new(k_s2c, iv_s2c);
    if (c->c_in == NULL || c->c_out == NULL) {
        ag_log(AG_LOG_WARN, "ssh", "kex: cipher init failed");
        return -AG_ENOMEM;
    }

    /* Our NEWKEYS: everything we send after it is encrypted. */
    const uint8_t newkeys = SSH_MSG_NEWKEYS;
    e = write_packet(c, &newkeys, 1);
    if (e != AG_OK) {
        return e;
    }
    c->enc_out = true;
    return AG_OK;
}

/* ---- authentication ---------------------------------------------------- */

/* The expected login: the live credentials if set, else SYSTEM.CFG, else the
 * defaults (user "root", no password - which refuses every login). */
static const char *want_user(void)
{
    if (s_user[0] != '\0') {
        return s_user;
    }
    return ag_cfg_get(ag_sysconfig(), "ssh.user", "root");
}

static const char *want_pass(void)
{
    if (s_pass[0] != '\0') {
        return s_pass;
    }
    return ag_cfg_get(ag_sysconfig(), "ssh.pass", "");
}

static ag_err_t send_userauth_failure(ssh_conn_t *c)
{
    uint8_t buf[32];
    wbuf_t  w;
    wb_init(&w, buf, sizeof(buf));
    wb_byte(&w, SSH_MSG_USERAUTH_FAILURE);
    wb_cstr(&w, "password"); /* the methods that can still continue */
    wb_byte(&w, 0);          /* partial success: no */
    return write_packet(c, buf, w.len);
}

/*
 * The userauth service (RFC 4252).  Answers requests until one succeeds or the
 * tries run out.  "none" and unknown methods are refused with the password
 * method offered; a correct password wins.  Returns true once authenticated.
 */
static bool do_userauth(ssh_conn_t *c)
{
    const char *user = want_user();
    const char *pass = want_pass();
    const size_t user_len = strlen(user);
    const size_t pass_len = strlen(pass);
    if (pass_len == 0) {
        ag_log(AG_LOG_WARN, "ssh",
               "no password set - refusing logins (set one: ssh user <name> <pass>)");
    }

    for (int tries = 0; tries < SSH_MAX_AUTH_TRIES; tries++) {
        const int32_t n = read_packet(c, c->msg, sizeof(c->msg));
        if (n < 1) {
            return false;
        }
        if (c->msg[0] != SSH_MSG_USERAUTH_REQUEST) {
            continue; /* not our business; ignore and keep waiting */
        }

        rbuf_t r;
        rb_init(&r, c->msg + 1, (size_t)n - 1);
        uint32_t ul = 0, sl = 0, ml = 0;
        const uint8_t *u = rb_string(&r, &ul);
        (void)rb_string(&r, &sl); /* service name: always "ssh-connection" */
        const uint8_t *m = rb_string(&r, &ml);
        if (r.err) {
            return false;
        }

        if (ml == 8 && memcmp(m, "password", 8) == 0) {
            const uint8_t change = rb_byte(&r);
            uint32_t      pw_len = 0;
            const uint8_t *pw = rb_string(&r, &pw_len);
            if (r.err || change != 0) {
                if (send_userauth_failure(c) != AG_OK) {
                    return false;
                }
                continue;
            }
            const bool ok = pass_len > 0 && ul == user_len &&
                            ct_eq(u, user, ul) && pw_len == pass_len &&
                            ct_eq(pw, pass, pw_len);
            if (ok) {
                const uint8_t s = SSH_MSG_USERAUTH_SUCCESS;
                if (write_packet(c, &s, 1) != AG_OK) {
                    return false;
                }
                ag_log(AG_LOG_INFO, "ssh", "authenticated %.*s (password)",
                       (int)ul, (const char *)u);
                return true;
            }
            ag_log(AG_LOG_WARN, "ssh", "password rejected for %.*s", (int)ul,
                   (const char *)u);
        }

        if (send_userauth_failure(c) != AG_OK) {
            return false;
        }
    }
    ag_log(AG_LOG_WARN, "ssh", "too many auth attempts; dropping");
    return false;
}

/* ---- the session channel ----------------------------------------------- */

/* Small control packets: byte + one channel number. */
static ag_err_t send_channel_u32(ssh_conn_t *c, uint8_t type, uint32_t chan)
{
    uint8_t b[8];
    wbuf_t  w;
    wb_init(&w, b, sizeof(b));
    wb_byte(&w, type);
    wb_u32(&w, chan);
    return write_packet(c, b, w.len);
}

/*
 * The console transport.  write() and read() are called on the console task;
 * the ssh_task runs do_session() below at the same time.  send_lock serialises
 * every write_packet across the two; io_lock guards the input ring.
 */

static int32_t ssh_ch_write(void *ctx, const char *data, size_t len)
{
    ssh_conn_t *c = (ssh_conn_t *)ctx;
    if (!c->ch_open || c->ch_gone) {
        return 0;
    }
    size_t off = 0;
    while (off < len) {
        ag_port_mutex_take(c->send_lock, AG_PORT_FOREVER);
        size_t chunk = len - off;
        if (chunk > SSH_DATA_CHUNK) {
            chunk = SSH_DATA_CHUNK;
        }
        if (chunk > c->send_window) {
            chunk = c->send_window; /* never overrun the client's window */
        }
        if (chunk == 0) {
            ag_port_mutex_give(c->send_lock);
            break; /* window spent; the rest is dropped (see file header note) */
        }
        wbuf_t w;
        wb_init(&w, c->wpay, sizeof(c->wpay));
        wb_byte(&w, SSH_MSG_CHANNEL_DATA);
        wb_u32(&w, c->ch_peer);
        wb_u32(&w, (uint32_t)chunk);
        wb_bytes(&w, data + off, chunk);
        const ag_err_t e = w.ovf ? -AG_ERANGE : write_packet_raw(c, c->wpay, w.len);
        if (e == AG_OK) {
            c->send_window -= (uint32_t)chunk;
        }
        ag_port_mutex_give(c->send_lock);
        if (e != AG_OK) {
            break;
        }
        off += chunk;
    }
    return (int32_t)off;
}

static int32_t ssh_ch_read(void *ctx, uint8_t *buf, size_t len)
{
    ssh_conn_t *c = (ssh_conn_t *)ctx;
    if (c->ch_gone) {
        return -1; /* the channel has ended: the console detaches us */
    }
    size_t n = 0;
    ag_port_mutex_take(c->io_lock, AG_PORT_FOREVER);
    while (n < len && c->in_tail != c->in_head) {
        buf[n++] = c->in_ring[c->in_tail];
        c->in_tail = (c->in_tail + 1) % SSH_IN_RING;
    }
    ag_port_mutex_give(c->io_lock);
    return (int32_t)n; /* 0 means nothing right now, still alive */
}

static void ssh_ch_close(void *ctx)
{
    ssh_conn_t *c = (ssh_conn_t *)ctx;
    c->ch_gone = true; /* the ssh_task owns the fd; just mark the channel */
}

static const ag_con_transport_t k_ssh_transport = {
    .name = "ssh",
    .write = ssh_ch_write,
    .read = ssh_ch_read,
    .close = ssh_ch_close,
};

/* CHANNEL_OPEN: only "session" is accepted, and only one at a time. */
static void on_channel_open(ssh_conn_t *c, const uint8_t *pl, size_t len)
{
    rbuf_t r;
    rb_init(&r, pl + 1, len - 1);
    uint32_t       tl = 0;
    const uint8_t *type = rb_string(&r, &tl);
    const uint32_t peer = rb_u32_read(&r);
    const uint32_t peer_win = rb_u32_read(&r);
    (void)rb_u32_read(&r); /* peer max packet: we chunk well under it */
    if (r.err) {
        return;
    }
    const bool is_session = (tl == 7 && memcmp(type, "session", 7) == 0);
    if (!is_session || c->ch_open) {
        uint8_t b[32];
        wbuf_t  w;
        wb_init(&w, b, sizeof(b));
        wb_byte(&w, SSH_MSG_CHANNEL_OPEN_FAILURE);
        wb_u32(&w, peer);
        wb_u32(&w, 1); /* SSH_OPEN_ADMINISTRATIVELY_PROHIBITED */
        wb_cstr(&w, is_session ? "one session only" : "session only");
        wb_cstr(&w, "");
        (void)write_packet(c, b, w.len);
        return;
    }
    c->ch_peer = peer;
    c->send_window = peer_win;
    c->ch_open = true;

    uint8_t b[32];
    wbuf_t  w;
    wb_init(&w, b, sizeof(b));
    wb_byte(&w, SSH_MSG_CHANNEL_OPEN_CONFIRMATION);
    wb_u32(&w, peer);               /* recipient: the client's channel */
    wb_u32(&w, 0);                  /* our channel number (only one)   */
    wb_u32(&w, c->recv_window);     /* how much the client may send us */
    wb_u32(&w, SSH_OUR_MAX_PACKET);
    (void)write_packet(c, b, w.len);
}

/* CHANNEL_REQUEST: pty-req and window-change are accepted quietly; "shell"
 * attaches the shared console; "exec" captures the command to run once (handled
 * back in do_session).  Returns true once a shell is attached. */
static bool on_channel_request(ssh_conn_t *c, const uint8_t *pl, size_t len,
                               bool *attached)
{
    rbuf_t r;
    rb_init(&r, pl + 1, len - 1);
    (void)rb_u32_read(&r); /* recipient channel: we have only one */
    uint32_t       tl = 0;
    const uint8_t *type = rb_string(&r, &tl);
    const uint8_t  want_reply = rb_byte(&r);
    if (r.err || !c->ch_open) {
        return false;
    }

    bool ok = false;
    bool start_shell = false;
    if (tl == 7 && memcmp(type, "pty-req", 7) == 0) {
        ok = true; /* the shared console has its own size; we accept the pty */
    } else if (tl == 13 && memcmp(type, "window-change", 13) == 0) {
        ok = true; /* size change: the console is not per-endpoint, so noted only */
    } else if (tl == 5 && memcmp(type, "shell", 5) == 0) {
        ok = true;
        start_shell = !*attached;
    } else if (tl == 4 && memcmp(type, "exec", 4) == 0) {
        /* exec carries a command string; run it once, then the channel closes. */
        uint32_t       cl = 0;
        const uint8_t *cmd = rb_string(&r, &cl);
        if (!r.err && !*attached) {
            const size_t n =
                (cl < sizeof(c->exec_cmd)) ? cl : sizeof(c->exec_cmd) - 1;
            memcpy(c->exec_cmd, cmd, n);
            c->exec_cmd[n] = '\0';
            c->exec_pending = true;
        }
        ok = true;
    }

    if (want_reply) {
        (void)send_channel_u32(c, ok ? SSH_MSG_CHANNEL_SUCCESS
                                     : SSH_MSG_CHANNEL_FAILURE,
                               c->ch_peer);
    }
    if (start_shell) {
        if (ag_console_attach(&k_ssh_transport, c) == AG_OK) {
            *attached = true;
            ag_log(AG_LOG_INFO, "ssh", "shell channel attached to the console");
        }
    }
    return start_shell;
}

/* CHANNEL_DATA: the client's keystrokes go to the input ring, and its window is
 * topped back up as it is spent. */
static void on_channel_data(ssh_conn_t *c, const uint8_t *pl, size_t len)
{
    rbuf_t r;
    rb_init(&r, pl + 1, len - 1);
    (void)rb_u32_read(&r); /* recipient */
    uint32_t       dl = 0;
    const uint8_t *data = rb_string(&r, &dl);
    if (r.err || !c->ch_open) {
        return;
    }

    ag_port_mutex_take(c->io_lock, AG_PORT_FOREVER);
    for (uint32_t i = 0; i < dl; i++) {
        const size_t next = (c->in_head + 1) % SSH_IN_RING;
        if (next == c->in_tail) {
            break; /* ring full: drop (flow control should prevent this) */
        }
        c->in_ring[c->in_head] = data[i];
        c->in_head = next;
    }
    ag_port_mutex_give(c->io_lock);

    c->recv_window = (dl < c->recv_window) ? c->recv_window - dl : 0;
    if (c->recv_window < SSH_WINDOW_LOW) {
        const uint32_t add = SSH_WINDOW_INITIAL - c->recv_window;
        uint8_t        b[16];
        wbuf_t         w;
        wb_init(&w, b, sizeof(b));
        wb_byte(&w, SSH_MSG_CHANNEL_WINDOW_ADJUST);
        wb_u32(&w, c->ch_peer);
        wb_u32(&w, add);
        if (write_packet(c, b, w.len) == AG_OK) {
            c->recv_window += add;
        }
    }
}

/*
 * The connection channel service (RFC 4254), entered once a login succeeds.
 * Opens one session channel, wires it to the shared console, and pumps packets
 * until the client closes it or the link drops.
 */
/*
 * Run one `exec` command and finish the channel.  The command's console output
 * is redirected to this channel only (ssh_ch_write is a console sink), so
 * `ssh host cmd` returns exactly that command's output; then the exit status is
 * reported and the channel is closed.  The redirect is global for the brief run
 * - fine for a headless server, where nothing else is watching the screen.
 */
static void run_exec(ssh_conn_t *c)
{
    ag_console_redirect(ssh_ch_write, c);
    const int status = ag_shell_execute(c->exec_cmd);
    ag_console_redirect(NULL, NULL);
    ag_log(AG_LOG_INFO, "ssh", "exec '%s' -> %d", c->exec_cmd, status);

    uint8_t b[64];
    wbuf_t  w;
    wb_init(&w, b, sizeof(b));
    wb_byte(&w, SSH_MSG_CHANNEL_REQUEST);
    wb_u32(&w, c->ch_peer);
    wb_cstr(&w, "exit-status");
    wb_byte(&w, 0); /* want_reply: no */
    wb_u32(&w, (uint32_t)status);
    (void)write_packet(c, b, w.len);

    (void)send_channel_u32(c, SSH_MSG_CHANNEL_EOF, c->ch_peer);
    (void)send_channel_u32(c, SSH_MSG_CHANNEL_CLOSE, c->ch_peer);
    c->ch_open = false;
}

static void do_session(ssh_conn_t *c)
{
    bool attached = false;

    c->send_lock = ag_port_mutex_new();
    c->io_lock = ag_port_mutex_new();
    if (c->send_lock == NULL || c->io_lock == NULL) {
        ag_log(AG_LOG_ERROR, "ssh", "no memory for channel locks");
        goto out;
    }
    c->recv_window = SSH_WINDOW_INITIAL;
    c->in_head = 0;
    c->in_tail = 0;

    for (;;) {
        const int32_t n = read_packet(c, c->msg, sizeof(c->msg));
        if (n < 1) {
            break; /* the client hung up or the link broke */
        }
        const uint8_t type = c->msg[0];
        if (type == SSH_MSG_CHANNEL_DATA) {
            on_channel_data(c, c->msg, (size_t)n);
        } else if (type == SSH_MSG_CHANNEL_REQUEST) {
            (void)on_channel_request(c, c->msg, (size_t)n, &attached);
            if (c->exec_pending) {
                run_exec(c); /* one-shot command; the channel is done after it */
                break;
            }
        } else if (type == SSH_MSG_CHANNEL_OPEN) {
            on_channel_open(c, c->msg, (size_t)n);
        } else if (type == SSH_MSG_CHANNEL_WINDOW_ADJUST) {
            rbuf_t r;
            rb_init(&r, c->msg + 1, (size_t)n - 1);
            (void)rb_u32_read(&r);
            const uint32_t add = rb_u32_read(&r);
            if (!r.err) {
                ag_port_mutex_take(c->send_lock, AG_PORT_FOREVER);
                c->send_window += add;
                ag_port_mutex_give(c->send_lock);
            }
        } else if (type == SSH_MSG_CHANNEL_EOF) {
            /* The client will send no more; we may still be writing output. */
        } else if (type == SSH_MSG_CHANNEL_CLOSE) {
            if (c->ch_open) {
                (void)send_channel_u32(c, SSH_MSG_CHANNEL_CLOSE, c->ch_peer);
                c->ch_open = false;
            }
            break;
        } else if (type == SSH_MSG_GLOBAL_REQUEST) {
            rbuf_t r;
            rb_init(&r, c->msg + 1, (size_t)n - 1);
            uint32_t       nl = 0;
            (void)rb_string(&r, &nl);
            const uint8_t want_reply = rb_byte(&r);
            if (!r.err && want_reply) {
                const uint8_t f = SSH_MSG_REQUEST_FAILURE;
                (void)write_packet(c, &f, 1);
            }
        } else if (type == SSH_MSG_DISCONNECT) {
            break;
        }
        /* anything else: ignored, as the RFC allows for unknown channel refs */
    }

out:
    c->ch_gone = true;
    if (attached) {
        ag_console_detach(c); /* on the console lock; no more write/read after */
    }
    if (c->send_lock != NULL) {
        ag_port_mutex_free(c->send_lock);
        c->send_lock = NULL;
    }
    if (c->io_lock != NULL) {
        ag_port_mutex_free(c->io_lock);
        c->io_lock = NULL;
    }
}

/* ---- one connection ---------------------------------------------------- */

static void handle_connection(ssh_conn_t *c)
{
    (void)ag_port_net_nonblock(c->fd, true);

    static const char banner[] = SSH_VERSION "\r\n";
    if (ag_netio_send_all(c->fd, banner, sizeof(banner) - 1) != AG_OK) {
        return;
    }
    ag_netio_init(&c->rd, c->fd, c->nbuf, sizeof(c->nbuf), 0);

    if (!read_client_version(c)) {
        ag_log(AG_LOG_WARN, "ssh", "no client version");
        return;
    }

    if (send_kexinit(c) != AG_OK) {
        return;
    }

    /* Client KEXINIT - kept whole for the exchange hash. */
    const int32_t ni = read_packet(c, c->i_c, sizeof(c->i_c));
    if (ni < 1 || c->i_c[0] != SSH_MSG_KEXINIT) {
        ag_log(AG_LOG_WARN, "ssh", "expected KEXINIT");
        return;
    }
    c->i_c_len = (size_t)ni;

    /* Client KEX_ECDH_INIT.  c->msg (heap) holds the payload so do_kex's deep
     * mbedTLS calls do not share the task stack with a 4 KB buffer. */
    const int32_t nk = read_packet(c, c->msg, sizeof(c->msg));
    if (nk < 1) {
        ag_log(AG_LOG_WARN, "ssh", "no KEX_ECDH_INIT");
        return;
    }
    if (do_kex(c, c->msg, (size_t)nk) != AG_OK) {
        ag_log(AG_LOG_WARN, "ssh", "key exchange failed");
        return;
    }

    /* Client NEWKEYS (still cleartext). */
    const int32_t nn = read_packet(c, c->msg, sizeof(c->msg));
    if (nn < 1 || c->msg[0] != SSH_MSG_NEWKEYS) {
        ag_log(AG_LOG_WARN, "ssh", "expected NEWKEYS");
        return;
    }
    c->enc_in = true;

    /* First encrypted packet: SERVICE_REQUEST.  Reading it proves the keys. */
    const int32_t ns = read_packet(c, c->msg, sizeof(c->msg));
    if (ns < 1) {
        ag_log(AG_LOG_WARN, "ssh", "no service request after NEWKEYS");
        return;
    }
    if (c->msg[0] != SSH_MSG_SERVICE_REQUEST) {
        ag_log(AG_LOG_WARN, "ssh", "expected SERVICE_REQUEST, got msg %u",
               (unsigned)c->msg[0]);
        return;
    }
    uint8_t acc[64];
    wbuf_t  w;
    wb_init(&w, acc, sizeof(acc));
    wb_byte(&w, SSH_MSG_SERVICE_ACCEPT);
    wb_cstr(&w, "ssh-userauth");
    if (write_packet(c, acc, w.len) != AG_OK) {
        return;
    }

    /* Authenticate, then run the session channel: a shell over the console. */
    if (do_userauth(c)) {
        do_session(c);
    }
}

/* The struct is ~13 KB, so it lives in PSRAM; the two cipher scratch buffers
 * must be internal DMA RAM (see ssh_conn_t).  NULL if any part cannot be had. */
static ssh_conn_t *conn_alloc(void)
{
    ssh_conn_t *c = ag_port_alloc(sizeof(*c), AG_MEM_SLOW | AG_MEM_BYTE);
    if (c == NULL) {
        c = ag_port_alloc(sizeof(*c), AG_MEM_FAST | AG_MEM_BYTE);
    }
    if (c == NULL) {
        return NULL;
    }
    memset(c, 0, sizeof(*c));
    c->rx = ag_port_alloc(SSH_RECORD_BUF, AG_MEM_DMA | AG_MEM_BYTE);
    c->tx = ag_port_alloc(SSH_RECORD_BUF, AG_MEM_DMA | AG_MEM_BYTE);
    if (c->rx == NULL || c->tx == NULL) {
        ag_port_free(c->rx);
        ag_port_free(c->tx);
        ag_port_free(c);
        return NULL;
    }
    return c;
}

static void conn_free(ssh_conn_t *c)
{
    if (c == NULL) {
        return;
    }
    if (c->c_in != NULL) {
        ag_crypto_aes_ctr_free(c->c_in);
    }
    if (c->c_out != NULL) {
        ag_crypto_aes_ctr_free(c->c_out);
    }
    if (c->send_lock != NULL) {
        ag_port_mutex_free(c->send_lock);
    }
    if (c->io_lock != NULL) {
        ag_port_mutex_free(c->io_lock);
    }
    ag_port_free(c->rx);
    ag_port_free(c->tx);
    ag_port_free(c);
}

/* ---- listener task ----------------------------------------------------- */

static void ssh_task(void *arg)
{
    (void)arg;
    while (!s_stop) {
        const int fd = ag_port_net_accept(s_listen_fd, SSH_ACCEPT_MS);
        if (fd < 0) {
            continue;
        }
        ssh_conn_t *c = conn_alloc();
        if (c != NULL) {
            c->fd = fd;
            handle_connection(c);
            conn_free(c);
        } else {
            ag_log(AG_LOG_ERROR, "ssh", "out of memory for a connection");
        }
        (void)ag_port_net_close(fd);
    }
    if (s_listen_fd >= 0) {
        (void)ag_port_net_close(s_listen_fd);
        s_listen_fd = -1;
    }
    s_running = false;
    ag_port_task_delete(NULL);
}

ag_err_t ag_ssh_start(uint16_t port)
{
    if (s_running) {
        return -AG_EBUSY;
    }
    if (port == 0) {
        port = SSH_DEFAULT_PORT;
    }
    const int lfd = ag_port_net_listen(port);
    if (lfd < 0) {
        return (ag_err_t)lfd;
    }
    s_listen_fd = lfd;
    s_port = port;
    s_stop = false;
    s_running = true;
    /* A generous stack: the key exchange runs mbedTLS ECDSA and X25519, whose
     * point arithmetic is several kilobytes deep.  The per-connection buffers
     * live on the heap (ssh_conn_t) so this is headroom for the maths alone. */
    if (!ag_port_task_create(ssh_task, "ag_ssh", 16384, NULL, 6, 0, 0, &s_task)) {
        (void)ag_port_net_close(s_listen_fd);
        s_listen_fd = -1;
        s_running = false;
        return -AG_ENOMEM;
    }
    ag_log(AG_LOG_INFO, "ssh", "listening on port %u", (unsigned)port);
    return AG_OK;
}

void ag_ssh_stop(void)
{
    if (!s_running) {
        return;
    }
    s_stop = true;
    for (int i = 0; i < 50 && s_running; i++) {
        ag_port_task_delay(ag_port_ms_to_ticks(20));
    }
}

bool ag_ssh_set_cred(const char *user, const char *pass)
{
    if (user != NULL && user[0] != '\0') {
        snprintf(s_user, sizeof(s_user), "%s", user);
    } else {
        s_user[0] = '\0';
    }
    if (pass != NULL) {
        snprintf(s_pass, sizeof(s_pass), "%s", pass);
    } else {
        s_pass[0] = '\0';
    }
    return ag_ssh_have_login();
}

bool ag_ssh_have_login(void) { return want_pass()[0] != '\0'; }

bool     ag_ssh_running(void) { return s_running; }
uint16_t ag_ssh_port(void) { return s_running ? s_port : 0; }

#endif /* CONFIG_ARGON_NET_SSH */
