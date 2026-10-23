/*
 * ArgonOS - the maths a signature needs, kept once for the whole system.
 *
 * These are the functions behind `api->crypto` (ABI 0.71): Ed25519 as RFC 8032
 * says, SHA-256 and SHA-512 as FIPS 180-4 says, and nothing else.  They are
 * plain C with no kernel pointers and no state of their own - the caller holds
 * the context - because a signature is checked over a file that never fits in
 * one buffer, and because a hash of something an application is holding should
 * cost the kernel no memory at all.
 *
 * The state types are the ABI's (ag_sha256_t, ag_sha512_t in argon/abi.h), so
 * an application and the kernel cannot disagree about the size of the thing
 * they pass each other: a change here that changes a layout is a compile error
 * in this file, not a wrong digest on a board.
 *
 * SHA-512 and Ed25519 are Monocypher (vendor/), which is why the vetting of
 * the curve lives in one place: host-tests/test_store_sig.c signs the standard
 * test vectors and compares them byte for byte, through the same API an
 * application uses.  SHA-256 is ours (src/sha256.c), carried over from the shop
 * that used to keep its own copy.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/crypto.h>

#include <stddef.h>

#include "monocypher.h"
#include "monocypher-ed25519.h"

_Static_assert(sizeof(ag_sha512_t) == sizeof(crypto_sha512_ctx),
               "ag_sha512_t in argon/abi.h must stay the size of the kernel's "
               "own SHA-512 state: the ABI hands this struct to applications");

/* ---- SHA-256 ------------------------------------------------------------ */
/*
 * It lives in src/sha256.c, written in the shop and moved here: FIPS 180-4 in
 * a hundred lines, with the standard's vectors in host-tests/test_store.c.  Its
 * functions are the ABI's, so there is nothing to wrap.
 */

/* ---- SHA-512 ------------------------------------------------------------ */

void ag_crypto_sha512_init(ag_sha512_t *c)
{
    crypto_sha512_init((crypto_sha512_ctx *)c);
}

void ag_crypto_sha512_update(ag_sha512_t *c, const void *data, size_t len)
{
    crypto_sha512_update((crypto_sha512_ctx *)c, data, len);
}

void ag_crypto_sha512_final(ag_sha512_t *c, uint8_t out[64])
{
    crypto_sha512_final((crypto_sha512_ctx *)c, out);
}

/* ---- Ed25519 ------------------------------------------------------------ */

bool ag_crypto_ed25519_verify(const uint8_t sig[64], const void *msg,
                              size_t len, const uint8_t pub[32])
{
    if (sig == NULL || pub == NULL || (msg == NULL && len != 0)) {
        return false;
    }
    /* Monocypher answers 0 for "this signature is this key's". */
    return crypto_ed25519_check(sig, pub, msg, len) == 0;
}

const ag_crypto_api_t ag_crypto_api_impl = {
    .size = sizeof(ag_crypto_api_t),
    .sha256_init = ag_crypto_sha256_init,
    .sha256_update = ag_crypto_sha256_update,
    .sha256_final = ag_crypto_sha256_final,
    .sha512_init = ag_crypto_sha512_init,
    .sha512_update = ag_crypto_sha512_update,
    .sha512_final = ag_crypto_sha512_final,
    .ed25519_verify = ag_crypto_ed25519_verify,
};
