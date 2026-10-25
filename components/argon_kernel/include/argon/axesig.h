/*
 * ArgonOS - HMAC signature over a .AXE file (header reserved[6]).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_AXESIG_H
#define ARGON_AXESIG_H

#include <argon/abi.h>
#include <argon/axe.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* reserved[0]: algorithm.  0 = unsigned (all-zero reserved is also unsigned). */
#define AG_AXE_SIG_ALGO_NONE 0u
#define AG_AXE_SIG_ALGO_HMAC_SHA256_128 1u

/* reserved[1]: which embedded key produced the tag. */
#define AG_AXE_SIG_KEY_DEV 0u

#define AG_AXE_SIG_TAG_LEN 16u

/*
 * Verifies reserved[] against the file bytes.  All-zero reserved (or algo 0)
 * is accepted — that is every image produced today.  A non-zero algo with a
 * bad tag or unknown key is rejected.
 */
ag_err_t ag_axe_check_sig(const void *file, size_t file_bytes);

/*
 * Whether this image carries a signature at all.
 *
 * The loader asks before it decides how to read the file: verifying a signature
 * means hashing every byte in order, which needs the whole file in memory, and
 * an unsigned image can be streamed into place instead.  Signing is optional,
 * so on a small machine the common case is the cheap one.
 */
bool ag_axe_is_signed(const ag_axe_header_t *h);

/*
 * The same check, fed a chunk at a time.
 *
 * One pass, in file order, from offset 0 to the end: an image can be verified
 * while it is being read, so the memory a signed image needs is a chunk and not
 * a file.  `begin` decides whether there is anything to check at all; an
 * unsigned image answers AG_OK and every later call is a no-op.
 */
typedef struct {
    uint8_t state[176]; /* the hash context; its size is checked in axesig.c */
    uint8_t mac[32];
    uint8_t tag[AG_AXE_SIG_TAG_LEN];
    size_t  at;       /* bytes of the image fed so far */
    size_t  file_bytes;
    size_t  prefix;   /* offsetof(ag_axe_header_t, reserved) */
    size_t  header;   /* sizeof(ag_axe_header_t) */
    uint32_t key_id;
    bool    started;
    bool    unsigned_image;
} ag_axe_sig_stream_t;

ag_err_t ag_axe_sig_stream_begin(ag_axe_sig_stream_t *st,
                                 const ag_axe_header_t *h, size_t file_bytes);

/* Sequential: the next `len` bytes of the image, starting at what was fed
 * before.  Anything that falls inside `reserved` is counted but not used - the
 * MAC has zeros there. */
ag_err_t ag_axe_sig_stream_feed(ag_axe_sig_stream_t *st, const uint8_t *bytes,
                                size_t len);

/* The tag, against what has been fed.  AG_OK when the image is whole. */
ag_err_t ag_axe_sig_stream_finish(ag_axe_sig_stream_t *st);

/*
 * Writes algo/key_id/tag into the file's reserved field (mutates in place).
 * Used by host tests; host tooling signs with tools/signaxe.py.
 */
ag_err_t ag_axe_sign(void *file, size_t file_bytes, uint32_t key_id);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_AXESIG_H */
