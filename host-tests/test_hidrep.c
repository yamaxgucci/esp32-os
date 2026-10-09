/*
 * ArgonOS - a mouse's layout from its report descriptor.
 *
 * Two descriptors: the boot mouse every HID document shows, which must come out
 * as buttons, x, y one byte each; and the one a Logitech receiver sends, with a
 * keyboard on report 1 ahead of the mouse on report 2, sixteen buttons and x
 * and y twelve bits each - the device that moved the pointer up and down when
 * the mouse went sideways (8 Oct 2026).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/hidrep.h>

#include "test.h"

static const uint8_t k_boot_mouse[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02,             /* 3 buttons     */
    0x95, 0x01, 0x75, 0x05, 0x81, 0x03,             /* 5 bits of pad */
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0xC0, 0xC0,
};

static const uint8_t k_logitech[] = {
    /* report 1: a keyboard's modifiers, so the mouse is not the first report */
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0xC0,
    /* report 2: the mouse */
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x10, 0x75, 0x01, 0x81, 0x02,             /* 16 buttons      */
    0x05, 0x01, 0x16, 0x01, 0xF8, 0x26, 0xFF, 0x07,
    0x75, 0x0C, 0x95, 0x02, 0x09, 0x30, 0x09, 0x31, 0x81, 0x06, /* x, y: 12 bits */
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x09, 0x38, 0x81, 0x06,
    0x05, 0x0C, 0x0A, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06,       /* AC Pan */
    0xC0, 0xC0,
};

/* The receiver itself, 046d:c542 interface 1, as its journal printed it on
 * 8 Oct 2026: report 1 is the mouse (5 buttons, x and y sixteen bits each, a
 * wheel and a pan), then system control, consumer keys and a vendor report. */
static const uint8_t k_c542[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05,
    0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x01, 0x05, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0xF8, 0x26, 0xFF, 0x07, 0x75, 0x10,
    0x95, 0x02, 0x81, 0x06, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08,
    0x95, 0x01, 0x81, 0x06, 0x05, 0x0C, 0x0A, 0x38, 0x02, 0x95, 0x01, 0x81,
    0x06, 0xC0, 0xC0, 0x05, 0x01, 0x09, 0x80, 0xA1, 0x01, 0x85, 0x02, 0x05,
    0x01, 0x19, 0x81, 0x29, 0x88, 0x15, 0x00, 0x25, 0x01, 0x95, 0x08, 0x75,
    0x01, 0x81, 0x06, 0xC0, 0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x03,
    0x15, 0x00, 0x26, 0x80, 0x03, 0x19, 0x00, 0x2A, 0x80, 0x03, 0x75, 0x10,
    0x95, 0x01, 0x81, 0x00, 0xC0, 0x06, 0x00, 0xFF, 0x09, 0x0E, 0xA1, 0x01,
    0x85, 0xBA, 0x95, 0x1F, 0x75, 0x08, 0x26, 0xFF, 0x00, 0x15, 0x00, 0x09,
    0x01, 0x91, 0x02, 0x85, 0xBA, 0x95, 0x1F, 0x75, 0x08, 0x26, 0xFF, 0x00,
    0x15, 0x00, 0x09, 0x01, 0x81, 0x02, 0xC0,
};

static void test_boot_mouse_layout(void)
{
    ag_hidrep_mouse_t m;
    AG_CHECK(ag_hidrep_parse_mouse(k_boot_mouse, sizeof(k_boot_mouse), &m));
    AG_CHECK_INT(m.report_id, 0);
    AG_CHECK_INT(m.btn_off, 0);
    AG_CHECK_INT(m.btn_count, 3);
    AG_CHECK_INT(m.x_off, 8);
    AG_CHECK_INT(m.x_size, 8);
    AG_CHECK_INT(m.y_off, 16);
    AG_CHECK_INT(m.wheel_off, 24);

    const uint8_t r[] = {0x01, 0xFE, 0x03, 0x01};
    uint8_t b = 0;
    int32_t dx = 0, dy = 0, w = 0;
    AG_CHECK(ag_hidrep_decode_mouse(&m, r, sizeof(r), &b, &dx, &dy, &w));
    AG_CHECK_INT(b, 1);
    AG_CHECK_INT(dx, -2);
    AG_CHECK_INT(dy, 3);
    AG_CHECK_INT(w, 1);
}

