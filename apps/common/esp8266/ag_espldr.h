/*
 * ArgonOS - ESP8266 ROM download protocol, the codec only.
 *
 * This is the wire format esptool speaks to a bare ESP8266's ROM bootloader:
 * SLIP framing and the fixed command header, nothing else.  It is here, apart
 * from the UART and the file, for the same reason RLINK's codec is (ag_rlink.h):
 * every framing mistake worth catching is caught on the development machine
 * (host-tests/test_espldr.c), not on a board with a soldered module answering.
 *
 * WHAT DRIVES IT: ESPFLASH.AXE (apps/espflash) reads a firmware image off C:/A:/H:
 * and pushes it into an ESP8266 wired to a UART the `io` layer will hand out
 * (i.e. NOT the console UART0).  The PC only produces the .bin; the flashing
 * itself runs on the ArgonOS board.  This is the no-stub path - slower than
 * esptool's RAM stub, but the whole protocol is ROM commands and there is no
 * second binary to carry.
 *
 * SHAPE OF A COMMAND (all little-endian)
 *
 *   request  = 0x00, op, size(2), checksum(4), data[size]
 *   response = 0x01, op, size(2), value(4),    data[size]
 *
 * `checksum` is meaningful only for the *_DATA commands (an XOR of the data
 * block, seeded 0xEF); it is zero otherwise.  On the ESP8266 ROM the response's
 * data ends in a two-byte status: data[size-2] == 0 means success, data[size-1]
 * is the error code.  (The ESP32 and the RAM stub use four; this codec is the
 * ROM, so two.)  Both request and response travel inside a SLIP frame: 0xC0 at
 * each end, 0xC0 in the body escaped as 0xDB 0xDC and 0xDB as 0xDB 0xDD.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_ESPLDR_H
#define AG_ESPLDR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* SLIP */
#define ESP_SLIP_END     0xC0u
#define ESP_SLIP_ESC     0xDBu
#define ESP_SLIP_ESC_END 0xDCu
#define ESP_SLIP_ESC_ESC 0xDDu

/*
 * Commands (the subset the no-stub flasher needs).  These are identical across
 * the whole ESP family - what differs per chip is small (the length of the
 * response's status trailer, whether SPI_ATTACH is required, a few register
 * addresses), and that lives in the flasher's chip table, not here.
 */
#define ESP_CMD_FLASH_BEGIN 0x02u
#define ESP_CMD_FLASH_DATA  0x03u
#define ESP_CMD_FLASH_END   0x04u
#define ESP_CMD_SYNC        0x08u
#define ESP_CMD_WRITE_REG   0x09u
#define ESP_CMD_READ_REG    0x0Au
#define ESP_CMD_SPI_ATTACH  0x0Du /* ESP32 family (not ESP8266) before flashing */

/* FLASH_DATA / MEM_DATA block checksum seed. */
#define ESP_CHECKSUM_SEED 0xEFu

/*
 * The ROM's flash write block.  esptool uses 0x400 for the ROM loader and only
 * grows it (to 0x4000) once its RAM stub is running, which we do not use.
 */
#define ESP_FLASH_BLOCK 0x400u

/* The command header (before the data) and the SYNC payload, in bytes. */
#define ESP_CMD_HDR_SIZE 8u
#define ESP_SYNC_PAYLOAD 36u

/*
 * Chip identification, so the flasher can tell which chip is on the wire (and
 * `espflash id` can prove the wire both ways without writing a byte of flash).
 * esptool reads this one register on every ESP chip and matches its value; the
 * table of magic-value-to-chip lives in the flasher.  The MAC registers are the
 * ESP8266's; other chips keep their MAC elsewhere.
 */
#define ESP_CHIP_MAGIC_REG   0x40001000u
#define ESP_CHIP_MAGIC_8266  0xfff0c101u
#define ESP_OTP_MAC0         0x3ff00050u
#define ESP_OTP_MAC1         0x3ff00054u

