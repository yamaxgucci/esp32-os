/*
 * ArgonOS - flash an ESP chip that is wired to one of this board's UARTs, using
 * a firmware image already on disk.  The board is the programmer; the PC only
 * made the .bin.  ESP8266 and the ESP32 family (ESP32/S2/S3/C3/C6/C2/H2) all
 * speak the same ROM download protocol; the chip is detected after the SYNC
 * handshake (or named with -chip), and the few per-chip differences come from a
 * small table at the top of this file.
 *
 *   espflash id    [-c CHIP] [-p PORT] [-b BAUD] [-r GPIO0 RST]
 *   espflash write <file> [-a ADDR] [-c CHIP] [-p PORT] [-b BAUD] [-r GPIO0 RST]
 *
 *     -c CHIP   auto (default) detects it from the chip's magic register, or
 *               name it: esp8266 esp32 esp32s2 esp32s3 esp32c3 esp32c6 ...
 *     -p PORT   which UART the module is on (default 1; UART0 is the console and
 *               the io layer will not hand it out)
 *     -b BAUD   line rate for both ends (default 115200; keep them equal)
 *     -a ADDR   flash offset, hex or decimal (default 0)
 *     -r G0 RST drive the module's GPIO0 and RST/EN to enter download mode
 *               automatically.  Omit it where those pins are not wired (the CYD)
 *               and do it by hand: jumper GPIO0 to GND, then power-cycle the
 *               module, before running this.
 *
 * WHY THIS EXISTS, and why it is an .AXE and not firmware: a radio coprocessor
 * (an ESP-01 as a Wi-Fi/BLE front end, say) needs its own firmware, and carrying
 * a whole flasher inside the ArgonOS image to do that once is the wrong trade -
 * see docs "minimise the firmware".  UART1 is not the console, so this reaches it
 * through the ordinary io ABI with no kernel change at all.
 *
 * The protocol is the ROM's, no RAM stub: slower than esptool's stub path, but
 * it is all ROM commands and there is no second binary to ship.  The codec
 * (SLIP, command frames, the erase-size quirk) is apps/common/esp8266 and is
 * host-tested; this file is the chip table, the UART, the file and the
 * sequencing.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

#include "esp8266/ag_espldr.h"

AG_APP("ESPFLASH", "1.0", "argon", 0);

/* ---- the chip table -------------------------------------------------- */

/*
 * What differs between one ESP chip and the next, once you are past the shared
 * SLIP protocol.  There are only a few knobs:
 *
 *   status_len   the ROM's response status trailer - 2 bytes on the ESP8266,
 *                4 on the ESP32 family (docs/plans/ext-radio.md).
 *   spi_attach   the ESP32 family must be told which SPI flash to talk to
 *                before FLASH_BEGIN; the ESP8266 has no such command.
 *   esp8266_erase  the ESP8266 ROM's own erase-size arithmetic; everyone else
 *                  erases exactly the region.
 *   magic[]      the values register 0x40001000 reads as on this chip.  Some
 *                chips have more than one (silicon revisions), so it is a list;
 *                a 0 entry ends it.  A chip esptool tells apart only by a
 *                security-info query rather than this register is not auto-
 *                detected here - name it with -chip.
 *
 * The list mirrors esptool's; the ones this project can actually put on a wire
 * (ESP8266, and the ESP32 on the C6/older boards) are the tested ones, the rest
 * are by the datasheet until a module of that kind is on the desk.
 */
typedef struct {
    const char *name;
    uint8_t     status_len;
    bool        spi_attach;
    bool        esp8266_erase;
    uint32_t    magic[4];
} esp_chip_t;

static const esp_chip_t k_chips[] = {
    {"esp8266", 2, false, true,  {ESP_CHIP_MAGIC_8266, 0, 0, 0}},
    {"esp32",   4, true,  false, {0x00f01d83u, 0, 0, 0}},
    {"esp32s2", 4, true,  false, {0x000007c6u, 0, 0, 0}},
    {"esp32s3", 4, true,  false, {0x00000009u, 0xeb004136u, 0, 0}},
    {"esp32c3", 4, true,  false, {0x6921506fu, 0x1b31506fu, 0x4881606fu,
                                  0x4361606fu}},
    {"esp32c6", 4, true,  false, {0x2ce0806fu, 0, 0, 0}},
    {"esp32c2", 4, true,  false, {0x6f51306fu, 0x7c41a06fu, 0, 0}},
    {"esp32h2", 4, true,  false, {0xd7b73e80u, 0, 0, 0}},
};

