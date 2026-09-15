/*
 * ArgonOS - AT codec tests.  The wire to a stock ESP-01: line classification,
 * the +IPD framing (the one that is a byte count and not a delimiter), and the
 * command strings a modem is fussy about.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"

#include "atproto/ag_atproto.h"

static ag_at_kind_t k(const char *line)
{
    ag_at_line_t l;
    ag_at_classify(line, &l);
    return l.kind;
}

static void test_classify_simple(void)
{
    AG_CHECK_INT(k("OK"), AG_AT_OK);
    AG_CHECK_INT(k("ERROR"), AG_AT_ERROR);
    AG_CHECK_INT(k("FAIL"), AG_AT_FAIL);
    AG_CHECK_INT(k("SEND OK"), AG_AT_SEND_OK);
    AG_CHECK_INT(k("SEND FAIL"), AG_AT_SEND_FAIL);
    AG_CHECK_INT(k("ALREADY CONNECTED"), AG_AT_ALREADY);
    AG_CHECK_INT(k("WIFI CONNECTED"), AG_AT_WIFI_UP);
    AG_CHECK_INT(k("WIFI GOT IP"), AG_AT_WIFI_GOTIP);
    AG_CHECK_INT(k("WIFI DISCONNECT"), AG_AT_WIFI_DOWN);
    AG_CHECK_INT(k(">"), AG_AT_PROMPT);
    AG_CHECK_INT(k("busy p..."), AG_AT_BUSY);
    /* leading space tolerated (some firmwares indent) */
    AG_CHECK_INT(k("  OK"), AG_AT_OK);
    /* chatter is not an error */
    AG_CHECK_INT(k("ready"), AG_AT_OTHER);
    AG_CHECK_INT(k(""), AG_AT_OTHER);
    /* a bare '>' only, not "> something" */
    AG_CHECK_INT(k("> x"), AG_AT_OTHER);
}

static void test_classify_conn(void)
{
    ag_at_line_t l;
    ag_at_classify("0,CONNECT", &l);
    AG_CHECK_INT(l.kind, AG_AT_CONNECT);
    AG_CHECK_INT(l.link, 0);

    ag_at_classify("3,CLOSED", &l);
    AG_CHECK_INT(l.kind, AG_AT_CLOSED);
    AG_CHECK_INT(l.link, 3);

    /* single-connection firmware: no link prefix */
    ag_at_classify("CONNECT", &l);
    AG_CHECK_INT(l.kind, AG_AT_CONNECT);
    AG_CHECK_INT(l.link, -1);

    ag_at_classify("CLOSED", &l);
    AG_CHECK_INT(l.kind, AG_AT_CLOSED);
    AG_CHECK_INT(l.link, -1);
}

static void test_classify_ip(void)
{
    ag_at_line_t l;
    /* +CIPDOMAIN unquoted */
    ag_at_classify("+CIPDOMAIN:93.184.216.34", &l);
    AG_CHECK_INT(l.kind, AG_AT_CIPDOMAIN);
    AG_CHECK_INT(l.addr, (long)0x5DB8D822u); /* 93.184.216.34 */

    /* +CIPDOMAIN quoted (newer firmware) */
    ag_at_classify("+CIPDOMAIN:\"10.0.2.2\"", &l);
    AG_CHECK_INT(l.kind, AG_AT_CIPDOMAIN);
    AG_CHECK_INT(l.addr, (long)0x0A000202u);

    /* the three spellings of "my address" */
    ag_at_classify("+CIFSR:STAIP,\"192.168.0.102\"", &l);
    AG_CHECK_INT(l.kind, AG_AT_STA_IP);
    AG_CHECK_INT(l.addr, (long)0xC0A80066u);

    ag_at_classify("+CIPSTA:ip:\"192.168.0.102\"", &l);
    AG_CHECK_INT(l.kind, AG_AT_STA_IP);
    AG_CHECK_INT(l.addr, (long)0xC0A80066u);

    ag_at_classify("+CIPSTA:\"192.168.0.102\"", &l);
    AG_CHECK_INT(l.kind, AG_AT_STA_IP);
    AG_CHECK_INT(l.addr, (long)0xC0A80066u);

    /* a malformed address stays OTHER, not a half-parsed STA_IP */
    ag_at_classify("+CIPDOMAIN:999.1.1.1", &l);
    AG_CHECK_INT(l.kind, AG_AT_OTHER);
}

/* +IPD is the frame that cannot be found by splitting on newlines: its payload
 * is a byte count and may itself contain CR, LF and ':'. */
