/*
 * ArgonOS - the supervisor (kernel private).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_SUPERVISOR_H
#define ARGON_SUPERVISOR_H

#include <argon/abi.h>

/* Starts the process table and the supervisor task.  A boot stage. */
ag_err_t ag_supervisor_init(void);

bool     ag_supervisor_running(void);
/* How many times something has been stopped from the keyboard, for ver/mem. */
uint32_t ag_supervisor_stops(void);

#endif /* ARGON_SUPERVISOR_H */
