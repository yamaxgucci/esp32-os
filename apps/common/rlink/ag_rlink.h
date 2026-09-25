/*
 * ArgonOS - RLINK, the wire protocol between the guest and an external radio
 * coprocessor (guest UART/SPI <-> modem firmware).
 *
 * This header is the whole contract, and the .c beside it is the whole codec:
 * bytes in, a header struct out, and a header struct in, bytes out.  No UART,
 * no sockets, no allocation - so every framing mistake that is worth catching
 * is caught on the development machine (test_rlink.c), the same division that
 * keeps netmsg.c honest.
 *
 * Little-endian on the wire, because both ends that matter are little-endian
 * and saying so once is cheaper than byte-swapping twice.
 *
 * SHAPE OF A CONVERSATION
 *
 *   The guest is the master.  It sends a request with a sequence number and
 *   waits for the reply that carries the same number; the coprocessor answers
 *   exactly one frame per request.  `status` is 0 on a request and an ag_err_t
 *   (0 or negative) - or a small non-negative result - on a reply.
 *
 *   Received data is NOT pulled.  The coprocessor pushes it, unsolicited, as
 *   RL_OP_DATA frames tagged with the channel, whenever bytes arrive from the
 *   far side of a socket - exactly as a real modem raises "+IPD" or an
 *   ESP-Hosted slave raises a data event.  The guest buffers those per channel
 *   and answers recv_now/wait_readable out of that buffer without touching the
 *   wire.  This is what port/net.h's recv_now/wait_readable split requires: a
 *   read must be able to say "what has already arrived" without a round trip.
 *
 *   So there is no RL_OP_RECV.  The guest SENDS: HELLO, START, RESOLVE, LISTEN,
 *   ACCEPT, CONNECT, SEND, CLOSE, NONBLOCK, READY, IFADDR - and, when the
 *   coprocessor is a Wi-Fi radio rather than a modem, WSTART, WSTOP, WSCAN,
 *   WJOIN, WLEAVE, WSTATUS.  The coprocessor PUSHES: DATA (per channel;
 *   RL_F_EOF marks the far side closed) and EVENT (link up / got address /
 *   link down).
 *
 * THE STATION HALF is what makes a radio on a wire indistinguishable from one
 * on the board.  Sockets alone are not enough: `wifi scan` and `wifi connect`
 * are how a person points at the air, and a board whose only radio is at the
 * far end of a cable has to be able to do both.  A coprocessor that is not a
 * Wi-Fi radio (a GSM modem, say) simply leaves RL_CAP_STATION clear in its
 * HELLO reply and answers none of these; the driver then publishes no station
 * entries and the kernel reports no radio.  That is why this needed no protocol
 * version bump - the capability bitmask already says who can do what.
 *
 *   The coprocessor owns its own radio, so the guest names the network to join
 *   by handing the credentials to START (proto v2).  A coprocessor that keeps
 *   its own stored network - or the QEMU fake, which rides the host's - ignores
 *   them; a guest with none sends START empty, meaning "use what you have".
 *
 * A CHANNEL is the coprocessor's own small-integer socket id (0..N).  It is the
 * `ch` field; the guest's kernel wraps it into an ag_handle_t exactly as it
 * wraps a port fd (src/net/net.c adopt_fd), and because only one provider is
 * active at a time the id space is unambiguous.
 *
 * ARGUMENT PACKING (the one place to get wrong once and never again)
 *
 *   RESOLVE  req : payload = host name (not NUL-terminated; len = name length)
 *            rep : a0 = host-order IPv4, status = 0 / -AG_ENOENT / -AG_EAGAIN
 *   LISTEN   req : a0 = port
 *            rep : ch = listen channel (>=0), or status < 0
 *   ACCEPT   req : ch = listen channel, a1 = timeout_ms
 *            rep : ch = new channel (>=0), or status < 0 (-AG_EAGAIN on timeout)
 *   CONNECT  req : a0 = host-order IPv4, ch = port, a1 = timeout_ms
 *            rep : ch = new channel (>=0), or status < 0
 *   SEND     req : ch = channel, payload = bytes
 *            rep : status = bytes accepted (>=0) or -AG_E*  (-AG_EAGAIN allowed)
 *   CLOSE    req : ch = channel                     rep : status
 *   NONBLOCK req : ch = channel, a0 = (on ? 1 : 0)  rep : status
 *   READY    req : (none)        rep : status = 1 ready / 0 not
 *   IFADDR   req : (none)        rep : a0 = host-order IPv4, status
 *   START    req : a0 = ssid length; payload = ssid bytes then passphrase bytes
 *                  (passphrase = payload[a0 ..]).  len == 0 => no credentials,
 *                  join the coprocessor's own stored/host network.
 *            rep : status
 *   HELLO    req : a0 = RL_PROTO_VERSION the guest speaks
 *            rep : a0 = version the coprocessor speaks, a1 = capability bitmask
 *   DATA    push : ch = channel, payload = received bytes; RL_F_EOF => far side
 *                  closed (payload may still carry the last bytes; len may be 0)
 *   EVENT   push : a0 = RL_EV_*, a1 = host-order IPv4 on RL_EV_GOTIP
 *
 *   WSTART   req : (none)        rep : status  (radio on, joined to nothing)
 *   WSTOP    req : (none)        rep : status
 *   WSCAN    req : a0 = how many networks the guest has room for
 *            rep : a0 = how many the radio SAW (may exceed the count returned),
 *                  len = n * RL_AP_SIZE, payload = n packed ag_rlink_ap_t.
 *                  n is len / RL_AP_SIZE and is <= the requested a0; the guest
 *                  reports a0 as `found`, which is what the ABI's scan means.
 *   WJOIN    req : a0 = 1 when the bssid is to be pinned, 0 to join any,
 *                  payload = RL_JOIN_SIZE bytes: ssid[33] pass[64] bssid[6],
 *                  each NUL-padded.  Returns when the attempt is MADE, not when
 *                  it succeeded - the guest polls WSTATUS, exactly as it polls
 *                  the built-in radio.
 *            rep : status
 *   WLEAVE   req : (none)        rep : status
 *   WSTATUS  req : (none)
 *            rep : len = RL_STATUS_SIZE, payload = a packed ag_rlink_status_t
 *
 *   A password crosses this wire in the clear, and there is no pretending
 *   otherwise: the link is two boards in one box, and if that wire is
 *   untrusted then so is the radio at the end of it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef AG_RLINK_H
#define AG_RLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RL_MAGIC         0x4B4E4C52u /* 'RLNK' little-endian on the wire */
#define RL_PROTO_VERSION 2u /* v2: START carries Wi-Fi credentials (see above) */

