/*
 * ArgonOS port contract - the board as a USB device, on demand.
 *
 * The mirror image of usb.h: where that is the OTG peripheral as a HOST for a
 * keyboard, this is it as a DEVICE that a PC mounts.  The shape is deliberately
 * small and the reason is the same as everywhere else in the port: the port
 * does only what an application cannot, and the application owns the policy.
 *
 * What the port supplies, when AG_PORT_HAS_USBD is 1:
 *
 *   ag_err_t ag_port_usbd_start(const ag_usbd_msc_cbs_t *cbs)
 *   ag_err_t ag_port_usbd_stop(void)
 *   ag_err_t ag_port_usbd_status(ag_usbd_status_t *out)
 *
 * Contract, not advice:
 *
 * - start() is the whole PHY handover.  It uninstalls the USB-Serial-JTAG
 *   driver (the console's peripheral) and installs TinyUSB as a mass-storage
 *   device, taking the one internal PHY for the duration.  The two cannot run
 *   at once - one PHY, two owners - so the console is off its USB port until
 *   stop(), which uninstalls TinyUSB and reinstalls the Serial-JTAG driver.
 *   The kernel detaches and re-attaches the console ENDPOINT around these two
 *   calls; the port only owns the drivers and the PHY.
 * - The MSC callbacks (tud_msc_capacity_cb and the rest) are the port's own
 *   functions, required by TinyUSB to be global symbols.  Each one forwards to
 *   the matching entry of `cbs`, the table the application registered.  The
 *   table is copied, not borrowed, and cleared on stop() so an application that
 *   ends mid-session cannot leave the port calling freed code.
 * - The callbacks run on TinyUSB's task.  They must not block: the port calls
 *   straight into the application's code, and a callback that sleeps stalls the
 *   USB task and loses the host.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_USBD_H
#define ARGON_PORT_USBD_H

#include <stdbool.h>
#include <stdint.h>

#include <argon/abi.h>

#include <argon/port/impl/usbd.h>

#if AG_PORT_HAS_USBD

ag_err_t ag_port_usbd_start(const ag_usbd_msc_cbs_t *cbs);
ag_err_t ag_port_usbd_stop(void);
ag_err_t ag_port_usbd_status(ag_usbd_status_t *out);

#endif /* AG_PORT_HAS_USBD */

#endif /* ARGON_PORT_USBD_H */
