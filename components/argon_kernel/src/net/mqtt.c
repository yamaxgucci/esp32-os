/*
 * ArgonOS - a small MQTT 3.1.1 client, from scratch over the socket layer.
 *
 * Two shapes, which is all a shell needs: publish one message and leave, or
 * subscribe and print what arrives until Ctrl+C.  The wire protocol is tiny -
 * a one-byte type, a variable-length size, and a body - so there is no library
 * here either; it rides ag_netio like http and ftp do, and the TLS port carries
 * mqtts the same way https rides it.
 *
 *   mqtt pub <host[:port]> <topic> <message> [/tls] [/u user] [/p pass] [/id id]
 *   mqtt sub <host[:port]> <topic>           [/tls] [/u user] [/p pass] [/id id]
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/config.h>

#if defined(CONFIG_ARGON_NET_MQTT) && CONFIG_ARGON_NET_MQTT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <argon/console.h>
#include <argon/net.h>
#include <argon/path.h>
#include <argon/shell.h>

#include <argon/port/mem.h>
#include <argon/port/net.h>
#include <argon/port/random.h>
#include <argon/port/task.h>
#include <argon/port/time.h>
#include <argon/port/tls.h>

#include "net/netio.h"

#define MQTT_PORT       1883
#define MQTT_TLS_PORT   8883
#define MQTT_CONNECT_MS 15000
#define MQTT_KEEPALIVE  60      /* seconds advertised to the broker */
#define MQTT_RXBUF      2048
#define MQTT_TXBUF      1536

/* Control packet types (high nibble of byte 1). */
#define MQ_CONNECT     0x10
#define MQ_CONNACK     0x20
#define MQ_PUBLISH     0x30
#define MQ_PUBACK      0x40
#define MQ_SUBSCRIBE   0x80
#define MQ_SUBACK      0x90
#define MQ_PINGREQ     0xC0
#define MQ_PINGRESP    0xD0
#define MQ_DISCONNECT  0xE0

/* ---- one connection, plain or TLS ------------------------------------- */

typedef struct {
    int        fd;
    void      *tls;   /* NULL for plain mqtt */
    ag_netio_t rd;
    uint8_t    rxbuf[MQTT_RXBUF];
} mq_conn_t;

static ag_err_t mq_send(mq_conn_t *c, const void *buf, size_t len)
{
#if AG_PORT_HAS_TLS
    if (c->tls != NULL) {
        const uint8_t *p = (const uint8_t *)buf;
        size_t         left = len;
        const int64_t  deadline = ag_port_us() + (int64_t)MQTT_CONNECT_MS * 1000;
        while (left > 0) {
            const int32_t n = ag_port_tls_send((ag_port_tls_t)c->tls, p, left);
            if (n == -AG_EAGAIN) {
                if (ag_shell_interrupted() || ag_port_us() > deadline) {
                    return -AG_EIO;
                }
                ag_port_task_delay(ag_port_ms_to_ticks(20));
                continue;
            }
            if (n < 0) {
                return (ag_err_t)n;
            }
            p += (size_t)n;
            left -= (size_t)n;
        }
        return AG_OK;
    }
#endif
    return ag_netio_send_all(c->fd, buf, len);
}

static void mq_close(mq_conn_t *c)
{
#if AG_PORT_HAS_TLS
    if (c->tls != NULL) {
        ag_port_tls_close((ag_port_tls_t)c->tls);
        c->tls = NULL;
    }
#endif
    if (c->fd >= 0) {
        (void)ag_port_net_close(c->fd);
        c->fd = -1;
    }
}

static ag_err_t mq_open(mq_conn_t *c, const char *host, uint16_t port, bool tls)
{
    c->fd = -1;
    c->tls = NULL;

    if (tls) {
#if AG_PORT_HAS_TLS
        c->tls = ag_port_tls_connect(host, port, MQTT_CONNECT_MS);
        if (c->tls == NULL) {
            ag_console_puts("no answer (or the certificate did not check out)\n");
            return -AG_EIO;
        }
#else
        ag_console_puts("no TLS in this build (mqtts needs CONFIG_ARGON_NET_TLS)\n");
        return -AG_ENOTSUP;
#endif
    } else {
        uint32_t addr = 0;
        if (ag_net_lookup(host, &addr) != AG_OK) {
            ag_console_printf("%s: cannot be resolved\n", host);
            return -AG_EIO;
        }
        c->fd = ag_port_net_connect(addr, port, MQTT_CONNECT_MS);
        if (c->fd < 0) {
            ag_console_puts("no answer\n");
            return (ag_err_t)c->fd;
        }
        (void)ag_port_net_nonblock(c->fd, true);
    }
    ag_netio_init(&c->rd, c->fd, c->rxbuf, sizeof(c->rxbuf), 0);
    c->rd.tls = c->tls;
    return AG_OK;
}

