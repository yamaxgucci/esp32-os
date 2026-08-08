/*
 * ArgonOS - Tiny C → .AXE compiler (shared host / guest).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_CC_COMPILE_H
#define AG_CC_COMPILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *axe;
    size_t   axe_len;
    char     err[160];
} cc_result_t;

/* Compile source to a complete Xtensa .AXE image. 0 on success. */
int cc_compile_to_axe(const char *src, size_t src_len, cc_result_t *out);

void cc_result_free(cc_result_t *out);

/*
 * Evaluate a pure integer expression with the same operators as the compiler
 * (+ - * / % < > <= >= == != && || ! and parentheses). Used by host tests.
 */
int cc_eval_expr(const char *expr, int32_t *out_value, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* AG_CC_COMPILE_H */
