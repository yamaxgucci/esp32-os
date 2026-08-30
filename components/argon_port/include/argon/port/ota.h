/*
 * ArgonOS port contract - over-the-air firmware update.
 *
 * Unlike MQTT or Modbus, OTA is not a wire protocol to reimplement: it is
 * bootloader and flash machinery, and the platform already has it right
 * (streaming a signed image into the spare app slot, validating it, and
 * pointing the bootloader at it).  So this wraps that rather than replacing it.
 *
 *   bool        ag_port_ota_capable(void)   -- is there a spare app slot?
 *   const char *ag_port_ota_running(void)   -- which slot is running now
 *   int         ag_port_ota_pull(url)       -- download + install; 0 on success
 *
 * ag_port_ota_pull returns only when done: it fetches the image at `url` (http
 * or https, verified against the certificate bundle), writes it to the slot not
 * running, checks it, and marks it to boot next.  The reboot is the caller's.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_OTA_H
#define ARGON_PORT_OTA_H

#include <stdbool.h>

bool        ag_port_ota_capable(void);
const char *ag_port_ota_running(void);
int         ag_port_ota_pull(const char *url);

#endif /* ARGON_PORT_OTA_H */
