/*
 * ArgonOS - code page tests.
 *
 * The tables are generated from Python's codecs (tools/gen-codepage.py), so what
 * is worth testing is not every entry but the properties the rest of the system
 * relies on: that the two directions agree, that a character absent from a page
 * is reported as absent rather than mapped to something plausible, and that the
 * bytes a Cyrillic screen is made of are the ones DOS used.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/codepage.h>

#include "test.h"

static void test_ascii_is_the_same_everywhere(void)
{
    for (int i = 0; i < AG_CP_COUNT; i++) {
        const ag_cp_t cp = (ag_cp_t)i;
        for (uint32_t c = 0; c < 0x80u; c++) {
            AG_CHECK_INT(ag_cp_to_unicode(cp, (uint8_t)c), c);
            AG_CHECK_INT(ag_cp_from_unicode(cp, c), (int32_t)c);
        }
    }
}

static void test_both_directions_agree(void)
{
    for (int i = 0; i < AG_CP_COUNT; i++) {
        const ag_cp_t cp = (ag_cp_t)i;
        for (uint32_t b = 0x80u; b < 0x100u; b++) {
            const uint32_t point = ag_cp_to_unicode(cp, (uint8_t)b);
            if (point == 0) {
                continue; /* the page leaves this byte undefined */
            }
            /*
             * Back to the same byte.  Not merely "to some byte": two bytes of
             * one page mapping to one code point would make the round trip lose
             * information, and this is where that would show.
             */
            AG_CHECK_INT(ag_cp_from_unicode(cp, point), (int32_t)b);
        }
    }
}

static void test_cyrillic_bytes_are_the_dos_ones(void)
{
    /* CP866: uppercase at 0x80, lowercase а..п at 0xa0, р..я at 0xe0. */
    AG_CHECK_INT(ag_cp_from_unicode(AG_CP_866, 0x0410), 0x80); /* А */
    AG_CHECK_INT(ag_cp_from_unicode(AG_CP_866, 0x042f), 0x9f); /* Я */
    AG_CHECK_INT(ag_cp_from_unicode(AG_CP_866, 0x0430), 0xa0); /* а */
    AG_CHECK_INT(ag_cp_from_unicode(AG_CP_866, 0x0434), 0xa4); /* д */
    AG_CHECK_INT(ag_cp_from_unicode(AG_CP_866, 0x0440), 0xe0); /* р */
    AG_CHECK_INT(ag_cp_from_unicode(AG_CP_866, 0x044f), 0xef); /* я */
    AG_CHECK_INT(ag_cp_from_unicode(AG_CP_866, 0x0451), 0xf1); /* ё */

    /* CP1251 is contiguous from 0xc0, which is why text files use it. */
    AG_CHECK_INT(ag_cp_from_unicode(AG_CP_1251, 0x0410), 0xc0);
    AG_CHECK_INT(ag_cp_from_unicode(AG_CP_1251, 0x0430), 0xe0);
    AG_CHECK_INT(ag_cp_from_unicode(AG_CP_1251, 0x044f), 0xff);

    /* And CP437 has no Cyrillic at all, which callers have to handle. */
    AG_CHECK_INT(ag_cp_from_unicode(AG_CP_437, 0x0430), -1);
    AG_CHECK_INT(ag_cp_from_unicode(AG_CP_437, 0x0410), -1);
}

static void test_box_drawing_survives_866(void)
{
    /*
     * The reason 866 and not 1251 is the screen page: a file manager drawn with
     * box characters keeps working when the machine switches to Cyrillic.
     */
    static const uint32_t k_box[] = {0x2500, 0x2502, 0x250c, 0x2510, 0x2514,
                                     0x2518, 0x251c, 0x2524, 0x252c, 0x2534,
                                     0x253c, 0x2550, 0x2551, 0x2554, 0x255d};

    for (size_t i = 0; i < sizeof(k_box) / sizeof(k_box[0]); i++) {
        const int32_t in_437 = ag_cp_from_unicode(AG_CP_437, k_box[i]);
        AG_CHECK(in_437 >= 0);
        /* The same byte in both pages, not merely present in both. */
        AG_CHECK_INT(ag_cp_from_unicode(AG_CP_866, k_box[i]), in_437);
        /* CP1251 has none of them - a panel drawn in it comes out as letters. */
        AG_CHECK_INT(ag_cp_from_unicode(AG_CP_1251, k_box[i]), -1);
    }
}