/* Header is fixed length; the payload (len bytes) follows it. */
#define RL_HDR_SIZE 28u

/*
 * Largest payload in one frame.  A SEND longer than this is split by the caller
 * into several frames, exactly as HostFS slices a write; the codec only ever
 * looks at one frame.  Kept modest because the guest RX buffer and the
 * coprocessor's are both small.
 */
#define RL_MAX_PAYLOAD 1600u

enum ag_rlink_op {
    RL_OP_HELLO = 0,
    RL_OP_START = 1,
    RL_OP_READY = 2,
    RL_OP_IFADDR = 3,
    RL_OP_RESOLVE = 4,
    RL_OP_LISTEN = 5,
    RL_OP_ACCEPT = 6,
    RL_OP_CONNECT = 7,
    RL_OP_SEND = 8,
    RL_OP_CLOSE = 9,
    RL_OP_NONBLOCK = 10,
    /* Coprocessor -> guest, unsolicited. */
    RL_OP_DATA = 11,
    RL_OP_EVENT = 12,
    /* The station half - only when the HELLO reply set RL_CAP_STATION. */
    RL_OP_WSTART = 13,
    RL_OP_WSTOP = 14,
    RL_OP_WSCAN = 15,
    RL_OP_WJOIN = 16,
    RL_OP_WLEAVE = 17,
    RL_OP_WSTATUS = 18,
};

