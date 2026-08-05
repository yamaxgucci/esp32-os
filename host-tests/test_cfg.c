/*
 * ArgonOS - configuration parser tests.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/cfg.h>

#include "test.h"

/* ag_cfg_parse chops the buffer up in place, so each test needs its own copy. */
static char *dup_text(char *dst, size_t dstlen, const char *src)
{
    strncpy(dst, src, dstlen - 1);
    dst[dstlen - 1] = '\0';
    return dst;
}

static const char SAMPLE[] =
    "; ArgonOS SYSTEM.CFG\n"
    "\n"
    "[kernel]\n"
    "app_core_exclusive = yes\n"
    "app_text_arena     = 2M\n"
    "stack              = 16K\n"
    "log_level          = 3   ; noisy\n"
    "prompt             = \"$P; $G\"\n"
    "\n"
    "[display]\n"
    "driver = st7789\n"
    "width  = 320\n"
    "height = 0x0F0\n"
    "rotate = -90\n"
    "\n"
    "[modules]\n"
    "device = /sd/drv/bme280.sys\n"
    "device = /sd/drv/mcp23017.sys\n"
    "# device = /sd/drv/disabled.sys\n";

static void test_basic(void)
{
    char     text[1024];
    ag_cfg_t cfg;

    ag_cfg_reset(&cfg);
    AG_CHECK_INT(ag_cfg_parse(dup_text(text, sizeof(text), SAMPLE), &cfg), AG_OK);
    AG_CHECK_INT(cfg.bad_lines, 0);
    AG_CHECK_INT(cfg.dropped, 0);

    AG_CHECK_STR(ag_cfg_get(&cfg, "display.driver", NULL), "st7789");
    AG_CHECK_INT(ag_cfg_get_int(&cfg, "display.width", -1), 320);
    AG_CHECK_INT(ag_cfg_get_int(&cfg, "display.height", -1), 240);
    AG_CHECK_INT(ag_cfg_get_int(&cfg, "display.rotate", 0), -90);
    AG_CHECK(ag_cfg_get_bool(&cfg, "kernel.app_core_exclusive", false));

    /* Lookups are case insensitive on both halves of the key. */
    AG_CHECK_STR(ag_cfg_get(&cfg, "DISPLAY.Driver", NULL), "st7789");

    /* Missing keys fall back rather than crash. */
    AG_CHECK_STR(ag_cfg_get(&cfg, "display.missing", "none"), "none");
    AG_CHECK_INT(ag_cfg_get_int(&cfg, "display.missing", 42), 42);
    AG_CHECK(ag_cfg_get_bool(&cfg, "nope.nothing", true));
    AG_CHECK_STR(ag_cfg_get(&cfg, "kernel.driver", "none"), "none");
}

static void test_comments_and_quotes(void)
{
    char     text[1024];
    ag_cfg_t cfg;

    ag_cfg_reset(&cfg);
    ag_cfg_parse(dup_text(text, sizeof(text), SAMPLE), &cfg);

    /* A trailing comment is stripped, whitespace with it. */
    AG_CHECK_INT(ag_cfg_get_int(&cfg, "kernel.log_level", -1), 3);
    /* Quotes protect a ';' that would otherwise start a comment. */
    AG_CHECK_STR(ag_cfg_get(&cfg, "kernel.prompt", NULL), "$P; $G");
    /* A commented-out line contributes nothing. */
    size_t      it = 0;
    const char *first = ag_cfg_next(&cfg, "modules.device", &it);
    const char *second = ag_cfg_next(&cfg, "modules.device", &it);
    const char *third = ag_cfg_next(&cfg, "modules.device", &it);
    AG_CHECK_STR(first, "/sd/drv/bme280.sys");
    AG_CHECK_STR(second, "/sd/drv/mcp23017.sys");
    AG_CHECK(third == NULL);
}

