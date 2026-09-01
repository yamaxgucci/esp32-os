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

#include <argon/netprov.h>
#include <argon/port/net.h>
#include <argon/port/uart.h>

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

/* ---- Modbus RTU (serial) ---------------------------------------------- */

/* The Modbus CRC-16 (polynomial 0xA001), low byte first on the wire. */
static uint16_t mb_crc(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

/*
 * One RTU request/response over a UART.  Frame is slave + PDU + CRC16; the
 * reply is read until the line goes quiet (a short read timeout stands in for
 * the 3.5-character gap that ends a frame), then the CRC is checked and the PDU
 * (function code and data) is returned - the same shape mb_exchange gives, so
 * the caller parses reads and writes identically.
 */
static ag_err_t mb_rtu_exchange(int port, uint8_t slave, const uint8_t *pdu,
                                size_t pdu_len, uint8_t *resp, size_t cap,
                                size_t *resp_len)
{
    uint8_t frame[260];
    if (pdu_len + 3 > sizeof(frame)) {
        return -AG_ERANGE;
    }
    frame[0] = slave;
    memcpy(frame + 1, pdu, pdu_len);
    const uint16_t crc = mb_crc(frame, 1 + pdu_len);
    frame[1 + pdu_len] = (uint8_t)crc;         /* low byte first */
    frame[2 + pdu_len] = (uint8_t)(crc >> 8);

    (void)ag_port_uart_flush(port);
    if (ag_port_uart_write(port, frame, 3 + pdu_len) < 0) {
        ag_console_puts("send failed\n");
        return -AG_EIO;
    }

    /* Read the reply: wait up to 1 s for the first byte, then read on until the
     * line is quiet for a beat. */
    uint8_t buf[260];
    size_t  got = 0;
    int32_t k = ag_port_uart_read(port, buf, sizeof(buf), 1000);
    if (k <= 0) {
        ag_console_puts("no reply\n");
        return -AG_EIO;
    }
    got = (size_t)k;
    for (;;) {
        k = ag_port_uart_read(port, buf + got, sizeof(buf) - got, 20);
        if (k <= 0) {
            break; /* the gap that ends the frame */
        }
        got += (size_t)k;
        if (got >= sizeof(buf)) {
            break;
        }
    }

    if (got < 5) {
        ag_console_puts("short reply\n");
        return -AG_EIO;
    }
    const uint16_t rx_crc = (uint16_t)(buf[got - 2] | (buf[got - 1] << 8));
    if (mb_crc(buf, got - 2) != rx_crc) {
        ag_console_puts("bad CRC in reply\n");
        return -AG_EFORMAT;
    }
    if (buf[0] != slave) {
        ag_console_puts("reply from a different slave\n");
        return -AG_EFORMAT;
    }
    const size_t plen = got - 1 - 2; /* strip slave and CRC -> the PDU */
    if (plen > cap) {
        return -AG_ERANGE;
    }
    if ((buf[1] & 0x80) != 0) {
        ag_console_printf("modbus exception %u (function 0x%02x)\n",
                          plen > 1 ? (unsigned)buf[2] : 0u,
                          (unsigned)(buf[1] & 0x7F));
        return -AG_EIO;
    }
    memcpy(resp, buf + 1, plen);
    *resp_len = plen;
    return AG_OK;
}

/* Build the request PDU (function code + data) shared by TCP and RTU. */
static size_t mb_build(bool is_read, bool input, uint16_t addr,
                       const uint16_t *vals, int nvals, uint8_t *pdu)
{
    if (is_read) {
        uint16_t count = (nvals >= 1) ? vals[0] : 1;
        if (count < 1 || count > MB_MAX_REGS) {
            count = 1;
        }
        pdu[0] = input ? MB_FC_READ_INPUT : MB_FC_READ_HOLDING;
        wr16(pdu + 1, addr);
        wr16(pdu + 3, count);
        return 5;
    }
    if (nvals == 1) {
        pdu[0] = MB_FC_WRITE_SINGLE;
        wr16(pdu + 1, addr);
        wr16(pdu + 3, vals[0]);
        return 5;
    }
    pdu[0] = MB_FC_WRITE_MULTI;
    wr16(pdu + 1, addr);
    wr16(pdu + 3, (uint16_t)nvals);
    pdu[5] = (uint8_t)(nvals * 2);
    for (int i = 0; i < nvals; i++) {
        wr16(pdu + 6 + i * 2, vals[i]);
    }
    return 6 + (size_t)nvals * 2;
}

/* Print what came back, given the response PDU. */
static void mb_show(bool is_read, uint16_t addr, const uint16_t *vals,
                    int nvals, const uint8_t *resp, size_t rlen)
{
    if (is_read) {
        if (rlen < 2) {
            return;
        }
        const int regs = resp[1] / 2;
        for (int i = 0; i < regs && (size_t)(2 + i * 2 + 1) < rlen + 1; i++) {
            const uint16_t v = rd16(resp + 2 + i * 2);
            ag_console_printf("  [%u] %u  (0x%04x)\n", (unsigned)(addr + i),
                              (unsigned)v, (unsigned)v);
        }
    } else if (nvals == 1) {
        ag_console_printf("wrote %u to [%u]\n", (unsigned)vals[0],
                          (unsigned)addr);
    } else {
        ag_console_printf("wrote %d registers from [%u]\n", nvals,
                          (unsigned)addr);
    }
}

/* ---- the shell command ------------------------------------------------- */

/* Collect the numeric args (count for read, values for write) and options. */
static int mb_args(int argc, char **argv, int start, uint16_t *vals, int cap,
                   uint8_t *unit, bool *input, int *tx, int *rx)
{
    int nvals = 0;
    for (int i = start; i < argc; i++) {
        if (ag_path_icmp(argv[i], "/unit") == 0 && i + 1 < argc) {
            *unit = (uint8_t)atoi(argv[++i]);
        } else if (ag_path_icmp(argv[i], "/input") == 0) {
            *input = true;
        } else if (tx != NULL && ag_path_icmp(argv[i], "/tx") == 0 && i + 1 < argc) {
            *tx = atoi(argv[++i]);
        } else if (rx != NULL && ag_path_icmp(argv[i], "/rx") == 0 && i + 1 < argc) {
            *rx = atoi(argv[++i]);
        } else if (argv[i][0] != '/' && nvals < cap) {
            vals[nvals++] = (uint16_t)strtoul(argv[i], NULL, 0);
        }
    }
    return nvals;
}

/*
 * modbus rtu <uart> <baud> read|write <slave> <addr> [count|vals] [/input]
 *            [/tx <pin>] [/rx <pin>]
 * The same PDU as TCP, framed with a CRC over a UART.
 */
static int modbus_rtu_cmd(int argc, char **argv)
{
    if (argc < 7) {
        ag_console_puts(
            "usage: modbus rtu <uart> <baud> read|write <slave> <addr> "
            "[count|vals] [/input] [/tx pin] [/rx pin]\n");
        return 1;
    }
    const int  uart = atoi(argv[2]);
    const bool is_read = (ag_path_icmp(argv[4], "read") == 0);
    if (!is_read && ag_path_icmp(argv[4], "write") != 0) {
        ag_console_puts("modbus rtu: 'read' or 'write'\n");
        return 1;
    }
    const uint8_t  slave = (uint8_t)atoi(argv[5]);
    const uint16_t addr = (uint16_t)strtoul(argv[6], NULL, 0);

    uint8_t  unit_ignored = slave;
    bool     input = false;
    int      tx = -1, rx = -1;
    uint16_t vals[MB_MAX_REGS];
    const int nvals = mb_args(argc, argv, 7, vals, MB_MAX_REGS, &unit_ignored,
                              &input, &tx, &rx);
    if (!is_read && nvals < 1) {
        ag_console_puts("modbus write needs a value\n");
        return 1;
    }

    const ag_port_uart_cfg_t cfg = {
        .baud = (uint32_t)strtoul(argv[3], NULL, 0),
        .data_bits = 8,
        .parity = 0,
        .stop_bits = 1,
    };
    if (tx >= 0 || rx >= 0) {
        (void)ag_port_uart_pins(uart, tx, rx);
    }
    if (ag_port_uart_open(uart, &cfg, 512, 512) != AG_OK) {
        ag_console_printf("cannot open UART%d\n", uart);
        return 1;
    }

    uint8_t  pdu[256], resp[256];
    size_t   rlen = 0;
    const size_t plen = mb_build(is_read, input, addr, vals, nvals, pdu);
    ag_console_printf("UART%d @ %u ... ", uart, (unsigned)cfg.baud);
    const ag_err_t err =
        mb_rtu_exchange(uart, slave, pdu, plen, resp, sizeof(resp), &rlen);
    if (err != AG_OK) {
        return 1;
    }
    ag_console_puts("ok\n");
    mb_show(is_read, addr, vals, nvals, resp, rlen);
    return 0;
}

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
    if (argc >= 2 && ag_path_icmp(argv[1], "rtu") == 0) {
        return modbus_rtu_cmd(argc, argv);
    }
    if (argc < 4) {
        ag_console_puts(
            "usage: modbus read  <host[:port]> <addr> [count] [/unit N] [/input]\n"
            "       modbus write <host[:port]> <addr> <value> [value...] [/unit N]\n"
            "       modbus rtu <uart> <baud> read|write <slave> <addr> ...\n");
        return 1;
    }
    const bool is_read = (ag_path_icmp(argv[1], "read") == 0);
    const bool is_write = (ag_path_icmp(argv[1], "write") == 0);
    if (!is_read && !is_write) {
        ag_console_puts("modbus: 'read', 'write' or 'rtu'\n");
        return 1;
    }

    char hostbuf[128];
    snprintf(hostbuf, sizeof(hostbuf), "%s", argv[2]);
    const uint16_t addr = (uint16_t)strtoul(argv[3], NULL, 0);

    uint8_t  unit = 1;
    bool     input = false;
    uint16_t vals[MB_MAX_REGS];
    const int nvals =
        mb_args(argc, argv, 4, vals, MB_MAX_REGS, &unit, &input, NULL, NULL);

    const uint16_t port = split_host_port(hostbuf, MB_PORT);
    uint32_t netaddr = 0;
    if (ag_net_lookup(hostbuf, &netaddr) != AG_OK) {
        ag_console_printf("%s: cannot be resolved\n", hostbuf);
        return 1;
    }
    ag_console_printf("%s:%u ... ", hostbuf, (unsigned)port);
    const int fd = ag_netprov_connect(netaddr, port, MB_CONNECT_MS);
    if (fd < 0) {
        ag_console_puts("no answer\n");
        return 1;
    }
    (void)ag_netprov_nonblock(fd, true);

    uint8_t    rxbuf[300];
    ag_netio_t rd;
    ag_netio_init(&rd, fd, rxbuf, sizeof(rxbuf), 0);
    ag_console_puts("connected\n");

    if (!is_read && nvals < 1) {
        ag_console_puts("modbus write needs a value\n");
        (void)ag_netprov_close(fd);
        return 1;
    }
    uint8_t      pdu[256], resp[256];
    size_t       rlen = 0;
    const size_t plen = mb_build(is_read, input, addr, vals, nvals, pdu);
    int          rc = 1;
    if (mb_exchange(fd, &rd, unit, pdu, plen, resp, sizeof(resp), &rlen) ==
        AG_OK) {
        mb_show(is_read, addr, vals, nvals, resp, rlen);
        rc = 0;
    }
    (void)ag_netprov_close(fd);
    return rc;
}

#endif /* CONFIG_ARGON_NET_MODBUS */
