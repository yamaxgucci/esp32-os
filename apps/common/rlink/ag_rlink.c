/*
 * ArgonOS - RLINK codec.  See ag_rlink.h for the protocol; this file is only
 * the byte layout, and it is deliberately the only place that knows it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ag_rlink.h"

#include <string.h>

/* Little-endian put/get, done by hand so the layout does not depend on the
 * host's struct packing or byte order. */
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

void ag_rlink_pack(uint8_t out[RL_HDR_SIZE], const ag_rlink_hdr_t *h)
{
    put_u32(out + 0, RL_MAGIC);
    out[4] = h->op;
    out[5] = h->flags;
    put_u16(out + 6, h->seq);
    put_u32(out + 8, (uint32_t)h->status);
    put_u32(out + 12, (uint32_t)h->ch);
    put_u32(out + 16, h->a0);
    put_u32(out + 20, h->a1);
    put_u32(out + 24, h->len);
}

bool ag_rlink_unpack(ag_rlink_hdr_t *h, const uint8_t in[RL_HDR_SIZE])
{
    const uint32_t magic = get_u32(in + 0);
    if (magic != RL_MAGIC) {
        return false;
    }
    const uint32_t len = get_u32(in + 24);
    if (len > RL_MAX_PAYLOAD) {
        return false;
    }
    h->magic = magic;
    h->op = in[4];
    h->flags = in[5];
    h->seq = get_u16(in + 6);
    h->status = (int32_t)get_u32(in + 8);
    h->ch = (int32_t)get_u32(in + 12);
    h->a0 = get_u32(in + 16);
    h->a1 = get_u32(in + 20);
    h->len = len;
    return true;
}

void ag_rlink_req(ag_rlink_hdr_t *h, uint8_t op, uint16_t seq)
{
    memset(h, 0, sizeof(*h));
    h->magic = RL_MAGIC;
    h->op = op;
    h->seq = seq;
    h->ch = -1;
}

void ag_rlink_req_connect(ag_rlink_hdr_t *h, uint16_t seq, uint32_t addr,
                          uint16_t port, uint32_t timeout_ms)
{
    ag_rlink_req(h, RL_OP_CONNECT, seq);
    h->a0 = addr;
    h->ch = (int32_t)port;
    h->a1 = timeout_ms;
}

void ag_rlink_req_accept(ag_rlink_hdr_t *h, uint16_t seq, int32_t listen_ch,
                         uint32_t timeout_ms)
{
    ag_rlink_req(h, RL_OP_ACCEPT, seq);
    h->ch = listen_ch;
    h->a1 = timeout_ms;
}

void ag_rlink_req_send(ag_rlink_hdr_t *h, uint16_t seq, int32_t ch,
                       uint32_t len)
{
    ag_rlink_req(h, RL_OP_SEND, seq);
    h->ch = ch;
    h->len = len;
}
