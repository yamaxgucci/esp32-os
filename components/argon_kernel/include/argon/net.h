/*
 * ArgonOS - networking bring-up and the api->net table.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_NET_H
#define ARGON_NET_H

#include <argon/abi.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_ARGON_ENABLE_NET) && CONFIG_ARGON_ENABLE_NET
ag_err_t ag_net_init(void);
bool ag_net_ready(void);
extern const ag_net_api_t ag_net_api_impl;
const ag_net_api_t *ag_net_api_table(void);
#else
static inline ag_err_t ag_net_init(void) { return -AG_ENOSYS; }
static inline bool ag_net_ready(void) { return false; }
static inline const ag_net_api_t *ag_net_api_table(void) { return NULL; }
#endif

#ifdef __cplusplus
}
#endif

#endif /* ARGON_NET_H */
