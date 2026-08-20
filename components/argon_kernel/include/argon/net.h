/*
 * ArgonOS - networking bring-up and the api->net table.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_NET_H
#define ARGON_NET_H

#include <argon/abi.h>
#include <argon/port/config.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_ARGON_ENABLE_NET) && CONFIG_ARGON_ENABLE_NET

/* For the command table and anything else that is conditional on there
 * being a network at all, rather than on which link it runs over. */
#define AG_HAS_NET 1
ag_err_t ag_net_init(void);
bool ag_net_ready(void);

/*
 * A name or a dotted quad into a host-order address.  The same code the ABI's
 * net->resolve hands out, so a built-in command and an application resolve
 * alike.
 *
 * Called lookup rather than resolve for one reason: the SDK's inline wrapper
 * for that ABI slot is ag_net_resolve(), and the two applications that are
 * *also* built into the kernel (fm, edit) include the SDK header.  One name for
 * two different functions in one translation unit is a compile error waiting
 * for whoever adds the other include.
 */
ag_err_t ag_net_lookup(const char *host, uint32_t *addr_out);
extern const ag_net_api_t ag_net_api_impl;
const ag_net_api_t *ag_net_api_table(void);
#else
#define AG_HAS_NET 0

static inline ag_err_t ag_net_init(void) { return -AG_ENOSYS; }
static inline bool ag_net_ready(void) { return false; }
static inline ag_err_t ag_net_lookup(const char *host, uint32_t *addr_out)
{
    (void)host;
    (void)addr_out;
    return -AG_ENOSYS;
}
static inline const ag_net_api_t *ag_net_api_table(void) { return NULL; }
#endif

#ifdef __cplusplus
}
#endif

#endif /* ARGON_NET_H */
