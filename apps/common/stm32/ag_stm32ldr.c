/*
 * ArgonOS - STM32 system-bootloader protocol codec.  See ag_stm32ldr.h.
 *
 * Freestanding: no libc, no allocation, no I/O.  Host-tested in
 * host-tests/test_stm32ldr.c.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "stm32/ag_stm32ldr.h"

uint8_t ag_stm32_xor(uint8_t seed, const uint8_t *data, size_t len)
{
    uint8_t x = seed;
    for (size_t i = 0; i < len; i++) {
        x ^= data[i];
    }
    return x;
}

void ag_stm32_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

size_t ag_stm32_cmd(uint8_t cmd, uint8_t out[2])
{
    out[0] = cmd;
    out[1] = (uint8_t)(cmd ^ 0xFFu);
    return 2;
}

size_t ag_stm32_addr(uint32_t addr, uint8_t out[5])
{
    ag_stm32_put_be32(out, addr);
    out[4] = ag_stm32_xor(0, out, 4);
    return 5;
}

size_t ag_stm32_write_block(const uint8_t *data, uint16_t n, uint8_t *out,
                            size_t out_cap)
{
    if (n == 0 || n > STM32_WRITE_MAX || out == NULL ||
        out_cap < (size_t)n + 2u) {
        return 0;
    }
    out[0] = (uint8_t)(n - 1u);
    for (uint16_t i = 0; i < n; i++) {
        out[1 + i] = data[i];
    }
    /* Checksum is over the length byte and every data byte. */
    out[1 + n] = ag_stm32_xor(out[0], data, n);
    return (size_t)n + 2u;
}

size_t ag_stm32_ext_erase_special(uint16_t code, uint8_t out[3])
{
    out[0] = (uint8_t)(code >> 8);
    out[1] = (uint8_t)code;
    out[2] = ag_stm32_xor(0, out, 2);
    return 3;
}
