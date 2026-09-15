/*
 * ArgonOS - flash an STM32 that is wired to one of this board's UARTs, from a
 * firmware image on disk.  The sibling of ESPFLASH for the other family a radio
 * or sensor coprocessor is likely to be: a different bootloader protocol (ST
 * AN3155), so a different tool, sharing only the UART and the file.
 *
 *   stm32flash id    [-p PORT] [-b BAUD] [-r BOOT0 RST]
 *   stm32flash write <file> [-a ADDR] [-p PORT] [-b BAUD] [-r BOOT0 RST]
 *
 *     -p PORT   which UART the chip is on (default 1; UART0 is the console)
 *     -b BAUD   line rate (default 115200)
 *     -a ADDR   flash address, hex or decimal (default 0x08000000, the flash base)
 *     -r B0 RST drive BOOT0 and RST to enter the bootloader (BOOT0 HIGH at reset)
 *               and, after writing, to run the new image (BOOT0 LOW at reset).
 *               Omit where those are not wired and do it by hand.
 *
 * THE ONE THING THAT CATCHES EVERYONE: the STM32 ROM bootloader is 8 bits EVEN
 * parity, one stop bit - not 8N1.  We open the port that way.  The wake byte
 * 0x7F sets the baud; every command is answered ACK (0x79) or NACK (0x1F).  The
 * frame details are in apps/common/stm32 and are host-tested; this file is the
 * UART, the file and the sequencing.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

#include "stm32/ag_stm32ldr.h"

AG_APP("STM32FLASH", "1.0", "argon", 0);

static int      s_port = 1;
static uint32_t s_baud = 115200u;

/* STM32 flash is written in 4-byte units; a block is 256 bytes at most. */
static uint8_t s_chunk[STM32_WRITE_MAX];
static uint8_t s_wr[STM32_WRITE_MAX + 2u];

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

/* ---- UART ------------------------------------------------------------ */

static int read_byte(uint32_t timeout_ms)
{
    uint8_t       b;
    const int32_t n = ag_uart_read(s_port, &b, 1, timeout_ms);
    return (n == 1) ? (int)b : -1;
}

static bool write_all(const uint8_t *buf, size_t len)
{
    return ag_uart_write(s_port, buf, len) == (int32_t)len;
}

static void flush_input(void)
{
    uint8_t tmp[64];
    while (ag_uart_read(s_port, tmp, sizeof(tmp), 0) > 0) {
    }
}

/* One byte, waited for; true iff it is ACK.  NACK and silence are both false. */
static bool wait_ack(uint32_t timeout_ms)
{
    const int b = read_byte(timeout_ms);
    return b == (int)STM32_ACK;
}

/* Send a command (byte + complement) and wait for its ACK. */
static bool send_cmd(uint8_t cmd, uint32_t timeout_ms)
{
    uint8_t frame[2];
    ag_stm32_cmd(cmd, frame);
    if (!write_all(frame, 2)) {
        return false;
    }
    return wait_ack(timeout_ms);
}

/* ---- reset (optional, needs BOOT0 and RST wired) --------------------- */

static void reset_into(int boot0, int rst, int boot0_level)
{
    const ag_io_api_t *io = g_ag_api->io;
    io->gpio_config(boot0, AG_GPIO_OUT);
    io->gpio_config(rst, AG_GPIO_OUT);
    io->gpio_write(boot0, boot0_level); /* HIGH = bootloader, LOW = run */
    io->gpio_write(rst, 0);             /* hold in reset (RST is active low) */
    ag_delay(50);
    io->gpio_write(rst, 1);             /* release; BOOT0 is sampled now */
    ag_delay(100);
}

/* ---- connect --------------------------------------------------------- */

/*
 * Wake the bootloader with 0x7F.  A fresh bootloader answers ACK; one that has
 * already been woken answers NACK, which is still "it is there" - so both count,
 * and only silence is a failure.
 */
static bool connect_boot(int attempts, int boot0, int rst)
{
    for (int a = 0; a < attempts; a++) {
        if (boot0 >= 0 && rst >= 0) {
            reset_into(boot0, rst, 1);
        }
        flush_input();
        const uint8_t init = STM32_INIT;
        if (write_all(&init, 1)) {
            const int b = read_byte(300);
            if (b == (int)STM32_ACK || b == (int)STM32_NACK) {
                flush_input();
                return true;
            }
        }
        ag_heartbeat();
        if (ag_kbhit()) {
            (void)ag_getch();
            return false;
        }
    }
    return false;
}

