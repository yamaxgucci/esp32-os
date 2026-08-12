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

void run_axesig_tests(void)
{
    printf("axesig\n");
    test_unsigned_ok();
    test_sign_and_verify();
    test_tamper_body();
    test_bad_tag();
    test_unknown_algo();
    test_algo_zero_dirty();
}
