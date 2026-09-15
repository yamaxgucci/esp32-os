/*
 * ArgonOS - STM32 system-bootloader protocol (ST AN3155), the codec only.
 *
 * A different family, a different protocol - so a separate tool from ESPFLASH,
 * sharing only the board's UART and file plumbing.  This header is the whole
 * wire format; the .c beside it builds and checks the little frames, and
 * host-tests/test_stm32ldr.c pins their bytes.  No UART, no file, no allocation.
 *
 * THE PROTOCOL, in one breath.  The STM32 ROM bootloader speaks 8 bits, EVEN
 * parity, one stop bit (not 8N1 - this catches everyone once).  The host wakes
 * it with a single 0x7F, which also sets the baud; the ROM answers ACK (0x79) or
 * NACK (0x1F).  Every command after that is two bytes - the command and its
 * ones-complement - each answered by ACK/NACK, followed by whatever that command
 * exchanges.  Addresses go big-endian with an XOR checksum byte; a data block is
 * (N-1), then N bytes, then an XOR checksum over all of them.
 *
 * DOWNLOAD MODE is BOOT0 held HIGH at reset (the opposite sense to an ESP's
 * GPIO0), so the reset sequence is this tool's, not ESPFLASH's.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_STM32LDR_H
#define AG_STM32LDR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STM32_INIT 0x7Fu /* the auto-baud wake byte (sent alone, no complement) */
#define STM32_ACK  0x79u
#define STM32_NACK 0x1Fu

/* Commands (AN3155). */
#define STM32_CMD_GET        0x00u /* version + the list of supported commands */
#define STM32_CMD_GET_VER    0x01u
#define STM32_CMD_GET_ID     0x02u /* the 2-byte product id (PID)              */
#define STM32_CMD_READ       0x11u
#define STM32_CMD_GO         0x21u
#define STM32_CMD_WRITE      0x31u
#define STM32_CMD_ERASE      0x43u /* legacy erase (F1/F0 older)               */
#define STM32_CMD_EXT_ERASE  0x44u /* extended erase (most modern parts)       */
#define STM32_CMD_WR_UNPROT  0x73u
#define STM32_CMD_RD_UNPROT  0x92u

/* The default flash base an image is written to when no address is given. */
#define STM32_FLASH_BASE 0x08000000u

/* Write Memory takes at most 256 bytes per block. */
#define STM32_WRITE_MAX 256u

/* Extended-erase "special" codes (sent as two big-endian bytes + checksum). */
#define STM32_ERASE_GLOBAL 0xFFFFu
#define STM32_ERASE_BANK1  0xFFFEu
#define STM32_ERASE_BANK2  0xFFFDu

/* XOR of a run of bytes, starting from `seed` (0 for a plain XOR). */
uint8_t ag_stm32_xor(uint8_t seed, const uint8_t *data, size_t len);

/*
 * A command frame: the byte and its complement.  `out` must hold 2.  Returns 2.
 * (STM32_INIT is the one exception and is sent as a single raw byte, not this.)
 */
size_t ag_stm32_cmd(uint8_t cmd, uint8_t out[2]);

/*
 * An address frame: four bytes big-endian, then their XOR as a checksum.  `out`
 * must hold 5.  Returns 5.
 */
size_t ag_stm32_addr(uint32_t addr, uint8_t out[5]);

/*
 * A Write Memory data block: (N-1), the N bytes, then the XOR checksum of all of
 * those (the (N-1) byte included).  n is 1..256; `out` must hold n + 2.  Returns
 * n + 2, or 0 if n is out of range or the buffer is too small.
 */
size_t ag_stm32_write_block(const uint8_t *data, uint16_t n, uint8_t *out,
                            size_t out_cap);

/*
 * The three-byte payload of an extended (0x44) global/bank erase: the special
 * code big-endian, then its checksum.  `out` must hold 3.  Returns 3.  (Page
 * erase is a different shape and not built here - a coprocessor is mass-erased.)
 */
size_t ag_stm32_ext_erase_special(uint16_t code, uint8_t out[3]);

/* Little helpers shared with the app so both pack an address the same way. */
void ag_stm32_put_be32(uint8_t *p, uint32_t v);

#endif /* AG_STM32LDR_H */
