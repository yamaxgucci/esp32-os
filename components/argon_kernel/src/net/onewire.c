/*
 * ArgonOS - the `onewire` command: read a DS18B20 thermometer (and any part's
 * 64-bit ROM code) on a single pin.
 *
 *   onewire temp <gpio>    convert and read the temperature, in degrees C
 *   onewire rom  <gpio>    read the one device's 64-bit ROM code
 *
 * The line needs an external ~4.7k pull-up to 3.3 V and one DS18B20 across the
 * pin and ground (parasite power works, but a third wire to 3.3 V is steadier).
 * The bit timing lives in the port (onewire_hw.c); here is the device: the ROM
 * and function commands, the nine-byte scratchpad, and the two CRCs that say the
 * bytes arrived intact.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/config.h>

#if defined(CONFIG_ARGON_ONEWIRE) && CONFIG_ARGON_ONEWIRE

#include <stdlib.h>

#include <argon/console.h>
#include <argon/path.h>
#include <argon/shell.h>

#include <argon/port/onewire.h>
#include <argon/port/task.h>

/* Dallas/Maxim CRC-8, poly x^8+x^5+x^4+1 fed least-significant bit first. */
static uint8_t ow_crc8(const uint8_t *d, int n)
{
    uint8_t crc = 0;
    for (int i = 0; i < n; i++) {
        uint8_t b = d[i];
        for (int j = 0; j < 8; j++) {
            const uint8_t mix = (uint8_t)((crc ^ b) & 1u);
            crc >>= 1;
            if (mix) {
                crc ^= 0x8Cu;
            }
            b >>= 1;
        }
    }
    return crc;
}

static int ow_read_temp(int pin)
{
    if (ag_port_ow_reset(pin) != 1) {
        ag_console_puts("no device answered on that pin\n");
        return 1;
    }
    /* Skip ROM (one device), Convert T. */
    const uint8_t conv[] = {0xCC, 0x44};
    ag_port_ow_write(pin, conv, (int)sizeof(conv));

    /* A 12-bit conversion takes up to 750 ms; wait it out rather than poll. */
    ag_port_task_delay(ag_port_ms_to_ticks(750));

    if (ag_port_ow_reset(pin) != 1) {
        ag_console_puts("device left the bus mid-conversion\n");
        return 1;
    }
    const uint8_t rd[] = {0xCC, 0xBE};
    ag_port_ow_write(pin, rd, (int)sizeof(rd));

    uint8_t sp[9];
    ag_port_ow_read(pin, sp, (int)sizeof(sp));
    if (ow_crc8(sp, 8) != sp[8]) {
        ag_console_puts("scratchpad CRC failed (wiring or pull-up?)\n");
        return 1;
    }

    const int16_t raw = (int16_t)(((uint16_t)sp[1] << 8) | sp[0]);
    const int      neg = raw < 0;
    const uint16_t mag = neg ? (uint16_t)(-raw) : (uint16_t)raw;
    const int      whole = mag >> 4;             /* 1 LSB = 1/16 C */
    const int      tenk = (mag & 0xF) * 625;     /* ten-thousandths of a degree */
    ag_console_printf("%s%d.%04d C\n", neg ? "-" : "", whole, tenk);
    return 0;
}

static int ow_read_rom(int pin)
{
    if (ag_port_ow_reset(pin) != 1) {
        ag_console_puts("no device answered on that pin\n");
        return 1;
    }
    const uint8_t cmd = 0x33; /* Read ROM - only valid with one device present */
    ag_port_ow_write(pin, &cmd, 1);

    uint8_t rom[8];
    ag_port_ow_read(pin, rom, (int)sizeof(rom));
    ag_console_printf("rom %02x %02x %02x %02x %02x %02x %02x %02x",
                      rom[0], rom[1], rom[2], rom[3],
                      rom[4], rom[5], rom[6], rom[7]);
    if (ow_crc8(rom, 7) != rom[7]) {
        ag_console_puts("  (CRC failed - more than one device, or noise?)\n");
        return 1;
    }
    ag_console_printf("  family %02x\n", rom[0]);
    return 0;
}

int ag_cmd_onewire(int argc, char **argv)
{
    if (argc < 3) {
        ag_console_puts("usage: onewire temp <gpio>\n"
                        "       onewire rom  <gpio>\n");
        return 1;
    }
    const int pin = atoi(argv[2]);
    if (ag_path_icmp(argv[1], "temp") == 0) {
        return ow_read_temp(pin);
    }
    if (ag_path_icmp(argv[1], "rom") == 0) {
        return ow_read_rom(pin);
    }
    ag_console_puts("onewire: 'temp' or 'rom'\n");
    return 1;
}

#endif /* CONFIG_ARGON_ONEWIRE */
