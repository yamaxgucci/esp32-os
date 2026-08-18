/*
 * ArgonOS port contract - where build-time options come from.
 *
 * A handful of things are decided when the firmware is built rather than when
 * it runs: how big the code arena is, whether the network is compiled in,
 * whether analogue input is linked at all.  They live in Kconfig here and the
 * build system turns them into macros; a port with no Kconfig defines them
 * however it likes, or leaves them undefined and lets the defaults stand.
 *
 * Every option the kernel reads is spelled CONFIG_ARGON_* and every one of them
 * has a default in the code that reads it.  That is the contract: a port that
 * supplies an empty header still builds a working system.
 *
 *   CONFIG_ARGON_APP_ARENA_KB       code arena ceiling
 *   CONFIG_ARGON_APP_HEAP_KB        default process arena
 *   CONFIG_ARGON_APP_STACK_KB       default process stack
 *   CONFIG_ARGON_CONSOLE_KEY_EVENTS console event queue depth
 *   CONFIG_ARGON_ENABLE_ADC         link analogue input (see io.h for why not)
 *   CONFIG_ARGON_ENABLE_HOSTFS      mount H: from a host helper on UART1
 *   CONFIG_ARGON_ENABLE_NET         build the network subsystem
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_CONFIG_H
#define ARGON_PORT_CONFIG_H

#include <argon/port/impl/config.h>

#endif /* ARGON_PORT_CONFIG_H */
