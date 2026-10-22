/*
 * ArgonOS - the screen coming back down the cable, in messages.  See ag_vid.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ag_vid.h"

#include <string.h>

uint16_t ag_vid_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t ag_vid_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

bool ag_vid_header(uint8_t *hdr, uint8_t type, uint8_t flags, uint32_t len)
{
    if (hdr == NULL || len > AG_VID_MAX_PAYLOAD) {
        return false;
    }
    hdr[0] = AG_VID_MAGIC0;
    hdr[1] = AG_VID_MAGIC1;
    hdr[2] = type;
    hdr[3] = flags;
    hdr[4] = (uint8_t)(len & 0xFFu);
    hdr[5] = (uint8_t)((len >> 8) & 0xFFu);
    hdr[6] = (uint8_t)((len >> 16) & 0xFFu);
    hdr[7] = (uint8_t)((len >> 24) & 0xFFu);
    return true;
}

void ag_vid_rx_init(ag_vid_rx_t *rx, uint8_t *buf, uint32_t cap)
{
    if (rx == NULL) {
        return;
    }
    memset(rx, 0, sizeof(*rx));
    rx->buf = buf;
    rx->cap = cap;
}

/* Drops `n` bytes off the front. */
static void rx_drop(ag_vid_rx_t *rx, uint32_t n)
{
    if (n >= rx->have) {
        rx->have = 0;
        return;
    }
    memmove(rx->buf, rx->buf + n, rx->have - n);
    rx->have -= n;
}

/*
 * The message handed out last time goes now.  Both entry points do this first,
 * which is what "valid until the next call to either function" means: the
 * caller gets a pointer into the buffer rather than a copy, and the buffer
 * cannot move under it until it asks for something else.
 */
static void rx_retire(ag_vid_rx_t *rx)
{
    if (rx->pending > 0) {
        rx_drop(rx, rx->pending);
        rx->pending = 0;
    }
}

uint32_t ag_vid_rx_room(const ag_vid_rx_t *rx)
{
    if (rx == NULL || rx->buf == NULL) {
        return 0;
    }
    /*
     * The message handed out last time still occupies the front of the buffer
     * and will go on the next call, so it counts as room: a caller asking how
     * much to read is about to make that call.
     */
    const uint32_t used = (rx->have > rx->pending) ? rx->have - rx->pending : 0;
    return (used < rx->cap) ? rx->cap - used : 0;
}

uint32_t ag_vid_rx_feed(ag_vid_rx_t *rx, const void *data, uint32_t len)
{
    if (rx == NULL || rx->buf == NULL || data == NULL) {
        return 0;
    }
    rx_retire(rx);
    const uint32_t room = (rx->have < rx->cap) ? (rx->cap - rx->have) : 0u;
    const uint32_t take = (len < room) ? len : room;
    if (take > 0) {
        memcpy(rx->buf + rx->have, data, take);
        rx->have += take;
    }
    return take;
}

/*
 * Steps forward to the next place a header could start.  Returns true when the
 * buffer now begins with the magic; false when it does not and there is nothing
 * left to look at.
 *
 * The search starts at byte 1, not byte 0: this is only called when byte 0 has
 * already been judged not to be the start of a message, and starting at 0 would
 * find the same bad header forever.
 */
static bool rx_resync(ag_vid_rx_t *rx)
{
    for (uint32_t i = 1; i + 1 < rx->have; i++) {
        if (rx->buf[i] == AG_VID_MAGIC0 && rx->buf[i + 1] == AG_VID_MAGIC1) {
            rx_drop(rx, i);
            rx->resyncs++;
            return true;
        }
    }
    /*
     * No magic anywhere.  Keep the last byte: it may be the first half of a
     * magic whose second half has not arrived.
     */
    if (rx->have > 1) {
        rx_drop(rx, rx->have - 1);
    }
    rx->resyncs++;
    return false;
}

bool ag_vid_rx_next(ag_vid_rx_t *rx, ag_vid_msg_t *msg)
{
    if (rx == NULL || rx->buf == NULL || msg == NULL) {
        return false;
    }
    rx_retire(rx);

    for (;;) {
        if (rx->have < 2) {
            return false;
        }
        if (rx->buf[0] != AG_VID_MAGIC0 || rx->buf[1] != AG_VID_MAGIC1) {
            if (!rx_resync(rx)) {
                return false;
            }
            continue;
        }
        if (rx->have < AG_VID_HDR) {
            return false;
        }

        const uint32_t len = ag_vid_u32(rx->buf + 4);
        if (len > AG_VID_MAX_PAYLOAD) {
            /*
             * A length nothing would ever send.  Either the stream lost bytes
             * and this is not a header at all, or the far end is not ours;
             * both are answered by hunting for the next magic rather than by
             * trusting a number that would size a read.
             */
            if (!rx_resync(rx)) {
                return false;
            }
            continue;
        }
        if (rx->have < AG_VID_HDR + len) {
            return false; /* the rest is still on the wire */
        }

        msg->type = rx->buf[2];
        msg->flags = rx->buf[3];
        msg->payload = rx->buf + AG_VID_HDR;
        msg->len = len;
        msg->raw = rx->buf;
        msg->raw_len = AG_VID_HDR + len;
        rx->msgs++;
        rx->pending = msg->raw_len;
        return true;
    }
}

bool ag_vid_parse_hello(const uint8_t *payload, uint32_t len,
                        ag_vid_hello_t *out)
{
    if (payload == NULL || out == NULL || len < AG_VID_HELLO_LEN) {
        return false;
    }
    out->version = payload[0];
    out->flags = payload[1];
    out->width = ag_vid_u16(payload + 2);
    out->height = ag_vid_u16(payload + 4);
    out->tile = ag_vid_u16(payload + 6);
    return out->version == 1u && out->width > 0 && out->height > 0;
}

bool ag_vid_parse_tile(const uint8_t *payload, uint32_t len,
                       ag_vid_rect_t *out, const uint8_t **png,
                       uint32_t *png_len)
{
    if (payload == NULL || out == NULL || len < AG_VID_RECT_LEN) {
        return false;
    }
    out->x = ag_vid_u16(payload + 0);
    out->y = ag_vid_u16(payload + 2);
    out->w = ag_vid_u16(payload + 4);
    out->h = ag_vid_u16(payload + 6);
    if (out->w == 0 || out->h == 0) {
        return false;
    }
    if (png != NULL) {
        *png = payload + AG_VID_RECT_LEN;
    }
    if (png_len != NULL) {
        *png_len = len - AG_VID_RECT_LEN;
    }
    return true;
}
