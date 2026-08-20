/*
 * ArgonOS - the network services the shell drives (kernel private).
 *
 * Three engines, one header.  Each one owns a conversation with the other end
 * and prints what it is doing on the console, because on this system these are
 * commands rather than daemons: nothing here runs unless somebody typed it.
 *
 * They are in the kernel rather than in .AXE images for the same reason the
 * file manager is.  A board whose card is empty - or whose card is what is
 * being fetched - still has to be able to fetch it, and a network tool that
 * has to be copied onto the machine before it can copy anything onto the
 * machine is not a tool.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_NETSVC_H
#define ARGON_NETSVC_H

#include <argon/abi.h>
#include <argon/netmsg.h>

/*
 * Fetches `url` into `dest`, an already resolved absolute path.  Prints
 * progress; follows redirects; leaves a partial file behind when interrupted,
 * because a partial file is a real file and deleting the operator's bytes is
 * not this command's decision.
 *
 * -AG_ENOENT for 404, -AG_EACCES for 401/403, -AG_EIO for any other status the
 * server refused with: the shell prints the reason, so the code only has to be
 * distinguishable.
 */
ag_err_t ag_http_fetch(const char *url, const char *dest);

/*
 * Serves `root` (an absolute path) over HTTP on `port` until Ctrl+C.  One
 * connection at a time, on purpose: see the file.
 */
ag_err_t ag_httpd_run(uint16_t port, const char *root);

/*
 * An FTP session at the console: connects, logs in, and reads commands until
 * `bye`.  `cwd` is where local files are read and written.
 */
ag_err_t ag_ftp_run(const ag_url_t *url, const char *cwd);

/*
 * One transfer and out, for scripts and for `wget ftp://...`: logs in, gets
 * `remote` into `dest` (an absolute path), logs out.
 */
ag_err_t ag_ftp_fetch(const ag_url_t *url, const char *dest);

#endif /* ARGON_NETSVC_H */
