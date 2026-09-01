/*
 * ArgonOS port contract - the cryptographic primitives SSH is built from.
 *
 * The kernel may not include mbedTLS (or any ESP-IDF header); it speaks the SSH
 * transport itself and reaches for the maths through here.  Everything below is
 * already linked for TLS, so turning SSH on adds almost no code - see the note
 * in CMakeLists.txt.  All buffers belong to the caller; nothing here allocates
 * except the AES stream context, which is opaque and freed explicitly.
 *
 * Byte orders are the ones the SSH wire wants, so the kernel never converts:
 *   - X25519 public keys and shared secrets are the 32-byte RFC 7748 strings
 *     (little-endian u), exactly as they travel and as OpenSSH hashes them.
 *   - ECDSA P-256 keys and signatures are big-endian fixed-width (X, Y, r, s
 *     are each 32 bytes), which mpint/string encoding then wraps.
 *
 * Functions returning int give 0 on success and negative on failure.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_CRYPTO_H
#define ARGON_PORT_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

/* ---- hashing ----------------------------------------------------------- */

/* SHA-256 of one buffer. */
void ag_crypto_sha256(const void *data, size_t len, uint8_t out[32]);

/* HMAC-SHA-256 over one buffer. */
void ag_crypto_hmac_sha256(const uint8_t *key, size_t keylen, const void *data,
                           size_t len, uint8_t out[32]);

/* ---- X25519 (curve25519-sha256 key exchange) --------------------------- */

/*
 * A fresh ephemeral keypair.  `priv` is kept only long enough to feed the one
 * matching ag_crypto_x25519() call below; `pub` is what goes on the wire.
 */
int ag_crypto_x25519_keypair(uint8_t priv[32], uint8_t pub[32]);

/* The shared secret from our private half and the peer's public half. */
int ag_crypto_x25519(uint8_t out[32], const uint8_t priv[32],
                     const uint8_t peer_pub[32]);

/* ---- ECDSA P-256 (the host key, ecdsa-sha2-nistp256) ------------------- */

/*
 * A new host keypair.  `priv` is the secret scalar; `pub` is the public point
 * as X||Y.  The caller persists both (there is no derive-public-from-private
 * here, deliberately - it would need the curve generator and buy nothing).
 */
int ag_crypto_ecdsa_p256_genkey(uint8_t priv[32], uint8_t pub[64]);

/*
 * Sign a 32-byte digest with the host key.  `digest` is already SHA-256(H) -
 * this is the raw ECDSA step over it.  Outputs the signature as r||s, each a
 * fixed-width 32-byte big-endian integer.
 */
int ag_crypto_ecdsa_p256_sign(const uint8_t priv[32], const uint8_t digest[32],
                              uint8_t sig_r[32], uint8_t sig_s[32]);

/* ---- AES-256-CTR (the record cipher, both directions) ------------------ */

/*
 * A keyed CTR stream.  One per direction: the counter advances across packets,
 * so the same context is reused for the whole connection and freed at the end.
 */
typedef struct ag_aes_ctr ag_aes_ctr_t;

ag_aes_ctr_t *ag_crypto_aes_ctr_new(const uint8_t key[32], const uint8_t iv[16]);

/*
 * XOR the keystream over len bytes in place-safe fashion (in may equal out).
 *
 * The `in` and `out` buffers MUST be in DMA-reachable memory (AG_MEM_DMA -
 * internal SRAM on this part), NOT PSRAM.  The AES here runs on the chip's
 * accelerator, which reaches its buffers by DMA, and DMA cannot touch PSRAM:
 * a call over a PSRAM buffer hangs the task with no error.
 */
void ag_crypto_aes_ctr_xcrypt(ag_aes_ctr_t *c, const uint8_t *in, uint8_t *out,
                              size_t len);

void ag_crypto_aes_ctr_free(ag_aes_ctr_t *c);

#endif /* ARGON_PORT_CRYPTO_H */
