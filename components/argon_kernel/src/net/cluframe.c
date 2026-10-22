/*
 * ArgonOS - cutting a cluster message into frames and putting it back.
 * See argon/cluframe.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/cluframe.h>

#include <string.h>

#include <argon/abi.h>

/* Little-endian by hand, as everywhere else in this tree that puts bytes on a
 * wire: the two ends may not be the same chip. */
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

size_t ag_cluf_put(uint8_t *buf, const ag_cluf_hdr_t *h, const void *payload)
{
    if (buf == NULL || h == NULL) {
        return 0;
    }
    buf[0] = h->op;
    buf[1] = h->flags;
    put_u16(buf + 2, h->len);
    put_u32(buf + 4, (uint32_t)h->a);
    put_u32(buf + 8, (uint32_t)h->b);
    put_u32(buf + 12, h->c);
    if (h->len > 0 && payload != NULL) {
        memcpy(buf + AG_CLUF_HDR, payload, h->len);
    }
    return (size_t)AG_CLUF_HDR + h->len;
}

bool ag_cluf_get(const uint8_t *buf, uint16_t n, ag_cluf_hdr_t *h,
                 const uint8_t **payload)
{
    if (buf == NULL || h == NULL || n < AG_CLUF_HDR) {
        return false;
    }
    h->op = buf[0];
    h->flags = buf[1];
    h->len = get_u16(buf + 2);
    h->a = (int32_t)get_u32(buf + 4);
    h->b = (int32_t)get_u32(buf + 8);
    h->c = get_u32(buf + 12);

    if ((uint32_t)h->len + AG_CLUF_HDR > (uint32_t)n) {
        return false;
    }
    if (payload != NULL) {
        *payload = buf + AG_CLUF_HDR;
    }
    return true;
}

/* ---------------------------------------------------------------------- */

static void slot_drop(ag_cluf_rx_t *rx, ag_cluf_slot_t *s)
{
    if (s->buf != NULL && rx->free_fn != NULL) {
        rx->free_fn(s->buf, rx->ctx);
    }
    memset(s, 0, sizeof(*s));
}

static ag_cluf_slot_t *slot_find(ag_cluf_rx_t *rx, int32_t from, int32_t to)
{
    for (uint32_t i = 0; i < AG_CLUF_SLOTS; i++) {
        ag_cluf_slot_t *s = &rx->slot[i];
        if (s->used && s->from == from && s->to == to) {
            return s;
        }
    }
    return NULL;
}

void ag_cluf_rx_reset(ag_cluf_rx_t *rx)
{
    if (rx == NULL) {
        return;
    }
    for (uint32_t i = 0; i < AG_CLUF_SLOTS; i++) {
        slot_drop(rx, &rx->slot[i]);
    }
}

int ag_cluf_rx_data(ag_cluf_rx_t *rx, const ag_cluf_hdr_t *h,
                    const uint8_t *payload, uint8_t **out_buf,
                    uint32_t *out_len, int32_t *from, int32_t *to)
{
    if (rx == NULL || h == NULL || h->op != AG_CLUF_DATA) {
        return -AG_EINVAL;
    }

    ag_cluf_slot_t *s = slot_find(rx, h->a, h->b);

    if ((h->flags & AG_CLUF_F_FIRST) != 0) {
        /*
         * A first fragment for a conversation that already has one means the
         * sender gave up part way - a thread that ended, a job that was cut
         * off - and started again.  The old half is rubbish; say so by
         * counting it, because a drop that leaves no trace is the kind of
         * fault that gets blamed on the wire.
         */
        if (s != NULL) {
            slot_drop(rx, s);
            rx->dropped++;
        }
        for (uint32_t i = 0; i < AG_CLUF_SLOTS && s == NULL; i++) {
            if (!rx->slot[i].used) {
                s = &rx->slot[i];
            }
        }
        if (s == NULL) {
            rx->dropped++;
            return -AG_EBUSY;
        }
        if (h->c == 0 || h->c > AG_CLU_MSG_MAX) {
            rx->dropped++;
            return -AG_ERANGE;
        }
        s->buf = (uint8_t *)((rx->alloc_fn != NULL) ? rx->alloc_fn(h->c, rx->ctx)
                                                    : NULL);
        if (s->buf == NULL) {
            memset(s, 0, sizeof(*s));
            rx->dropped++;
            return -AG_ENOMEM;
        }
        s->used = true;
        s->from = h->a;
        s->to = h->b;
        s->total = h->c;
        s->got = 0;
    } else if (s == NULL) {
        /*
         * A continuation with nothing to continue.  It happens after the first
         * fragment was dropped above, and the rest of that message must not be
         * mistaken for the start of another.
         */
        rx->dropped++;
        return -AG_ENOENT;
    }

    if ((uint32_t)s->got + h->len > s->total) {
        slot_drop(rx, s);
        rx->dropped++;
        return -AG_ERANGE;
    }
    if (h->len > 0 && payload != NULL) {
        memcpy(s->buf + s->got, payload, h->len);
    }
    s->got += h->len;

    if ((h->flags & AG_CLUF_F_LAST) == 0) {
        return 0;
    }
    if (s->got != s->total) {
        slot_drop(rx, s);
        rx->dropped++;
        return -AG_EIO;
    }

    if (out_buf != NULL) {
        *out_buf = s->buf;
    }
    if (out_len != NULL) {
        *out_len = s->total;
    }
    if (from != NULL) {
        *from = s->from;
    }
    if (to != NULL) {
        *to = s->to;
    }
    /* Handed over, so cleared without freeing: it is the caller's now. */
    memset(s, 0, sizeof(*s));
    return 1;
}
