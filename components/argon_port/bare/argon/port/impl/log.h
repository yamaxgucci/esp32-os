/*
 * ArgonOS port: bare metal - the log underneath.
 *
 * The easiest file here, and a complete implementation may be two empty
 * functions: this is about capturing what the *lower* layer prints, and bare
 * metal has no lower layer.  Define the levels, make both calls no-ops, and
 * move on.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_LOG_H
#define ARGON_PORT_IMPL_LOG_H

#define AG_PORT_LOG_NONE    0
#define AG_PORT_LOG_ERROR   1
#define AG_PORT_LOG_WARN    2
#define AG_PORT_LOG_INFO    3
#define AG_PORT_LOG_DEBUG   4
#define AG_PORT_LOG_VERBOSE 5

/* Nothing below this layer prints, so there is nothing to redirect. */
static inline void ag_port_log_redirect(int (*sink)(const char *, va_list))
{
    (void)sink;
}

static inline void ag_port_log_level(const char *tag, int level)
{
    (void)tag;
    (void)level;
}

static inline int ag_port_log_level_get(const char *tag)
{
    (void)tag;
    return AG_PORT_LOG_NONE;
}

#endif /* ARGON_PORT_IMPL_LOG_H */