static void test_ipd_mux(void)
{
    const char buf[] = "+IPD,2,5:he\r\nOK\r\n";
    int off, link, poff;
    uint32_t plen;
    int r = ag_at_ipd_scan(buf, sizeof(buf) - 1, &off, &link, &plen, &poff);
    AG_CHECK_INT(r, 1);
    AG_CHECK_INT(off, 0);
    AG_CHECK_INT(link, 2);
    AG_CHECK_INT(plen, 5);
    AG_CHECK_INT(poff, 9); /* just past "+IPD,2,5:" (9 bytes, ':' at index 8) */
    /* the 5 payload bytes are "he\r\n" - CR and LF inside the payload */
    AG_CHECK_INT(buf[poff], 'h');
    AG_CHECK_INT(buf[poff + 2], '\r');
    AG_CHECK_INT(buf[poff + 3], '\n');
}

static void test_ipd_single(void)
{
    const char buf[] = "+IPD,4:data";
    int off, link, poff;
    uint32_t plen;
    int r = ag_at_ipd_scan(buf, sizeof(buf) - 1, &off, &link, &plen, &poff);
    AG_CHECK_INT(r, 1);
    AG_CHECK_INT(link, -1); /* single-connection: no link id */
    AG_CHECK_INT(plen, 4);
    AG_CHECK_INT(poff, 7);
}

static void test_ipd_embedded(void)
{
    /* +IPD after other protocol text: the scan points past it. */
    const char buf[] = "SEND OK\r\n+IPD,0,3:abc";
    int off, link, poff;
    uint32_t plen;
    int r = ag_at_ipd_scan(buf, sizeof(buf) - 1, &off, &link, &plen, &poff);
    AG_CHECK_INT(r, 1);
    AG_CHECK_INT(off, 9); /* "+IPD" starts after "SEND OK\r\n" */
    AG_CHECK_INT(link, 0);
    AG_CHECK_INT(plen, 3);
}

static void test_ipd_incomplete(void)
{
    int off, link, poff;
    uint32_t plen;

    /* header cut before the ':' - not ready, but its start is reported so the
     * caller keeps it. */
    const char a[] = "+IPD,2,50";
    AG_CHECK_INT(ag_at_ipd_scan(a, sizeof(a) - 1, &off, &link, &plen, &poff), 0);
    AG_CHECK_INT(off, 0);

    /* only a prefix of the tag at the tail must be preserved, not eaten. */
    const char b[] = "OK\r\n+IP";
    AG_CHECK_INT(ag_at_ipd_scan(b, sizeof(b) - 1, &off, &link, &plen, &poff), 0);
    AG_CHECK_INT(off, 4); /* the "+IP" prefix begins at index 4 */

    /* no +IPD and no prefix: nothing to keep. */
    const char c[] = "OK\r\n";
    AG_CHECK_INT(ag_at_ipd_scan(c, sizeof(c) - 1, &off, &link, &plen, &poff), 0);
    AG_CHECK_INT(off, -1);
}

static void test_builders(void)
{
    char b[64];

    AG_CHECK_INT(ag_at_cmd(b, sizeof(b), "ATE0"), 6);
    AG_CHECK_STR(b, "ATE0\r\n");

    ag_at_cmd_join(b, sizeof(b), "myssid", "secret");
    AG_CHECK_STR(b, "AT+CWJAP=\"myssid\",\"secret\"\r\n");

    /* 93.184.216.34 host-order = 0x5DB8D822 */
    ag_at_cmd_connect(b, sizeof(b), 1, 0x5DB8D822u, 443);
    AG_CHECK_STR(b, "AT+CIPSTART=1,\"TCP\",\"93.184.216.34\",443\r\n");

    ag_at_cmd_send(b, sizeof(b), 2, 128);
    AG_CHECK_STR(b, "AT+CIPSEND=2,128\r\n");

    ag_at_cmd_close(b, sizeof(b), 0);
    AG_CHECK_STR(b, "AT+CIPCLOSE=0\r\n");

    ag_at_cmd_resolve(b, sizeof(b), "example.com");
    AG_CHECK_STR(b, "AT+CIPDOMAIN=\"example.com\"\r\n");

    /* truncation is reported, not overrun */
    char small[8];
    AG_CHECK_INT(ag_at_cmd_connect(small, sizeof(small), 1, 0x5DB8D822u, 443),
                 -1);
}

void run_atproto_tests(void)
{
    test_classify_simple();
    test_classify_conn();
    test_classify_ip();
    test_ipd_mux();
    test_ipd_single();
    test_ipd_embedded();
    test_ipd_incomplete();
    test_builders();
}
