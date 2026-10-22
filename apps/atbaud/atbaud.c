/*
 * ArgonOS - atbaud: find an AT modem's baud (and whether it speaks AT at all).
 *
 * For a running module of unknown firmware/speed.  It does not power-cycle
 * anything: the module is already up, so this asks it.  For each candidate
 * baud it configures the UART, drains stale bytes, sends "AT\r\n", and reports
 * what came back - byte count, a short preview, and whether "OK" was in it.
 * The baud that returns "OK" is the modem's; a baud that returns bytes-but-not-
 * OK is its speed with some other firmware; silence everywhere means it ignores
 * serial input (needs reflashing) or is dead.
 *
 *   run t:\atbaud.axe [uart]
 *
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc \
 *       --include sdk/include -o build\apps\ATBAUD.AXE apps/atbaud/atbaud.c
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

AG_APP("ATBAUD", "0.1", "argon", 0);

static const uint32_t k_bauds[] = {
    115200, 74880, 9600, 57600, 38400, 19200, 230400, 460800,
};

int ag_main(int argc, char **argv)
{
    const int uart = (argc > 1) ? (int)(argv[1][0] - '0') : 1;

    const ag_io_api_t *io = ag_api()->io;
    if (io == NULL || !AG_HAS(io, uart_config) || !AG_HAS(io, uart_read) ||
        !AG_HAS(io, uart_write)) {
        ag_printf("atbaud: no io->uart on this build\n");
        return 1;
    }

    ag_printf("atbaud: probing uart%d - sending AT at each baud\n", uart);

    uint8_t buf[256];
    for (size_t i = 0; i < sizeof(k_bauds) / sizeof(k_bauds[0]); i++) {
        const uint32_t baud = k_bauds[i];
        if (io->uart_config(uart, baud, 8, 0, 1) != AG_OK) {
            ag_printf("  %6u: uart_config failed\n", (unsigned)baud);
            continue;
        }

        /* Drain whatever was buffered at the old baud / from noise. */
        for (int d = 0; d < 4; d++) {
            if (io->uart_read(uart, buf, sizeof(buf), 30) <= 0) {
                break;
            }
        }

        io->uart_write(uart, "AT\r\n", 4);

        /* Collect for ~500 ms. */
        uint32_t total = 0;
        bool     saw_ok = false;
        for (int r = 0; r < 5 && total < sizeof(buf); r++) {
            const int32_t n =
                io->uart_read(uart, buf + total, sizeof(buf) - total, 100);
            if (n > 0) {
                total += (uint32_t)n;
            }
        }
        for (uint32_t k = 0; k + 1 < total; k++) {
            if (buf[k] == 'O' && buf[k + 1] == 'K') {
                saw_ok = true;
                break;
            }
        }

        ag_printf("  %6u: %u bytes%s", (unsigned)baud, (unsigned)total,
                  saw_ok ? "  <-- OK!" : "");
        if (total > 0) {
            ag_printf("  [");
            const uint32_t show = total < 24 ? total : 24;
            for (uint32_t k = 0; k < show; k++) {
                const uint8_t c = buf[k];
                ag_printf("%c", (c >= 0x20 && c < 0x7f) ? (char)c : '.');
            }
            ag_printf("]");
        }
        ag_printf("\n");
    }

    ag_printf("atbaud: done\n");
    return 0;
}
