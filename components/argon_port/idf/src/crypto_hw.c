/*
 * ArgonOS port: ESP-IDF - the SSH crypto primitives, on mbedTLS.
 *
 * mbedTLS is already linked whenever TLS or SSH is built (see CMakeLists.txt),
 * and every curve, cipher and hash used here is one TLS already pulls in, so
 * this file is nearly all the extra code SSH costs.  Field access stays behind
 * the public API - read/write_binary for points and mpis, the crypt/sign calls
 * for the rest - so no MBEDTLS_ALLOW_PRIVATE_ACCESS is needed.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdkconfig.h"

#if defined(CONFIG_ARGON_NET_SSH) && CONFIG_ARGON_NET_SSH

#include <stdlib.h>
#include <string.h>

#include <argon/port/crypto.h>

#include "esp_random.h"

#include "mbedtls/aes.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

/* The RNG mbedTLS asks for: the same hardware CSPRNG TLS trusts. */
static int rng_cb(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    esp_fill_random(out, len);
    return 0;
}

/* ---- hashing ----------------------------------------------------------- */

void ag_crypto_sha256(const void *data, size_t len, uint8_t out[32])
{
    (void)mbedtls_sha256((const unsigned char *)data, len, out, 0 /* not 224 */);
}

void ag_crypto_hmac_sha256(const uint8_t *key, size_t keylen, const void *data,
                           size_t len, uint8_t out[32])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL) {
        memset(out, 0, 32);
        return;
    }
    (void)mbedtls_md_hmac(info, key, keylen, (const unsigned char *)data, len,
                          out);
}

/* ---- X25519 ------------------------------------------------------------ */

int ag_crypto_x25519_keypair(uint8_t priv[32], uint8_t pub[32])
{
    mbedtls_ecp_group grp;
    mbedtls_mpi       d;
    mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Q);

    int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    if (rc == 0) {
        rc = mbedtls_ecdh_gen_public(&grp, &d, &Q, rng_cb, NULL);
    }
    if (rc == 0) {
        rc = mbedtls_mpi_write_binary_le(&d, priv, 32);
    }
    if (rc == 0) {
        size_t olen = 0;
        rc = mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                            &olen, pub, 32);
        /* A Montgomery point serialises to the 32-byte little-endian u. */
        if (rc == 0 && olen != 32) {
            rc = -1;
        }
    }

    mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return rc == 0 ? 0 : -1;
}

int ag_crypto_x25519(uint8_t out[32], const uint8_t priv[32],
                     const uint8_t peer_pub[32])
{
    mbedtls_ecp_group grp;
    mbedtls_mpi       d;
    mbedtls_mpi       z;
    mbedtls_ecp_point Qp;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);
    mbedtls_ecp_point_init(&Qp);

    int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    if (rc == 0) {
        rc = mbedtls_mpi_read_binary_le(&d, priv, 32);
    }
    if (rc == 0) {
        rc = mbedtls_ecp_point_read_binary(&grp, &Qp, peer_pub, 32);
    }
    if (rc == 0) {
        rc = mbedtls_ecdh_compute_shared(&grp, &z, &Qp, &d, rng_cb, NULL);
    }
    if (rc == 0) {
        /* The RFC 7748 output: the u-coordinate, little-endian, 32 bytes. */
        rc = mbedtls_mpi_write_binary_le(&z, out, 32);
    }

    mbedtls_ecp_point_free(&Qp);
    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return rc == 0 ? 0 : -1;
}

/* ---- ECDSA P-256 host key ---------------------------------------------- */

int ag_crypto_ecdsa_p256_genkey(uint8_t priv[32], uint8_t pub[64])
{
    mbedtls_ecp_group grp;
    mbedtls_mpi       d;
    mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Q);

    int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc == 0) {
        rc = mbedtls_ecp_gen_keypair(&grp, &d, &Q, rng_cb, NULL);
    }
    if (rc == 0) {
        rc = mbedtls_mpi_write_binary(&d, priv, 32);
    }
    if (rc == 0) {
        uint8_t  buf[65];
        size_t   olen = 0;
        rc = mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                            &olen, buf, sizeof(buf));
        if (rc == 0 && olen == 65 && buf[0] == 0x04) {
            memcpy(pub, buf + 1, 64); /* drop the 0x04 tag: keep X||Y */
        } else if (rc == 0) {
            rc = -1;
        }
    }

    mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return rc == 0 ? 0 : -1;
}

int ag_crypto_ecdsa_p256_sign(const uint8_t priv[32], const uint8_t digest[32],
                              uint8_t sig_r[32], uint8_t sig_s[32])
{
    mbedtls_ecp_group grp;
    mbedtls_mpi       d;
    mbedtls_mpi       r;
    mbedtls_mpi       s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc == 0) {
        rc = mbedtls_mpi_read_binary(&d, priv, 32);
    }
    if (rc == 0) {
        rc = mbedtls_ecdsa_sign(&grp, &r, &s, &d, digest, 32, rng_cb, NULL);
    }
    if (rc == 0) {
        rc = mbedtls_mpi_write_binary(&r, sig_r, 32);
    }
    if (rc == 0) {
        rc = mbedtls_mpi_write_binary(&s, sig_s, 32);
    }

    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return rc == 0 ? 0 : -1;
}

/* ---- AES-256-CTR ------------------------------------------------------- */

struct ag_aes_ctr {
    mbedtls_aes_context aes;
    size_t              nc_off;
    uint8_t             nonce[16];  /* the running counter block */
    uint8_t             stream[16]; /* leftover keystream between calls */
};

ag_aes_ctr_t *ag_crypto_aes_ctr_new(const uint8_t key[32], const uint8_t iv[16])
{
    struct ag_aes_ctr *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    mbedtls_aes_init(&c->aes);
    if (mbedtls_aes_setkey_enc(&c->aes, key, 256) != 0) {
        mbedtls_aes_free(&c->aes);
        free(c);
        return NULL;
    }
    c->nc_off = 0;
    memcpy(c->nonce, iv, 16);
    memset(c->stream, 0, sizeof(c->stream));
    return c;
}

void ag_crypto_aes_ctr_xcrypt(ag_aes_ctr_t *c, const uint8_t *in, uint8_t *out,
                              size_t len)
{
    if (c == NULL) {
        return;
    }
    (void)mbedtls_aes_crypt_ctr(&c->aes, len, &c->nc_off, c->nonce, c->stream,
                                in, out);
}

void ag_crypto_aes_ctr_free(ag_aes_ctr_t *c)
{
    if (c == NULL) {
        return;
    }
    mbedtls_aes_free(&c->aes);
    free(c);
}

#endif /* CONFIG_ARGON_NET_SSH */
