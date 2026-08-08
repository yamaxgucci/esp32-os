/*
 * ArgonOS - HostFS: live host folder as guest drive H: (QEMU / UART1).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
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
 * Last host-pushed SMS pad snapshot (pad0, pad1, sys).  Returns true when a
 * push arrived within `max_age_ms` (0 = any cached value).  Used by the
 * virtual H:\sms.pad backend; apps normally open that path via VFS.
 */
bool ag_hostfs_pad_peek(uint8_t out[3], uint32_t max_age_ms);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_HOSTFS_H */
