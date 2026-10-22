/*
 * ArgonOS - ESP8266 ROM download codec tests.  SLIP framing (escaping, frame
 * boundaries, overflow), the command header layout, response parsing and its
 * status trailer, the block checksum, and the ROM's erase-size quirk - all the
 * places a byte can move and take a whole flash with it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "test.h"

#include "esp8266/ag_espldr.h"

/* Feed a whole buffer through the streaming decoder; return the count of frames
 * that completed, and copy the LAST one out for inspection. */
static int feed_all(ag_esp_slip_t *d, const uint8_t *in, size_t n,
                    uint8_t *last, size_t *last_len)
{
    int frames = 0;
    for (size_t i = 0; i < n; i++) {
        if (ag_esp_slip_feed(d, in[i])) {
            frames++;
            if (last != NULL) {
                memcpy(last, d->buf, d->len);
                *last_len = d->len;
            }
        }
    }
    return frames;
}

static void test_slip_roundtrip(void)
{
    /* Payload that contains both bytes SLIP must escape. */
    const uint8_t body[] = {0x01, ESP_SLIP_END, 0x02, ESP_SLIP_ESC, 0x03};
    uint8_t       wire[32];
    const size_t  w = ag_esp_slip_encode(body, sizeof(body), wire, sizeof(wire));
    AG_CHECK(w > 0);

    /* Framed and escaped: C0 01 DB DC 02 DB DD 03 C0 */
    AG_CHECK_INT(wire[0], ESP_SLIP_END);
    AG_CHECK_INT(wire[1], 0x01);
    AG_CHECK_INT(wire[2], ESP_SLIP_ESC);
    AG_CHECK_INT(wire[3], ESP_SLIP_ESC_END);
    AG_CHECK_INT(wire[4], 0x02);
    AG_CHECK_INT(wire[5], ESP_SLIP_ESC);
    AG_CHECK_INT(wire[6], ESP_SLIP_ESC_ESC);
    AG_CHECK_INT(wire[7], 0x03);
    AG_CHECK_INT(wire[w - 1], ESP_SLIP_END);

    uint8_t       buf[32], out[32];
    size_t        out_len = 0;
    ag_esp_slip_t d;
    ag_esp_slip_init(&d, buf, sizeof(buf));
    AG_CHECK_INT(feed_all(&d, wire, w, out, &out_len), 1);
    AG_CHECK_INT(out_len, sizeof(body));
    AG_CHECK(memcmp(out, body, sizeof(body)) == 0);
}

static void test_slip_two_frames_and_bootnoise(void)
{
    uint8_t       buf[32], out[32];
    size_t        out_len = 0;
    ag_esp_slip_t d;
    ag_esp_slip_init(&d, buf, sizeof(buf));

    /* Boot text before the first delimiter is ignored; then two frames, the
     * second sharing the delimiter that closed the first. */
    const uint8_t stream[] = {'l', 'd', ESP_SLIP_END, 0xAA, ESP_SLIP_END,
                              0xBB, 0xCC, ESP_SLIP_END};
    AG_CHECK_INT(feed_all(&d, stream, sizeof(stream), out, &out_len), 2);
    AG_CHECK_INT(out_len, 2);
    AG_CHECK_INT(out[0], 0xBB);
    AG_CHECK_INT(out[1], 0xCC);
}

static void test_slip_overflow_recovers(void)
{
    uint8_t       buf[4], out[16];
    size_t        out_len = 0;
    ag_esp_slip_t d;
    ag_esp_slip_init(&d, buf, sizeof(buf));

    /* First frame is too big for the 4-byte buffer and must be dropped; the
     * decoder then finds the next, well-sized frame. */
    const uint8_t stream[] = {ESP_SLIP_END, 1, 2, 3, 4, 5, ESP_SLIP_END,
                              ESP_SLIP_END, 9, 9, ESP_SLIP_END};
    AG_CHECK_INT(feed_all(&d, stream, sizeof(stream), out, &out_len), 1);
    AG_CHECK_INT(out_len, 2);
    AG_CHECK_INT(out[0], 9);
    AG_CHECK_INT(out[1], 9);
}

static void test_cmd_build_layout(void)
{
    const uint8_t data[] = {0xDE, 0xAD};
    uint8_t       cmd[16];
    const size_t  n = ag_esp_cmd_build(ESP_CMD_READ_REG, data, sizeof(data),
                                       0x11223344u, cmd, sizeof(cmd));
    AG_CHECK_INT(n, ESP_CMD_HDR_SIZE + 2);
    AG_CHECK_INT(cmd[0], 0x00);           /* request direction */
    AG_CHECK_INT(cmd[1], ESP_CMD_READ_REG);
    AG_CHECK_INT(cmd[2], 0x02);           /* size low */
    AG_CHECK_INT(cmd[3], 0x00);           /* size high */
    AG_CHECK_INT(cmd[4], 0x44);           /* checksum, little-endian */
    AG_CHECK_INT(cmd[5], 0x33);
    AG_CHECK_INT(cmd[6], 0x22);
    AG_CHECK_INT(cmd[7], 0x11);
    AG_CHECK_INT(cmd[8], 0xDE);
    AG_CHECK_INT(cmd[9], 0xAD);
}

