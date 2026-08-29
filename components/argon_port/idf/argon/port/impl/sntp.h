/*
 * ArgonOS port: ESP-IDF - is SNTP time sync in this build.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_SNTP_H
#define ARGON_PORT_IMPL_SNTP_H

#include "sdkconfig.h"

#if defined(CONFIG_ARGON_NET_SNTP) && CONFIG_ARGON_NET_SNTP
#define AG_PORT_HAS_SNTP 1
#else
#define AG_PORT_HAS_SNTP 0
#endif

#endif /* ARGON_PORT_IMPL_SNTP_H */
