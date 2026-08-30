/*
 * ArgonOS port: ESP-IDF - is a TLS client in this build.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_TLS_H
#define ARGON_PORT_IMPL_TLS_H

#include "sdkconfig.h"

#if defined(CONFIG_ARGON_NET_TLS) && CONFIG_ARGON_NET_TLS
#define AG_PORT_HAS_TLS 1
#else
#define AG_PORT_HAS_TLS 0
#endif

#endif /* ARGON_PORT_IMPL_TLS_H */
