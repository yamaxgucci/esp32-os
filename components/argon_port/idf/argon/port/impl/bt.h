/*
 * ArgonOS port: ESP-IDF - is there a Bluetooth host in this build.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_BT_H
#define ARGON_PORT_IMPL_BT_H

#include "sdkconfig.h"

#if defined(CONFIG_ARGON_ENABLE_BLE) && CONFIG_ARGON_ENABLE_BLE
#define AG_PORT_HAS_BT 1
#else
#define AG_PORT_HAS_BT 0
#endif

#endif /* ARGON_PORT_IMPL_BT_H */
