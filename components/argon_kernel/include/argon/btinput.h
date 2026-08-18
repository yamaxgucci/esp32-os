/*
 * ArgonOS - HID reports from a Bluetooth device, as console events.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_BTINPUT_H
#define ARGON_BTINPUT_H

#include <argon/abi.h>
#include <argon/port/bt.h>

#ifdef __cplusplus
extern "C" {
#endif

#if AG_PORT_HAS_BT
/* Registers the report handler.  The radio is started separately: a board can
 * have Bluetooth on and nothing paired to it. */
ag_err_t ag_btinput_init(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ARGON_BTINPUT_H */
