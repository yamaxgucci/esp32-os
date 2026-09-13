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
 *   ACCEPT, CONNECT, SEND, CLOSE, NONBLOCK, READY, IFADDR.  The coprocessor
 *   PUSHES: DATA (per channel; RL_F_EOF marks the far side closed) and EVENT
 *   (link up / got address / link down).
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

#endif /* AG_RLINK_H */
