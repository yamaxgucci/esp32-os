/*
 * ArgonOS application template.
 *
 * Build produces HELLO.AXE; copy it to the SD card and run it from the shell:
 *
 *     A:\> run hello
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

AG_APP("HELLO", "1.0", "ArgonOS", 0);

int ag_main(int argc, char **argv)
{
    ag_sysinfo_t si;
    ag_meminfo_t mi;

    ag_sysinfo_get(&si);
    ag_meminfo(&mi);

    ag_color(AG_LGREEN, AG_BLACK);
    ag_printf("Hello from %s %s on %s\n", si.os_name, si.os_version, si.chip);
    ag_color(AG_LGRAY, AG_BLACK);

    ag_printf("arena: %u KB free of %u KB, %u KB fast\n",
              (unsigned)(mi.arena_free / 1024), (unsigned)(mi.arena_total / 1024),
              (unsigned)(mi.fast_free / 1024));

    for (int i = 0; i < argc; i++) {
        ag_printf("argv[%d] = %s\n", i, argv[i]);
    }

    ag_printf("press a key to exit...\n");
    (void)ag_getch();
    return 0;
}
