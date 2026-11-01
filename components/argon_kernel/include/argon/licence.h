/*
 * ArgonOS - a program bought in the shop, made for one owner's boards.
 *
 * What it is for, and no more: a .AXE downloaded from the shop and handed to
 * somebody else does not run for them.  Free programs carry none of this and
 * run as they always did.
 *
 * The shop gives every account a key (32 bytes), and every board connected
 * to that account keeps it in C:\STORE\OWNER.KEY.  When an account buys a
 * program, the copy its boards download is made for that key: the header
 * says AG_AXE_LICENSED, the code and data parts are XORed with a keystream
 * derived from the key, and a block before the shop's trailer says which key
 * (its id) and with what nonce.  The loader undoes the XOR as it reads; with
 * no key, or another account's, it refuses to load.  Clearing the flag does
 * not help: the parts are still the keystream's, not the program's.
 *
 * Not more than that, said so here: the owner's boards hold the key, and a
 * board's flash holds the program as it runs.  Somebody who takes either has
 * the program.  What it stops is a file passed on.
 *
 * The keystream: K_file = HMAC-SHA256(key, "argon-licence 1\n" name "\n"
 * nonce), and the 32 bytes at file offset 32*i are SHA-256(K_file || le32 i).
 * Random access, because the loader reads the file in more than one order.
 * store/server/licence.py makes the same bytes.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_LICENCE_H
#define ARGON_LICENCE_H

#include <argon/axe.h>
#include <argon/abi.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Where a board keeps its owner's key: 64 hex digits, written by STORE. */
#define AG_LICENCE_KEY_PATH "C:\\STORE\\OWNER.KEY"

/* The block: "AXL1", version, three zero bytes, key id, nonce, check. */
#define AG_LICENCE_MAGIC   "AXL1"
#define AG_LICENCE_VERSION 1u
#define AG_LICENCE_ID_LEN    8u
#define AG_LICENCE_NONCE_LEN 16u
#define AG_LICENCE_CHECK_LEN 8u
#define AG_LICENCE_BLOCK_LEN \
    (8u + AG_LICENCE_ID_LEN + AG_LICENCE_NONCE_LEN + AG_LICENCE_CHECK_LEN)
/* How far from the end of the file the loader looks for it: the shop's
 * trailer (certificates and a signature) follows it. */
#define AG_LICENCE_SEARCH 1024u

typedef struct {
    uint8_t key_id[AG_LICENCE_ID_LEN];
    uint8_t nonce[AG_LICENCE_NONCE_LEN];
    uint8_t check[AG_LICENCE_CHECK_LEN];
} ag_licence_block_t;

typedef struct {
    uint8_t  file_key[32];
    /* The two ranges that are XORed: the parts as they are stored. */
    uint32_t from[2];
    uint32_t to[2];
    /* The last keystream block made, since reads mostly run forward. */
    uint32_t cached;   /* block index + 1, or 0 for none */
    uint8_t  stream[32];
} ag_licence_t;

/* Which key: the first 8 bytes of SHA-256("argon-ownerkey-id 1\n" key). */
void ag_licence_key_id(const uint8_t key[32], uint8_t id[AG_LICENCE_ID_LEN]);

/* The block, found in the last bytes of a file (`tail`, `len` of them):
 * the last "AXL1" in it.  -AG_ENOENT when there is none. */
ag_err_t ag_licence_find(const uint8_t *tail, size_t len,
                         ag_licence_block_t *out);

/* Ready to read the file with this owner key.  -AG_EPERM when the program is
 * another key's, or the key does not open it. */
ag_err_t ag_licence_begin(ag_licence_t *lic, const ag_axe_header_t *header,
                          const ag_licence_block_t *block,
                          const uint8_t key[32]);

/* Undoes the XOR on `len` bytes read from file offset `at` - only where they
 * fall in the parts; the header, relocations and icon are left alone. */
void ag_licence_apply(ag_licence_t *lic, uint64_t at, uint8_t *bytes,
                      size_t len);

/* 64 hex digits (spaces and line ends around them allowed) into a key. */
bool ag_licence_parse_key(const char *text, size_t len, uint8_t key[32]);

#endif /* ARGON_LICENCE_H */