static const esp_chip_t *chip_by_magic(uint32_t magic)
{
    for (unsigned i = 0; i < sizeof(k_chips) / sizeof(k_chips[0]); i++) {
        for (unsigned m = 0; m < 4 && k_chips[i].magic[m] != 0; m++) {
            if (k_chips[i].magic[m] == magic) {
                return &k_chips[i];
            }
        }
    }
    return NULL;
}

static const esp_chip_t *chip_by_name(const char *name)
{
    for (unsigned i = 0; i < sizeof(k_chips) / sizeof(k_chips[0]); i++) {
        if (ag_strcmp(k_chips[i].name, name) == 0) {
            return &k_chips[i];
        }
    }
    return NULL;
}

/* ---- settings, filled from the command line -------------------------- */

static int              s_port = 1;
static uint32_t         s_baud = 115200u;
static const esp_chip_t *s_chip; /* the active profile, set by detect/-chip */

/* ---- buffers (static: a flash block is 1 KB and the stack is small) --- */

#define SUB_HDR 16u /* the FLASH_DATA sub-header before the block itself */

static uint8_t       s_block[ESP_FLASH_BLOCK];
static uint8_t       s_data[SUB_HDR + ESP_FLASH_BLOCK];
static uint8_t       s_cmd[ESP_CMD_HDR_SIZE + SUB_HDR + ESP_FLASH_BLOCK];
static uint8_t       s_wire[2u * (ESP_CMD_HDR_SIZE + SUB_HDR + ESP_FLASH_BLOCK) + 4u];
static uint8_t       s_rxframe[512];
static ag_esp_slip_t s_dec;

/* A one-buffer byte source over the UART, so a frame decoder can stop at the
 * frame boundary and leave the rest for next time rather than losing it. */
static uint8_t s_src[256];
static int     s_src_len;
static int     s_src_pos;

/* ---------------------------------------------------------------------- */

static bool parse_u32(const char *s, uint32_t *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    uint32_t v = 0;
    for (; *s != '\0'; ++s) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        v = v * 10u + (uint32_t)(*s - '0');
    }
    *out = v;
    return true;
}

/* Hex with an 0x prefix, decimal otherwise: an address is written both ways. */
static bool parse_addr(const char *s, uint32_t *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        uint32_t v = 0;
        for (const char *p = s + 2; *p != '\0'; ++p) {
            uint32_t d;
            if (*p >= '0' && *p <= '9') {
                d = (uint32_t)(*p - '0');
            } else if (*p >= 'a' && *p <= 'f') {
                d = (uint32_t)(*p - 'a' + 10);
            } else if (*p >= 'A' && *p <= 'F') {
                d = (uint32_t)(*p - 'A' + 10);
            } else {
                return false;
            }
            v = (v << 4) | d;
        }
        *out = v;
        return true;
    }
    return parse_u32(s, out);
}

/* ---- UART plumbing --------------------------------------------------- */

static int next_byte(uint32_t wait_ms)
{
    if (s_src_pos >= s_src_len) {
        const int32_t n = ag_uart_read(s_port, s_src, sizeof(s_src), wait_ms);
        if (n <= 0) {
            s_src_len = 0;
            s_src_pos = 0;
            return -1;
        }
        s_src_len = (int)n;
        s_src_pos = 0;
    }
    return s_src[s_src_pos++];
}

static void flush_input(void)
{
    uint8_t tmp[128];
    while (ag_uart_read(s_port, tmp, sizeof(tmp), 0) > 0) {
    }
    s_src_len = 0;
    s_src_pos = 0;
    ag_esp_slip_init(&s_dec, s_rxframe, sizeof(s_rxframe));
}

/* A whole SLIP frame into s_rxframe, or false on timeout. */
static bool read_frame(uint32_t timeout_ms, size_t *out_len)
{
    const uint32_t start = ag_millis();
    for (;;) {
        const int c = next_byte(20);
        if (c >= 0 && ag_esp_slip_feed(&s_dec, (uint8_t)c)) {
            *out_len = s_dec.len;
            return true;
        }
        ag_heartbeat();
        if ((ag_millis() - start) >= timeout_ms) {
            return false;
        }
    }
}

