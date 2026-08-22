/*
 * ArgonOS port: ESP-IDF - is there general BLE (central role) in this build.
 *
 * bt.h is deliberately about one thing: a keyboard, over HID.  This is the rest
 * of what a BLE central can do and a keyboard did not need - listening to what
 * every device in range is advertising, and connecting to an arbitrary service
 * to read and write a characteristic.  It is the "consume anything" half, and
 * it rides the same NimBLE host bt_hw.c already starts, so it costs code but no
 * second controller.  A build that only wants a keyboard turns it off.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_BLE_H
#define ARGON_PORT_IMPL_BLE_H

#include "sdkconfig.h"

#include <argon/port/impl/bt.h> /* AG_PORT_HAS_BT */

#if AG_PORT_HAS_BT && defined(CONFIG_ARGON_BLE_CENTRAL) && \
    CONFIG_ARGON_BLE_CENTRAL
#define AG_PORT_HAS_BLE_CENTRAL 1
#else
#define AG_PORT_HAS_BLE_CENTRAL 0
#endif

/*
 * The peripheral half: the board being the device other machines connect to -
 * it advertises, holds a GATT server, and a phone or PC reads and writes it.
 * The other direction from central; a board can be built with either or both.
 */
#if AG_PORT_HAS_BT && defined(CONFIG_ARGON_BLE_PERIPHERAL) && \
    CONFIG_ARGON_BLE_PERIPHERAL
#define AG_PORT_HAS_BLE_PERIPH 1
#else
#define AG_PORT_HAS_BLE_PERIPH 0
#endif

#endif /* ARGON_PORT_IMPL_BLE_H */
