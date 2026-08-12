/*
 * ArgonOS - filesystem shell commands (kernel private).
 *
 * Split out of shell.c because between them they are longer than everything
 * else the shell does.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_CMD_FS_H
#define ARGON_CMD_FS_H

int ag_cmd_dir(int argc, char **argv);
int ag_cmd_cd(int argc, char **argv);
int ag_cmd_type(int argc, char **argv);
int ag_cmd_copy(int argc, char **argv);
int ag_cmd_del(int argc, char **argv);
int ag_cmd_mkdir(int argc, char **argv);
int ag_cmd_rmdir(int argc, char **argv);
int ag_cmd_rename(int argc, char **argv);
int ag_cmd_mount(int argc, char **argv);
int ag_cmd_hexdump(int argc, char **argv);
int ag_cmd_format(int argc, char **argv);
int ag_cmd_recv(int argc, char **argv);

#endif /* ARGON_CMD_FS_H */
