/*
 * ArgonOS port contract - CAN bus (the chip's TWAI controller).
 *
 * A frame is an identifier, up to eight data bytes, and a standard/extended
 * flag; the controller does arbitration and CRC.  open() installs and starts
 * it on a pair of pins at a bit rate, send()/recv() move one frame, close()
 * tears it down.  recv() returns 1 on a frame, 0 on timeout, negative on error.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_CAN_H
#define ARGON_PORT_CAN_H

#include <stdbool.h>
#include <stdint.h>

/* Bit rates the driver has a timing preset for (kbit/s): 25,50,100,125,250,500,
 * 800,1000.  Others are rejected. */
int  ag_port_can_open(int tx, int rx, uint32_t kbps);
int  ag_port_can_send(uint32_t id, const uint8_t *data, int len, bool extended);
int  ag_port_can_recv(uint32_t *id, uint8_t *data, int *len, bool *extended,
                      uint32_t timeout_ms);
void ag_port_can_close(void);

#endif /* ARGON_PORT_CAN_H */
