/*
 * ArgonOS - .AXE HMAC signature tests.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/axesig.h>
#include <argon/axe.h>

#include <stdlib.h>
#include <string.h>

#include "test.h"

#ifndef AG_FIXTURE_DIR
#define AG_FIXTURE_DIR "."
#endif

typedef struct {
    uint8_t *data;
    size_t   size;
} blob_t;

static blob_t load_fixture(const char *name)
{
    blob_t out = {NULL, 0};
    char   full[512];
    snprintf(full, sizeof(full), "%s/%s", AG_FIXTURE_DIR, name);
    FILE *f = fopen(full, "rb");
    if (f == NULL) {
        printf("     (cannot open %s)\n", full);
        return out;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0) {
        out.data = (uint8_t *)malloc((size_t)size);
        if (out.data != NULL &&
            fread(out.data, 1, (size_t)size, f) == (size_t)size) {
            out.size = (size_t)size;
        }
    }
    fclose(f);
    return out;
}

static void test_unsigned_ok(void)
{
    blob_t a = load_fixture("sample-nominal.axe");
    if (a.size == 0) {
        return;
    }
    AG_CHECK_INT(ag_axe_check_sig(a.data, a.size), AG_OK);
    free(a.data);
}

static void test_sign_and_verify(void)
{
    blob_t a = load_fixture("sample-nominal.axe");
    if (a.size == 0) {
        return;
    }
    AG_CHECK_INT(ag_axe_sign(a.data, a.size, AG_AXE_SIG_KEY_DEV), AG_OK);
    const ag_axe_header_t *h = (const ag_axe_header_t *)a.data;
    AG_CHECK(h->reserved[0] == AG_AXE_SIG_ALGO_HMAC_SHA256_128);
    AG_CHECK(h->reserved[1] == AG_AXE_SIG_KEY_DEV);
    AG_CHECK_INT(ag_axe_check_sig(a.data, a.size), AG_OK);
    free(a.data);
}

static void test_tamper_body(void)
{
    blob_t a = load_fixture("sample-nominal.axe");
    if (a.size == 0) {
        return;
    }
    AG_CHECK_INT(ag_axe_sign(a.data, a.size, AG_AXE_SIG_KEY_DEV), AG_OK);
    /* Flip a byte in the code part (after the header). */
    a.data[sizeof(ag_axe_header_t)] ^= 0x01u;
    AG_CHECK_INT(ag_axe_check_sig(a.data, a.size), -AG_EPERM);
    free(a.data);
}

static void test_bad_tag(void)
{
    blob_t a = load_fixture("sample-nominal.axe");
    if (a.size == 0) {
        return;
    }
    AG_CHECK_INT(ag_axe_sign(a.data, a.size, AG_AXE_SIG_KEY_DEV), AG_OK);
    ag_axe_header_t *h = (ag_axe_header_t *)a.data;
    h->reserved[2] ^= 1u;
    AG_CHECK_INT(ag_axe_check_sig(a.data, a.size), -AG_EPERM);
    free(a.data);
}

static void test_unknown_algo(void)
{
    blob_t a = load_fixture("sample-nominal.axe");
    if (a.size == 0) {
        return;
    }
    ag_axe_header_t *h = (ag_axe_header_t *)a.data;
    h->reserved[0] = 99u;
    h->reserved[1] = 0u;
    h->reserved[2] = 1u;
    AG_CHECK_INT(ag_axe_check_sig(a.data, a.size), -AG_ENOTSUP);
    free(a.data);
}

static void test_algo_zero_dirty(void)
{
    blob_t a = load_fixture("sample-nominal.axe");
    if (a.size == 0) {
        return;
    }
    ag_axe_header_t *h = (ag_axe_header_t *)a.data;
    h->reserved[0] = 0u;
    h->reserved[2] = 0xdeadbeefu;
    AG_CHECK_INT(ag_axe_check_sig(a.data, a.size), -AG_EFORMAT);
    free(a.data);
}

/* The same image through both checks, with the tag inside `reserved`. */
static ag_err_t sig_stream_of(const uint8_t *file, size_t size)
{
    ag_axe_sig_stream_t st;
    const ag_axe_header_t *h = (const ag_axe_header_t *)file;
    ag_err_t err = ag_axe_sig_stream_begin(&st, h, size);
    if (err != AG_OK) {
        return err;
    }
    /* Chunks that are not a divisor of anything: the boundaries between the
     * header, the tag and the tail are where a stream goes wrong. */
    static const size_t steps[] = {7, 1, 512, 13, 4096};
    size_t at = 0;
    size_t step = 0;
    while (at < size) {
        size_t want = steps[step++ % (sizeof(steps) / sizeof(steps[0]))];
        if (want > size - at) {
            want = size - at;
        }
        err = ag_axe_sig_stream_feed(&st, file + at, want);
        if (err != AG_OK) {
            return err;
        }
        at += want;
    }
    return ag_axe_sig_stream_finish(&st);
}

static void check_stream_agrees(uint8_t *data, size_t size)
{
    /* `size` may be short of the real file when a test truncates it, so the
     * header's file length is taken from the caller. */
    const ag_err_t whole = ag_axe_check_sig(data, size);
    const ag_err_t stream = sig_stream_of(data, size);
    AG_CHECK_INT(stream, whole);
}

static void test_stream_matches_whole(void)
{
    blob_t a = load_fixture("sample-nominal.axe");
    if (a.size == 0) {
        return;
    }
    AG_CHECK_INT(ag_axe_sign(a.data, a.size, AG_AXE_SIG_KEY_DEV), AG_OK);

    /* Good image: both accept. */
    check_stream_agrees(a.data, a.size);

    /* A byte in the body. */
    a.data[a.size / 2] ^= 0xFFu;
    check_stream_agrees(a.data, a.size);
    a.data[a.size / 2] ^= 0xFFu;

    /* A byte inside the tag itself: the stream substitutes zeros there, so a
     * changed tag has to fail the comparison and not the hash. */
    ag_axe_header_t *h = (ag_axe_header_t *)a.data;
    const uint8_t   keep = ((const uint8_t *)&h->reserved[2])[0];
    ((uint8_t *)&h->reserved[2])[0] = (uint8_t)(keep ^ 0xFFu);
    check_stream_agrees(a.data, a.size);
    ((uint8_t *)&h->reserved[2])[0] = keep;

    /* Truncated: the stream must notice it saw fewer bytes than the file has. */
    if (a.size > sizeof(ag_axe_header_t) + 1u) {
        /* One byte short is a file the MAC cannot match: any refusal is
         * right, and the stream must not say AG_OK. */
        AG_CHECK(sig_stream_of(a.data, a.size - 1u) != AG_OK);
    }
    free(a.data);
}

void run_axesig_tests(void)
{
    printf("axesig\n");
    test_unsigned_ok();
    test_sign_and_verify();
    test_tamper_body();
    test_bad_tag();
    test_unknown_algo();
    test_algo_zero_dirty();
    test_stream_matches_whole();
}