static void test_resp_parse_and_status(void)
{
    /* A READ_REG-shaped response: value 0xCAFEBABE, two-byte OK trailer. */
    const uint8_t ok[] = {0x01, ESP_CMD_READ_REG, 0x02, 0x00,
                          0xBE, 0xBA, 0xFE, 0xCA, 0x00, 0x00};
    ag_esp_resp_t r;
    AG_CHECK(ag_esp_resp_parse(ok, sizeof(ok), &r));
    AG_CHECK_INT(r.op, ESP_CMD_READ_REG);
    AG_CHECK_INT(r.value, (long)0xCAFEBABEu);
    AG_CHECK(ag_esp_resp_ok(&r, 2, NULL));

    /* Same, but the two-byte status says failure with error code 5. */
    const uint8_t bad[] = {0x01, ESP_CMD_FLASH_DATA, 0x02, 0x00,
                           0x00, 0x00, 0x00, 0x00, 0x01, 0x05};
    uint8_t err = 0;
    AG_CHECK(ag_esp_resp_parse(bad, sizeof(bad), &r));
    AG_CHECK(!ag_esp_resp_ok(&r, 2, &err));
    AG_CHECK_INT(err, 5);

    /* The ESP32 family's four-byte trailer: status in the first byte, error in
     * the second, the last two reserved.  Here it fails with code 7. */
    const uint8_t bad4[] = {0x01, ESP_CMD_FLASH_DATA, 0x04, 0x00,
                            0x00, 0x00, 0x00, 0x00, 0x01, 0x07, 0x00, 0x00};
    err = 0;
    AG_CHECK(ag_esp_resp_parse(bad4, sizeof(bad4), &r));
    AG_CHECK(!ag_esp_resp_ok(&r, 4, &err));
    AG_CHECK_INT(err, 7);
    /* Judged with the wrong (2-byte) length, that same frame reads as OK - so
     * the length really does have to come from the chip. */
    AG_CHECK(ag_esp_resp_ok(&r, 2, NULL));

    /* A request direction (0x00) is not a response. */
    const uint8_t req[] = {0x00, ESP_CMD_SYNC, 0x00, 0x00, 0, 0, 0, 0};
    AG_CHECK(!ag_esp_resp_parse(req, sizeof(req), &r));
}

static void test_checksum_and_sync(void)
{
    const uint8_t block[] = {0x00, 0xFF, 0x0F};
    /* seed 0xEF ^ 0x00 ^ 0xFF ^ 0x0F = 0xEF ^ 0xFF ^ 0x0F = 0x10 ^ 0x0F = 0x1F */
    AG_CHECK_INT(ag_esp_checksum(block, sizeof(block)), 0x1F);
    /* An empty block is just the seed. */
    AG_CHECK_INT(ag_esp_checksum(block, 0), ESP_CHECKSUM_SEED);

    uint8_t sync[ESP_SYNC_PAYLOAD];
    AG_CHECK_INT(ag_esp_sync_payload(sync, sizeof(sync)), ESP_SYNC_PAYLOAD);
    AG_CHECK_INT(sync[0], 0x07);
    AG_CHECK_INT(sync[1], 0x07);
    AG_CHECK_INT(sync[2], 0x12);
    AG_CHECK_INT(sync[3], 0x20);
    AG_CHECK_INT(sync[4], 0x55);
    AG_CHECK_INT(sync[ESP_SYNC_PAYLOAD - 1], 0x55);
}

/* Values pinned against esptool's get_erase_size for the ESP8266 ROM loader. */
static void test_erase_size(void)
{
    AG_CHECK_INT(ag_esp8266_erase_size(0, 1024), 4096);      /* head branch */
    AG_CHECK_INT(ag_esp8266_erase_size(0, 4096), 4096);
    AG_CHECK_INT(ag_esp8266_erase_size(0, 70000), 36864);    /* head branch */
    AG_CHECK_INT(ag_esp8266_erase_size(0, 200000), 135168);  /* body branch */
    AG_CHECK_INT(ag_esp8266_erase_size(0x1000, 4096), 4096);
}

static void test_put_get32(void)
{
    uint8_t b[4];
    ag_esp_put32(b, 0x89ABCDEFu);
    AG_CHECK_INT(b[0], 0xEF);
    AG_CHECK_INT(b[1], 0xCD);
    AG_CHECK_INT(b[2], 0xAB);
    AG_CHECK_INT(b[3], 0x89);
    AG_CHECK_INT(ag_esp_get32(b), (long)0x89ABCDEFu);
}

void run_espldr_tests(void)
{
    test_slip_roundtrip();
    test_slip_two_frames_and_bootnoise();
    test_slip_overflow_recovers();
    test_cmd_build_layout();
    test_resp_parse_and_status();
    test_checksum_and_sync();
    test_erase_size();
    test_put_get32();
}