static ag_err_t send_inner(const uint8_t *inner, size_t inner_len)
{
    const size_t w = ag_esp_slip_encode(inner, inner_len, s_wire, sizeof(s_wire));
    if (w == 0) {
        return -AG_ERANGE;
    }
    const int32_t n = ag_uart_write(s_port, s_wire, w);
    return (n < 0) ? (ag_err_t)n : AG_OK;
}

/*
 * Send one command and wait for the response that echoes its op, skipping any
 * frame that is not it (the ROM emits stray SYNC replies).  `status_len` is the
 * chip's status-trailer length, or 0 to accept the response without judging its
 * status (used while detecting the chip, before that length is known).  A ROM
 * status of non-zero comes back as -AG_EIO.
 */
static ag_err_t command(uint8_t op, const uint8_t *data, uint16_t len,
                        uint32_t checksum, uint32_t timeout_ms,
                        uint32_t *value_out, uint8_t status_len)
{
    const size_t inner =
        ag_esp_cmd_build(op, data, len, checksum, s_cmd, sizeof(s_cmd));
    if (inner == 0) {
        return -AG_ERANGE;
    }
    const ag_err_t se = send_inner(s_cmd, inner);
    if (se != AG_OK) {
        return se;
    }

    const uint32_t start = ag_millis();
    for (;;) {
        size_t flen = 0;
        if (!read_frame(timeout_ms, &flen)) {
            return -AG_ETIMEDOUT;
        }
        ag_esp_resp_t r;
        if (ag_esp_resp_parse(s_rxframe, flen, &r) && r.op == op) {
            if (value_out != NULL) {
                *value_out = r.value;
            }
            uint8_t err = 0;
            return ag_esp_resp_ok(&r, status_len, &err) ? AG_OK : -AG_EIO;
        }
        if ((ag_millis() - start) >= timeout_ms) {
            return -AG_ETIMEDOUT;
        }
    }
}

/* The chip's status length once known, else 0 (accept without judging). */
static uint8_t status_len(void)
{
    return s_chip != NULL ? s_chip->status_len : 0;
}

/*
 * Which chip is on the wire.  Read the magic register (its value is the same
 * ask on every ESP chip) and match the table; -chip forces a profile and
 * skips this.  Done once, right after the SYNC handshake.
 */
static ag_err_t detect_chip(void)
{
    if (s_chip != NULL) {
        return AG_OK; /* forced with -chip */
    }
    uint8_t reg[4];
    ag_esp_put32(reg, ESP_CHIP_MAGIC_REG);
    uint32_t magic = 0;
    /* status_len 0: the trailer length is exactly what we do not know yet. */
    const ag_err_t e = command(ESP_CMD_READ_REG, reg, 4, 0, 1000, &magic, 0);
    if (e != AG_OK) {
        return e;
    }
    s_chip = chip_by_magic(magic);
    if (s_chip == NULL) {
        ag_printf("unrecognised chip (magic 0x%08x); name it with -chip\n",
                  (unsigned)magic);
        return -AG_ENOTSUP;
    }
    ag_printf("chip: %s (magic 0x%08x)\n", s_chip->name, (unsigned)magic);
    return AG_OK;
}

/* The ESP32 family must be told which SPI flash to use before FLASH_BEGIN; the
 * ROM wants eight bytes (arg, 0), and the default (all zero) is the usual pins.
 * The ESP8266 has no such command. */
static ag_err_t spi_attach(void)
{
    if (s_chip == NULL || !s_chip->spi_attach) {
        return AG_OK;
    }
    uint8_t arg[8] = {0};
    return command(ESP_CMD_SPI_ATTACH, arg, sizeof(arg), 0, 3000, NULL,
                   status_len());
}

/* ---- download-mode reset (optional, needs the pins wired) ------------- */

