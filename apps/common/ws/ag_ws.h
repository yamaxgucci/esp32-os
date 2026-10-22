/*
 * ArgonOS - the WebSocket protocol, without a socket in sight.
 *
 * A browser cannot open a TCP connection, and a phone is a browser unless
 * somebody installs something.  So the board speaks the one framing a page can
 * open by itself.  This is that framing as pure logic - bytes in, bytes out -
 * for the same reason netmsg.c has no network headers: every defect worth
 * catching here is a parse, and a parse is catchable on the PC.
 *
 * What is here is the server half and only the server half:
 *
 *   - the handshake answer, which is a SHA-1 of the client's key and a magic
 *     string, base64'd.  That is the whole of the "security" in a WebSocket
 *     upgrade: it proves the server understood the protocol and is not a cache
 *     replaying an old response.  It is not authentication and nothing here
 *     pretends otherwise.
 *   - frame headers, built and parsed.  A client's frames are always masked and
 *     a server's must never be, which is not symmetry for its own sake: the
 *     mask exists so a malicious page cannot make a proxy see an attacker's
 *     bytes as a request of its own.
 *
 * Deliberately absent: fragmentation reassembly (a continuation frame is an
 * error to the caller, which then closes), extensions, and permessage-deflate.
 * Everything this carries is already compressed by ag_pixband, and a second
 * pass over the same bytes would cost a board's milliseconds to save a wire's
 * microseconds.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_WS_H
#define AG_WS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opcodes, the ones a server has any business seeing or sending. */
#define AG_WS_CONT 0x0u
#define AG_WS_TEXT 0x1u
#define AG_WS_BIN 0x2u
#define AG_WS_CLOSE 0x8u
#define AG_WS_PING 0x9u
#define AG_WS_PONG 0xau

/* The accept key is 28 base64 characters; this is with the terminator. */
#define AG_WS_ACCEPT_LEN 29

/*
 * The Sec-WebSocket-Accept value for a client's Sec-WebSocket-Key.
 *
 * `key` is the header's value as the client sent it, leading and trailing
 * spaces already trimmed by the caller.  Writes AG_WS_ACCEPT_LEN bytes into
 * `out`, terminated.  False when `key` is empty or absurdly long, which is a
 * request to refuse rather than a value to compute.
 */
bool ag_ws_accept_key(const char *key, char out[AG_WS_ACCEPT_LEN]);

/*
 * SHA-1 of a buffer, which this file has because RFC 6455 froze it into the
 * handshake above.
 *
 * Exposed, rather than left private, for one caller and one purpose: the
 * challenge-response that keeps a password off the wire.  The board sends a
 * nonce, both sides hash it with the password, and only the digest crosses -
 * so a password is never sent in the clear over a link that has no TLS.
 * Preimage resistance is what that needs, and SHA-1 still has it; its broken
 * collision resistance is not load-bearing here any more than it is in the
 * handshake.
 *
 * Do NOT reach for this for anything else.  The ABI offers SHA-256 for the
 * cases that want a hash on their own merits, and this one is here because it
 * was already here.
 */
void ag_ws_sha1(const void *data, size_t len, uint8_t out[20]);

/* One frame header as it arrived. */
typedef struct {
    uint8_t  opcode;
    bool     fin;
    bool     masked;
    uint8_t  mask[4];
    uint32_t len;  /* payload bytes                                          */
    uint32_t hdr;  /* header bytes, i.e. where the payload starts            */
} ag_ws_hdr_t;

/*
 * Parse a frame header out of the front of a buffer.
 *
 *   > 0   header bytes consumed; *out is filled and out->len says how much
 *         payload must arrive after them
 *   = 0   not enough bytes yet; ask again when more have come
 *   < 0   a protocol error the caller must close on: a reserved bit, a 64-bit
 *         length with the high word set (nothing here sends gigabytes and a
 *         peer claiming to is either broken or probing), or a payload longer
 *         than `max_payload`
 */
int32_t ag_ws_hdr_parse(const uint8_t *in, uint32_t len, uint32_t max_payload,
                        ag_ws_hdr_t *out);

/* Undo the client's mask, in place. */
void ag_ws_unmask(uint8_t *p, uint32_t len, const uint8_t mask[4]);

/*
 * Build an unmasked server frame header for `len` payload bytes.  Returns the
 * bytes written (2, 4 or 10); `out` needs 10.
 */
uint32_t ag_ws_hdr_build(uint8_t *out, uint8_t opcode, uint32_t len, bool fin);

/* ---- the client half ---------------------------------------------------- */
/*
 * Here because the board needs to be a client of its own protocol.
 *
 * Everything this link does was checked by a person with a phone, which is
 * slow, unrepeatable and unavailable at three in the morning: "the page does
 * not load" arrived hours after the state that caused it, and half the day
 * went into asking someone to press a key again.  A second board running
 * PHONECL.AXE is a client that can be scripted, so a scenario becomes a test
 * rather than a favour.
 *
 * It is the same framing read the other way round, which is why it belongs in
 * this file and not in a copy of it.
 */

/* A Sec-WebSocket-Key is 16 bytes base64'd: 24 characters and a terminator. */
#define AG_WS_KEY_LEN 25

/*
 * The Sec-WebSocket-Key for a request, from 16 bytes the caller supplies.
 *
 * The bytes should be unpredictable - RFC 6455 asks for a fresh nonce so a
 * cache cannot replay an old response - but nothing here is load bearing on
 * their quality, and this takes them rather than inventing them so that a test
 * can hand it a known value and expect a known answer.
 */
bool ag_ws_client_key(const uint8_t nonce[16], char out[AG_WS_KEY_LEN]);

/*
 * Does the server's Sec-WebSocket-Accept match the key we sent?
 *
 * The only thing the handshake actually proves, and worth checking: a server
 * that answers 101 with the wrong accept is not speaking this protocol, and
 * feeding frames to it produces a puzzle rather than an error.
 */
bool ag_ws_accept_ok(const char *sent_key, const char *accept);

/*
 * Build a masked client frame header for `len` payload bytes.  Returns the
 * bytes written (6, 8 or 14); `out` needs 14.
 *
 * A client's frames MUST be masked - the mask exists so a malicious page
 * cannot make a proxy see an attacker's bytes as a request of its own - and a
 * server is entitled to hang up on an unmasked one, which is exactly what ours
 * does.  Mask the payload with ag_ws_mask after this.
 */
uint32_t ag_ws_hdr_build_masked(uint8_t *out, uint8_t opcode, uint32_t len,
                                bool fin, const uint8_t mask[4]);

/*
 * Apply a mask, in place.  The same XOR as ag_ws_unmask - masking and
 * unmasking are one operation - under the name the sending side reads better
 * with.
 */
void ag_ws_mask(uint8_t *p, uint32_t len, const uint8_t mask[4]);

#ifdef __cplusplus
}
#endif

#endif /* AG_WS_H */
