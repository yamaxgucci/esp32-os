/*
 * ArgonOS - an SSH server, built on mbedTLS primitives (via the port), not on a
 * second SSH library.  The protocol (RFC 4251-4254) lives here; the crypto it
 * needs comes through argon/port.
 *
 * Being written in milestones, each testable with a real `ssh` client:
 *   1. transport: version exchange + KEXINIT negotiation      <- here now
 *   2. curve25519 key exchange, rsa host key, aes+hmac, NEWKEYS
 *   3. userauth (password)
 *   4. a session channel wired to the console (like telnet)
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_SSH_H
#define ARGON_SSH_H

#include <stdbool.h>
#include <stdint.h>

#include <argon/abi.h>
#include <argon/port/config.h> /* CONFIG_ARGON_NET_SSH */

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_ARGON_NET_SSH) && CONFIG_ARGON_NET_SSH

/* Start/stop the listener (default port 22).  AG_OK / -AG_EBUSY. */
ag_err_t ag_ssh_start(uint16_t port);
void     ag_ssh_stop(void);
bool     ag_ssh_running(void);
uint16_t ag_ssh_port(void);

#endif /* CONFIG_ARGON_NET_SSH */

#ifdef __cplusplus
}
#endif

#endif /* ARGON_SSH_H */
