/*
 * ArgonOS - the screen coming back down the cable, in messages.
 *
 * A serial port has no message boundaries, so this puts them there.  What flows
 * on /dev/usbcdc0 is a stream of these, sent by the agent on the far machine
 * and forwarded by the board to the phone; the board never looks inside one,
 * and that is the point of the whole design.  An ESP32 that does not decode a
 * pixel needs no frame buffer, no codec and no CPU, and the wire ends up being
 * the limit rather than we are.
 *
 * So this file is a framer and nothing else: find the start of a message,
 * decide it is whole, hand over the bytes. It is here rather than in the
 * application because getting a framer wrong looks like a bad cable, and a
 * host test can ask it questions a board cannot.
 *
 *   magic  2   'A' 'G' - so a resync after lost bytes has something to look for
 *   type   1   ag_vid_type_t
 *   flags  1
 *   len    4   little endian, the payload that follows
 *   payload
 *
 * Eight bytes of header for payloads of a few kilobytes: about a quarter of a
 * percent, which is not worth compressing away, and a magic that a resync can
 * hunt for is worth far more than that.
 *
 * Bytes *do* get lost.  A serial port cannot tell you when its receive buffer
 * filled - the bytes are simply gone and nothing counted them - so the framer
 * has to assume it will one day find rubbish where a header should be.  When it
 * does, it steps forward one byte at a time looking for the magic again and
 * counts the resync, which is the only honest measure of loss this system has.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef AG_VID_H
#define AG_VID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AG_VID_MAGIC0 'A'
#define AG_VID_MAGIC1 'G'
#define AG_VID_HDR 8u

/*
 * The largest payload anything may send.  Not a guess: it is what the board
 * must be able to hold at once, and the board is the machine with the least
 * memory in the chain.  The agent's job is to choose tiles whose compressed
 * form fits - if one does not, it splits the tile rather than growing this.
 */
#define AG_VID_MAX_PAYLOAD 8192u
#define AG_VID_MAX_MSG (AG_VID_HDR + AG_VID_MAX_PAYLOAD)

typedef enum {
    /*
     * Agent to board (and on to the phone).
     */
    AG_VID_HELLO = 0x01, /* screen size and what the agent is; see below     */
    AG_VID_TILE = 0x02,  /* x,y,w,h then a PNG of that rectangle             */
    AG_VID_BYE = 0x03,   /* the agent is going away on purpose               */

    /*
     * Phone to board to agent.  Input does *not* come this way - it goes out
     * as HID, which is the whole point: the keyboard keeps working when the
     * agent is dead, or was never started, or the far machine is in its BIOS.
     * What comes this way is only what HID cannot express.
     */
    AG_VID_CLIP = 0x10,  /* UTF-8 text for the far machine's clipboard       */
    AG_VID_WANT = 0x11,  /* send a whole screen: a phone that just connected */
    AG_VID_TYPE = 0x12,  /* UTF-8 text the agent injects as Unicode keys      */

    /*
     * The two that do not travel on to the phone: the board and the agent use
     * them to agree on Wi-Fi and the page password, so a dongle plugged into a
     * machine that has never seen it can be told which network to join without
     * any console on that machine.
     */
    AG_VID_CONFIG = 0x20,    /* agent->board: ssid '\0' pass '\0' webpass '\0' */
    AG_VID_CFG_STATE = 0x21, /* board->agent: one state byte then an IP string */
} ag_vid_type_t;

/* AG_VID_CFG_STATE's first payload byte. */
#define AG_VID_CFG_NEED 0u    /* no Wi-Fi configured yet - ask the user       */
#define AG_VID_CFG_JOINING 1u /* credentials in hand, not associated yet      */
#define AG_VID_CFG_READY 2u   /* on the network; the IP string follows        */

/* AG_VID_HELLO payload: fixed part, then a UTF-8 name with no terminator. */
typedef struct {
    uint8_t  version; /* 1                                                  */
    uint8_t  flags;
    uint16_t width;  /* the far machine's screen, in its own pixels         */
    uint16_t height;
    uint16_t tile;   /* the grid the agent works in, for the phone's canvas */
} ag_vid_hello_t;

#define AG_VID_HELLO_LEN 8u

/* AG_VID_TILE payload: eight bytes, then the PNG. */
typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
} ag_vid_rect_t;

#define AG_VID_RECT_LEN 8u

/* ------------------------------------------------------------------------ */
/* Writing                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * Writes a header for `len` payload bytes into `hdr` (AG_VID_HDR bytes).
 * Returns false for a payload this will not carry.
 */
bool ag_vid_header(uint8_t *hdr, uint8_t type, uint8_t flags, uint32_t len);

/* ------------------------------------------------------------------------ */
/* Reading                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * A framer over a byte stream.  Feed it whatever arrived; ask it for messages
 * until it says there are none.
 *
 * The buffer is the caller's, so that the one big allocation in the chain is
 * made where its cost is visible.  It must be at least AG_VID_MAX_MSG.
 */
typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t have;    /* bytes in buf                                       */
    uint32_t pending; /* a message handed out and not yet dropped           */
    uint32_t resyncs; /* how many times the stream had to be hunted for     */
    uint32_t msgs;    /* how many whole messages have come out              */
} ag_vid_rx_t;

void ag_vid_rx_init(ag_vid_rx_t *rx, uint8_t *buf, uint32_t cap);

/*
 * How many bytes may be added right now.  Read this much and no more.
 *
 * That is not advice.  A caller that reads a fixed-size chunk off the wire and
 * hands it over regardless will one day hand over more than fits, and the
 * excess is gone - which shows up as a stream that resynchronises forever and
 * a picture that never arrives.  This system had exactly that bug, found in
 * the emulator with a resync counter climbing into the hundreds while two
 * messages in total got through.
 */
uint32_t ag_vid_rx_room(const ag_vid_rx_t *rx);

/*
 * Adds bytes.  Returns how many were taken: fewer than offered means the
 * buffer was full, and the difference has been dropped - see ag_vid_rx_room.
 */
uint32_t ag_vid_rx_feed(ag_vid_rx_t *rx, const void *data, uint32_t len);

/*
 * The next whole message, if there is one.  On true, `msg` points into the
 * framer's buffer and stays valid until the next call to either function here.
 *
 * Rubbish where a header should be is not an error and does not stop the
 * stream: the framer steps forward looking for the magic, counts a resync and
 * carries on, which is what a stream that has lost bytes needs.
 */
typedef struct {
    uint8_t        type;
    uint8_t        flags;
    const uint8_t *payload;
    uint32_t       len;
    const uint8_t *raw; /* the whole message, header included, to forward    */
    uint32_t       raw_len;
} ag_vid_msg_t;

bool ag_vid_rx_next(ag_vid_rx_t *rx, ag_vid_msg_t *msg);

/* Little-endian readers, so a caller can take a message apart without
 * assuming anything about its own alignment or byte order. */
uint16_t ag_vid_u16(const uint8_t *p);
uint32_t ag_vid_u32(const uint8_t *p);

/* True when the payload is a well-formed HELLO, with the fields filled in. */
bool ag_vid_parse_hello(const uint8_t *payload, uint32_t len,
                        ag_vid_hello_t *out);

/* True when the payload is a well-formed TILE, with the rectangle filled in
 * and *png / *png_len pointing at the image inside it. */
bool ag_vid_parse_tile(const uint8_t *payload, uint32_t len,
                       ag_vid_rect_t *out, const uint8_t **png,
                       uint32_t *png_len);

#ifdef __cplusplus
}
#endif

#endif /* AG_VID_H */