/* ---- wire encoding ---------------------------------------------------- */

/* MQTT "remaining length": 7 bits per byte, MSB set means another follows. */
static size_t put_remlen(uint8_t *p, uint32_t v)
{
    size_t n = 0;
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        if (v > 0) {
            b |= 0x80;
        }
        p[n++] = b;
    } while (v > 0 && n < 4);
    return n;
}

static size_t put_str(uint8_t *p, const char *s)
{
    const size_t n = strlen(s);
    p[0] = (uint8_t)(n >> 8);
    p[1] = (uint8_t)n;
    memcpy(p + 2, s, n);
    return 2 + n;
}

/* ---- wire decoding ---------------------------------------------------- */

/* Read exactly n bytes, tolerating the odd read timeout mid-packet. */
static bool mq_read_full(mq_conn_t *c, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        const int32_t k = ag_netio_read(&c->rd, buf + got, n - got);
        if (k > 0) {
            got += (size_t)k;
        } else if (k == -AG_ETIMEDOUT) {
            if (ag_shell_interrupted()) {
                return false;
            }
            continue; /* the rest of the packet is just slow */
        } else {
            return false; /* eof, interrupt, or error */
        }
    }
    return true;
}

/*
 * Read a whole packet.  Returns the type byte (0x10..0xE0) with its flags, sets
 * *body_len, and fills body.  0 on Ctrl+C or a clean close, negative on error.
 * `first_wait` bounds how long to wait for the first byte (a subscribe loop
 * pings when it expires); the body then follows without a long wait.
 */
static int32_t mq_read_packet(mq_conn_t *c, uint8_t *body, size_t cap,
                              size_t *body_len)
{
    uint8_t hdr;
    const int32_t k = ag_netio_read(&c->rd, &hdr, 1);
    if (k == 0 || k == -AG_EINTR) {
        return 0;
    }
    if (k == -AG_ETIMEDOUT) {
        return -AG_ETIMEDOUT;
    }
    if (k < 0) {
        return k;
    }

    uint32_t rem = 0;
    uint32_t mul = 1;
    for (int i = 0; i < 4; i++) {
        uint8_t b;
        if (!mq_read_full(c, &b, 1)) {
            return -AG_EIO;
        }
        rem += (uint32_t)(b & 0x7F) * mul;
        if ((b & 0x80) == 0) {
            break;
        }
        mul <<= 7;
    }
    if (rem > cap) {
        return -AG_ERANGE;
    }
    if (rem > 0 && !mq_read_full(c, body, rem)) {
        return -AG_EIO;
    }
    *body_len = rem;
    return hdr;
}

/* ---- CONNECT + CONNACK ------------------------------------------------- */

static ag_err_t mq_connect(mq_conn_t *c, const char *client_id,
                           const char *user, const char *pass)
{
    uint8_t pkt[MQTT_TXBUF];
    uint8_t vp[MQTT_TXBUF]; /* variable header + payload */
    size_t  o = 0;

    /* Variable header: protocol name, level, flags, keepalive. */
    o += put_str(vp + o, "MQTT");
    vp[o++] = 0x04; /* protocol level 4 = MQTT 3.1.1 */
    uint8_t flags = 0x02; /* clean session */
    if (user != NULL) {
        flags |= 0x80;
    }
    if (pass != NULL) {
        flags |= 0x40;
    }
    vp[o++] = flags;
    vp[o++] = (uint8_t)(MQTT_KEEPALIVE >> 8);
    vp[o++] = (uint8_t)MQTT_KEEPALIVE;

    /* Payload: client id, then username and password if present. */
    o += put_str(vp + o, client_id);
    if (user != NULL) {
        o += put_str(vp + o, user);
    }
    if (pass != NULL) {
        o += put_str(vp + o, pass);
    }

    size_t h = 0;
    pkt[h++] = MQ_CONNECT;
    h += put_remlen(pkt + h, (uint32_t)o);
    if (h + o > sizeof(pkt)) {
        return -AG_ERANGE;
    }
    memcpy(pkt + h, vp, o);
    if (mq_send(c, pkt, h + o) != AG_OK) {
        return -AG_EIO;
    }

    uint8_t body[8];
    size_t  blen = 0;
    const int32_t t = mq_read_packet(c, body, sizeof(body), &blen);
    if (t < 0 || (t & 0xF0) != MQ_CONNACK || blen < 2) {
        ag_console_puts("no CONNACK from the broker\n");
        return -AG_EIO;
    }
    if (body[1] != 0x00) {
        ag_console_printf("broker refused the connection (code %u)\n",
                          (unsigned)body[1]);
        return -AG_EACCES;
    }
    return AG_OK;
}

