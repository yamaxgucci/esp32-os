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

/* ---- the station half ---------------------------------------------------- */

static void test_ap_roundtrip(void)
{
    ag_rlink_ap_t ap = {
        .ssid = "kitchen",
        .bssid = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11},
        .rssi = -67,
        .channel = 11,
        .auth = 3, /* AG_WIFI_SEC_WPA2 */
    };
    uint8_t buf[RL_AP_SIZE];
    ag_rlink_pack_ap(buf, &ap);

    ag_rlink_ap_t got;
    memset(&got, 0xAA, sizeof(got));
    AG_CHECK(ag_rlink_unpack_ap(&got, buf));
    AG_CHECK_STR(got.ssid, "kitchen");
    AG_CHECK_INT(got.bssid[0], 0xDE);
    AG_CHECK_INT(got.bssid[5], 0x11);
    AG_CHECK_INT(got.rssi, -67); /* the sign is the whole point of int8 here */
    AG_CHECK_INT(got.channel, 11);
    AG_CHECK_INT(got.auth, 3);
}

/*
 * An SSID of exactly 32 bytes has no NUL inside its field, so a reader that
 * trusts the padding runs into the bssid and reports a name with six bytes of
 * MAC glued to the end.
 */
static void test_ap_full_ssid(void)
{
    ag_rlink_ap_t ap = {.channel = 1};
    memset(ap.ssid, 'x', RL_SSID_MAX);
    ap.ssid[RL_SSID_MAX] = '\0';
    memset(ap.bssid, 0xFF, sizeof(ap.bssid));

    uint8_t buf[RL_AP_SIZE];
    ag_rlink_pack_ap(buf, &ap);

    ag_rlink_ap_t got;
    AG_CHECK(ag_rlink_unpack_ap(&got, buf));
    AG_CHECK_INT(strlen(got.ssid), RL_SSID_MAX);
    AG_CHECK_STR(got.ssid, ap.ssid);
}

/* A coprocessor that pads with rubbish instead of zeros must not walk the
 * reader off the field. */
static void test_ap_unterminated_field(void)
{
    uint8_t buf[RL_AP_SIZE];
    memset(buf, 'q', sizeof(buf)); /* no NUL anywhere, auth = 'q' = 113 */

    ag_rlink_ap_t got;
    AG_CHECK(ag_rlink_unpack_ap(&got, buf));
    AG_CHECK_INT(strlen(got.ssid), RL_SSID_MAX);
    /* Unknown security becomes OTHER rather than throwing away the network. */
    AG_CHECK_INT(got.auth, 6);
}

static void test_status_roundtrip(void)
{
    ag_rlink_status_t st = {
        .state = 3, /* AG_WIFI_ST_JOINED */
        .ssid = "kitchen",
        .bssid = {1, 2, 3, 4, 5, 6},
        .pinned = 1,
        .rssi = -80,
        .channel = 6,
        .attempts = 4000000000u, /* past INT32_MAX: it is unsigned on the wire */
        .last_reason = -201,
    };
    uint8_t buf[RL_STATUS_SIZE];
    ag_rlink_pack_status(buf, &st);

    ag_rlink_status_t got;
    AG_CHECK(ag_rlink_unpack_status(&got, buf));
    AG_CHECK_INT(got.state, 3);
    AG_CHECK_STR(got.ssid, "kitchen");
    AG_CHECK_INT(got.bssid[3], 4);
    AG_CHECK_INT(got.pinned, 1);
    AG_CHECK_INT(got.rssi, -80);
    AG_CHECK_INT(got.channel, 6);
    AG_CHECK(got.attempts == 4000000000u);
    AG_CHECK_INT(got.last_reason, -201);
}

/* AG_WIFI_ST_* is closed at four; a fifth is a coprocessor we do not
 * understand, and there is no honest state to report instead. */
static void test_status_unknown_state(void)
{
    ag_rlink_status_t st = {.state = 3, .ssid = "x"};
    uint8_t buf[RL_STATUS_SIZE];
    ag_rlink_pack_status(buf, &st);
    buf[0] = 4;

    ag_rlink_status_t got;
    AG_CHECK(!ag_rlink_unpack_status(&got, buf));
}