static void test_undefined_and_out_of_range(void)
{
    /* CP1251 leaves one byte undefined; 0 says so, and it maps back nowhere. */
    AG_CHECK_INT(ag_cp_to_unicode(AG_CP_1251, 0x98), 0);
    AG_CHECK_INT(ag_cp_from_unicode(AG_CP_1251, 0), 0); /* NUL is still NUL */

    /* Beyond the BMP nothing single-byte can exist. */
    AG_CHECK_INT(ag_cp_from_unicode(AG_CP_866, 0x1f600), -1);

    /* A page number nobody has: refused rather than silently defaulted. */
    ag_cp_t cp = AG_CP_1251;
    AG_CHECK(!ag_cp_from_number(65001, &cp));
    AG_CHECK_INT(cp, AG_CP_1251); /* and left alone */
    AG_CHECK(ag_cp_from_number(866, &cp));
    AG_CHECK_INT(cp, AG_CP_866);

    /* An invalid enum is answered, not dereferenced. */
    AG_CHECK_INT(ag_cp_to_unicode((ag_cp_t)99, 0x80), 0);
    AG_CHECK_INT(ag_cp_from_unicode((ag_cp_t)99, 0x0430), -1);
    AG_CHECK_INT(ag_cp_number((ag_cp_t)99), 0);
}

static void test_numbers_and_titles(void)
{
    AG_CHECK_INT(ag_cp_number(AG_CP_437), 437);
    AG_CHECK_INT(ag_cp_number(AG_CP_866), 866);
    AG_CHECK_INT(ag_cp_number(AG_CP_1251), 1251);

    for (int i = 0; i < AG_CP_COUNT; i++) {
        AG_CHECK(ag_cp_title((ag_cp_t)i)[0] != '\0');
    }
}

static void test_utf8_encoding(void)
{
    char   out[4];
    size_t n;

    n = ag_utf8_encode('A', out);
    AG_CHECK_INT(n, 1);
    AG_CHECK_INT(out[0], 'A');

    /* Cyrillic 'а' is two bytes: d0 b0. */
    n = ag_utf8_encode(0x0430, out);
    AG_CHECK_INT(n, 2);
    AG_CHECK_INT((unsigned char)out[0], 0xd0);
    AG_CHECK_INT((unsigned char)out[1], 0xb0);

    /* A box drawing character is three: e2 94 80. */
    n = ag_utf8_encode(0x2500, out);
    AG_CHECK_INT(n, 3);
    AG_CHECK_INT((unsigned char)out[0], 0xe2);
    AG_CHECK_INT((unsigned char)out[1], 0x94);
    AG_CHECK_INT((unsigned char)out[2], 0x80);

    /* Four for anything above the BMP. */
    AG_CHECK_INT(ag_utf8_encode(0x1f600, out), 4);

    /* And nothing at all for what is not a code point. */
    AG_CHECK_INT(ag_utf8_encode(0x110000, out), 0);
    AG_CHECK_INT(ag_utf8_encode('A', NULL), 0);
}

static void test_active_page(void)
{
    const ag_cp_t saved = ag_cp_active();

    /* CP437 unless something says otherwise: a system that booted Cyrillic
     * without being asked would surprise everyone whose files are not. */
    AG_CHECK_INT(saved, AG_CP_437);

    ag_cp_set_active(AG_CP_866);
    AG_CHECK_INT(ag_cp_active(), AG_CP_866);

    /* An invalid value leaves the page alone rather than resetting it. */
    ag_cp_set_active((ag_cp_t)77);
    AG_CHECK_INT(ag_cp_active(), AG_CP_866);

    ag_cp_set_active(saved);
}

void run_codepage_tests(void)
{
    test_ascii_is_the_same_everywhere();
    test_both_directions_agree();
    test_cyrillic_bytes_are_the_dos_ones();
    test_box_drawing_survives_866();
    test_undefined_and_out_of_range();
    test_numbers_and_titles();
    test_utf8_encoding();
    test_active_page();
}