/* ---- publish ----------------------------------------------------------- */

static ag_err_t mq_publish(mq_conn_t *c, const char *topic, const char *msg,
                           bool retain)
{
    uint8_t pkt[MQTT_TXBUF];
    size_t  o = 0;
    pkt[o++] = MQ_PUBLISH | (retain ? 0x01 : 0x00); /* QoS 0, no dup */

    const size_t tlen = strlen(topic);
    const size_t mlen = strlen(msg);
    const size_t rem = 2 + tlen + mlen; /* topic string + payload */

    size_t h = 1;
    h += put_remlen(pkt + h, (uint32_t)rem);
    if (h + rem > sizeof(pkt)) {
        ag_console_puts("message too large for this build\n");
        return -AG_ERANGE;
    }
    o = h;
    o += put_str(pkt + o, topic);
    memcpy(pkt + o, msg, mlen);
    o += mlen;
    return mq_send(c, pkt, o);
}

/* ---- subscribe + receive loop ------------------------------------------ */

static ag_err_t mq_subscribe(mq_conn_t *c, const char *topic)
{
    uint8_t pkt[MQTT_TXBUF];
    const size_t tlen = strlen(topic);
    const size_t rem = 2 + (2 + tlen) + 1; /* packet id + topic + qos byte */
    size_t h = 0;
    pkt[h++] = MQ_SUBSCRIBE | 0x02; /* SUBSCRIBE reserved bits = 0010 */
    h += put_remlen(pkt + h, (uint32_t)rem);
    if (h + rem > sizeof(pkt)) {
        return -AG_ERANGE;
    }
    pkt[h++] = 0x00; /* packet id high */
    pkt[h++] = 0x01; /* packet id low  */
    h += put_str(pkt + h, topic);
    pkt[h++] = 0x00; /* requested QoS 0 */
    if (mq_send(c, pkt, h) != AG_OK) {
        return -AG_EIO;
    }

    uint8_t body[8];
    size_t  blen = 0;
    const int32_t t = mq_read_packet(c, body, sizeof(body), &blen);
    if (t < 0 || (t & 0xF0) != MQ_SUBACK) {
        ag_console_puts("no SUBACK from the broker\n");
        return -AG_EIO;
    }
    if (blen >= 3 && body[2] == 0x80) {
        ag_console_puts("the broker refused the subscription\n");
        return -AG_EACCES;
    }
    return AG_OK;
}

/* Print one PUBLISH: "topic  payload". */
static void mq_print_publish(const uint8_t *body, size_t blen)
{
    if (blen < 2) {
        return;
    }
    const size_t tlen = ((size_t)body[0] << 8) | body[1];
    if (2 + tlen > blen) {
        return;
    }
    const uint8_t *payload = body + 2 + tlen;
    size_t         plen = blen - 2 - tlen;
    /* QoS 0 has no packet id, which is all we subscribe at, so the payload
     * starts right after the topic. */
    ag_console_printf("%.*s  ", (int)tlen, (const char *)body);
    ag_console_write((const char *)payload, plen);
    ag_console_puts("\n");
}

static ag_err_t mq_receive_loop(mq_conn_t *c)
{
    ag_console_puts("subscribed; Ctrl+C to stop\n");
    uint8_t *body = ag_port_alloc(MQTT_RXBUF, AG_MEM_SLOW | AG_MEM_BYTE);
    if (body == NULL) {
        body = ag_port_alloc(MQTT_RXBUF, AG_MEM_FAST | AG_MEM_BYTE);
    }
    if (body == NULL) {
        return -AG_ENOMEM;
    }

    ag_err_t rc = AG_OK;
    for (;;) {
        if (ag_shell_interrupted()) {
            break;
        }
        size_t        blen = 0;
        const int32_t t = mq_read_packet(c, body, MQTT_RXBUF, &blen);
        if (t == -AG_ETIMEDOUT) {
            const uint8_t ping[2] = {MQ_PINGREQ, 0x00};
            if (mq_send(c, ping, sizeof(ping)) != AG_OK) {
                break;
            }
            continue;
        }
        if (t <= 0) {
            break; /* Ctrl+C, close, or error */
        }
        const uint8_t type = (uint8_t)(t & 0xF0);
        if (type == MQ_PUBLISH) {
            mq_print_publish(body, blen);
        } else if (type == MQ_PINGRESP || type == MQ_PUBACK ||
                   type == MQ_SUBACK) {
            /* housekeeping, nothing to show */
        }
    }
    ag_port_free(body);
    const uint8_t disc[2] = {MQ_DISCONNECT, 0x00};
    (void)mq_send(c, disc, sizeof(disc));
    return rc;
}

