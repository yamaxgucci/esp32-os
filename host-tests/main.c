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
void run_vtin_tests(void);
void run_cmdline_tests(void);
void run_lineedit_tests(void);
void run_vfs_tests(void);
void run_device_tests(void);
void run_ioclaim_tests(void);
void run_journal_tests(void);
void run_axeload_tests(void);
void run_codepage_tests(void);
void run_reslist_tests(void);
void run_arena_tests(void);

int main(void)
{
    run_path_tests();
    run_cfg_tests();
    run_screen_tests();
    run_vtout_tests();
    run_vtin_tests();
    run_cmdline_tests();
    run_lineedit_tests();
    run_vfs_tests();
    run_device_tests();
    run_ioclaim_tests();
    run_journal_tests();
    run_axeload_tests();
    run_codepage_tests();
    run_reslist_tests();
    run_arena_tests();

    printf("%d checks, %d failures\n", ag_test_checks, ag_test_failures);
    return ag_test_failures == 0 ? 0 : 1;
}
