/*
 * ArgonOS - g_ag_api for a test binary, with the crypto table in it.
 *
 * Since ABI 0.71 the maths behind a signature lives in the kernel, and the
 * shop calls it through api->crypto - so a host test that compiles store_sig.c
 * has to hand it a table, exactly as the firmware does.  That is the point of
 * doing it here rather than letting the tests call Monocypher directly: what is
 * tested is the call an application makes, not a shortcut around it.
 *
 * Only `crypto` is filled in.  Everything else in the table is a kernel
 * facility a test has no opinion about, and the tests that need one of those
 * build their own piece of it (test_cfg, test_vfs and the rest).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "argon/abi.h"
#include "argon/crypto.h"

const ag_api_t *g_ag_api = &(const ag_api_t){
    .size = sizeof(ag_api_t),
    .abi_major = AG_ABI_MAJOR,
    .abi_minor = AG_ABI_MINOR,
    .crypto = &ag_crypto_api_impl,
};
