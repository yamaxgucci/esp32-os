/*
 * ArgonOS - a program bought in the shop, made for one owner's boards
 * (see argon/licence.h for what it is for and what it is not).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/licence.h>

#include <argon/crypto.h>

#include <string.h>

static void hmac_sha256(const uint8_t key[32], const void *a, size_t a_len,
                        const void *b, size_t b_len, uint8_t out[32])
{
    uint8_t     block[64];
    uint8_t     inner[32];
    ag_sha256_t c;

    memset(block, 0, sizeof(block));
    memcpy(block, key, 32);
    for (size_t i = 0; i < sizeof(block); i++) {
        block[i] ^= 0x36u;
    }
    ag_crypto_sha256_init(&c);
    ag_crypto_sha256_update(&c, block, sizeof(block));
    ag_crypto_sha256_update(&c, a, a_len);
    if (b_len > 0) {
        ag_crypto_sha256_update(&c, b, b_len);
    }
    ag_crypto_sha256_final(&c, inner);

    for (size_t i = 0; i < sizeof(block); i++) {
        block[i] ^= (uint8_t)(0x36u ^ 0x5Cu);
    }
    ag_crypto_sha256_init(&c);
    ag_crypto_sha256_update(&c, block, sizeof(block));
    ag_crypto_sha256_update(&c, inner, sizeof(inner));
    ag_crypto_sha256_final(&c, out);
}

void ag_licence_key_id(const uint8_t key[32], uint8_t id[AG_LICENCE_ID_LEN])
{
    static const char prefix[] = "argon-ownerkey-id 1\n";
    uint8_t     digest[32];
    ag_sha256_t c;
    ag_crypto_sha256_init(&c);
    ag_crypto_sha256_update(&c, prefix, sizeof(prefix) - 1u);
    ag_crypto_sha256_update(&c, key, 32);
    ag_crypto_sha256_final(&c, digest);
    memcpy(id, digest, AG_LICENCE_ID_LEN);
}

ag_err_t ag_licence_find(const uint8_t *tail, size_t len,
                         ag_licence_block_t *out)
{
    if (tail == NULL || out == NULL || len < AG_LICENCE_BLOCK_LEN) {
        return -AG_ENOENT;
    }
    /* The last one: whatever the program itself happens to contain comes
     * before it, and the block is the shop's, placed after the program. */
    for (size_t at = len - AG_LICENCE_BLOCK_LEN + 1u; at-- > 0;) {
        const uint8_t *b = tail + at;
        if (memcmp(b, AG_LICENCE_MAGIC, 4) != 0 ||
            b[4] != AG_LICENCE_VERSION || b[5] != 0 || b[6] != 0 || b[7] != 0) {
            continue;
        }
        memcpy(out->key_id, b + 8, AG_LICENCE_ID_LEN);
        memcpy(out->nonce, b + 8 + AG_LICENCE_ID_LEN, AG_LICENCE_NONCE_LEN);
        memcpy(out->check, b + 8 + AG_LICENCE_ID_LEN + AG_LICENCE_NONCE_LEN,
               AG_LICENCE_CHECK_LEN);
        return AG_OK;
    }
    return -AG_ENOENT;
}

ag_err_t ag_licence_begin(ag_licence_t *lic, const ag_axe_header_t *header,
                          const ag_licence_block_t *block,
                          const uint8_t key[32])
{
    if (lic == NULL || header == NULL || block == NULL || key == NULL) {
        return -AG_EINVAL;
    }
    memset(lic, 0, sizeof(*lic));

    uint8_t id[AG_LICENCE_ID_LEN];
    ag_licence_key_id(key, id);
    if (memcmp(id, block->key_id, sizeof(id)) != 0) {
        return -AG_EPERM;
    }

    /* "argon-licence 1\n" name "\n" nonce: the name as the header has it, up
     * to its first zero, so a copy renamed on disk is still the same program. */
    uint8_t msg[16 + 32 + 1 + AG_LICENCE_NONCE_LEN];
    size_t  n = 0;
    memcpy(msg, "argon-licence 1\n", 16);
    n = 16;
    for (size_t i = 0; i < sizeof(header->name) && header->name[i] != '\0'; i++) {
        msg[n++] = (uint8_t)header->name[i];
    }
    msg[n++] = '\n';
    memcpy(msg + n, block->nonce, AG_LICENCE_NONCE_LEN);
    n += AG_LICENCE_NONCE_LEN;
    hmac_sha256(key, msg, n, NULL, 0, lic->file_key);

    /* The check says this key opens this file - not only that its id matches,
     * which anyone can copy into the block. */
    static const char check_msg[] = "argon-licence-check 1";
    uint8_t check[32];
    hmac_sha256(lic->file_key, check_msg, sizeof(check_msg) - 1u, NULL, 0,
                check);
    uint8_t diff = 0;
    for (size_t i = 0; i < AG_LICENCE_CHECK_LEN; i++) {
        diff |= (uint8_t)(check[i] ^ block->check[i]);
    }
    if (diff != 0) {
        memset(lic, 0, sizeof(*lic));
        return -AG_EPERM;
    }

    lic->from[0] = header->code.offset;
    lic->to[0] = header->code.offset + header->code.file_size;
    lic->from[1] = header->data.offset;
    lic->to[1] = header->data.offset + header->data.file_size;
    return AG_OK;
}

static const uint8_t *stream_block(ag_licence_t *lic, uint32_t index)
{
    if (lic->cached != index + 1u) {
        uint8_t     le[4] = {(uint8_t)index, (uint8_t)(index >> 8),
                             (uint8_t)(index >> 16), (uint8_t)(index >> 24)};
        ag_sha256_t c;
        ag_crypto_sha256_init(&c);
        ag_crypto_sha256_update(&c, lic->file_key, sizeof(lic->file_key));
        ag_crypto_sha256_update(&c, le, sizeof(le));
        ag_crypto_sha256_final(&c, lic->stream);
        lic->cached = index + 1u;
    }
    return lic->stream;
}

void ag_licence_apply(ag_licence_t *lic, uint64_t at, uint8_t *bytes,
                      size_t len)
{
    if (lic == NULL || bytes == NULL) {
        return;
    }
    for (int r = 0; r < 2; r++) {
        const uint64_t from = lic->from[r];
        const uint64_t to = lic->to[r];
        uint64_t       lo = (at > from) ? at : from;
        const uint64_t hi = (at + len < to) ? at + len : to;
        for (; lo < hi; lo++) {
            const uint8_t *s = stream_block(lic, (uint32_t)(lo / 32u));
            bytes[lo - at] ^= s[lo % 32u];
        }
    }
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool ag_licence_parse_key(const char *text, size_t len, uint8_t key[32])
{
    size_t got = 0;
    int    high = -1;
    for (size_t i = 0; i < len; i++) {
        const char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }
        const int v = hex_value(c);
        if (v < 0 || got >= 32u) {
            return false;
        }
        if (high < 0) {
            high = v;
        } else {
            key[got++] = (uint8_t)((high << 4) | v);
            high = -1;
        }
    }
    return got == 32u && high < 0;
}