static void test_sizes(void)
{
    char     text[1024];
    ag_cfg_t cfg;

    ag_cfg_reset(&cfg);
    ag_cfg_parse(dup_text(text, sizeof(text), SAMPLE), &cfg);

    AG_CHECK_INT(ag_cfg_parse_size(ag_cfg_get(&cfg, "kernel.app_text_arena", ""),
                                   -1),
                 2 * 1024 * 1024);
    AG_CHECK_INT(ag_cfg_parse_size(ag_cfg_get(&cfg, "kernel.stack", ""), -1),
                 16 * 1024);

    AG_CHECK_INT(ag_cfg_parse_size("1024", -1), 1024);
    AG_CHECK_INT(ag_cfg_parse_size("0x400", -1), 1024);
    AG_CHECK_INT(ag_cfg_parse_size("4KB", -1), 4096);
    AG_CHECK_INT(ag_cfg_parse_size("nonsense", -1), -1);
    AG_CHECK_INT(ag_cfg_parse_size("", -1), -1);
    AG_CHECK_INT(ag_cfg_parse_size(NULL, -1), -1);
    /* 4 GB does not fit in the int32 the loader works with. */
    AG_CHECK_INT(ag_cfg_parse_size("4G", -1), -1);
}

static void test_override_and_layering(void)
{
    char     base[256];
    char     user[256];
    ag_cfg_t cfg;

    ag_cfg_reset(&cfg);
    ag_cfg_parse(dup_text(base, sizeof(base),
                          "[display]\ndriver = ili9341\nwidth = 240\n"),
                 &cfg);
    ag_cfg_parse(dup_text(user, sizeof(user), "[display]\ndriver = st7789\n"),
                 &cfg);

    /* The later file wins, key by key, without erasing what it omits. */
    AG_CHECK_STR(ag_cfg_get(&cfg, "display.driver", NULL), "st7789");
    AG_CHECK_INT(ag_cfg_get_int(&cfg, "display.width", -1), 240);
}

static void test_malformed(void)
{
    char     text[512];
    ag_cfg_t cfg;

    ag_cfg_reset(&cfg);
    ag_cfg_parse(dup_text(text, sizeof(text),
                          "orphan = 1\n"          /* key before any section */
                          "[unclosed\n"           /* bad section header     */
                          "no_equals_here\n"      /* not a KEY=VALUE line   */
                          "= novalue\n"           /* empty key              */
                          "[ok]\n"
                          "k = v\n"),
                 &cfg);

    AG_CHECK_INT(cfg.bad_lines, 3);
    AG_CHECK_STR(ag_cfg_get(&cfg, "orphan", NULL), "1");
    AG_CHECK_STR(ag_cfg_get(&cfg, "ok.k", NULL), "v");
}

static void test_crlf(void)
{
    char     text[256];
    ag_cfg_t cfg;

    /* Config files will be edited on Windows; CR must not end up in values. */
    ag_cfg_reset(&cfg);
    ag_cfg_parse(dup_text(text, sizeof(text),
                          "[display]\r\ndriver = st7789\r\nwidth = 320\r\n"),
                 &cfg);

    AG_CHECK_STR(ag_cfg_get(&cfg, "display.driver", NULL), "st7789");
    AG_CHECK_INT(ag_cfg_get_int(&cfg, "display.width", -1), 320);
}

static void test_overflow(void)
{
    static char text[AG_CFG_MAX_ENTRIES * 12 + 64];
    ag_cfg_t    cfg;
    size_t      n = 0;

    n += (size_t)snprintf(text + n, sizeof(text) - n, "[s]\n");
    for (int i = 0; i < AG_CFG_MAX_ENTRIES + 5; i++) {
        n += (size_t)snprintf(text + n, sizeof(text) - n, "k%d=%d\n", i, i);
    }

    ag_cfg_reset(&cfg);
    ag_cfg_parse(text, &cfg);

    /* Overflow is reported, not silently truncated. */
    AG_CHECK_INT(cfg.count, AG_CFG_MAX_ENTRIES);
    AG_CHECK_INT(cfg.dropped, 5);
}

void run_cfg_tests(void)
{
    test_basic();
    test_comments_and_quotes();
    test_sizes();
    test_override_and_layering();
    test_malformed();
    test_crlf();
    test_overflow();
}