static void do_reset(int gpio0, int rst)
{
    const ag_io_api_t *io = g_ag_api->io;
    io->gpio_config(gpio0, AG_GPIO_OUT);
    io->gpio_config(rst, AG_GPIO_OUT);

    /* Classic esptool sequence, driven straight onto the module's own pins
     * (both active low): hold in reset with GPIO0 high, release reset with
     * GPIO0 low so the ROM samples it and enters download mode, then let go. */
    io->gpio_write(gpio0, 1);
    io->gpio_write(rst, 0);
    ag_delay(100);
    io->gpio_write(gpio0, 0);
    io->gpio_write(rst, 1);
    ag_delay(50);
    io->gpio_write(gpio0, 1);
}

/* ---- connect (SYNC handshake) ---------------------------------------- */

static ag_err_t sync_once(void)
{
    uint8_t payload[ESP_SYNC_PAYLOAD];
    ag_esp_sync_payload(payload, sizeof(payload));

    const size_t inner = ag_esp_cmd_build(ESP_CMD_SYNC, payload,
                                          ESP_SYNC_PAYLOAD, 0, s_cmd,
                                          sizeof(s_cmd));
    if (inner == 0 || send_inner(s_cmd, inner) != AG_OK) {
        return -AG_EIO;
    }

    const uint32_t start = ag_millis();
    while ((ag_millis() - start) < 200u) {
        size_t flen = 0;
        if (read_frame(100, &flen)) {
            ag_esp_resp_t r;
            if (ag_esp_resp_parse(s_rxframe, flen, &r) &&
                r.op == ESP_CMD_SYNC) {
                return AG_OK;
            }
        }
    }
    return -AG_ETIMEDOUT;
}

static ag_err_t connect_rom(int attempts, int gpio0, int rst)
{
    for (int a = 0; a < attempts; a++) {
        if (gpio0 >= 0 && rst >= 0) {
            do_reset(gpio0, rst);
        }
        flush_input();
        for (int i = 0; i < 7; i++) {
            if (sync_once() == AG_OK) {
                flush_input(); /* swallow the ROM's extra SYNC echoes */
                return AG_OK;
            }
        }
        ag_heartbeat();
        if (ag_kbhit()) {
            (void)ag_getch();
            return -AG_EKILLED;
        }
    }
    return -AG_ETIMEDOUT;
}

static ag_err_t open_port(void)
{
    const ag_err_t e = g_ag_api->io->uart_config(s_port, s_baud, 8, 0, 1);
    if (e != AG_OK) {
        ag_printf("uart%d at %u baud: %s\n", s_port, (unsigned)s_baud,
                  ag_strerror(e));
        ag_printf("is [uart%d] tx/rx set in C:\\BOARD.CFG?\n", s_port);
    }
    return e;
}

static void say_download_mode(int gpio0, int rst)
{
    if (gpio0 >= 0 && rst >= 0) {
        ag_printf("entering download mode via GPIO0=%d RST=%d...\n", gpio0, rst);
    } else {
        ag_printf("put the module in download mode by hand:\n");
        ag_printf("  jumper its GPIO0 to GND, then power-cycle it, then wait.\n");
    }
}

/* ---- subcommands ----------------------------------------------------- */

static int do_id(int gpio0, int rst)
{
    if (open_port() != AG_OK) {
        return 1;
    }
    say_download_mode(gpio0, rst);

    const ag_err_t c = connect_rom(gpio0 >= 0 ? 10 : 40, gpio0, rst);
    if (c != AG_OK) {
        ag_printf("no answer from the module: %s\n", ag_strerror(c));
        return 1;
    }
    ag_printf("connected\n");

    if (detect_chip() != AG_OK) {
        return 1;
    }

    /* The MAC registers are the ESP8266's; other chips keep it elsewhere, so
     * only read it when we know we are talking to one. */
    if (s_chip->esp8266_erase) {
        uint8_t  reg[4];
        uint32_t m0 = 0, m1 = 0;
        ag_esp_put32(reg, ESP_OTP_MAC0);
        (void)command(ESP_CMD_READ_REG, reg, 4, 0, 1000, &m0, status_len());
        ag_esp_put32(reg, ESP_OTP_MAC1);
        (void)command(ESP_CMD_READ_REG, reg, 4, 0, 1000, &m1, status_len());
        ag_printf("efuse MAC words: %08x %08x\n", (unsigned)m1, (unsigned)m0);
    }
    return 0;
}

