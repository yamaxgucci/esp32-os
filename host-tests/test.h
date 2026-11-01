/*
 * ArgonOS - minimal host test harness.
 *
 * Deliberately tiny: the point of host tests is to run kernel logic without a
 * board, not to adopt a framework.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_TEST_H
#define ARGON_TEST_H

#include <stdio.h>
#include <string.h>

extern int ag_test_failures;
extern int ag_test_checks;

/*
 * Lower-case hex, with the terminator.
 *
 * It used to travel with the SHA-256 code; since ABI 0.71 that maths is a
 * kernel library and formatting a digest is the caller's business, so the
 * tests that print one share this instead of each writing it again.
 */
static inline void ag_test_hex(const unsigned char *bytes, size_t len,
                               char *out)
{
    static const char k_digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = k_digits[bytes[i] >> 4];
        out[2 * i + 1] = k_digits[bytes[i] & 0x0Fu];
    }
    out[2 * len] = '\0';
}

#define AG_CHECK(cond)                                                       \
    do {                                                                     \
        ag_test_checks++;                                                    \
        if (!(cond)) {                                                       \
            ag_test_failures++;                                              \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

#define AG_CHECK_STR(got, want)                                              \
    do {                                                                     \
        ag_test_checks++;                                                    \
        const char *g_ = (got);                                              \
        const char *w_ = (want);                                             \
        if (g_ == NULL || w_ == NULL || strcmp(g_, w_) != 0) {               \
            ag_test_failures++;                                              \
            printf("FAIL %s:%d  got \"%s\", want \"%s\"\n", __FILE__,        \
                   __LINE__, g_ ? g_ : "(null)", w_ ? w_ : "(null)");        \
        }                                                                    \
    } while (0)

#define AG_CHECK_INT(got, want)                                              \
    do {                                                                     \
        ag_test_checks++;                                                    \
        const long g_ = (long)(got);                                         \
        const long w_ = (long)(want);                                        \
        if (g_ != w_) {                                                      \
            ag_test_failures++;                                              \
            printf("FAIL %s:%d  got %ld, want %ld\n", __FILE__, __LINE__,    \
                   g_, w_);                                                  \
        }                                                                    \
    } while (0)

#endif /* ARGON_TEST_H */
