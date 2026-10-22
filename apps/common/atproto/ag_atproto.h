/*
 * ArgonOS - the AT codec: talking to a stock ESP-01 (or any modem) on its
 * factory AT firmware, the way RLINK talks to our own coprocessor firmware.
 *
 * Same division as ag_rlink.h and netmsg.c: this file and the .c beside it are
 * only the text of the protocol - a line in, a classification out; fields in, a
 * command string out - with no UART and no sockets, so every parsing mistake
 * that is worth catching is caught on the development machine (test_atproto.c).
 * The driver (apps/atradio) does the I/O and owns the sockets; it publishes the
 * same ag_net_ops_t an RLINK radio does, so `net use` cannot tell them apart.
 *
 * WHY AT AT ALL.  The ESP-01 in a drawer already runs AI-Thinker/ESP-AT
 * firmware: it answers `AT`, joins a network with AT+CWJAP, opens TCP with
 * AT+CIPSTART, and raises incoming bytes unsolicited as `+IPD`.  That is a
 * whole radio for the price of a wire and no reflash - the fastest first result
 * on hardware.  It buys only sockets: no monitor mode, no ESP-NOW, no injection.
 * For those, our own RLINK firmware (ag_rlink.h) is the other backend.
 *
 * THE MODEL WE TARGET (documented so the fixture and the driver agree):
 *   - AT+CWMODE=1  station.
 *   - AT+CIPMUX=1  multi-connection: link ids 0..4 are the "sockets", and they
 *     are exactly the small integers the kernel wraps into handles, the same
 *     role RLINK's channel plays.
 *   - Active receive (CIPRECVMODE=0, the factory default): the modem PUSHES
 *     arriving bytes as  +IPD,<link>,<len>:<payload>  with no request.  The
 *     driver buffers those per link and answers recv_now/wait_readable out of
 *     the buffer - the same shape port/net.h's recv_now split needs, and the
 *     same thing RLINK's DATA push does.
 *
 * THE TWO HARD PARTS, and the reason this is a codec and not inline string
 * poking:
 *   1. +IPD is NOT a line.  It is "+IPD,<link>,<len>:" followed by exactly
 *      <len> RAW bytes that may contain CR, LF, and ':' - so it cannot be found
 *      by splitting on newlines, and the payload boundary is a byte count, not
 *      a delimiter.  ag_at_ipd_scan finds and measures one without copying it.
 *   2. Every other reply IS a line, and there are many of them (OK, ERROR,
 *      SEND OK, '>', "<n>,CONNECT", "<n>,CLOSED", "+CIPDOMAIN:...", "WIFI GOT
 *      IP", ...).  ag_at_classify turns one line into one enum plus its fields,
 *      so the driver's state machine reads as a switch, not as a pile of
 *      strstr/strncmp that each new firmware quirk grows.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef AG_ATPROTO_H
#define AG_ATPROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Link ids in AT+CIPMUX=1 are 0..4: five connections, the modem's whole budget. */
#define AG_AT_MAX_LINK 5

/* What one reply line means. */
typedef enum ag_at_kind {
    AG_AT_OTHER = 0, /* an unrecognised or informational line - ignore or log  */
    AG_AT_OK,        /* "OK"                                                    */
    AG_AT_ERROR,     /* "ERROR"                                                 */
    AG_AT_FAIL,      /* "FAIL" (e.g. a failed join)                             */
    AG_AT_BUSY,      /* "busy p..." / "busy s..." - a command is still running  */
    AG_AT_PROMPT,    /* ">" - CIPSEND is ready for the payload                  */
    AG_AT_SEND_OK,   /* "SEND OK"                                              */
    AG_AT_SEND_FAIL, /* "SEND FAIL"                                            */
    AG_AT_CONNECT,   /* "<link>,CONNECT" - .link is set                        */
    AG_AT_CLOSED,    /* "<link>,CLOSED"  - .link is set                        */
    AG_AT_ALREADY,   /* "ALREADY CONNECTED"                                    */
    AG_AT_WIFI_UP,   /* "WIFI CONNECTED"                                       */
    AG_AT_WIFI_GOTIP,/* "WIFI GOT IP"                                          */
    AG_AT_WIFI_DOWN, /* "WIFI DISCONNECT"                                      */
    AG_AT_CIPDOMAIN, /* "+CIPDOMAIN:<ip>" - .addr is the host-order IPv4        */
    AG_AT_STA_IP,    /* "+CIFSR:STAIP,\"ip\"" or "+CIPSTA:ip:\"ip\"" - .addr    */
} ag_at_kind_t;

typedef struct ag_at_line {
    ag_at_kind_t kind;
    int          link; /* CONNECT/CLOSED: the link id; else -1                 */
    uint32_t     addr; /* CIPDOMAIN/STA_IP: host-order IPv4; else 0            */
} ag_at_line_t;

/*
 * Classify one reply line.  `line` is NUL-terminated and must NOT include the
 * trailing CR/LF (the caller splits on newlines first).  Leading spaces are
 * tolerated.  Never fails: an unknown line is AG_AT_OTHER, which the driver is
 * free to ignore - AT firmwares print chatter, and a codec that treated every
 * unexpected line as an error would fall over on the first banner.
 */
void ag_at_classify(const char *line, ag_at_line_t *out);

/*
 * Look for the header of one +IPD frame anywhere in `buf` (`len` bytes, raw -
 * it may hold partial lines and binary payload).  Returns:
 *
 *   ag_at_ipd_scan == 0  and *hdr_off < 0 : no "+IPD" seen at all.
 *                   == 0  and *hdr_off >= 0: "+IPD" seen at *hdr_off but its
 *                          "...:" header is not complete yet - read more bytes
 *                          and scan again (do not consume before *hdr_off).
 *                   == 1 : a complete header was found.  *hdr_off is where
 *                          "+IPD" starts, *link the connection (or -1 in
 *                          single-conn mode), *plen the payload length, and
 *                          *payload_off the offset in `buf` where the <plen>
 *                          raw payload bytes begin.  The caller then needs
 *                          payload_off + plen bytes buffered to have the whole
 *                          frame; anything before hdr_off is other protocol
 *                          text to be handled first.
 *
 * The scan is deliberately tolerant of where "+IPD" sits: AT interleaves it
 * with "OK"/"SEND OK"/status lines on the same stream, so the driver hands the
 * whole receive buffer in and this says "the next +IPD, if any, is here".
 */
int ag_at_ipd_scan(const char *buf, size_t len, int *hdr_off, int *link,
                   uint32_t *plen, int *payload_off);

/*
 * Command builders.  Each writes a NUL-terminated command WITH its trailing
 * "\r\n" into `out` (capacity `cap`) and returns the length written excluding
 * the NUL, or -1 if it would not fit.  They never do I/O; the driver writes the
 * bytes and then reads for the classified reply.
 *
 * Host order in, quoted-and-dotted out where AT wants a string.
 */
int ag_at_cmd(char *out, size_t cap, const char *cmd); /* raw + CRLF, e.g "ATE0" */
int ag_at_cmd_join(char *out, size_t cap, const char *ssid, const char *pass);
int ag_at_cmd_connect(char *out, size_t cap, int link, uint32_t addr,
                      uint16_t port);
int ag_at_cmd_send(char *out, size_t cap, int link, uint32_t plen);
int ag_at_cmd_close(char *out, size_t cap, int link);
int ag_at_cmd_resolve(char *out, size_t cap, const char *host);

#endif /* AG_ATPROTO_H */