static int do_write(const char *path, uint32_t addr, int gpio0, int rst)
{
    ag_stat_t st;
    if (ag_stat(path, &st) != AG_OK) {
        ag_printf("%s: not found\n", path);
        return 1;
    }
    const uint32_t size = (uint32_t)st.size;
    if (size == 0) {
        ag_printf("%s: is empty\n", path);
        return 1;
    }

    const ag_handle_t h = ag_open(path, AG_O_RDONLY);
    if (h < 0) {
        ag_printf("%s: %s\n", path, ag_strerror((ag_err_t)h));
        return 1;
    }

    if (open_port() != AG_OK) {
        ag_close(h);
        return 1;
    }
    say_download_mode(gpio0, rst);

    const ag_err_t c = connect_rom(gpio0 >= 0 ? 10 : 40, gpio0, rst);
    if (c != AG_OK) {
        ag_printf("no answer from the module: %s\n", ag_strerror(c));
        ag_printf("check the baud, that TX/RX are crossed, grounds tied,"
                  " and that it really is in download mode.\n");
        ag_close(h);
        return 1;
    }
    ag_printf("connected\n");

    if (detect_chip() != AG_OK) {
        ag_close(h);
        return 1;
    }

    ag_err_t err = spi_attach();
    if (err != AG_OK) {
        ag_printf("spi attach failed: %s\n", ag_strerror(err));
        ag_close(h);
        return 1;
    }

    /* The ESP8266 ROM has its own erase-size arithmetic; the rest erase exactly
     * the region they are about to write. */
    const uint32_t erase = s_chip->esp8266_erase
                               ? ag_esp8266_erase_size(addr, size)
                               : size;
    const uint32_t num_blocks =
        (size + ESP_FLASH_BLOCK - 1u) / ESP_FLASH_BLOCK;

    uint8_t begin[16];
    ag_esp_put32(begin + 0, erase);
    ag_esp_put32(begin + 4, num_blocks);
    ag_esp_put32(begin + 8, ESP_FLASH_BLOCK);
    ag_esp_put32(begin + 12, addr);

    ag_printf("flashing %u bytes at 0x%x, %u block(s) - erasing...\n",
              (unsigned)size, (unsigned)addr, (unsigned)num_blocks);
    /* The erase inside FLASH_BEGIN can take several seconds on a full chip. */
    err = command(ESP_CMD_FLASH_BEGIN, begin, 16, 0, 15000, NULL, status_len());
    if (err != AG_OK) {
        ag_printf("flash begin failed: %s\n", ag_strerror(err));
        ag_close(h);
        return 1;
    }

    uint32_t seq = 0, done = 0;
    for (;;) {
        const int32_t rd = ag_read(h, s_block, ESP_FLASH_BLOCK);
        if (rd < 0) {
            ag_printf("\n%s: read error: %s\n", path, ag_strerror((ag_err_t)rd));
            ag_close(h);
            return 1;
        }
        if (rd == 0) {
            break;
        }
        /* The ROM writes a whole block; pad the last one with 0xFF. */
        for (int32_t i = rd; i < (int32_t)ESP_FLASH_BLOCK; i++) {
            s_block[i] = 0xFF;
        }

        ag_esp_put32(s_data + 0, ESP_FLASH_BLOCK);
        ag_esp_put32(s_data + 4, seq);
        ag_esp_put32(s_data + 8, 0);
        ag_esp_put32(s_data + 12, 0);
        memcpy(s_data + SUB_HDR, s_block, ESP_FLASH_BLOCK);

        const uint8_t cks = ag_esp_checksum(s_block, ESP_FLASH_BLOCK);
        err = command(ESP_CMD_FLASH_DATA, s_data, SUB_HDR + ESP_FLASH_BLOCK,
                      cks, 5000, NULL, status_len());
        if (err != AG_OK) {
            ag_printf("\nblock %u failed: %s\n", (unsigned)seq,
                      ag_strerror(err));
            ag_close(h);
            return 1;
        }

        seq++;
        done += (uint32_t)rd;
        ag_printf("\r%u/%u KB", (unsigned)(done / 1024u),
                  (unsigned)((size + 1023u) / 1024u));
        ag_heartbeat();

        if (ag_kbhit()) {
            (void)ag_getch();
            ag_printf("\nstopped; the module's flash is now partly written\n");
            ag_close(h);
            return 1;
        }
        if (rd < (int32_t)ESP_FLASH_BLOCK) {
            break; /* that was the final, short block */
        }
    }
    ag_printf("\n");
    ag_close(h);

    /* FLASH_END with reboot=0 tells the ROM to run the new image.  It may reset
     * before its reply reaches us, so a timeout here is not a failure. */
    uint8_t end[4];
    ag_esp_put32(end, 0);
    err = command(ESP_CMD_FLASH_END, end, 4, 0, 3000, NULL, status_len());
    if (err == AG_OK || err == -AG_ETIMEDOUT) {
        ag_printf("done - the module should be running the new firmware\n");
        return 0;
    }
    ag_printf("flash end reported %s; the data was written, but the module may "
              "still be in the bootloader\n", ag_strerror(err));
    return 0;
}

