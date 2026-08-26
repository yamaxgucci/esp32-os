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

/* ---------------------------------------------------------------------- *
 * The other camera: the whole thing in the image (CONFIG_ARGON_CAMERA_BUILTIN)
 *
 * This is a different contract from the transport above, on purpose.  The
 * transport is the ABI half a loadable sensor .SYS drives; the built-in camera
 * is esp32-camera compiled in, sensor and all, so it is not reached through the
 * ABI at all and needs things the ABI has no reason to carry - the SCCB pins it
 * runs itself, a power-down line, a reset.  The kernel fills this from
 * BOARD.CFG's [camera] section and registers /dev/cam0; no module is loaded.
 *
 * A build without CONFIG_ARGON_CAMERA_BUILTIN compiles none of this (the kernel
 * that would call it is behind the same switch), so there is no stub.
 * ---------------------------------------------------------------------- */
typedef struct {
    int16_t  xclk;
    int16_t  pclk;
    int16_t  vsync;
    int16_t  href;
    int16_t  data[8];    /* D0..D7 */
    int16_t  sccb_sda;
    int16_t  sccb_scl;
    int16_t  pwdn;       /* AG_PIN_NONE (-1) if none */
    int16_t  reset;      /* AG_PIN_NONE (-1) if none */
    uint32_t xclk_hz;
} ag_cam_builtin_pins_t;

ag_err_t       ag_port_cam_builtin_start(const ag_cam_builtin_pins_t *pins,
                                         uint32_t width, uint32_t height);
const uint8_t *ag_port_cam_builtin_capture(size_t *len, uint32_t timeout_ms);
void           ag_port_cam_builtin_stop(void);

#endif /* ARGON_PORT_CAMERA_H */
