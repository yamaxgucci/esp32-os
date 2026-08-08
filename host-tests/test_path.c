/*
 * ArgonOS - path handling tests.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/path.h>

#include "test.h"

static const char *resolve(const char *in, const char *cwd)
{
    static char buf[AG_PATH_MAX];
    const ag_err_t err = ag_path_resolve(in, cwd, buf, sizeof(buf));
    if (err != AG_OK) {
        static char errbuf[32];
        snprintf(errbuf, sizeof(errbuf), "<err %d>", (int)err);
        return errbuf;
    }
    return buf;
}

static void test_resolve_absolute(void)
{
    AG_CHECK_STR(resolve("/sd/apps/hello.axe", NULL), "/sd/apps/hello.axe");
    AG_CHECK_STR(resolve("/", NULL), "/");
    AG_CHECK_STR(resolve("", NULL), "/");
    AG_CHECK_STR(resolve("/sd/", NULL), "/sd");
    AG_CHECK_STR(resolve("//sd///apps//", NULL), "/sd/apps");
}

static void test_resolve_relative(void)
{
    AG_CHECK_STR(resolve("hello.axe", "/sd/apps"), "/sd/apps/hello.axe");
    AG_CHECK_STR(resolve("", "/sd/apps"), "/sd/apps");
    AG_CHECK_STR(resolve(".", "/sd/apps"), "/sd/apps");
    AG_CHECK_STR(resolve("..", "/sd/apps"), "/sd");
    AG_CHECK_STR(resolve("../..", "/sd/apps"), "/");
    AG_CHECK_STR(resolve("../../..", "/sd/apps"), "/");
    AG_CHECK_STR(resolve("./x/../y", "/sd"), "/sd/y");
    /* An absolute input ignores the working directory. */
    AG_CHECK_STR(resolve("/tmp/x", "/sd/apps"), "/tmp/x");
}

static void test_resolve_dos(void)
{
    AG_CHECK_STR(resolve("A:", NULL), "/sd");
    AG_CHECK_STR(resolve("a:", NULL), "/sd");
    AG_CHECK_STR(resolve("A:\\", NULL), "/sd");
    AG_CHECK_STR(resolve("A:\\APPS\\HELLO.AXE", NULL), "/sd/APPS/HELLO.AXE");
    AG_CHECK_STR(resolve("C:\\SYSTEM.CFG", NULL), "/sys/SYSTEM.CFG");
    AG_CHECK_STR(resolve("T:\\scratch", NULL), "/tmp/scratch");
    AG_CHECK_STR(resolve("H:\\roms\\game.sms", NULL), "/host/roms/game.sms");
    /* Mixed separators are a fact of life once both styles are accepted. */
    AG_CHECK_STR(resolve("A:\\apps/sub\\x", NULL), "/sd/apps/sub/x");
    /* A drive letter with no mapping is an error, not a silent guess. */
    char buf[AG_PATH_MAX];
    AG_CHECK_INT(ag_path_resolve("Z:\\x", NULL, buf, sizeof(buf)), -AG_EINVAL);
}

static void test_resolve_limits(void)
{
    char small[8];
    AG_CHECK_INT(ag_path_resolve("/sd/apps/hello.axe", NULL, small,
                                 sizeof(small)),
                 -AG_ERANGE);
    AG_CHECK_INT(ag_path_resolve("/x", NULL, NULL, 64), -AG_EINVAL);
    AG_CHECK_INT(ag_path_resolve("/x", NULL, small, 1), -AG_EINVAL);
}

static void test_join(void)
{
    char buf[AG_PATH_MAX];

    AG_CHECK_INT(ag_path_join("/sd", "apps/hello.axe", buf, sizeof(buf)), AG_OK);
    AG_CHECK_STR(buf, "/sd/apps/hello.axe");

    AG_CHECK_INT(ag_path_join("/sd/", "/tmp/x", buf, sizeof(buf)), AG_OK);
    AG_CHECK_STR(buf, "/tmp/x");

    AG_CHECK_INT(ag_path_join("/sd/apps", "..", buf, sizeof(buf)), AG_OK);
    AG_CHECK_STR(buf, "/sd");
}

static void test_basename_dirname_ext(void)
{
    char buf[AG_PATH_MAX];

    AG_CHECK_STR(ag_path_basename("/sd/apps/hello.axe"), "hello.axe");
    AG_CHECK_STR(ag_path_basename("/"), "");
    AG_CHECK_STR(ag_path_basename("hello.axe"), "hello.axe");

    AG_CHECK_INT(ag_path_dirname("/sd/apps/hello.axe", buf, sizeof(buf)), AG_OK);
    AG_CHECK_STR(buf, "/sd/apps");
    AG_CHECK_INT(ag_path_dirname("/sd", buf, sizeof(buf)), AG_OK);
    AG_CHECK_STR(buf, "/");
    AG_CHECK_INT(ag_path_dirname("hello.axe", buf, sizeof(buf)), AG_OK);
    AG_CHECK_STR(buf, ".");

    AG_CHECK_STR(ag_path_ext("/sd/apps/hello.axe"), ".axe");
    AG_CHECK_STR(ag_path_ext("archive.tar.gz"), ".gz");
    AG_CHECK(ag_path_ext("/sd/apps/hello") == NULL);
    AG_CHECK(ag_path_ext("/sd/.config") == NULL);
}

static void test_icmp(void)
{
    AG_CHECK_INT(ag_path_icmp("HELLO.AXE", "hello.axe"), 0);
    AG_CHECK(ag_path_icmp("a", "b") < 0);
    AG_CHECK(ag_path_icmp("b", "a") > 0);
    AG_CHECK(ag_path_icmp("abc", "ab") > 0);
}

static void test_match(void)
{
    AG_CHECK(ag_path_match("*", "anything"));
    AG_CHECK(ag_path_match("*.axe", "hello.axe"));
    AG_CHECK(ag_path_match("*.AXE", "hello.axe"));
    AG_CHECK(ag_path_match("hello.*", "hello.axe"));
    AG_CHECK(ag_path_match("h?llo.axe", "hello.axe"));
    AG_CHECK(ag_path_match("*o.a*e", "hello.axe"));
    AG_CHECK(ag_path_match("hello.axe", "hello.axe"));

    AG_CHECK(!ag_path_match("*.axe", "hello.sys"));
    AG_CHECK(!ag_path_match("h?llo", "hallo.axe"));
    AG_CHECK(!ag_path_match("hello", "hell"));
    /* '?' must consume exactly one character. */
    AG_CHECK(!ag_path_match("?", ""));
    AG_CHECK(ag_path_match("*", ""));
}

static void test_is_absolute(void)
{
    AG_CHECK(ag_path_is_absolute("/"));
    AG_CHECK(ag_path_is_absolute("/sd/apps"));
    AG_CHECK(!ag_path_is_absolute("sd/apps"));
    AG_CHECK(!ag_path_is_absolute("/sd/apps/"));
    AG_CHECK(!ag_path_is_absolute("/sd//apps"));
    AG_CHECK(!ag_path_is_absolute("/sd/./apps"));
    AG_CHECK(!ag_path_is_absolute("/sd/../apps"));
    AG_CHECK(!ag_path_is_absolute("/sd\\apps"));
    /* A name that merely starts with a dot is fine. */
    AG_CHECK(ag_path_is_absolute("/sd/.config"));
}

void run_path_tests(void)
{
    test_resolve_absolute();
    test_resolve_relative();
    test_resolve_dos();
    test_resolve_limits();
    test_join();
    test_basename_dirname_ext();
    test_icmp();
    test_match();
    test_is_absolute();
}
