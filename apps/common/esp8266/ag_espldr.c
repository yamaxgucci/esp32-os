/*
 * ArgonOS - ESP8266 ROM download protocol codec.  See ag_espldr.h.
 *
 * Freestanding on purpose: no libc, no allocation, no I/O.  It builds bytes and
 * reads bytes, and every path is exercised by host-tests/test_espldr.c.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "esp8266/ag_espldr.h"

void ag_esp_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

uint32_t ag_esp_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* ---------------------------------------------------------------------- */
/* SLIP                                                                    */
/* ---------------------------------------------------------------------- */

/* Append one raw byte, escaping the two that mean something in a frame. */
static size_t slip_put(uint8_t b, uint8_t *out, size_t cap, size_t at)
{
    if (b == ESP_SLIP_END) {
        if (at + 2 > cap) {
            return 0;
        }
        out[at++] = ESP_SLIP_ESC;
        out[at++] = ESP_SLIP_ESC_END;
    } else if (b == ESP_SLIP_ESC) {
        if (at + 2 > cap) {
            return 0;
        }
        out[at++] = ESP_SLIP_ESC;
        out[at++] = ESP_SLIP_ESC_ESC;
    } else {
        if (at + 1 > cap) {
            return 0;
        }
        out[at++] = b;
    }
    return at;
}

size_t ag_esp_slip_encode(const uint8_t *in, size_t in_len, uint8_t *out,
                          size_t out_cap)
{
    if (out == NULL || out_cap < 2) {
        return 0;
    }
    size_t at = 0;
    out[at++] = ESP_SLIP_END;
    for (size_t i = 0; i < in_len; i++) {
        at = slip_put(in[i], out, out_cap, at);
        if (at == 0) {
            return 0;
        }
    }
    if (at + 1 > out_cap) {
        return 0;
    }
    out[at++] = ESP_SLIP_END;
    return at;
}

void ag_esp_slip_init(ag_esp_slip_t *d, uint8_t *buf, size_t cap)
{
    d->buf = buf;
    d->cap = cap;
    d->len = 0;
    d->in_frame = false;
    d->esc = false;
    d->overflow = false;
    d->complete = false;
}

/*
 * One received byte in; true when a frame's closing 0xC0 has just arrived, with
 * the body left in buf/len for the caller to read BEFORE the next feed.  The
 * contract is that read: the frame stays valid only until feed is called again,
 * which is when the buffer is reused for the next one.
 */
bool ag_esp_slip_feed(ag_esp_slip_t *d, uint8_t b)
{
    /*
     * The previous call returned a frame.  Now that the caller has read it, the
     * buffer is free again; the closing 0xC0 already re-opened the frame, so a
     * shared delimiter (one 0xC0 that both ends one frame and begins the next)
     * loses nothing.
     */
    if (d->complete) {
        d->complete = false;
        d->len = 0;
    }

    if (b == ESP_SLIP_END) {
        /*
         * A close only counts when something sits between the delimiters, so
         * idle 0xC0 padding and back-to-back delimiters do not produce empty
         * frames.  Either way the port is now inside a frame for what follows.
         */
        if (d->in_frame && d->len > 0 && !d->overflow) {
            d->complete = true;
            d->esc = false;
            return true; /* len holds the frame; in_frame stays true */
        }
        d->in_frame = true;
        d->len = 0;
        d->esc = false;
        d->overflow = false;
        return false;
    }

    if (!d->in_frame) {
        return false; /* bytes before the first 0xC0 (boot text) are ignored */
    }

    uint8_t v = b;
    if (d->esc) {
        d->esc = false;
        if (b == ESP_SLIP_ESC_END) {
            v = ESP_SLIP_END;
        } else if (b == ESP_SLIP_ESC_ESC) {
            v = ESP_SLIP_ESC;
        } else {
            /* A bad escape is a corrupt frame; drop it and resync on 0xC0. */
            d->overflow = true;
            return false;
        }
    } else if (b == ESP_SLIP_ESC) {
        d->esc = true;
        return false;
    }

    if (d->len >= d->cap) {
        d->overflow = true; /* too big for our buffer; wait for the next 0xC0 */
        return false;
    }
    d->buf[d->len++] = v;
    return false;
}