/* ---------------------------------------------------------------------- */
/* SLIP                                                                    */
/* ---------------------------------------------------------------------- */

/*
 * Wrap `in` in one SLIP frame (leading and trailing 0xC0, body escaped) into
 * `out`.  Returns the number of bytes written, or 0 if `out_cap` is too small -
 * the worst case is 2*in_len + 2.
 */
size_t ag_esp_slip_encode(const uint8_t *in, size_t in_len, uint8_t *out,
                          size_t out_cap);

/*
 * Streaming SLIP decoder.  Feed it one received byte at a time; it returns true
 * exactly once per frame, at the closing 0xC0, with the un-escaped body left in
 * `buf` (length in `len`).  A frame longer than `cap` sets `overflow` and is
 * dropped rather than smashing the buffer; the next 0xC0 starts over.
 */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    bool     in_frame;
    bool     esc;
    bool     overflow;
    bool     complete; /* a frame was just returned; reset len on next feed */
} ag_esp_slip_t;

void ag_esp_slip_init(ag_esp_slip_t *d, uint8_t *buf, size_t cap);
bool ag_esp_slip_feed(ag_esp_slip_t *d, uint8_t b);

/* ---------------------------------------------------------------------- */
/* Command frames                                                         */
/* ---------------------------------------------------------------------- */

/*
 * Build the un-framed request (header + data) for one command into `out`.
 * Returns ESP_CMD_HDR_SIZE + data_len, or 0 if `out_cap` is too small.  The
 * caller SLIP-encodes the result before it goes on the wire.
 */
size_t ag_esp_cmd_build(uint8_t op, const uint8_t *data, uint16_t data_len,
                        uint32_t checksum, uint8_t *out, size_t out_cap);

/* A parsed response, pointing into the caller's frame buffer. */
typedef struct {
    uint8_t        op;
    uint16_t       size;
    uint32_t       value;
    const uint8_t *data;
    uint16_t       data_len;
} ag_esp_resp_t;

/*
 * Parse a SLIP-decoded frame as a response.  Returns false when it is too short
 * or its direction byte is not 0x01 (a response), so a stray request echo or a
 * line of boot text is rejected rather than misread.
 */
bool ag_esp_resp_parse(const uint8_t *frame, size_t frame_len,
                       ag_esp_resp_t *out);

/*
 * The ROM's status trailer, whose length is the chip's: two bytes on the
 * ESP8266, four on the ESP32 family (the extra two are reserved, the meaning is
 * in the first).  Returns true on success; on failure, when `err_out` is not
 * NULL, it is set to the error byte.  status_len 0, or a response too short to
 * hold the trailer (SYNC's, for one), is treated as success - those commands
 * are judged by their op matching, not by a status.
 */
bool ag_esp_resp_ok(const ag_esp_resp_t *r, uint8_t status_len, uint8_t *err_out);

/* XOR checksum of a data block, seeded 0xEF (for FLASH_DATA / MEM_DATA). */
uint8_t ag_esp_checksum(const uint8_t *data, size_t len);

/* Write the 36-byte SYNC payload into `out` (cap must be >= ESP_SYNC_PAYLOAD). */
size_t ag_esp_sync_payload(uint8_t *out, size_t cap);

/*
 * The ESP8266 ROM's own idea of how much to erase for a FLASH_BEGIN, quirks and
 * all.  esptool carries this exact arithmetic (get_erase_size) because the ROM
 * erases in a way that a plain "round up to a sector" gets wrong at block
 * boundaries; the number goes straight into the FLASH_BEGIN packet.
 */
uint32_t ag_esp8266_erase_size(uint32_t offset, uint32_t size);

/* Little-endian helpers, shared with the app so both pack the same way. */
void     ag_esp_put32(uint8_t *p, uint32_t v);
uint32_t ag_esp_get32(const uint8_t *p);

#endif /* AG_ESPLDR_H */