static void test_join_roundtrip(void)
{
    const uint8_t want_bssid[6] = {0xA0, 0xB1, 0xC2, 0xD3, 0xE4, 0xF5};
    uint8_t       buf[RL_JOIN_SIZE];
    AG_CHECK(ag_rlink_pack_join(buf, "kitchen", "hunter2", want_bssid));

    char    ssid[RL_SSID_MAX + 1];
    char    pass[RL_PASS_MAX + 1];
    uint8_t bssid[6];
    AG_CHECK(ag_rlink_unpack_join(ssid, pass, bssid, buf));
    AG_CHECK_STR(ssid, "kitchen");
    AG_CHECK_STR(pass, "hunter2");
    AG_CHECK_INT(bssid[0], 0xA0);
    AG_CHECK_INT(bssid[5], 0xF5);

    /* No bssid pinned: six zero bytes, which is what the header's a0 = 0
     * tells the far end to ignore. */
    AG_CHECK(ag_rlink_pack_join(buf, "kitchen", "", NULL));
    AG_CHECK(ag_rlink_unpack_join(ssid, pass, bssid, buf));
    AG_CHECK_STR(pass, "");
    AG_CHECK_INT(bssid[0], 0);
    AG_CHECK_INT(bssid[5], 0);

    /* A NULL pass is an open network, not a crash. */
    AG_CHECK(ag_rlink_pack_join(buf, "open-net", NULL, NULL));
    AG_CHECK(ag_rlink_unpack_join(ssid, pass, bssid, buf));
    AG_CHECK_STR(ssid, "open-net");
    AG_CHECK_STR(pass, "");
}

/*
 * The refusals.  A truncated key joins nothing and looks exactly like a wrong
 * password, so it must never be sent - and an empty ssid is a join request with
 * nothing to join.
 */
static void test_join_refusals(void)
{
    uint8_t buf[RL_JOIN_SIZE];
    char    too_long_ssid[RL_SSID_MAX + 2];
    char    too_long_pass[RL_PASS_MAX + 2];

    memset(too_long_ssid, 'x', sizeof(too_long_ssid) - 1);
    too_long_ssid[sizeof(too_long_ssid) - 1] = '\0';
    memset(too_long_pass, 'y', sizeof(too_long_pass) - 1);
    too_long_pass[sizeof(too_long_pass) - 1] = '\0';

    AG_CHECK(!ag_rlink_pack_join(buf, too_long_ssid, "ok", NULL));
    AG_CHECK(!ag_rlink_pack_join(buf, "kitchen", too_long_pass, NULL));
    AG_CHECK(!ag_rlink_pack_join(buf, "", "ok", NULL));
    AG_CHECK(!ag_rlink_pack_join(buf, NULL, "ok", NULL));

    /* Exactly at the limits is accepted - the boundary is off by one either
     * way, and the wrong side of it silently drops a legal network. */
    too_long_ssid[RL_SSID_MAX] = '\0';
    too_long_pass[RL_PASS_MAX] = '\0';
    AG_CHECK(ag_rlink_pack_join(buf, too_long_ssid, too_long_pass, NULL));
}

/* A whole scan reply is n records back to back, and the guest slices it by
 * RL_AP_SIZE alone - so a record whose size drifts takes every later one with
 * it. */
static void test_scan_payload(void)
{
    uint8_t payload[3 * RL_AP_SIZE];
    for (unsigned i = 0; i < 3; i++) {
        ag_rlink_ap_t ap = {.channel = (uint8_t)(i + 1), .rssi = (int8_t)(-40 - i)};
        ap.ssid[0] = (char)('a' + i);
        ap.ssid[1] = '\0';
        ag_rlink_pack_ap(payload + i * RL_AP_SIZE, &ap);
    }

    AG_CHECK_INT(sizeof(payload) / RL_AP_SIZE, 3);
    for (unsigned i = 0; i < 3; i++) {
        ag_rlink_ap_t got;
        AG_CHECK(ag_rlink_unpack_ap(&got, payload + i * RL_AP_SIZE));
        AG_CHECK_INT(got.channel, (long)(i + 1));
        AG_CHECK_INT(got.rssi, (long)(-40 - (int)i));
        AG_CHECK_INT(got.ssid[0], 'a' + (int)i);
    }
    /* The reply cap has to leave room for a whole number of records. */
    AG_CHECK(RL_SCAN_MAX * RL_AP_SIZE <= RL_MAX_PAYLOAD);
    AG_CHECK(RL_SCAN_MAX >= 16);
}

void run_rlink_tests(void)
{
    test_roundtrip();
    test_exact_bytes();
    test_reject();
    test_connect_packing();
    test_accept_send_builders();
    test_ap_roundtrip();
    test_ap_full_ssid();
    test_ap_unterminated_field();
    test_status_roundtrip();
    test_status_unknown_state();
    test_join_roundtrip();
    test_join_refusals();
    test_scan_payload();
}
