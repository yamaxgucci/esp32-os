/*
 * ArgonOS - network shell commands (kernel private).
 *
 * Thin: each of these turns words into an engine call in src/net and prints
 * what came back.  The protocols are not here.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_CMD_NET_H
#define ARGON_CMD_NET_H

int ag_cmd_net(int argc, char **argv);
int ag_cmd_wget(int argc, char **argv);
int ag_cmd_httpd(int argc, char **argv);
int ag_cmd_ftp(int argc, char **argv);

#endif /* ARGON_CMD_NET_H */
