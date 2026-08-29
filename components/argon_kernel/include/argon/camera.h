/*
 * ArgonOS - the built-in camera, brought up at boot.
 *
 * Only the CONFIG_ARGON_CAMERA_BUILTIN build has one: there, esp32-camera lives
 * in the image and this registers /dev/cam0 from BOARD.CFG's [camera] pins, so
 * a fixed product has its camera the moment it powers up with no module to
 * load.  Every other build compiles this to a no-op - the loadable path
 * (GC2145.SYS over api->cam) is how those get a camera.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_CAMERA_H
#define ARGON_CAMERA_H

#include <argon/abi.h> /* ag_err_t */

/* Registers /dev/cam0 when the image carries the camera and BOARD.CFG wires
 * one; AG_OK and does nothing otherwise, so devices_init calls it always. */
ag_err_t ag_camera_init(void);

#endif /* ARGON_CAMERA_H */
