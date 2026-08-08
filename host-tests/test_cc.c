/*
 * ArgonOS - tiny C compiler host tests (expressions + .AXE shape).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cc_compile.h"
#include "test.h"

static void check_expr(const char *expr, int32_t want)
{
    int32_t got = 0;
    char err[160];
    const int rc = cc_eval_expr(expr, &got, err, sizeof(err));
    AG_CHECK(rc == 0);
    if (rc != 0) {
        printf("     expr \"%s\": %s\n", expr, err);
        return;
    }
    AG_CHECK_INT(got, want);
}

static void check_prog(const char *src, int expect_ok)
{
    cc_result_t res;
    const int rc = cc_compile_to_axe(src, strlen(src), &res);
    if (expect_ok) {
        AG_CHECK(rc == 0);
        if (rc != 0) {
            printf("     compile: %s\n", res.err);
            return;
        }
        AG_CHECK(res.axe_len >= 176);
        AG_CHECK(memcmp(res.axe, "AXE1", 4) == 0);
        /* arch = 1 (xtensa) at offset 8 */
        AG_CHECK(res.axe[8] == 1 && res.axe[9] == 0);
        /* code size > 0 at offset 20 */
        const uint32_t code_size = (uint32_t)res.axe[20] | ((uint32_t)res.axe[21] << 8) |
                                   ((uint32_t)res.axe[22] << 16) |
                                   ((uint32_t)res.axe[23] << 24);
        AG_CHECK(code_size > 0);
        cc_result_free(&res);
    } else {
        AG_CHECK(rc != 0);
        cc_result_free(&res);
    }
}

void run_cc_tests(void)
{
    printf("cc\n");

    check_expr("1+2*3", 7);
    check_expr("(1+2)*3", 9);
    check_expr("10-3-2", 5);
    check_expr("20/3", 6);
    check_expr("20%3", 2);
    check_expr("-5+2", -3);
    check_expr("!0", 1);
    check_expr("!2", 0);
    check_expr("1==1", 1);
    check_expr("1!=2", 1);
    check_expr("3<4", 1);
    check_expr("4<=4", 1);
    check_expr("5>3", 1);
    check_expr("5>=6", 0);
    check_expr("1&&0", 0);
    check_expr("0||2", 1);
    check_expr("1+2*3-4/2", 5);

    check_prog("int ag_main(void) { return 42; }\n", 1);
    check_prog("int ag_main(void) {\n"
               "  int x = 1;\n"
               "  int y = 2;\n"
               "  while (x < 10) { x = x + y; }\n"
               "  if (x > 5) return x; else return 0;\n"
               "}\n",
               1);
    check_prog("int foo(void) { return 1; }\n", 0);
    check_prog("int ag_main(void) { return unknown; }\n", 0);
}
