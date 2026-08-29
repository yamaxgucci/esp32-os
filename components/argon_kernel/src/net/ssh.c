/*
 * ArgonOS - SSH server, milestone 1: the transport handshake.
 *
 * A listener on port 22.  For each connection: exchange version banners, then
 * exchange SSH_MSG_KEXINIT (algorithm negotiation).  That is as far as this
 * milestone goes - the actual key exchange (curve25519), the host key and the
 * ciphers are the next step, so after KEXINIT this logs what was negotiated and
 * closes.  A real `ssh -v` reaches "SSH2_MSG_KEXINIT sent/received" and then
 * fails at key exchange, which is the expected end of milestone 1.
 *
 * The binary packet protocol here is the cleartext form (RFC 4253 sec 6): no
 * MAC, block size 8, used only until NEWKEYS - which milestone 2 adds.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/ssh.h>

#if defined(CONFIG_ARGON_NET_SSH) && CONFIG_ARGON_NET_SSH

#include <string.h>

#include <argon/log.h>

#include <argon/port/net.h>
#include <argon/port/random.h>
#include <argon/port/task.h>
#include <argon/port/time.h>

#include "net/netio.h"

#define SSH_DEFAULT_PORT 22
#define SSH_ACCEPT_MS    400
#define SSH_VERSION      "SSH-2.0-ArgonOS_0.1"
#define SSH_MSG_KEXINIT  20

#define SSH_MAX_PACKET   4096 /* cleartext handshake packets are small */

static volatile bool  s_running;
static volatile bool  s_stop;
static int            s_listen_fd = -1;
static uint16_t       s_port;
static ag_port_task_t s_task;

/* The algorithms this server will speak.  Milestone 1 only advertises them;
 * milestone 2 implements the chosen ones.  aes256-ctr + hmac-sha2-256 is the
 * classic pairing that is simplest to build on mbedTLS next. */
static const char *const NL_KEX = "curve25519-sha256";
static const char *const NL_HOSTKEY = "rsa-sha2-256";
static const char *const NL_ENC = "aes256-ctr";
static const char *const NL_MAC = "hmac-sha2-256";
static const char *const NL_COMP = "none";

/* ---- raw socket helpers ------------------------------------------------ */

static bool read_full(ag_netio_t *r, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        const int32_t k = ag_netio_read(r, buf + got, n - got);
        if (k <= 0) {
            return false; /* eof, error or interrupt */
        }
        got += (size_t)k;
    }
    return true;
}

/* ---- version exchange -------------------------------------------------- */

/* Read the client's identification line (may be preceded by other CRLF lines,
 * RFC 4253 sec 4.2).  Returns false on error. */
static bool read_client_version(ag_netio_t *r)
{
    char line[256];
    for (int tries = 0; tries < 8; tries++) {
        if (ag_netio_line(r, line, sizeof(line)) != AG_OK) {
            return false;
        }
        if (strncmp(line, "SSH-2.0-", 8) == 0 ||
            strncmp(line, "SSH-1.99-", 9) == 0) {
            ag_log(AG_LOG_INFO, "ssh", "client is %s", line);
            return true;
        }
        /* Anything else before the banner is informational; keep looking. */
    }
    return false;
}

/* ---- binary packet protocol (cleartext) -------------------------------- */

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

/* Read one cleartext packet's payload into buf.  Returns payload length, or
 * negative on error. */
static int32_t read_packet(ag_netio_t *r, uint8_t *buf, size_t cap)
{
    uint8_t hdr[5];
    if (!read_full(r, hdr, 5)) {
        return -AG_EIO;
    }
    const uint32_t pkt_len = rd_u32(hdr);
    const uint8_t  pad_len = hdr[4];
    if (pkt_len < 2 || pkt_len > SSH_MAX_PACKET) {
        return -AG_EFORMAT;
    }
    const uint32_t payload_len = pkt_len - pad_len - 1;
    if (payload_len > cap) {
        return -AG_ERANGE;
    }
    if (!read_full(r, buf, payload_len)) {
        return -AG_EIO;
    }
    uint8_t pad[256];
    if (pad_len > 0 && !read_full(r, pad, pad_len)) {
        return -AG_EIO;
    }
    return (int32_t)payload_len;
}

