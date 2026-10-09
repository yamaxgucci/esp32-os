/*
 * ArgonOS - uartmon: dump whatever a UART is saying, raw.
 *
 * A bring-up tool for anything on a serial line - an external radio, a sensor,
 * a modem of unknown firmware.  It does not interpret a byte: it opens the port
 * at the baud you name and prints, as hex and ASCII, everything that arrives for
 * a few seconds.  That is exactly what you want facing an unknown module: before
 * asking "does it answer AT" you ask "does it say ANYTHING", and at what baud.
 *
 *   run t:\uartmon.axe [uart] [baud] [seconds]
 *   run t:\uartmon.axe 1 74880          # catch an ESP8266's ROM boot log
 *   run t:\uartmon.axe 1 115200         # catch AT firmware's banner/replies
 *
 * Pins come from BOARD.CFG (uart<n>.tx/rx), same as any UART user.  Power-cycle
 * the module while this listens to catch its boot chatter.
 *
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc \
 *       --include sdk/include -o build\apps\UARTMON.AXE apps/uartmon/uartmon.c
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

AG_APP("UARTMON", "0.1", "argon", 0);

static uint32_t parse_u(const char *s, uint32_t def)
{
    if (s == NULL || *s == '\0') {
        return def;
    }
    uint32_t v = 0;
    for (const char *p = s; *p >= '0' && *p <= '9'; p++) {
        v = v * 10u + (uint32_t)(*p - '0');
    }
    return v;
}

int ag_main(int argc, char **argv)
{
    const int      uart = (argc > 1) ? (int)parse_u(argv[1], 1) : 1;
    const uint32_t baud = (argc > 2) ? parse_u(argv[2], 74880u) : 74880u;
    /* How long to WAIT for the module to say anything.  0 (the default) = no
     * limit: listen until it talks or Ctrl+C.  Once bytes start, capture
     * continues until a ~1 s gap, then exits - so there is no window to hit:
     * power-cycle the module whenever, we catch it. */
    const uint32_t secs = (argc > 3) ? parse_u(argv[3], 0u) : 0u;

    const ag_io_api_t *io = ag_api()->io;
    if (io == NULL || !AG_HAS(io, uart_config) || !AG_HAS(io, uart_read)) {
        ag_printf("uartmon: no io->uart on this build\n");
        return 1;
    }

    const ag_err_t e = io->uart_config(uart, baud, 8, 0, 1);
    if (e != AG_OK) {
        ag_printf("uartmon: uart%d did not come up: %d "
                  "(set uart%d.tx/rx in BOARD.CFG)\n",
                  uart, (int)e, uart);
        return 1;
    }

    if (secs == 0) {
        ag_printf("uartmon: listening on uart%d at %u baud, no limit "
                  "(Ctrl+C to stop) - power-cycle the module any time\n",
                  uart, (unsigned)baud);
    } else {
        ag_printf("uartmon: listening on uart%d at %u baud, up to %us "
                  "(Ctrl+C to stop) - power-cycle the module any time\n",
                  uart, (unsigned)baud, (unsigned)secs);
    }

    uint8_t  buf[128];
    uint32_t total = 0;
    uint8_t  line[16];
    uint32_t col = 0;
    /* Each read blocks up to 200 ms.  Wait (up to secs, or forever if 0) for the
     * first byte; once data has come, stop after ~1 s of silence (5 empty
     * reads).  Ctrl+C breaks out at any time. */
    const uint32_t reads = secs * 5u;
    bool     seen = false;
    uint32_t idle = 0;
    for (uint32_t i = 0; !ag_interrupted(); i++) {
        if (secs != 0 && i >= reads) {
            break;
        }
        const int32_t n = io->uart_read(uart, buf, sizeof(buf), 200);
        if (n <= 0) {
            if (seen && ++idle >= 5u) {
                break; /* the burst is over */
            }
            continue;
        }
        seen = true;
        idle = 0;
        for (int32_t k = 0; k < n; k++) {
            const uint8_t b = buf[k];
            line[col++] = b;
            ag_printf("%02X ", (unsigned)b);
            if (col == 16) {
                ag_printf(" |");
                for (uint32_t j = 0; j < 16; j++) {
                    const uint8_t c = line[j];
                    ag_printf("%c", (c >= 0x20 && c < 0x7f) ? (char)c : '.');
                }
                ag_printf("|\n");
                col = 0;
            }
        }
        total += (uint32_t)n;
    }

    /* Flush a partial last line. */
    if (col > 0) {
        for (uint32_t j = col; j < 16; j++) {
            ag_printf("   ");
        }
        ag_printf(" |");
        for (uint32_t j = 0; j < col; j++) {
            const uint8_t c = line[j];
            ag_printf("%c", (c >= 0x20 && c < 0x7f) ? (char)c : '.');
        }
        ag_printf("|\n");
    }

    ag_printf("uartmon: %u bytes on uart%d at %u baud\n", (unsigned)total, uart,
              (unsigned)baud);
    if (total == 0) {
        ag_printf("uartmon: silence - module dead/unpowered, RX pin wrong, or "
                  "no boot chatter (did you power-cycle it?)\n");
    }
    return 0;
}
