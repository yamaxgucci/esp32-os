/*
 * ArgonOS - probe line parser tests.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/probe.h>

#include <string.h>

#include "test.h"

static void test_bus_addr_path(void)
{
    ag_probe_entry_t e;
    AG_CHECK_INT(ag_probe_parse("0:0x76:t:\\drv\\bme280.sys", &e), AG_OK);
    AG_CHECK_INT(e.hint.bus, 0);
    AG_CHECK_INT(e.hint.addr, 0x76);
    AG_CHECK(!e.hint.has_id);
    AG_CHECK_STR(e.path, "t:\\drv\\bme280.sys");
}

static void test_with_id(void)
{
    ag_probe_entry_t e;
    AG_CHECK_INT(ag_probe_parse("1:0x20:0xD0=0x60:a:\\mcp.sys", &e), AG_OK);
    AG_CHECK_INT(e.hint.bus, 1);
    AG_CHECK_INT(e.hint.addr, 0x20);
    AG_CHECK(e.hint.has_id);
    AG_CHECK_INT(e.hint.id_reg, 0xD0);
    AG_CHECK_INT(e.hint.id_val, 0x60);
    AG_CHECK_STR(e.path, "a:\\mcp.sys");
}

static void test_decimal_and_spaces(void)
{
    ag_probe_entry_t e;
    AG_CHECK_INT(ag_probe_parse("  0:118:  /sd/drv/x.sys  ", &e), AG_OK);
    AG_CHECK_INT(e.hint.bus, 0);
    AG_CHECK_INT(e.hint.addr, 118);
    AG_CHECK_STR(e.path, "/sd/drv/x.sys");
}

static void test_rejects_junk(void)
{
    ag_probe_entry_t e;
    AG_CHECK_INT(ag_probe_parse("", &e), -AG_EINVAL);
    AG_CHECK_INT(ag_probe_parse("0:0x76", &e), -AG_EINVAL);
    AG_CHECK_INT(ag_probe_parse("0:0x76:", &e), -AG_EINVAL);
    AG_CHECK_INT(ag_probe_parse("x:0x76:t:\\a.sys", &e), -AG_EINVAL);
    AG_CHECK_INT(ag_probe_parse("0:0x200:t:\\a.sys", &e), -AG_EINVAL);
    AG_CHECK_INT(ag_probe_parse("0:0x76:ZZ=1:t:\\a.sys", &e), -AG_EINVAL);
}

void run_probe_tests(void)
{
    test_bus_addr_path();
    test_with_id();
    test_decimal_and_spaces();
    test_rejects_junk();
}
