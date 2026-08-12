/*
 * ArgonOS - PATH search and file association tests.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/cfg.h>
#include <argon/ramfs.h>
#include <argon/shell_path.h>
#include <argon/vfs.h>

#include <stdlib.h>
#include <string.h>

#include "test.h"

static ag_ramfs_t *g_sys;
static ag_ramfs_t *g_sd;

static uint64_t fake_clock(void) { return 1000000; }

static void setup(void)
{
    if (g_sys != NULL) {
        ag_ramfs_destroy(g_sys);
        g_sys = NULL;
    }
    if (g_sd != NULL) {
        ag_ramfs_destroy(g_sd);
        g_sd = NULL;
    }
    ag_vfs_init(NULL);
    const ag_ramfs_config_t cfg = {.budget = 64 * 1024, .now_unix = fake_clock};
    g_sys = ag_ramfs_create(&cfg);
    g_sd = ag_ramfs_create(&cfg);
    AG_CHECK(g_sys != NULL && g_sd != NULL);
    AG_CHECK_INT(ag_vfs_mount("/sys", ag_ramfs_ops(), g_sys, 0), AG_OK);
    AG_CHECK_INT(ag_vfs_mount("/sd", ag_ramfs_ops(), g_sd, 0), AG_OK);
}

static ag_err_t write_file(const char *path, const char *text)
{
    const ag_handle_t h =
        ag_vfs_open(path, NULL, AG_O_RDWR | AG_O_CREATE | AG_O_TRUNC);
    if (h < 0) {
        return h;
    }
    const int32_t n = ag_vfs_write(h, text, strlen(text));
    ag_vfs_close(h);
    return (n < 0) ? n : AG_OK;
}

static void test_name_is_path(void)
{
    AG_CHECK(ag_shell_name_is_path("a:\\bin\\hello.axe"));
    AG_CHECK(ag_shell_name_is_path("/sd/bin/hello.axe"));
    AG_CHECK(ag_shell_name_is_path("bin/hello"));
    AG_CHECK(ag_shell_name_is_path("C:hello"));
    AG_CHECK(!ag_shell_name_is_path("hello"));
    AG_CHECK(!ag_shell_name_is_path("hello.axe"));
}

static void test_resolve_path(void)
{
    setup();
    AG_CHECK_INT(ag_vfs_mkdir("/sd/bin", NULL), AG_OK);
    AG_CHECK_INT(write_file("/sd/bin/hello.axe", "X"), AG_OK);
    AG_CHECK_INT(write_file("/sys/readme.txt", "hi"), AG_OK);

    char out[AG_PATH_MAX];
    AG_CHECK_INT(ag_shell_resolve_cmd("hello", "/tmp", "a:\\bin;c:\\bin", out,
                                      sizeof(out)),
                 AG_OK);
    AG_CHECK_STR(out, "/sd/bin/hello.axe");

    /* .txt is not an application — PATH must not claim it. */
    AG_CHECK_INT(ag_shell_resolve_cmd("readme.txt", "/sys", "c:\\", out,
                                      sizeof(out)),
                 -AG_ENOENT);

    AG_CHECK_INT(
        ag_shell_resolve_cmd("a:\\bin\\hello.axe", "/", NULL, out, sizeof(out)),
        AG_OK);
    AG_CHECK_STR(out, "/sd/bin/hello.axe");
}

static void test_assoc_and_autoexec(void)
{
    char     text[512];
    ag_cfg_t cfg;
    strncpy(text,
            "[assoc]\n"
            ".txt = edit\n"
            "c = edit\n"
            ".AXE = run\n",
            sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
    ag_cfg_reset(&cfg);
    AG_CHECK_INT(ag_cfg_parse(text, &cfg), AG_OK);

    AG_CHECK_STR(ag_shell_assoc_lookup(&cfg, "/sys/notes.TXT"), "edit");
    AG_CHECK_STR(ag_shell_assoc_lookup(&cfg, "foo.c"), "edit");
    AG_CHECK(ag_shell_assoc_lookup(&cfg, "foo.h") == NULL);

    AG_CHECK(ag_shell_autoexec_skip_line(""));
    AG_CHECK(ag_shell_autoexec_skip_line("  ; comment"));
    AG_CHECK(ag_shell_autoexec_skip_line("REM ignore"));
    AG_CHECK(!ag_shell_autoexec_skip_line("echo hi"));
}

void run_shell_path_tests(void)
{
    test_name_is_path();
    test_resolve_path();
    test_assoc_and_autoexec();
}
