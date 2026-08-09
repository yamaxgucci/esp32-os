/*
 * ArgonOS - tiny C compiler host tests (expressions + .AXE shape).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

static int axe_contains(const uint8_t *axe, size_t len, const char *needle)
{
    const size_t n = strlen(needle);
    if (n == 0 || len < n) {
        return 0;
    }
    for (size_t i = 0; i + n <= len; i++) {
        if (memcmp(axe + i, needle, n) == 0) {
            return 1;
        }
    }
    return 0;
}

static void check_asteroids_example(void)
{
    /* Path relative to repo when tests run from build-host via argon.ps1. */
    const char *paths[] = {
        "../apps/cc/examples/asteroids.c",
        "apps/cc/examples/asteroids.c",
        NULL,
    };
    FILE *f = NULL;
    for (int i = 0; paths[i] != NULL; i++) {
        f = fopen(paths[i], "rb");
        if (f != NULL) {
            break;
        }
    }
    AG_CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    char *buf = (char *)malloc(64 * 1024);
    AG_CHECK(buf != NULL);
    if (buf == NULL) {
        fclose(f);
        return;
    }
    const size_t n = fread(buf, 1, 64 * 1024 - 1, f);
    fclose(f);
    buf[n] = '\0';
    cc_result_t res;
    const int rc = cc_compile_to_axe(buf, n, &res);
    AG_CHECK(rc == 0);
    if (rc != 0) {
        printf("     asteroids: %s\n", res.err);
    } else {
        const uint32_t flags = (uint32_t)res.axe[12] | ((uint32_t)res.axe[13] << 8) |
                               ((uint32_t)res.axe[14] << 16) |
                               ((uint32_t)res.axe[15] << 24);
        AG_CHECK((flags & 2u) != 0);
        cc_result_free(&res);
    }
    free(buf);
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

    /* Multiple functions + call */
    check_prog("int add(int a, int b) { return a + b; }\n"
               "int ag_main(void) { return add(20, 22); }\n",
               1);

    /* Globals + array access */
    check_prog("int g;\n"
               "int a[4];\n"
               "int ag_main(void) {\n"
               "  g = 1;\n"
               "  a[0] = 2;\n"
               "  a[1] = a[0] + g;\n"
               "  return a[1];\n"
               "}\n",
               1);

    /* for loop */
    check_prog("int ag_main(void) {\n"
               "  int s = 0;\n"
               "  int i;\n"
               "  for (i = 0; i < 5; i = i + 1) { s = s + i; }\n"
               "  return s;\n"
               "}\n",
               1);
    check_prog("int ag_main(void) {\n"
               "  int s = 0;\n"
               "  for (int i = 0; i < 3; i = i + 1) { s = s + 1; }\n"
               "  return s;\n"
               "}\n",
               1);

    /* String literal present in .AXE data */
    {
        const char *src =
            "int ag_main(void) {\n"
            "  ag_gfx_acquire();\n"
            "  ag_gfx_text(0, 0, \"HiCC\", 1, 0);\n"
            "  ag_gfx_release();\n"
            "  return 0;\n"
            "}\n";
        cc_result_t res;
        const int rc = cc_compile_to_axe(src, strlen(src), &res);
        AG_CHECK(rc == 0);
        if (rc == 0) {
            AG_CHECK(axe_contains(res.axe, res.axe_len, "HiCC"));
            /* NEEDS_GFX flag at offset 12 */
            const uint32_t flags = (uint32_t)res.axe[12] | ((uint32_t)res.axe[13] << 8) |
                                   ((uint32_t)res.axe[14] << 16) |
                                   ((uint32_t)res.axe[15] << 24);
            AG_CHECK((flags & 2u) != 0);
            /* abi_minor = 10 at offset 6 */
            AG_CHECK(res.axe[6] == 10 && res.axe[7] == 0);
            cc_result_free(&res);
        } else {
            printf("     string/gfx compile: %s\n", res.err);
            cc_result_free(&res);
        }
    }

    check_asteroids_example();
}
