/*
 * ArgonOS - the node link, at the prompt (kernel private).
 *
 * Thin, like the network commands next door: words in, one call into
 * src/net/linkprov.c, and whatever came back printed.  No protocol here.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_CMD_LINK_H
#define ARGON_CMD_LINK_H

int ag_cmd_link(int argc, char **argv);

#endif /* ARGON_CMD_LINK_H */