static void test_logitech_layout(void)
{
    ag_hidrep_mouse_t m;
    AG_CHECK(ag_hidrep_parse_mouse(k_logitech, sizeof(k_logitech), &m));
    AG_CHECK_INT(m.report_id, 2);
    AG_CHECK_INT(m.btn_off, 0);
    AG_CHECK_INT(m.btn_count, 8);
    AG_CHECK_INT(m.x_off, 16);
    AG_CHECK_INT(m.x_size, 12);
    AG_CHECK_INT(m.y_off, 28);
    AG_CHECK_INT(m.y_size, 12);
    AG_CHECK_INT(m.wheel_off, 40);
    AG_CHECK_INT(m.wheel_size, 8);
}

static void test_logitech_sideways_is_sideways(void)
{
    ag_hidrep_mouse_t m;
    AG_CHECK(ag_hidrep_parse_mouse(k_logitech, sizeof(k_logitech), &m));

    uint8_t b = 0;
    int32_t dx = 0, dy = 0, w = 0;
    /* right by 5, left button held: what boot read as "down by 5" */
    const uint8_t right[] = {0x02, 0x01, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00};
    AG_CHECK(ag_hidrep_decode_mouse(&m, right, sizeof(right), &b, &dx, &dy, &w));
    AG_CHECK_INT(b, 1);
    AG_CHECK_INT(dx, 5);
    AG_CHECK_INT(dy, 0);

    /* x = -3 and y = +2, the nibbles shared in the middle byte */
    const uint8_t diag[] = {0x02, 0x00, 0x00, 0xFD, 0x2F, 0x00, 0xFF, 0x00};
    AG_CHECK(ag_hidrep_decode_mouse(&m, diag, sizeof(diag), &b, &dx, &dy, &w));
    AG_CHECK_INT(b, 0);
    AG_CHECK_INT(dx, -3);
    AG_CHECK_INT(dy, 2);
    AG_CHECK_INT(w, -1);

    /* y = -300 needs all twelve bits */
    const uint8_t far[] = {0x02, 0x00, 0x00, 0x00, 0x40, 0xED, 0x00, 0x00};
    AG_CHECK(ag_hidrep_decode_mouse(&m, far, sizeof(far), &b, &dx, &dy, &w));
    AG_CHECK_INT(dx, 0);
    AG_CHECK_INT(dy, -300);
}

static void test_other_reports_are_not_moves(void)
{
    ag_hidrep_mouse_t m;
    AG_CHECK(ag_hidrep_parse_mouse(k_logitech, sizeof(k_logitech), &m));
    const uint8_t keys[] = {0x01, 0x02, 0x00, 0x00};
    AG_CHECK(!ag_hidrep_decode_mouse(&m, keys, sizeof(keys), NULL, NULL, NULL,
                                     NULL));
    const uint8_t cut[] = {0x02, 0x00, 0x00};
    AG_CHECK(!ag_hidrep_decode_mouse(&m, cut, sizeof(cut), NULL, NULL, NULL,
                                     NULL));
}

static void test_not_a_mouse(void)
{
    /* the keyboard half alone has no X or Y */
    ag_hidrep_mouse_t m;
    AG_CHECK(!ag_hidrep_parse_mouse(k_logitech, 25, &m));
    AG_CHECK_INT(m.x_size, 0);
    AG_CHECK(!ag_hidrep_parse_mouse(k_logitech, 3, &m)); /* cut mid-item */
}

static void test_c542_as_it_is(void)
{
    ag_hidrep_mouse_t m;
    AG_CHECK(ag_hidrep_parse_mouse(k_c542, sizeof(k_c542), &m));
    AG_CHECK_INT(m.report_id, 1);
    AG_CHECK_INT(m.btn_count, 5);
    AG_CHECK_INT(m.x_off, 8);
    AG_CHECK_INT(m.x_size, 16);
    AG_CHECK_INT(m.y_off, 24);
    AG_CHECK_INT(m.wheel_off, 40);

    /* sideways by 7: read as boot this was "left held, down by 7" */
    const uint8_t r[] = {0x01, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t b = 0xFF;
    int32_t dx = 0, dy = 0, w = 0;
    AG_CHECK(ag_hidrep_decode_mouse(&m, r, sizeof(r), &b, &dx, &dy, &w));
    AG_CHECK_INT(b, 0);
    AG_CHECK_INT(dx, 7);
    AG_CHECK_INT(dy, 0);
    /* the consumer-keys report is not a move */
    const uint8_t keys[] = {0x03, 0xE9, 0x00};
    AG_CHECK(!ag_hidrep_decode_mouse(&m, keys, sizeof(keys), NULL, NULL, NULL,
                                     NULL));
}

void run_hidrep_tests(void)
{
    test_boot_mouse_layout();
    test_logitech_layout();
    test_logitech_sideways_is_sideways();
    test_other_reports_are_not_moves();
    test_not_a_mouse();
    test_c542_as_it_is();
}
