/*
 * ArgonOS - the `can` command: send and watch CAN (TWAI) frames.
 *
 *   can send <tx> <rx> <kbps> <id> [byte...] [/ext]
 *   can recv <tx> <rx> <kbps> [/ext-ids]        (until Ctrl+C)
 *
 * The controller is opened for the command and closed after it; the bus needs a
 * CAN transceiver (e.g. SN65HVD230) on the tx/rx pins and at least one other
 * node, or transmit fails with no acknowledgement.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/config.h>

#if defined(CONFIG_ARGON_CAN) && CONFIG_ARGON_CAN

#include <stdlib.h>

#include <argon/console.h>
#include <argon/path.h>
#include <argon/shell.h>

#include <argon/port/can.h>

int ag_cmd_can(int argc, char **argv)
{
    if (argc < 5) {
        ag_console_puts(
            "usage: can send <tx> <rx> <kbps> <id> [byte...] [/ext]\n"
            "       can recv <tx> <rx> <kbps>            (Ctrl+C to stop)\n");
        return 1;
    }
    const bool is_send = (ag_path_icmp(argv[1], "send") == 0);
    const bool is_recv = (ag_path_icmp(argv[1], "recv") == 0);
    if (!is_send && !is_recv) {
        ag_console_puts("can: 'send' or 'recv'\n");
        return 1;
    }
    const int      tx = atoi(argv[2]);
    const int      rx = atoi(argv[3]);
    const uint32_t kbps = (uint32_t)strtoul(argv[4], NULL, 0);

    if (ag_port_can_open(tx, rx, kbps) != 0) {
        ag_console_puts("cannot open the CAN controller (pins or bit rate)\n");
        return 1;
    }

    int rc = 0;
    if (is_send) {
        if (argc < 6) {
            ag_console_puts("can send needs an id\n");
            ag_port_can_close();
            return 1;
        }
        bool     ext = false;
        uint32_t id = (uint32_t)strtoul(argv[5], NULL, 0);
        uint8_t  data[8];
        int      len = 0;
        for (int i = 6; i < argc; i++) {
            if (ag_path_icmp(argv[i], "/ext") == 0) {
                ext = true;
            } else if (len < 8) {
                data[len++] = (uint8_t)strtoul(argv[i], NULL, 0);
            }
        }
        if (ag_port_can_send(id, data, len, ext) == 0) {
            ag_console_printf("sent id 0x%lx, %d bytes\n", (unsigned long)id,
                              len);
        } else {
            ag_console_puts("send failed (no other node acknowledged?)\n");
            rc = 1;
        }
    } else {
        ag_console_puts("listening; Ctrl+C to stop\n");
        while (!ag_shell_interrupted()) {
            uint32_t id = 0;
            uint8_t  data[8];
            int      len = 0;
            bool     ext = false;
            const int r = ag_port_can_recv(&id, data, &len, &ext, 200);
            if (r == 1) {
                ag_console_printf("id 0x%-8lx [%d] ", (unsigned long)id, len);
                for (int i = 0; i < len; i++) {
                    ag_console_printf("%02x ", data[i]);
                }
                ag_console_puts("\n");
            } else if (r < 0) {
                break;
            }
        }
    }

    ag_port_can_close();
    return rc;
}

#endif /* CONFIG_ARGON_CAN */
