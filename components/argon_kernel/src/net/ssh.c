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
 * At the end the encrypted channel is proven live by reading the client's
 * SERVICE_REQUEST and answering SERVICE_ACCEPT.  Authentication (a real login)
 * and the session channel are the next milestones; for now the connection stops
 * there.  A real `ssh -v` reaches "SSH2_MSG_NEWKEYS received" and then waits for
 * the userauth banner, which is the expected end of milestone 2.
 *
 * All the maths is behind argon/port/crypto.h - the kernel does the protocol,
 * the port does the primitives on mbedTLS.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/ssh.h>

#if defined(CONFIG_ARGON_NET_SSH) && CONFIG_ARGON_NET_SSH

#include <stdlib.h>
#include <string.h>

#include <argon/log.h>
#include <argon/vfs.h>

#include <argon/port/crypto.h>
#include <argon/port/net.h>
#include <argon/port/random.h>
#include <argon/port/task.h>
#include <argon/port/time.h>

#include "net/netio.h"

#define SSH_DEFAULT_PORT 22
#define SSH_ACCEPT_MS    400
#define SSH_VERSION      "SSH-2.0-ArgonOS_0.1"

#define SSH_MSG_DISCONNECT      1
#define SSH_MSG_SERVICE_REQUEST 5
#define SSH_MSG_SERVICE_ACCEPT  6
#define SSH_MSG_KEXINIT         20
#define SSH_MSG_NEWKEYS         21
#define SSH_MSG_KEX_ECDH_INIT   30
#define SSH_MSG_KEX_ECDH_REPLY  31

#define SSH_MAX_PACKET   4096 /* handshake and control packets are small */
#define SSH_HOSTKEY_PATH "/sys/SSH_HOST.KEY"

static const char SSH_HOSTKEY_ID[] = "ecdsa-sha2-nistp256";

static volatile bool  s_running;
static volatile bool  s_stop;
static int            s_listen_fd = -1;
static uint16_t       s_port;
static ag_port_task_t s_task;

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

    /* record scratch: [0..3] holds the sequence number for the MAC, the packet
     * follows at +4 so hmac covers seq||packet in one contiguous call.  rx/tx
     * are the assembled packet; nbuf is the raw socket buffer netio reads into,
     * kept separate so decrypting into rx never clobbers bytes still queued. */
    uint8_t     nbuf[SSH_MAX_PACKET];
    uint8_t     rx[4 + SSH_MAX_PACKET + 16];
    uint8_t     tx[4 + SSH_MAX_PACKET + 16];
} ssh_conn_t;

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
static ag_err_t write_packet(ssh_conn_t *c, const uint8_t *payload, size_t len)
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

    /* Client KEX_ECDH_INIT. */
    uint8_t pl[SSH_MAX_PACKET];
    const int32_t nk = read_packet(c, pl, sizeof(pl));
    if (nk < 1) {
        ag_log(AG_LOG_WARN, "ssh", "no KEX_ECDH_INIT");
        return;
    }
    if (do_kex(c, pl, (size_t)nk) != AG_OK) {
        ag_log(AG_LOG_WARN, "ssh", "key exchange failed");
        return;
    }

    /* Client NEWKEYS (still cleartext). */
    const int32_t nn = read_packet(c, pl, sizeof(pl));
    if (nn < 1 || pl[0] != SSH_MSG_NEWKEYS) {
        ag_log(AG_LOG_WARN, "ssh", "expected NEWKEYS");
        return;
    }
    c->enc_in = true;

    /* First encrypted packet: SERVICE_REQUEST.  Reading it proves the keys. */
    const int32_t ns = read_packet(c, pl, sizeof(pl));
    if (ns < 1) {
        ag_log(AG_LOG_WARN, "ssh", "no service request after NEWKEYS");
        return;
    }
    if (pl[0] == SSH_MSG_SERVICE_REQUEST) {
        uint8_t acc[64];
        wbuf_t  w;
        wb_init(&w, acc, sizeof(acc));
        wb_byte(&w, SSH_MSG_SERVICE_ACCEPT);
        wb_cstr(&w, "ssh-userauth");
        (void)write_packet(c, acc, w.len);
        ag_log(AG_LOG_INFO, "ssh",
               "encrypted channel up (aes256-ctr); userauth is the next step");
    } else {
        ag_log(AG_LOG_INFO, "ssh", "encrypted packet type %u received",
               (unsigned)pl[0]);
    }
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
    free(c);
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
        ssh_conn_t *c = calloc(1, sizeof(*c));
        if (c != NULL) {
            c->fd = fd;
            handle_connection(c);
            conn_free(c);
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
    /* A larger stack than telnet: the KEX arithmetic and mbedTLS want room. */
    if (!ag_port_task_create(ssh_task, "ag_ssh", 12288, NULL, 6, 0, 0, &s_task)) {
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

bool     ag_ssh_running(void) { return s_running; }
uint16_t ag_ssh_port(void) { return s_running ? s_port : 0; }

#endif /* CONFIG_ARGON_NET_SSH */