static ag_err_t open_port(void)
{
    /* 8 bits, EVEN parity, 1 stop - the STM32 bootloader's line, not 8N1. */
    const ag_err_t e = g_ag_api->io->uart_config(s_port, s_baud, 8, 2, 1);
    if (e != AG_OK) {
        ag_printf("uart%d at %u baud 8E1: %s\n", s_port, (unsigned)s_baud,
                  ag_strerror(e));
        ag_printf("is [uart%d] tx/rx set in C:\\BOARD.CFG?\n", s_port);
    }
    return e;
}

static void say_boot_mode(int boot0, int rst)
{
    if (boot0 >= 0 && rst >= 0) {
        ag_printf("entering the bootloader via BOOT0=%d RST=%d...\n", boot0, rst);
    } else {
        ag_printf("put the chip in the bootloader by hand:\n");
        ag_printf("  BOOT0 high, then reset (or power-cycle), then run this.\n");
    }
}

/*
 * Ask Get (0x00) which erase command this part has.  Returns the command code
 * (0x44 or 0x43), or 0 if it could not be learned - in which case the caller
 * falls back to extended erase, which the great majority of parts have.
 */
static uint8_t learn_erase_cmd(void)
{
    if (!send_cmd(STM32_CMD_GET, 1000)) {
        return 0;
    }
    const int n = read_byte(1000); /* bytes to follow, minus one */
    if (n < 0) {
        return 0;
    }
    (void)read_byte(1000); /* bootloader version */
    uint8_t found = 0;
    for (int i = 0; i < n; i++) {
        const int c = read_byte(1000);
        if (c == (int)STM32_CMD_EXT_ERASE) {
            found = STM32_CMD_EXT_ERASE;
        } else if (c == (int)STM32_CMD_ERASE && found == 0) {
            found = STM32_CMD_ERASE;
        }
    }
    (void)wait_ack(1000); /* trailing ACK */
    return found;
}

static bool mass_erase(void)
{
    const uint8_t which = learn_erase_cmd();
    const uint8_t cmd = (which == STM32_CMD_ERASE) ? STM32_CMD_ERASE
                                                   : STM32_CMD_EXT_ERASE;

    ag_printf("erasing (%s)...\n",
              cmd == STM32_CMD_ERASE ? "legacy" : "extended");
    /* A mass erase can take several seconds. */
    if (!send_cmd(cmd, 2000)) {
        return false;
    }
    if (cmd == STM32_CMD_EXT_ERASE) {
        uint8_t sp[3];
        ag_stm32_ext_erase_special(STM32_ERASE_GLOBAL, sp);
        if (!write_all(sp, sizeof(sp))) {
            return false;
        }
    } else {
        const uint8_t legacy[2] = {0xFF, 0x00}; /* global erase + checksum */
        if (!write_all(legacy, sizeof(legacy))) {
            return false;
        }
    }
    return wait_ack(30000);
}

/* ---- subcommands ----------------------------------------------------- */

static int do_id(int boot0, int rst)
{
    if (open_port() != AG_OK) {
        return 1;
    }
    say_boot_mode(boot0, rst);

    if (!connect_boot(boot0 >= 0 ? 10 : 40, boot0, rst)) {
        ag_printf("no answer from the chip (check 8E1, TX/RX crossed, BOOT0"
                  " high, grounds tied)\n");
        return 1;
    }
    ag_printf("connected\n");

    if (!send_cmd(STM32_CMD_GET_ID, 1000)) {
        ag_printf("get id refused\n");
        return 1;
    }
    const int n = read_byte(1000);
    if (n < 0) {
        ag_printf("no id returned\n");
        return 1;
    }
    uint16_t pid = 0;
    for (int i = 0; i <= n; i++) {
        const int b = read_byte(1000);
        if (b < 0) {
            ag_printf("id truncated\n");
            return 1;
        }
        pid = (uint16_t)((pid << 8) | (uint16_t)b);
    }
    (void)wait_ack(1000);
    ag_printf("STM32 product id 0x%03x\n", (unsigned)pid);
    return 0;
}