/* ---- the shell command ------------------------------------------------- */

/* Split "host" or "host:port"; returns the port or the default. */
static uint16_t split_host_port(char *hostport, uint16_t def)
{
    char *colon = strrchr(hostport, ':');
    if (colon == NULL) {
        return def;
    }
    *colon = '\0';
    const int p = atoi(colon + 1);
    return (p > 0 && p < 65536) ? (uint16_t)p : def;
}

int ag_cmd_mqtt(int argc, char **argv)
{
    if (argc < 4) {
        ag_console_puts(
            "usage: mqtt pub <host[:port]> <topic> <message> [/tls] [/u user] [/p pass] [/id id]\n"
            "       mqtt sub <host[:port]> <topic> [/tls] [/u user] [/p pass] [/id id]\n");
        return 1;
    }

    const bool is_pub = (ag_path_icmp(argv[1], "pub") == 0);
    const bool is_sub = (ag_path_icmp(argv[1], "sub") == 0);
    if (!is_pub && !is_sub) {
        ag_console_puts("mqtt: first word is 'pub' or 'sub'\n");
        return 1;
    }
    if (is_pub && argc < 5) {
        ag_console_puts("mqtt pub needs a message\n");
        return 1;
    }

    char        hostbuf[128];
    const char *topic = argv[3];
    const char *msg = is_pub ? argv[4] : NULL;
    snprintf(hostbuf, sizeof(hostbuf), "%s", argv[2]);

    bool        tls = false;
    bool        retain = false;
    const char *user = NULL;
    const char *pass = NULL;
    const char *id = NULL;
    for (int i = is_pub ? 5 : 4; i < argc; i++) {
        if (ag_path_icmp(argv[i], "/tls") == 0) {
            tls = true;
        } else if (ag_path_icmp(argv[i], "/retain") == 0) {
            retain = true;
        } else if (ag_path_icmp(argv[i], "/u") == 0 && i + 1 < argc) {
            user = argv[++i];
        } else if (ag_path_icmp(argv[i], "/p") == 0 && i + 1 < argc) {
            pass = argv[++i];
        } else if (ag_path_icmp(argv[i], "/id") == 0 && i + 1 < argc) {
            id = argv[++i];
        }
    }

    const uint16_t port =
        split_host_port(hostbuf, tls ? MQTT_TLS_PORT : MQTT_PORT);

    char idbuf[24];
    if (id == NULL) {
        uint8_t r[3];
        ag_port_random(r, sizeof(r));
        snprintf(idbuf, sizeof(idbuf), "argon-%02x%02x%02x", r[0], r[1], r[2]);
        id = idbuf;
    }

    mq_conn_t *c = ag_port_alloc(sizeof(*c), AG_MEM_SLOW | AG_MEM_BYTE);
    if (c == NULL) {
        c = ag_port_alloc(sizeof(*c), AG_MEM_FAST | AG_MEM_BYTE);
    }
    if (c == NULL) {
        ag_console_puts("out of memory\n");
        return 1;
    }

    ag_console_printf("%s:%u ... ", hostbuf, (unsigned)port);
    int rc = 1;
    if (mq_open(c, hostbuf, port, tls) != AG_OK) {
        ag_port_free(c);
        return 1;
    }
    if (mq_connect(c, id, user, pass) != AG_OK) {
        mq_close(c);
        ag_port_free(c);
        return 1;
    }
    ag_console_puts("connected\n");

    if (is_pub) {
        if (mq_publish(c, topic, msg, retain) == AG_OK) {
            ag_console_printf("published to %s\n", topic);
            rc = 0;
        } else {
            ag_console_puts("publish failed\n");
        }
        const uint8_t disc[2] = {MQ_DISCONNECT, 0x00};
        (void)mq_send(c, disc, sizeof(disc));
    } else {
        if (mq_subscribe(c, topic) == AG_OK) {
            (void)mq_receive_loop(c);
            rc = 0;
        } else {
            ag_console_puts("subscribe failed\n");
        }
    }

    mq_close(c);
    ag_port_free(c);
    return rc;
}

#endif /* CONFIG_ARGON_NET_MQTT */
