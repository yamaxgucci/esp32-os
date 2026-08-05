/*
 * ArgonOS - host test runner.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "test.h"

int ag_test_failures = 0;
int ag_test_checks = 0;

void run_path_tests(void);
void run_cfg_tests(void);
void run_screen_tests(void);
void run_vtout_tests(void);

int main(void)
{
    run_path_tests();
    run_cfg_tests();
    run_screen_tests();
    run_vtout_tests();

    printf("%d checks, %d failures\n", ag_test_checks, ag_test_failures);
    return ag_test_failures == 0 ? 0 : 1;
}
