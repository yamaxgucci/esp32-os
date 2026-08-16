/*
 * ArgonOS port: bare metal - pins and buses, the constants.
 *
 * Twenty-odd short functions in src/io_hw.c and the handful of numbers here.
 * None of it is hard and none of it is subtle, but read the "contract, not
 * advice" section of argon/port/io.h before starting: three of the rules there
 * are written down because breaking them cost this project real time.
 *
 * Nothing above this layer needs any of it to boot.  A port can define the
 * counts as zero, return -AG_ENOTSUP from everything, and come up to a prompt.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_IO_H
#define ARGON_PORT_IMPL_IO_H

#error "bare: no pins or buses yet.  See argon/port/io.h for the contract."

#endif /* ARGON_PORT_IMPL_IO_H */