/* ---------------------------------------------------------------------- */
/* Command frames                                                         */
/* ---------------------------------------------------------------------- */

size_t ag_esp_cmd_build(uint8_t op, const uint8_t *data, uint16_t data_len,
                        uint32_t checksum, uint8_t *out, size_t out_cap)
{
    const size_t total = ESP_CMD_HDR_SIZE + (size_t)data_len;
    if (out == NULL || out_cap < total) {
        return 0;
    }
    out[0] = 0x00; /* request */
    out[1] = op;
    out[2] = (uint8_t)data_len;
    out[3] = (uint8_t)(data_len >> 8);
    ag_esp_put32(out + 4, checksum);
    for (uint16_t i = 0; i < data_len; i++) {
        out[ESP_CMD_HDR_SIZE + i] = data[i];
    }
    return total;
}

bool ag_esp_resp_parse(const uint8_t *frame, size_t frame_len,
                       ag_esp_resp_t *out)
{
    if (frame == NULL || out == NULL || frame_len < ESP_CMD_HDR_SIZE) {
        return false;
    }
    if (frame[0] != 0x01) {
        return false; /* not a response direction */
    }
    const uint16_t size = (uint16_t)(frame[2] | ((uint16_t)frame[3] << 8));
    if ((size_t)size + ESP_CMD_HDR_SIZE > frame_len) {
        return false; /* truncated */
    }
    out->op = frame[1];
    out->size = size;
    out->value = ag_esp_get32(frame + 4);
    out->data = frame + ESP_CMD_HDR_SIZE;
    out->data_len = size;
    return true;
}

bool ag_esp_resp_ok(const ag_esp_resp_t *r, uint8_t status_len, uint8_t *err_out)
{
    if (status_len == 0 || r->size < status_len) {
        return true; /* nothing to judge by (e.g. SYNC, or caller opted out) */
    }
    /* The status is the first byte of the trailer; the second is its error
     * code.  On the four-byte trailer the remaining two are reserved. */
    const uint8_t status = r->data[r->size - status_len];
    if (status != 0 && err_out != NULL) {
        *err_out = r->data[r->size - status_len + 1];
    }
    return status == 0;
}

uint8_t ag_esp_checksum(const uint8_t *data, size_t len)
{
    uint8_t sum = ESP_CHECKSUM_SEED;
    for (size_t i = 0; i < len; i++) {
        sum ^= data[i];
    }
    return sum;
}

size_t ag_esp_sync_payload(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < ESP_SYNC_PAYLOAD) {
        return 0;
    }
    out[0] = 0x07;
    out[1] = 0x07;
    out[2] = 0x12;
    out[3] = 0x20;
    for (size_t i = 4; i < ESP_SYNC_PAYLOAD; i++) {
        out[i] = 0x55;
    }
    return ESP_SYNC_PAYLOAD;
}

/*
 * The ROM erase size, copied field for field from esptool's get_erase_size for
 * the ESP8266 ROM loader.  A block is sixteen 4 KB sectors; the head of the
 * region up to the next block boundary is erased differently from the body, and
 * getting it wrong leaves the tail of an image un-erased.
 */
uint32_t ag_esp8266_erase_size(uint32_t offset, uint32_t size)
{
    const uint32_t sector = 4096u;
    const uint32_t per_block = 16u;

    const uint32_t num_sectors = (size + sector - 1u) / sector;
    const uint32_t start_sector = offset / sector;

    uint32_t head = per_block - (start_sector % per_block);
    if (num_sectors < head) {
        head = num_sectors;
    }

    if (num_sectors < 2u * head) {
        return ((num_sectors + 1u) / 2u) * sector;
    }
    return (num_sectors - head) * sector;
}
