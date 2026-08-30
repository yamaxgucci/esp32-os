/*
 * ArgonOS - the `mcpwm` command: a complementary PWM pair for a half-bridge.
 *
 *   mcpwm start <gpioA> <gpioB> <hz> <duty%> [deadtime_ns]
 *   mcpwm stop
 *
 * A and B are inverses with a dead-time on every edge, so they can drive the
 * two switches of a motor half-bridge without shoot-through.  start leaves the
 * peripheral running and returns; stop tears it down.  Watch it on two LEDs or
 * a scope on gpioA/gpioB - at a few hertz the pair is visible by eye.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/config.h>

#if defined(CONFIG_ARGON_MCPWM) && CONFIG_ARGON_MCPWM

#include <stdlib.h>

#include <argon/console.h>
#include <argon/path.h>

#include <argon/port/mcpwm.h>

int ag_cmd_mcpwm(int argc, char **argv)
{
    if (argc >= 2 && ag_path_icmp(argv[1], "stop") == 0) {
        ag_port_mcpwm_stop();
        ag_console_puts("stopped\n");
        return 0;
    }
    if (argc < 6 || ag_path_icmp(argv[1], "start") != 0) {
        ag_console_puts(
            "usage: mcpwm start <gpioA> <gpioB> <hz> <duty%> [deadtime_ns]\n"
            "       mcpwm stop\n");
        return 1;
    }

    const int      ga = atoi(argv[2]);
    const int      gb = atoi(argv[3]);
    const uint32_t hz = (uint32_t)strtoul(argv[4], NULL, 0);
    const long     duty_pct = atol(argv[5]);
    const uint32_t dead_ns = (argc > 6) ? (uint32_t)strtoul(argv[6], NULL, 0)
                                        : 0u;

    if (duty_pct < 0 || duty_pct > 100) {
        ag_console_puts("duty is a percentage, 0..100\n");
        return 1;
    }

    if (ag_port_mcpwm_pair(ga, gb, hz, (uint32_t)duty_pct * 10u, dead_ns) != 0) {
        ag_console_puts("mcpwm: could not start (pins, frequency, or "
                        "dead-time out of range)\n");
        return 1;
    }
    ag_console_printf("running: gpio %d / %d, %lu Hz, %ld%%, %lu ns dead\n", ga,
                      gb, (unsigned long)hz, duty_pct, (unsigned long)dead_ns);
    return 0;
}

#endif /* CONFIG_ARGON_MCPWM */
