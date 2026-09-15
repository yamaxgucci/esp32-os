/*
 * ArgonOS - STM32 bootloader (AN3155) codec tests.  Command complements, the
 * big-endian address with its XOR checksum, the write-block length-and-checksum
 * layout, and the extended-erase special codes.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "test.h"

#include "stm32/ag_stm32ldr.h"

static void test_cmd_frame(void)
{
    uint8_t f[2];
    AG_CHECK_INT(ag_stm32_cmd(STM32_CMD_WRITE, f), 2);
    AG_CHECK_INT(f[0], 0x31);
    AG_CHECK_INT(f[1], 0xCE); /* 0x31 ^ 0xFF */

    ag_stm32_cmd(STM32_CMD_GET, f);
    AG_CHECK_INT(f[0], 0x00);
    AG_CHECK_INT(f[1], 0xFF);
}

static void test_addr_frame(void)
{
    uint8_t a[5];
    AG_CHECK_INT(ag_stm32_addr(0x08000000u, a), 5);
    AG_CHECK_INT(a[0], 0x08); /* big-endian */
    AG_CHECK_INT(a[1], 0x00);
    AG_CHECK_INT(a[2], 0x00);
    AG_CHECK_INT(a[3], 0x00);
    AG_CHECK_INT(a[4], 0x08); /* 08^00^00^00 */

    ag_stm32_addr(0x08001234u, a);
    AG_CHECK_INT(a[0], 0x08);
    AG_CHECK_INT(a[1], 0x00);
    AG_CHECK_INT(a[2], 0x12);
    AG_CHECK_INT(a[3], 0x34);
    AG_CHECK_INT(a[4], 0x2E); /* 08^00^12^34 */
}

static void test_write_block(void)
{
    const uint8_t data[] = {0xAA, 0xBB};
    uint8_t       out[8];
    const size_t  n = ag_stm32_write_block(data, 2, out, sizeof(out));
    AG_CHECK_INT(n, 4);
    AG_CHECK_INT(out[0], 0x01);            /* N - 1 */
    AG_CHECK_INT(out[1], 0xAA);
    AG_CHECK_INT(out[2], 0xBB);
    AG_CHECK_INT(out[3], 0x10);            /* 0x01 ^ 0xAA ^ 0xBB */

    /* Out of range and too-small buffer are refused, not truncated. */
    AG_CHECK_INT(ag_stm32_write_block(data, 0, out, sizeof(out)), 0);
    AG_CHECK_INT(ag_stm32_write_block(data, 257, out, sizeof(out)), 0);
    AG_CHECK_INT(ag_stm32_write_block(data, 2, out, 3), 0);
}

static void test_ext_erase_special(void)
{
    uint8_t sp[3];
    AG_CHECK_INT(ag_stm32_ext_erase_special(STM32_ERASE_GLOBAL, sp), 3);
    AG_CHECK_INT(sp[0], 0xFF);
    AG_CHECK_INT(sp[1], 0xFF);
    AG_CHECK_INT(sp[2], 0x00); /* 0xFF ^ 0xFF */

    ag_stm32_ext_erase_special(STM32_ERASE_BANK1, sp);
    AG_CHECK_INT(sp[0], 0xFF);
    AG_CHECK_INT(sp[1], 0xFE);
    AG_CHECK_INT(sp[2], 0x01); /* 0xFF ^ 0xFE */
}

static void test_xor_and_be32(void)
{
    const uint8_t d[] = {0x01, 0x02, 0x04};
    AG_CHECK_INT(ag_stm32_xor(0, d, sizeof(d)), 0x07);
    AG_CHECK_INT(ag_stm32_xor(0x10, d, sizeof(d)), 0x17);

    uint8_t b[4];
    ag_stm32_put_be32(b, 0x89ABCDEFu);
    AG_CHECK_INT(b[0], 0x89);
    AG_CHECK_INT(b[1], 0xAB);
    AG_CHECK_INT(b[2], 0xCD);
    AG_CHECK_INT(b[3], 0xEF);
}

void run_stm32ldr_tests(void)
{
    test_cmd_frame();
    test_addr_frame();
    test_write_block();
    test_ext_erase_special();
    test_xor_and_be32();
}
