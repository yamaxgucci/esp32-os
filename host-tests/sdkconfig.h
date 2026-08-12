/*
 * ArgonOS - empty sdkconfig.h for the host build.
 *
 * Kernel sources that build both ways include "sdkconfig.h" for their Kconfig
 * options; on the target ESP-IDF generates it, on the host there is no build
 * system to generate it and no chip to configure.  Every such source already
 * carries an #ifndef fallback for the options it reads, so an empty header is
 * the whole contract: the host build stays independent of ESP-IDF, which is
 * the point of host-tests.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_HOST_SDKCONFIG_H
#define ARGON_HOST_SDKCONFIG_H

#endif /* ARGON_HOST_SDKCONFIG_H */
