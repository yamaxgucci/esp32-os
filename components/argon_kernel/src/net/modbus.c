/*
 * ArgonOS - a Modbus TCP client (master), from scratch over the socket layer.
 *
 * Modbus TCP is a seven-byte MBAP header and a short PDU - a function code and
 * a few registers - so, like MQTT, there is no library: it rides ag_netio and
 * speaks the wire directly.  Enough of it to read and write the holding and
 * input registers a PLC or a sensor gateway exposes:
 *
 *   modbus read  <host[:port]> <addr> [count] [/unit N] [/input]
 *   modbus write <host[:port]> <addr> <value> [value...] [/unit N]
 *
 * read uses function 3 (holding) or 4 (input, with /input); write uses 6 for
 * one register and 16 for several.  Default port 502, unit 1.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/config.h>

#if defined(CONFIG_ARGON_NET_MODBUS) && CONFIG_ARGON_NET_MODBUS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <argon/console.h>
#include <argon/net.h>
#include <argon/path.h>

#include <argon/port/net.h>

#include "net/netio.h"

#define MB_PORT       502
#define MB_CONNECT_MS 10000
#define MB_MAX_REGS   125    /* function 3/4 read limit */

#define MB_FC_READ_HOLDING 0x03
#define MB_FC_READ_INPUT   0x04
#define MB_FC_WRITE_SINGLE 0x06
#define MB_FC_WRITE_MULTI  0x10

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

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

/*
 * One request/response exchange.  `pdu`/`pdu_len` is the function code and its
 * data; the reply's PDU is returned in `resp` (up to `cap`), its length in
 * `resp_len`.  Returns AG_OK, or prints and returns an error - including a
 * Modbus exception, which arrives as the function code with its top bit set.
 */
static ag_err_t mb_exchange(int fd, ag_netio_t *rd, uint8_t unit,
                            const uint8_t *pdu, size_t pdu_len, uint8_t *resp,
                            size_t cap, size_t *resp_len)
{
    uint8_t req[7 + 256];
    if (pdu_len > 256) {
        return -AG_ERANGE;
    }
    wr16(req + 0, 1);                       /* transaction id */
    wr16(req + 2, 0);                       /* protocol id (0 = Modbus) */
    wr16(req + 4, (uint16_t)(1 + pdu_len)); /* length: unit + pdu */
    req[6] = unit;
    memcpy(req + 7, pdu, pdu_len);
    if (ag_netio_send_all(fd, req, 7 + pdu_len) != AG_OK) {
        ag_console_puts("send failed\n");
        return -AG_EIO;
    }

    uint8_t mbap[7];
    if (!read_full(rd, mbap, sizeof(mbap))) {
        ag_console_puts("no reply\n");
        return -AG_EIO;
    }
    const uint16_t len = rd16(mbap + 4);
    if (len < 2 || (size_t)(len - 1) > cap) {
        return -AG_EFORMAT;
    }
    const size_t plen = (size_t)(len - 1); /* PDU length (len counts unit id) */
    if (!read_full(rd, resp, plen)) {
        ag_console_puts("short reply\n");
        return -AG_EIO;
    }
    if ((resp[0] & 0x80) != 0) {
        ag_console_printf("modbus exception %u (function 0x%02x)\n",
                          plen > 1 ? (unsigned)resp[1] : 0u,
                          (unsigned)(resp[0] & 0x7F));
        return -AG_EIO;
    }
    *resp_len = plen;
    return AG_OK;
}

/* ---- the shell command ------------------------------------------------- */

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

