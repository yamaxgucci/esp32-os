/*
 * ArgonOS port: the DVP camera transport (ABI 0.41's api->cam).
 *
 * This is the thin, sensor-agnostic half of a camera - the S3's LCD_CAM
 * peripheral and its DMA, which a loadable sensor .SYS cannot reach through the
 * io ABI and so must live in the image.  It knows nothing about which sensor is
 * on the wires; the driver configures it with the sensor's pins and format and
 * pulls frames.  Same tier as the I2S and radio ports.
 *
 * A build without CONFIG_ARGON_ENABLE_CAMERA compiles the stubs and api->cam is
 * NULL.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_CAMERA_H
#define ARGON_PORT_CAMERA_H

#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h> /* ag_err_t, ag_cam_pins_t, ag_cam_fmt_t */

bool           ag_port_cam_present(void);
ag_err_t       ag_port_cam_configure(const ag_cam_pins_t *pins, ag_cam_fmt_t fmt,
                                     uint32_t width, uint32_t height);
const uint8_t *ag_port_cam_capture(size_t *len, uint32_t timeout_ms);
void           ag_port_cam_stop(void);

#endif /* ARGON_PORT_CAMERA_H */