static int do_write(const char *path, uint32_t addr, int boot0, int rst)
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
    say_boot_mode(boot0, rst);

    if (!connect_boot(boot0 >= 0 ? 10 : 40, boot0, rst)) {
        ag_printf("no answer from the chip (check 8E1, TX/RX crossed, BOOT0"
                  " high, grounds tied)\n");
        ag_close(h);
        return 1;
    }
    ag_printf("connected\n");

    if (!mass_erase()) {
        ag_printf("erase failed (is flash read-protected? RDP must be off)\n");
        ag_close(h);
        return 1;
    }

    ag_printf("writing %u bytes at 0x%08x...\n", (unsigned)size,
              (unsigned)addr);
    uint32_t done = 0;
    uint32_t at = addr;
    for (;;) {
        const int32_t rd = ag_read(h, s_chunk, STM32_WRITE_MAX);
        if (rd < 0) {
            ag_printf("\n%s: read error: %s\n", path, ag_strerror((ag_err_t)rd));
            ag_close(h);
            return 1;
        }
        if (rd == 0) {
            break;
        }
        /* The bootloader writes in 4-byte units; pad the tail with 0xFF (an
         * erased flash byte) up to a multiple of four. */
        uint16_t n = (uint16_t)rd;
        while ((n & 3u) != 0u) {
            s_chunk[n++] = 0xFF;
        }

        if (!send_cmd(STM32_CMD_WRITE, 1000)) {
            ag_printf("\nwrite command refused at 0x%08x\n", (unsigned)at);
            ag_close(h);
            return 1;
        }
        uint8_t af[5];
        ag_stm32_addr(at, af);
        if (!write_all(af, sizeof(af)) || !wait_ack(1000)) {
            ag_printf("\naddress 0x%08x refused\n", (unsigned)at);
            ag_close(h);
            return 1;
        }
        const size_t wlen = ag_stm32_write_block(s_chunk, n, s_wr, sizeof(s_wr));
        if (wlen == 0 || !write_all(s_wr, wlen) || !wait_ack(2000)) {
            ag_printf("\nblock at 0x%08x refused\n", (unsigned)at);
            ag_close(h);
            return 1;
        }

        at += n;
        done += (uint32_t)rd;
        ag_printf("\r%u/%u KB", (unsigned)((done + 1023u) / 1024u),
                  (unsigned)((size + 1023u) / 1024u));
        ag_heartbeat();
        if (ag_kbhit()) {
            (void)ag_getch();
            ag_printf("\nstopped; the chip's flash is now partly written\n");
            ag_close(h);
            return 1;
        }
    }
    ag_printf("\n");
    ag_close(h);

    if (boot0 >= 0 && rst >= 0) {
        ag_printf("resetting to run the new firmware...\n");
        reset_into(boot0, rst, 0); /* BOOT0 low = run from flash */
    } else {
        ag_printf("done - set BOOT0 low and reset the chip to run it\n");
    }
    return 0;
}

/* ---------------------------------------------------------------------- */

static void usage(void)
{
    ag_printf("usage:\n");
    ag_printf("  stm32flash id    [-p PORT] [-b BAUD] [-r BOOT0 RST]\n");
    ag_printf("  stm32flash write <file> [-a ADDR] [-p PORT] [-b BAUD]"
              " [-r BOOT0 RST]\n");
    ag_printf("PORT default 1 (UART0 is the console), BAUD default 115200,"
              " ADDR default 0x08000000.\n");
    ag_printf("The bootloader line is 8E1.  Omit -r and enter the bootloader"
              " by hand (BOOT0 high, reset).\n");
}

int ag_main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 1;
    }

    const bool is_write = (ag_strcmp(argv[1], "write") == 0);
    const bool is_id = (ag_strcmp(argv[1], "id") == 0);
    if (!is_write && !is_id) {
        usage();
        return 1;
    }

    const char *file = NULL;
    uint32_t    addr = STM32_FLASH_BASE;
    int         boot0 = -1, rst = -1;

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
        } else if (ag_strcmp(a, "-r") == 0 && i + 2 < argc) {
            uint32_t b0, r;
            if (!parse_u32(argv[i + 1], &b0) || !parse_u32(argv[i + 2], &r)) {
                ag_printf("bad reset pins '%s %s'\n", argv[i + 1], argv[i + 2]);
                return 1;
            }
            boot0 = (int)b0;
            rst = (int)r;
            i += 2;
        } else {
            ag_printf("don't understand '%s'\n", a);
            usage();
            return 1;
        }
    }

    if (s_port == 0) {
        ag_printf("port 0 is the console; wire the chip to another UART\n");
        return 1;
    }

    return is_id ? do_id(boot0, rst) : do_write(file, addr, boot0, rst);
}
