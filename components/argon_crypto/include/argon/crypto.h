/*
 * ArgonOS - the maths a signature needs, in the kernel (ABI 0.71).
 *
 * See components/argon_crypto/Kconfig for why this is a kernel library rather
 * than a vendored copy in each image, and sdk/include/argon/abi.h for the
 * table itself.
 *
 * Applications do not include this header: they call the inline helpers in
 * argon.h - ag_sha256_init(), ag_sha512(), ag_ed25519_verify() - which go
 * through api->crypto.  This is what the kernel links against.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_CRYPTO_H
#define ARGON_CRYPTO_H

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

void ag_crypto_sha256_init(ag_sha256_t *c);
void ag_crypto_sha256_update(ag_sha256_t *c, const void *data, size_t len);
void ag_crypto_sha256_final(ag_sha256_t *c, uint8_t out[32]);

void ag_crypto_sha512_init(ag_sha512_t *c);
void ag_crypto_sha512_update(ag_sha512_t *c, const void *data, size_t len);
void ag_crypto_sha512_final(ag_sha512_t *c, uint8_t out[64]);

bool ag_crypto_ed25519_verify(const uint8_t sig[64], const void *msg,
                              size_t len, const uint8_t pub[32]);

/* The table api->crypto points at; the kernel wires it up in loader/api.c. */
extern const ag_crypto_api_t ag_crypto_api_impl;

#ifdef __cplusplus
}
#endif

#endif /* ARGON_CRYPTO_H */