/* flags */
enum ag_rlink_flag {
    RL_F_RESPONSE = 1u << 0, /* set on a reply; clear on a request/push */
    RL_F_EOF = 1u << 1,      /* DATA: far side closed after this frame   */
};

/* EVENT a0 values */
enum ag_rlink_event {
    RL_EV_LINKUP = 1,
    RL_EV_GOTIP = 2,
    RL_EV_LINKDOWN = 3,
};

/* HELLO reply capability bits - which surfaces this coprocessor fills. */
enum ag_rlink_cap {
    RL_CAP_SOCKETS = 1u << 0, /* net: connect/listen/accept/send/data       */
    RL_CAP_DGRAM = 1u << 1,   /* espnow-shaped datagrams (future)           */
    RL_CAP_MONITOR = 1u << 2, /* 802.11 capture/inject (future)            */
    RL_CAP_STATION = 1u << 3, /* wifi: WSTART/WSTOP/WSCAN/WJOIN/WLEAVE/    */
                              /*       WSTATUS - the station half           */
};

typedef struct ag_rlink_hdr {
    uint32_t magic;  /* RL_MAGIC */
    uint8_t  op;     /* ag_rlink_op */
    uint8_t  flags;  /* ag_rlink_flag bitmask */
    uint16_t seq;    /* request/reply pairing; wraps freely */
    int32_t  status; /* 0 on request; result / -AG_E* on reply */
    int32_t  ch;     /* channel (socket id / port), or -1 */
    uint32_t a0;     /* op-specific, see packing table above */
    uint32_t a1;     /* op-specific */
    uint32_t len;    /* payload bytes following this header */
} ag_rlink_hdr_t;

/*
 * Pack a header into exactly RL_HDR_SIZE bytes, little-endian.  `out` must have
 * room for RL_HDR_SIZE.  magic is written for the caller, so a builder does not
 * have to remember it.
 */
void ag_rlink_pack(uint8_t out[RL_HDR_SIZE], const ag_rlink_hdr_t *h);

/*
 * Unpack RL_HDR_SIZE bytes into a header.  Returns false (and leaves *h
 * untouched save for what was read) when the magic is wrong or when len is
 * beyond RL_MAX_PAYLOAD - a desync on the wire, to be resynced rather than
 * trusted.  A true return means the header is well formed; it says nothing
 * about whether the op makes sense.
 */
bool ag_rlink_unpack(ag_rlink_hdr_t *h, const uint8_t in[RL_HDR_SIZE]);

/*
 * Convenience builders.  Each fills *h (magic, op, flags, seq set; unused
 * fields zeroed) so a caller writes one line, not eight.  They never touch the
 * payload - that is the caller's buffer, sent after the packed header.
 */
void ag_rlink_req(ag_rlink_hdr_t *h, uint8_t op, uint16_t seq);
void ag_rlink_req_connect(ag_rlink_hdr_t *h, uint16_t seq, uint32_t addr,
                          uint16_t port, uint32_t timeout_ms);
void ag_rlink_req_accept(ag_rlink_hdr_t *h, uint16_t seq, int32_t listen_ch,
                         uint32_t timeout_ms);
void ag_rlink_req_send(ag_rlink_hdr_t *h, uint16_t seq, int32_t ch,
                       uint32_t len);

/* ------------------------------------------------------------------------ */
/* The station half: networks and link state on the wire                    */
/* ------------------------------------------------------------------------ */
/*
 * These mirror ag_wifi_ap_t and ag_wifi_status_t, and they are declared here
 * rather than included from <argon/abi.h> on purpose: this header knows no ABI,
 * no allocation and no transport, which is what lets the whole codec be tested
 * on a development machine (host-tests/test_rlink.c).  The driver copies field
 * for field across the one-line gap.
 *
 * The numeric values of `auth` and `state` are deliberately the ABI's own
 * (AG_WIFI_SEC_* and AG_WIFI_ST_*), so that copy is a copy and not a
 * translation table nobody remembers to extend.  Renumbering either enum on
 * one side alone silently mislabels every network a scan returns.
 */