/* Send one cleartext packet.  Padding is 4..255 so the whole record is a
 * multiple of 8, per RFC 4253. */
static ag_err_t write_packet(int fd, const uint8_t *payload, size_t len)
{
    uint8_t out[SSH_MAX_PACKET + 64];
    size_t  need = 4 + 1 + len;
    size_t  pad = 8 - (need % 8);
    if (pad < 4) {
        pad += 8;
    }
    const uint32_t pkt_len = (uint32_t)(1 + len + pad);
    if (4 + pkt_len > sizeof(out)) {
        return -AG_ERANGE;
    }
    wr_u32(out, pkt_len);
    out[4] = (uint8_t)pad;
    memcpy(out + 5, payload, len);
    ag_port_random(out + 5 + len, pad);
    return ag_netio_send_all(fd, out, 4 + (size_t)pkt_len);
}

/* Append a name-list (uint32 length + string) to a buffer. */
static size_t put_namelist(uint8_t *p, const char *s)
{
    const size_t n = strlen(s);
    wr_u32(p, (uint32_t)n);
    memcpy(p + 4, s, n);
    return 4 + n;
}

static ag_err_t send_kexinit(int fd)
{
    uint8_t pl[512];
    size_t  o = 0;
    pl[o++] = SSH_MSG_KEXINIT;
    ag_port_random(pl + o, 16); /* cookie */
    o += 16;
    o += put_namelist(pl + o, NL_KEX);
    o += put_namelist(pl + o, NL_HOSTKEY);
    o += put_namelist(pl + o, NL_ENC); /* enc c2s */
    o += put_namelist(pl + o, NL_ENC); /* enc s2c */
    o += put_namelist(pl + o, NL_MAC); /* mac c2s */
    o += put_namelist(pl + o, NL_MAC); /* mac s2c */
    o += put_namelist(pl + o, NL_COMP); /* comp c2s */
    o += put_namelist(pl + o, NL_COMP); /* comp s2c */
    o += put_namelist(pl + o, "");      /* lang c2s */
    o += put_namelist(pl + o, "");      /* lang s2c */
    pl[o++] = 0;                        /* first_kex_packet_follows */
    wr_u32(pl + o, 0);                  /* reserved */
    o += 4;
    return write_packet(fd, pl, o);
}

/* ---- one connection ---------------------------------------------------- */

static void handle_connection(int fd)
{
    (void)ag_port_net_nonblock(fd, true);

    /* Version exchange: send ours, read theirs. */
    static const char banner[] = SSH_VERSION "\r\n";
    if (ag_netio_send_all(fd, banner, sizeof(banner) - 1) != AG_OK) {
        return;
    }

    uint8_t     rxbuf[SSH_MAX_PACKET];
    ag_netio_t  rdr;
    ag_netio_init(&rdr, fd, rxbuf, sizeof(rxbuf), 0);

    if (!read_client_version(&rdr)) {
        ag_log(AG_LOG_WARN, "ssh", "no client version");
        return;
    }

    /* KEXINIT both ways. */
    if (send_kexinit(fd) != AG_OK) {
        return;
    }
    uint8_t       pl[SSH_MAX_PACKET];
    const int32_t n = read_packet(&rdr, pl, sizeof(pl));
    if (n < 1) {
        ag_log(AG_LOG_WARN, "ssh", "no kexinit from client");
        return;
    }
    if (pl[0] != SSH_MSG_KEXINIT) {
        ag_log(AG_LOG_WARN, "ssh", "expected KEXINIT, got msg %u",
               (unsigned)pl[0]);
        return;
    }

    /* Milestone 1 ends here: the handshake is framed and algorithms are
     * negotiated.  Key exchange is milestone 2. */
    ag_log(AG_LOG_INFO, "ssh",
           "KEXINIT exchanged (%d bytes); key exchange not implemented yet",
           (int)n);
}

/* ---- listener task ----------------------------------------------------- */

static void ssh_task(void *arg)
{
    (void)arg;
    while (!s_stop) {
        const int fd = ag_port_net_accept(s_listen_fd, SSH_ACCEPT_MS);
        if (fd < 0) {
            continue; /* timeout or transient */
        }
        handle_connection(fd);
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
    if (!ag_port_task_create(ssh_task, "ag_ssh", 8192, NULL, 6, 0, 0, &s_task)) {
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
