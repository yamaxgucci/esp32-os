/*
 * ArgonOS - AT codec.  See ag_atproto.h for the protocol; this file is only the
 * text of it, kept in one place so a firmware quirk is a change here and nowhere
 * else.
 *
 * Freestanding on purpose: it compiles into a .SYS built with -fno-builtin,
 * where snprintf/strncmp/strchr are not linked (only strcmp/strlen/mem*).  The
 * builders format by hand and the parser compares by hand, exactly as ag_rlink
 * packs bytes by hand for the same reason.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ag_atproto.h"

#include <string.h> /* strlen only - the one string fn a .SYS also links */

/* ------------------------------------------------------------------------ */
/* Tiny helpers that stand in for the libc a .SYS does not get.  A .SYS built  */
/* with -fno-builtin links neither strcmp/strncmp/strchr/snprintf/memcmp, and  */
/* the host build has no ag_strcmp - so this codec, shared by both, carries    */
/* its own.  strlen is the exception: present in both.                         */
/* ------------------------------------------------------------------------ */

static bool streq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Does `s` start with the literal `p`? */
static bool pfx(const char *s, const char *p)
{
    while (*p != '\0') {
        if (*s != *p) {
            return false;
        }
        s++;
        p++;
    }
    return true;
}

static const char *find_char(const char *s, char c)
{
    for (; *s != '\0'; s++) {
        if (*s == c) {
            return s;
        }
    }
    return NULL;
}

/* A bounded string builder: append, then sb_end reports the length or -1 if it
 * would not have fit (nothing is ever written past cap-1, and a NUL is put at
 * the end on success). */
typedef struct {
    char  *p;
    size_t cap;
    size_t n;
    bool   ovf;
} sb_t;

