/*
 * ArgonOS - RLINK codec tests.  The wire between the guest and an external
 * radio coprocessor: exact byte layout, endianness, the argument packing, and
 * the two ways a frame is rejected (bad magic, oversize length).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"

#include "rlink/ag_rlink.h"

static void test_roundtrip(void)
{
    ag_rlink_hdr_t h = {
        .op = RL_OP_SEND,
        .flags = RL_F_RESPONSE,
        .seq = 0xBEEF,
        .status = -5,
        .ch = 7,
        .a0 = 0x01020304u,
        .a1 = 0xAABBCCDDu,
        .len = 42,
    };
    uint8_t buf[RL_HDR_SIZE];
    ag_rlink_pack(buf, &h);

    ag_rlink_hdr_t g;
    memset(&g, 0, sizeof(g));
    AG_CHECK(ag_rlink_unpack(&g, buf));
    AG_CHECK_INT(g.op, RL_OP_SEND);
    AG_CHECK_INT(g.flags, RL_F_RESPONSE);
    AG_CHECK_INT(g.seq, 0xBEEF);
    AG_CHECK_INT(g.status, -5);
    AG_CHECK_INT(g.ch, 7);
    AG_CHECK_INT(g.a0, 0x01020304);
    AG_CHECK_INT(g.a1, (long)0xAABBCCDDu);
    AG_CHECK_INT(g.len, 42);
}

/* The layout is a contract with a Python peer that packs by hand; pin the
 * exact bytes so a field can never quietly move or change width. */
static void test_exact_bytes(void)
{
    ag_rlink_hdr_t h;
    ag_rlink_req(&h, RL_OP_LISTEN, 0x0201);
    h.a0 = 8080; /* port */
    uint8_t buf[RL_HDR_SIZE];
    ag_rlink_pack(buf, &h);

    /* magic 'RLNK' = 0x4B4E4C52, little-endian => 52 4C 4E 4B */
    AG_CHECK_INT(buf[0], 0x52);
    AG_CHECK_INT(buf[1], 0x4C);
    AG_CHECK_INT(buf[2], 0x4E);
    AG_CHECK_INT(buf[3], 0x4B);
    AG_CHECK_INT(buf[4], RL_OP_LISTEN); /* op */
    AG_CHECK_INT(buf[5], 0);            /* flags */
    AG_CHECK_INT(buf[6], 0x01);         /* seq low */
    AG_CHECK_INT(buf[7], 0x02);         /* seq high */
    /* ch defaults to -1 for a request => 0xFFFFFFFF at offset 12 */
    AG_CHECK_INT(buf[12], 0xFF);
    AG_CHECK_INT(buf[13], 0xFF);
    AG_CHECK_INT(buf[14], 0xFF);
    AG_CHECK_INT(buf[15], 0xFF);
    /* a0 = 8080 = 0x1F90 => 90 1F 00 00 at offset 16 */
    AG_CHECK_INT(buf[16], 0x90);
    AG_CHECK_INT(buf[17], 0x1F);
    AG_CHECK_INT(buf[18], 0x00);
    AG_CHECK_INT(buf[19], 0x00);
}

static void test_reject(void)
{
    ag_rlink_hdr_t h;
    ag_rlink_req(&h, RL_OP_READY, 1);
    uint8_t buf[RL_HDR_SIZE];
    ag_rlink_pack(buf, &h);

    ag_rlink_hdr_t g;

    /* Corrupt the magic: a desync, not a frame. */
    uint8_t bad = buf[0];
    buf[0] ^= 0xFFu;
    AG_CHECK(!ag_rlink_unpack(&g, buf));
    buf[0] = bad;

    /* A length beyond the payload cap is rejected rather than trusted. */
    ag_rlink_hdr_t big = h;
    big.len = RL_MAX_PAYLOAD + 1;
    ag_rlink_pack(buf, &big);
    AG_CHECK(!ag_rlink_unpack(&g, buf));

    /* Exactly the cap is fine. */
    big.len = RL_MAX_PAYLOAD;
    ag_rlink_pack(buf, &big);
    AG_CHECK(ag_rlink_unpack(&g, buf));
    AG_CHECK_INT(g.len, RL_MAX_PAYLOAD);
}

/* CONNECT is the frame with the most cross-field packing: addr in a0, port in
 * ch, timeout in a1.  Getting one of those slots wrong dials a wrong number. */
static void test_connect_packing(void)
{
    ag_rlink_hdr_t h;
    /* 192.168.0.102 host-order = 0xC0A80066 */
    ag_rlink_req_connect(&h, 3, 0xC0A80066u, 443, 5000);
    AG_CHECK_INT(h.op, RL_OP_CONNECT);
    AG_CHECK_INT(h.seq, 3);
    AG_CHECK_INT(h.a0, (long)0xC0A80066u);
    AG_CHECK_INT(h.ch, 443);
    AG_CHECK_INT(h.a1, 5000);

    uint8_t buf[RL_HDR_SIZE];
    ag_rlink_pack(buf, &h);
    ag_rlink_hdr_t g;
    AG_CHECK(ag_rlink_unpack(&g, buf));
    AG_CHECK_INT(g.a0, (long)0xC0A80066u);
    AG_CHECK_INT(g.ch, 443);
    AG_CHECK_INT(g.a1, 5000);
}

static void test_accept_send_builders(void)
{
    ag_rlink_hdr_t h;
    ag_rlink_req_accept(&h, 9, 4, 250);
    AG_CHECK_INT(h.op, RL_OP_ACCEPT);
    AG_CHECK_INT(h.ch, 4);
    AG_CHECK_INT(h.a1, 250);

    ag_rlink_req_send(&h, 10, 6, 128);
    AG_CHECK_INT(h.op, RL_OP_SEND);
    AG_CHECK_INT(h.ch, 6);
    AG_CHECK_INT(h.len, 128);
    AG_CHECK_INT(h.flags, 0);
}

void run_rlink_tests(void)
{
    test_roundtrip();
    test_exact_bytes();
    test_reject();
    test_connect_packing();
    test_accept_send_builders();
}
