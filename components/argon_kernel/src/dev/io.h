/*
 * ArgonOS - direct hardware access (kernel private).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_DEV_IO_H
#define ARGON_DEV_IO_H

#include <argon/abi.h>

/*
 * Sets up the pin table and reserves what the system is already using.  Part of
 * the devices stage; no bus is touched here, because a bus is brought up on
 * first use and a board that never uses I2C should not pay for it - nor should
 * it drive pins on the strength of a configuration file nobody checked.
 */
ag_err_t ag_io_init(void);

/*
 * Gives back every pin a process held, removing its interrupt handlers first.
 * Called from the process reclaim path.  Returns how many pins came back.
 */
uint32_t ag_io_reclaim(ag_pid_t pid);

/* The io subtable of the ABI. */
extern const ag_io_api_t ag_io_api_table;

/*
 * Registers what the board describes with the device registry: the pins as one
 * device, and each configured bus as its own.  Called after ag_io_init.
 */
void ag_io_register_devices(void);

#endif /* ARGON_DEV_IO_H */
