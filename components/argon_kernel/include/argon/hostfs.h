/*
 * ArgonOS - HostFS: live host folder as guest drive H: (QEMU / UART1).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_HOSTFS_H
#define ARGON_HOSTFS_H

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Opens UART1, handshakes with the host helper, mounts /host read-only.
 * Returns AG_OK when mounted, -AG_ENODEV when no helper answers (ordinary).
 */
ag_err_t ag_hostfs_try_mount(void);

bool ag_hostfs_mounted(void);

/*
 * Task currently inside a HostFS UART RPC (holding the RPC mutex), or NULL.
 * Used by the process killer so it does not delete a task mid-RPC.
 */
void *ag_hostfs_rpc_holder(void);

/*
 * Last host-pushed pad snapshot (AG_PAD_BYTES).  Returns true when a push
 * arrived within `max_age_ms` (0 = any cached value).  Prefer the input
 * layer (ag_input_pad_peek / inp->btn); this remains for the virtual
 * H:\sms.pad compatibility path.
 */
bool ag_hostfs_pad_peek(uint8_t out[6], uint32_t max_age_ms);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_HOSTFS_H */
