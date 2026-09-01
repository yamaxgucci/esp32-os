/*
 * ArgonOS - the `ota` command: update the firmware over the network.
 *
 * Thin, like the other network commands: it asks the port to pull an image
 * into the spare app slot and, if told to, reboots into it.  The work is in
 * argon/port/ota.h; the partition table with two app slots is what makes a
 * spare slot exist (see partitions.csv).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/config.h>

#if defined(CONFIG_ARGON_NET_OTA) && CONFIG_ARGON_NET_OTA

#include <argon/console.h>
#include <argon/path.h>

#include <argon/port/ota.h>
#include <argon/port/sys.h>

int ag_cmd_ota(int argc, char **argv)
{
    if (argc < 2) {
        ag_console_printf("running from %s; ", ag_port_ota_running());
        if (ag_port_ota_capable()) {
            ag_console_puts("`ota <url>` to update, add /reboot to run it\n");
        } else {
            ag_console_puts("no spare slot in this image (single-slot table)\n");
        }
        return 0;
    }

    if (!ag_port_ota_capable()) {
        ag_console_puts(
            "OTA needs a two-slot partition table; this image has one app slot\n");
        return 1;
    }

    const char *url = argv[1];
    bool        reboot = false;
    for (int i = 2; i < argc; i++) {
        if (ag_path_icmp(argv[i], "/reboot") == 0) {
            reboot = true;
        }
    }

    ag_console_printf("updating from %s ...\n", url);
    if (ag_port_ota_pull(url) != 0) {
        ag_console_puts(
            "update failed (download, or the image did not check out)\n");
        return 1;
    }
    ag_console_puts("installed to the spare slot\n");

    if (reboot) {
        ag_console_puts("rebooting into it ...\n");
        ag_port_restart();
    } else {
        ag_console_puts("reboot to run it (add /reboot to do both)\n");
    }
    return 0;
}

#endif /* CONFIG_ARGON_NET_OTA */