int ag_cmd_modbus(int argc, char **argv)
{
    if (argc < 4) {
        ag_console_puts(
            "usage: modbus read  <host[:port]> <addr> [count] [/unit N] [/input]\n"
            "       modbus write <host[:port]> <addr> <value> [value...] [/unit N]\n");
        return 1;
    }
    const bool is_read = (ag_path_icmp(argv[1], "read") == 0);
    const bool is_write = (ag_path_icmp(argv[1], "write") == 0);
    if (!is_read && !is_write) {
        ag_console_puts("modbus: first word is 'read' or 'write'\n");
        return 1;
    }

    char hostbuf[128];
    snprintf(hostbuf, sizeof(hostbuf), "%s", argv[2]);
    const uint16_t addr = (uint16_t)strtoul(argv[3], NULL, 0);

    /* Collect the numeric args (count for read, values for write) and options. */
    uint8_t  unit = 1;
    bool     input = false;
    uint16_t vals[MB_MAX_REGS];
    int      nvals = 0;
    for (int i = 4; i < argc; i++) {
        if (ag_path_icmp(argv[i], "/unit") == 0 && i + 1 < argc) {
            unit = (uint8_t)atoi(argv[++i]);
        } else if (ag_path_icmp(argv[i], "/input") == 0) {
            input = true;
        } else if (argv[i][0] != '/' && nvals < MB_MAX_REGS) {
            vals[nvals++] = (uint16_t)strtoul(argv[i], NULL, 0);
        }
    }

    const uint16_t port = split_host_port(hostbuf, MB_PORT);
    uint32_t netaddr = 0;
    if (ag_net_lookup(hostbuf, &netaddr) != AG_OK) {
        ag_console_printf("%s: cannot be resolved\n", hostbuf);
        return 1;
    }
    ag_console_printf("%s:%u ... ", hostbuf, (unsigned)port);
    const int fd = ag_port_net_connect(netaddr, port, MB_CONNECT_MS);
    if (fd < 0) {
        ag_console_puts("no answer\n");
        return 1;
    }
    (void)ag_port_net_nonblock(fd, true);

    uint8_t    rxbuf[300];
    ag_netio_t rd;
    ag_netio_init(&rd, fd, rxbuf, sizeof(rxbuf), 0);
    ag_console_puts("connected\n");

    uint8_t  pdu[256];
    uint8_t  resp[256];
    size_t   rlen = 0;
    ag_err_t err = -AG_EINVAL;
    int      rc = 1;

    if (is_read) {
        uint16_t count = (nvals >= 1) ? vals[0] : 1;
        if (count < 1 || count > MB_MAX_REGS) {
            count = 1;
        }
        pdu[0] = input ? MB_FC_READ_INPUT : MB_FC_READ_HOLDING;
        wr16(pdu + 1, addr);
        wr16(pdu + 3, count);
        err = mb_exchange(fd, &rd, unit, pdu, 5, resp, sizeof(resp), &rlen);
        if (err == AG_OK && rlen >= 2) {
            const uint8_t bc = resp[1];
            const int     regs = bc / 2;
            for (int i = 0; i < regs && (size_t)(2 + i * 2 + 1) < rlen + 1; i++) {
                const uint16_t v = rd16(resp + 2 + i * 2);
                ag_console_printf("  [%u] %u  (0x%04x)\n",
                                  (unsigned)(addr + i), (unsigned)v,
                                  (unsigned)v);
            }
            rc = 0;
        }
    } else if (nvals == 1) {
        pdu[0] = MB_FC_WRITE_SINGLE;
        wr16(pdu + 1, addr);
        wr16(pdu + 3, vals[0]);
        err = mb_exchange(fd, &rd, unit, pdu, 5, resp, sizeof(resp), &rlen);
        if (err == AG_OK) {
            ag_console_printf("wrote %u to [%u]\n", (unsigned)vals[0],
                              (unsigned)addr);
            rc = 0;
        }
    } else if (nvals > 1) {
        pdu[0] = MB_FC_WRITE_MULTI;
        wr16(pdu + 1, addr);
        wr16(pdu + 3, (uint16_t)nvals);
        pdu[5] = (uint8_t)(nvals * 2);
        for (int i = 0; i < nvals; i++) {
            wr16(pdu + 6 + i * 2, vals[i]);
        }
        err = mb_exchange(fd, &rd, unit, pdu, 6 + (size_t)nvals * 2, resp,
                          sizeof(resp), &rlen);
        if (err == AG_OK) {
            ag_console_printf("wrote %d registers from [%u]\n", nvals,
                              (unsigned)addr);
            rc = 0;
        }
    } else {
        ag_console_puts("modbus write needs a value\n");
    }

    (void)ag_port_net_close(fd);
    return rc;
}

#endif /* CONFIG_ARGON_NET_MODBUS */
