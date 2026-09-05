/*
 * ArgonOS - just enough WebSocket to carry a finger.
 *
 * Why a WebSocket at all, when this system already has an HTTP server: because
 * a touchpad produces something like sixty events a second, and a request per
 * event means sixty sets of headers, sixty responses and sixty round trips a
 * second for a payload of about twelve bytes.  On a local network that is not
 * a bandwidth problem, it is a latency and a load problem, and it is the
 * difference between a pointer that follows a finger and one that arrives after
 * it.  A WebSocket is one connection, opened once, with two bytes of framing.
 *
 * Written here rather than taken from somewhere because the part that is needed
 * is small and the part that is not is most of a library.  What is here:
 *
 *   - the handshake's one piece of arithmetic (SHA-1 of the client's key and a
 *     fixed string, base64), which is the whole of RFC 6455 §4.2.2 that a
 *     server has to compute;
 *   - reading a frame from a client, which is always masked, and unmasking it;
 *   - writing a frame to a client, which is never masked.
 *
 * What is not: fragmentation across frames, extensions, compression, and any
 * payload longer than sixty-four kilobytes.  An event from a phone is a line of
 * text about twelve bytes long, and a system that quietly grew a reassembly
 * buffer to accept a fragmented one would be carrying that buffer for the
 * lifetime of the connection to no purpose.  A fragmented frame is refused and
 * says so.
 *
 * SHA-1 is here as a hash, not as a signature: RFC 6455 uses it to prove that
 * both ends understood the handshake, and it has been unfit for anything that
 * needs collision resistance for years.  It is used for nothing else in this
 * tree.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef AG_WS_H
#define AG_WS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* SHA-1 and base64, which the handshake needs and nothing else here does    */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint32_t h[5];
    uint64_t bits;
    uint8_t  block[64];
    uint32_t used;
} ag_sha1_t;

void ag_sha1_init(ag_sha1_t *s);
void ag_sha1_update(ag_sha1_t *s, const void *data, size_t len);
void ag_sha1_final(ag_sha1_t *s, uint8_t out[20]);

/*
 * Base64 of `len` bytes into `out`, NUL-terminated.  Returns the number of
 * characters written, not counting the NUL, or 0 if `out` is too small.  `out`
 * needs 4*((len+2)/3) + 1 bytes.
 */
size_t ag_base64(const void *data, size_t len, char *out, size_t out_size);

/* ------------------------------------------------------------------------ */
/* The handshake                                                            */
/* ------------------------------------------------------------------------ */

/* Sec-WebSocket-Accept is 28 characters plus a NUL. */
#define AG_WS_ACCEPT_LEN 29

/*
 * The value of Sec-WebSocket-Accept for a given Sec-WebSocket-Key.  `key` is
 * the header's value as it arrived, without surrounding spaces.  False when
 * `key` is empty or `out` is too small.
 */
bool ag_ws_accept_key(const char *key, size_t key_len,
                      char out[AG_WS_ACCEPT_LEN]);

/* ------------------------------------------------------------------------ */
/* Frames                                                                   */
/* ------------------------------------------------------------------------ */

enum {
    AG_WS_CONT = 0x0,
    AG_WS_TEXT = 0x1,
    AG_WS_BIN = 0x2,
    AG_WS_CLOSE = 0x8,
    AG_WS_PING = 0x9,
    AG_WS_PONG = 0xA,
};

typedef struct {
    uint8_t  opcode;
    bool     fin;
    uint8_t *payload; /* points into the caller's buffer, unmasked in place */
    uint32_t len;
} ag_ws_frame_t;

/*
 * Take one frame off the front of `buf`.  The payload is unmasked in place, so
 * `buf` must be writable and the frame's payload pointer is only good until the
 * buffer is reused.
 *
 * Returns the number of bytes consumed, 0 when the buffer does not yet hold a
 * whole frame, and a negative value for a frame this will not accept: an
 * unmasked frame from a client (RFC 6455 §5.1 says the connection must fail), a
 * fragment, or a payload longer than AG_WS_MAX_PAYLOAD.
 */
#define AG_WS_MAX_PAYLOAD 65535u

#define AG_WS_ERR_UNMASKED (-1)
#define AG_WS_ERR_FRAGMENT (-2)
#define AG_WS_ERR_TOOBIG (-3)

int32_t ag_ws_decode(uint8_t *buf, size_t len, ag_ws_frame_t *out);

/*
 * The header for a frame a server sends: never masked, so two bytes for
 * anything shorter than 126 and four up to 65535.  Returns the header length,
 * or 0 for a payload this cannot describe.  `hdr` needs four bytes.
 */
uint32_t ag_ws_header(uint8_t *hdr, uint8_t opcode, size_t payload_len);

#ifdef __cplusplus
}
#endif

#endif /* AG_WS_H */
