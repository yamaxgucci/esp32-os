/*
 * ArgonOS - the [memory] section's values (core/place_parse.c).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#define AG_PLACE_HOST_ONLY
#include <argon/place.h>

#include "test.h"

#define DATA  (AG_PLACE_BIT(AG_PLACE_IRAM) | AG_PLACE_BIT(AG_PLACE_PSRAM))
#define CODE  (AG_PLACE_BIT(AG_PLACE_IRAM) | AG_PLACE_BIT(AG_PLACE_FLASH))
#define ALL   (DATA | CODE | AG_PLACE_BIT(AG_PLACE_ROM))

static void test_order(void)
{
    ag_place_policy_t p;
    char              why[80];

    AG_CHECK_INT(ag_place_parse("psram, iram", DATA, &p, why, sizeof(why)), AG_OK);
    AG_CHECK_INT(p.count, 2);
    AG_CHECK_INT(p.kind[0], AG_PLACE_PSRAM);
    AG_CHECK_INT(p.kind[1], AG_PLACE_IRAM);
    AG_CHECK(!p.only);

    /* spaces alone, odd case, the synonyms */
    AG_CHECK_INT(ag_place_parse("  PSRAM   Internal ", DATA, &p, why, sizeof(why)), AG_OK);
    AG_CHECK_INT(p.count, 2);
    AG_CHECK_INT(p.kind[1], AG_PLACE_IRAM);
    AG_CHECK_INT(ag_place_parse("sram", DATA, &p, why, sizeof(why)), AG_OK);
    AG_CHECK_INT(p.kind[0], AG_PLACE_IRAM);

    AG_CHECK_INT(ag_place_parse("iram, psram, flash", ALL, &p, why, sizeof(why)), AG_OK);
    AG_CHECK_INT(p.count, 3);
    AG_CHECK_INT(p.kind[2], AG_PLACE_FLASH);
}

static void test_only(void)
{
    ag_place_policy_t p;
    char              why[80];

    AG_CHECK_INT(ag_place_parse("only iram", DATA, &p, why, sizeof(why)), AG_OK);
    AG_CHECK(p.only);
    AG_CHECK_INT(p.count, 1);
    AG_CHECK_INT(ag_place_parse("only psram, iram", DATA, &p, why, sizeof(why)), AG_OK);
    AG_CHECK(p.only);
    AG_CHECK_INT(p.count, 2);

    /* `only` in the middle is a mistake, not a kind */
    AG_CHECK_INT(ag_place_parse("psram only", DATA, &p, why, sizeof(why)), -AG_EINVAL);
    AG_CHECK(strstr(why, "before the list") != NULL);
    /* and `only` alone names nothing */
    AG_CHECK_INT(ag_place_parse("only", DATA, &p, why, sizeof(why)), -AG_EINVAL);
}

static void test_refused(void)
{
    ag_place_policy_t p;
    char              why[80];

    p.count = 7; /* untouched on failure */
    AG_CHECK_INT(ag_place_parse("dram", DATA, &p, why, sizeof(why)), -AG_EINVAL);
    AG_CHECK(strstr(why, "dram") != NULL);
    AG_CHECK_INT(p.count, 7);

    /* data cannot live in flash; code cannot live in psram here */
    AG_CHECK_INT(ag_place_parse("flash", DATA, &p, why, sizeof(why)), -AG_EINVAL);
    AG_CHECK(strstr(why, "cannot live in flash") != NULL);
    AG_CHECK_INT(ag_place_parse("psram", CODE, &p, why, sizeof(why)), -AG_EINVAL);

    AG_CHECK_INT(ag_place_parse("psram, psram", DATA, &p, why, sizeof(why)), -AG_EINVAL);
    AG_CHECK(strstr(why, "twice") != NULL);
    AG_CHECK_INT(ag_place_parse("", DATA, &p, why, sizeof(why)), -AG_EINVAL);
    AG_CHECK_INT(ag_place_parse(" , ", DATA, &p, why, sizeof(why)), -AG_EINVAL);
    AG_CHECK_INT(ag_place_parse(NULL, DATA, &p, why, sizeof(why)), -AG_EINVAL);
}

static void test_format(void)
{
    ag_place_policy_t p;
    char              why[80], out[40];

    AG_CHECK_INT(ag_place_parse("only PSRAM,iram", DATA, &p, why, sizeof(why)), AG_OK);
    ag_place_format(&p, out, sizeof(out));
    AG_CHECK_STR(out, "only psram, iram");
    AG_CHECK_INT(ag_place_parse("rom", ALL, &p, why, sizeof(why)), AG_OK);
    ag_place_format(&p, out, sizeof(out));
    AG_CHECK_STR(out, "rom");
    AG_CHECK_STR(ag_place_kind_name(AG_PLACE_NONE), "?");
}

void run_place_tests(void)
{
    test_order();
    test_only();
    test_refused();
    test_format();
}