/* ---------------------------------------------------------------------- */

static void usage(void)
{
    ag_printf("usage:\n");
    ag_printf("  espflash id    [-c CHIP] [-p PORT] [-b BAUD] [-r GPIO0 RST]\n");
    ag_printf("  espflash write <file> [-a ADDR] [-c CHIP] [-p PORT] [-b BAUD]"
              " [-r GPIO0 RST]\n");
    ag_printf("PORT default 1 (UART0 is the console), BAUD default 115200,"
              " ADDR default 0.\n");
    ag_printf("-c CHIP: auto (default) detects it; or name it - esp8266 esp32"
              " esp32s2 esp32s3 esp32c3 esp32c6 esp32c2 esp32h2\n");
    ag_printf("Omit -r on the CYD and enter download mode by hand.\n");
}

int ag_main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 1;
    }

    /* Options may follow the subcommand and (for write) the file. */
    const char *file = NULL;
    uint32_t    addr = 0;
    int         gpio0 = -1, rst = -1;

    const bool is_write = (ag_strcmp(argv[1], "write") == 0);
    const bool is_id = (ag_strcmp(argv[1], "id") == 0);
    if (!is_write && !is_id) {
        usage();
        return 1;
    }

    int i = 2;
    if (is_write) {
        if (argc < 3 || argv[2][0] == '-') {
            ag_printf("write: needs a file\n");
            usage();
            return 1;
        }
        file = argv[2];
        i = 3;
    }

    for (; i < argc; i++) {
        const char *a = argv[i];
        if (ag_strcmp(a, "-p") == 0 && i + 1 < argc) {
            uint32_t v;
            if (!parse_u32(argv[++i], &v)) {
                ag_printf("bad port '%s'\n", argv[i]);
                return 1;
            }
            s_port = (int)v;
        } else if (ag_strcmp(a, "-b") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], &s_baud) || s_baud == 0) {
                ag_printf("bad baud '%s'\n", argv[i]);
                return 1;
            }
        } else if (ag_strcmp(a, "-a") == 0 && i + 1 < argc) {
            if (!parse_addr(argv[++i], &addr)) {
                ag_printf("bad address '%s'\n", argv[i]);
                return 1;
            }
        } else if (ag_strcmp(a, "-c") == 0 && i + 1 < argc) {
            const char *name = argv[++i];
            if (ag_strcmp(name, "auto") != 0) {
                s_chip = chip_by_name(name);
                if (s_chip == NULL) {
                    ag_printf("unknown chip '%s'\n", name);
                    usage();
                    return 1;
                }
            }
        } else if (ag_strcmp(a, "-r") == 0 && i + 2 < argc) {
            uint32_t g, r;
            if (!parse_u32(argv[i + 1], &g) || !parse_u32(argv[i + 2], &r)) {
                ag_printf("bad reset pins '%s %s'\n", argv[i + 1], argv[i + 2]);
                return 1;
            }
            gpio0 = (int)g;
            rst = (int)r;
            i += 2;
        } else {
            ag_printf("don't understand '%s'\n", a);
            usage();
            return 1;
        }
    }

    if (s_port == 0) {
        ag_printf("port 0 is the console and cannot be used; wire the module to "
                  "another UART\n");
        return 1;
    }

    return is_id ? do_id(gpio0, rst) : do_write(file, addr, gpio0, rst);
}