static void sb_init(sb_t *b, char *out, size_t cap)
{
    b->p = out;
    b->cap = cap;
    b->n = 0;
    b->ovf = false;
}
static void sb_ch(sb_t *b, char c)
{
    if (b->n + 1 < b->cap) {
        b->p[b->n] = c;
    } else {
        b->ovf = true;
    }
    b->n++;
}
static void sb_str(sb_t *b, const char *s)
{
    while (*s != '\0') {
        sb_ch(b, *s++);
    }
}
static void sb_u(sb_t *b, uint32_t v)
{
    char t[10];
    int  i = 0;
    if (v == 0) {
        sb_ch(b, '0');
        return;
    }
    while (v > 0 && i < (int)sizeof(t)) {
        t[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (i > 0) {
        sb_ch(b, t[--i]);
    }
}
static int sb_crlf_end(sb_t *b)
{
    sb_ch(b, '\r');
    sb_ch(b, '\n');
    if (b->ovf || b->n >= b->cap) {
        return -1;
    }
    b->p[b->n] = '\0';
    return (int)b->n;
}

/* ------------------------------------------------------------------------ */
/* IPv4 <-> host order                                                       */
/* ------------------------------------------------------------------------ */

static bool dotted_to_u32(const char *s, size_t len, uint32_t *out)
{
    uint32_t addr = 0;
    int      octets = 0;
    size_t   i = 0;
    while (octets < 4) {
        if (i >= len || s[i] < '0' || s[i] > '9') {
            return false;
        }
        uint32_t v = 0;
        int      digits = 0;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            v = v * 10u + (uint32_t)(s[i] - '0');
            i++;
            if (++digits > 3 || v > 255u) {
                return false;
            }
        }
        addr = (addr << 8) | v;
        octets++;
        if (octets < 4) {
            if (i >= len || s[i] != '.') {
                return false;
            }
            i++;
        }
    }
    if (i != len) {
        return false;
    }
    *out = addr;
    return true;
}

static void sb_ip(sb_t *b, uint32_t addr)
{
    sb_u(b, (addr >> 24) & 0xffu);
    sb_ch(b, '.');
    sb_u(b, (addr >> 16) & 0xffu);
    sb_ch(b, '.');
    sb_u(b, (addr >> 8) & 0xffu);
    sb_ch(b, '.');
    sb_u(b, addr & 0xffu);
}

/* A dotted quad, possibly double-quoted, at the tail of a line. */
static bool parse_ip_field(const char *p, uint32_t *addr)
{
    while (*p == ' ') {
        p++;
    }
    if (*p == '"') {
        p++;
        const char *q = find_char(p, '"');
        if (q == NULL) {
            return false;
        }
        return dotted_to_u32(p, (size_t)(q - p), addr);
    }
    return dotted_to_u32(p, strlen(p), addr);
}

/* ------------------------------------------------------------------------ */
/* Line classification                                                       */
/* ------------------------------------------------------------------------ */

static bool all_digits(const char *s, const char *end)
{
    if (s == end) {
        return false;
    }
    for (const char *p = s; p < end; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }
    return true;
}

void ag_at_classify(const char *line, ag_at_line_t *out)
{
    out->kind = AG_AT_OTHER;
    out->link = -1;
    out->addr = 0;

    while (*line == ' ') {
        line++;
    }

    if (streq(line, "OK")) {
        out->kind = AG_AT_OK;
        return;
    }
    if (streq(line, "ERROR")) {
        out->kind = AG_AT_ERROR;
        return;
    }
    if (streq(line, "FAIL")) {
        out->kind = AG_AT_FAIL;
        return;
    }
    if (streq(line, "SEND OK")) {
        out->kind = AG_AT_SEND_OK;
        return;
    }
    if (streq(line, "SEND FAIL")) {
        out->kind = AG_AT_SEND_FAIL;
        return;
    }
    if (streq(line, "ALREADY CONNECTED")) {
        out->kind = AG_AT_ALREADY;
        return;
    }
    if (streq(line, "WIFI CONNECTED")) {
        out->kind = AG_AT_WIFI_UP;
        return;
    }
    if (streq(line, "WIFI GOT IP")) {
        out->kind = AG_AT_WIFI_GOTIP;
        return;
    }
    if (streq(line, "WIFI DISCONNECT")) {
        out->kind = AG_AT_WIFI_DOWN;
        return;
    }
    if (line[0] == '>' && line[1] == '\0') {
        out->kind = AG_AT_PROMPT;
        return;
    }
    if (pfx(line, "busy ")) {
        out->kind = AG_AT_BUSY;
        return;
    }

    /* "<n>,CONNECT" / "<n>,CLOSED" and the bare single-conn forms. */
    {
        const char *comma = find_char(line, ',');
        const char *verb = line;
        int         link = -1;
        if (comma != NULL && all_digits(line, comma)) {
            link = 0;
            for (const char *p = line; p < comma; p++) {
                link = link * 10 + (*p - '0');
            }
            verb = comma + 1;
        }
        if (streq(verb, "CONNECT")) {
            out->kind = AG_AT_CONNECT;
            out->link = link;
            return;
        }
        if (streq(verb, "CLOSED")) {
            out->kind = AG_AT_CLOSED;
            out->link = link;
            return;
        }
    }

    if (pfx(line, "+CIPDOMAIN:")) {
        if (parse_ip_field(line + 11, &out->addr)) {
            out->kind = AG_AT_CIPDOMAIN;
        }
        return;
    }
    if (pfx(line, "+CIFSR:STAIP,")) {
        if (parse_ip_field(line + 13, &out->addr)) {
            out->kind = AG_AT_STA_IP;
        }
        return;
    }
    if (pfx(line, "+CIPSTA:ip:")) {
        if (parse_ip_field(line + 11, &out->addr)) {
            out->kind = AG_AT_STA_IP;
        }
        return;
    }
    if (pfx(line, "+CIPSTA:\"")) {
        if (parse_ip_field(line + 8, &out->addr)) {
            out->kind = AG_AT_STA_IP;
        }
        return;
    }
}

/* ------------------------------------------------------------------------ */
/* +IPD framing                                                              */
/* ------------------------------------------------------------------------ */

static int trailing_ipd_prefix(const char *buf, size_t len)
{
    static const char tag[4] = {'+', 'I', 'P', 'D'};
    for (int plen = 3; plen >= 1; plen--) {
        if (len < (size_t)plen) {
            continue;
        }
        bool eq = true;
        for (int j = 0; j < plen; j++) {
            if (buf[len - (size_t)plen + (size_t)j] != tag[j]) {
                eq = false;
                break;
            }
        }
        if (eq) {
            return plen;
        }
    }
    return 0;
}

int ag_at_ipd_scan(const char *buf, size_t len, int *hdr_off, int *link,
                   uint32_t *plen, int *payload_off)
{
    *hdr_off = -1;
    *link = -1;
    *plen = 0;
    *payload_off = -1;

    size_t i = 0;
    bool   found = false;
    if (len >= 4) {
        for (i = 0; i + 4 <= len; i++) {
            if (buf[i] == '+' && buf[i + 1] == 'I' && buf[i + 2] == 'P' &&
                buf[i + 3] == 'D') {
                found = true;
                break;
            }
        }
    }
    if (!found) {
        const int p = trailing_ipd_prefix(buf, len);
        if (p > 0) {
            *hdr_off = (int)(len - (size_t)p);
        }
        return 0;
    }

    *hdr_off = (int)i;

    size_t p = i + 4;
    if (p >= len || buf[p] != ',') {
        return 0;
    }
    p++;

    uint32_t n1 = 0;
    int      d1 = 0;
    while (p < len && buf[p] >= '0' && buf[p] <= '9') {
        n1 = n1 * 10u + (uint32_t)(buf[p] - '0');
        p++;
        d1++;
    }
    if (d1 == 0 || p >= len) {
        return 0;
    }

    if (buf[p] == ':') {
        *link = -1;
        *plen = n1;
        *payload_off = (int)(p + 1);
        return 1;
    }
    if (buf[p] != ',') {
        return 0;
    }
    p++;
    uint32_t n2 = 0;
    int      d2 = 0;
    while (p < len && buf[p] >= '0' && buf[p] <= '9') {
        n2 = n2 * 10u + (uint32_t)(buf[p] - '0');
        p++;
        d2++;
    }
    if (d2 == 0 || p >= len || buf[p] != ':') {
        return 0;
    }
    *link = (int)n1;
    *plen = n2;
    *payload_off = (int)(p + 1);
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Command builders                                                          */
/* ------------------------------------------------------------------------ */

int ag_at_cmd(char *out, size_t cap, const char *cmd)
{
    sb_t b;
    sb_init(&b, out, cap);
    sb_str(&b, cmd);
    return sb_crlf_end(&b);
}

int ag_at_cmd_join(char *out, size_t cap, const char *ssid, const char *pass)
{
    sb_t b;
    sb_init(&b, out, cap);
    sb_str(&b, "AT+CWJAP=\"");
    sb_str(&b, ssid);
    sb_str(&b, "\",\"");
    sb_str(&b, pass);
    sb_str(&b, "\"");
    return sb_crlf_end(&b);
}

int ag_at_cmd_connect(char *out, size_t cap, int link, uint32_t addr,
                      uint16_t port)
{
    sb_t b;
    sb_init(&b, out, cap);
    sb_str(&b, "AT+CIPSTART=");
    sb_u(&b, (uint32_t)link);
    sb_str(&b, ",\"TCP\",\"");
    sb_ip(&b, addr);
    sb_str(&b, "\",");
    sb_u(&b, port);
    return sb_crlf_end(&b);
}

int ag_at_cmd_send(char *out, size_t cap, int link, uint32_t plen)
{
    sb_t b;
    sb_init(&b, out, cap);
    sb_str(&b, "AT+CIPSEND=");
    sb_u(&b, (uint32_t)link);
    sb_ch(&b, ',');
    sb_u(&b, plen);
    return sb_crlf_end(&b);
}

int ag_at_cmd_close(char *out, size_t cap, int link)
{
    sb_t b;
    sb_init(&b, out, cap);
    sb_str(&b, "AT+CIPCLOSE=");
    sb_u(&b, (uint32_t)link);
    return sb_crlf_end(&b);
}

int ag_at_cmd_resolve(char *out, size_t cap, const char *host)
{
    sb_t b;
    sb_init(&b, out, cap);
    sb_str(&b, "AT+CIPDOMAIN=\"");
    sb_str(&b, host);
    sb_str(&b, "\"");
    return sb_crlf_end(&b);
}