#define RL_SSID_MAX 32 /* an SSID is at most 32 bytes, not NUL-counted */
#define RL_PASS_MAX 63

/* Wire sizes.  Fixed-width records, not NUL-scanned strings: a length nobody
 * has to compute is a length nobody gets wrong, and 103 bytes is nothing next
 * to RL_MAX_PAYLOAD. */
#define RL_AP_SIZE     42u  /* ssid[33] bssid[6] rssi channel auth          */
#define RL_STATUS_SIZE 51u  /* see ag_rlink_pack_status                     */
#define RL_JOIN_SIZE   103u /* ssid[33] pass[64] bssid[6]                   */

/* How many networks fit in one WSCAN reply.  38 of them, and the guest asks
 * for no more than this. */
#define RL_SCAN_MAX (RL_MAX_PAYLOAD / RL_AP_SIZE)

typedef struct ag_rlink_ap {
    char    ssid[RL_SSID_MAX + 1];
    uint8_t bssid[6];
    int8_t  rssi;    /* dBm, negative      */
    uint8_t channel;
    uint8_t auth;    /* AG_WIFI_SEC_*      */
} ag_rlink_ap_t;

typedef struct ag_rlink_status {
    uint8_t  state;       /* AG_WIFI_ST_*                                   */
    char     ssid[RL_SSID_MAX + 1];
    uint8_t  bssid[6];
    uint8_t  pinned;      /* this access point was asked for by name         */
    int8_t   rssi;
    uint8_t  channel;
    uint32_t attempts;
    int32_t  last_reason; /* the far radio's own disconnect reason code      */
} ag_rlink_status_t;

/*
 * Pack/unpack one record.  Both unpackers NUL-terminate `ssid` themselves, so a
 * coprocessor that pads the field with rubbish rather than zeros cannot walk a
 * reader off its end.
 *
 * They differ in what they do with a value they do not recognise, and the
 * difference is not an inconsistency.  An unknown `auth` becomes
 * AG_WIFI_SEC_OTHER, which is the enum's own name for exactly this, and
 * unpack_ap therefore cannot fail: one unfamiliar network must not throw away a
 * whole scan.  An unknown `state` fails the record, because AG_WIFI_ST_* is a
 * closed set of four and there is no honest fallback - saying OFF about a
 * station that is joined is worse than saying nothing.
 */
void ag_rlink_pack_ap(uint8_t out[RL_AP_SIZE], const ag_rlink_ap_t *ap);
bool ag_rlink_unpack_ap(ag_rlink_ap_t *ap, const uint8_t in[RL_AP_SIZE]);

void ag_rlink_pack_status(uint8_t out[RL_STATUS_SIZE],
                          const ag_rlink_status_t *st);
bool ag_rlink_unpack_status(ag_rlink_status_t *st,
                            const uint8_t in[RL_STATUS_SIZE]);

/*
 * The join request's payload.  `pass` may be NULL or empty (an open network, or
 * the key the far radio already holds); `bssid` NULL joins any access point of
 * that name and is what clears the header's a0.  An ssid or a pass too long for
 * the field is refused rather than truncated - a truncated key joins nothing
 * and looks like a wrong password forever.
 */
bool ag_rlink_pack_join(uint8_t out[RL_JOIN_SIZE], const char *ssid,
                        const char *pass, const uint8_t bssid[6]);
bool ag_rlink_unpack_join(char ssid[RL_SSID_MAX + 1], char pass[RL_PASS_MAX + 1],
                          uint8_t bssid[6], const uint8_t in[RL_JOIN_SIZE]);

#endif /* AG_RLINK_H */
