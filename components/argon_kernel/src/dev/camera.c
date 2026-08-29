/*
 * ArgonOS - /dev/cam0 for the build that carries the camera (CAMERA_BUILTIN).
 *
 * The loadable path (GC2145.SYS) registers its own /dev/cam0 over api->cam; this
 * is the other one, for the image that has esp32-camera compiled in.  Same
 * device, same reads, so CAM.AXE and everything else that opens /dev/cam0 does
 * not know or care which build it is running on - the difference is only whether
 * a module had to be loaded first.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/camera.h>

#include "sdkconfig.h"

#if defined(CONFIG_ARGON_CAMERA_BUILTIN) && CONFIG_ARGON_CAMERA_BUILTIN

#include <string.h>

#include <argon/board.h>
#include <argon/device.h>
#include <argon/log.h>
#include <argon/port/camera.h>

#define TAG "camera"

/* /dev/cam0: a read returns one captured frame, raw RGB565, exactly as the
 * loadable driver's does. */
static int32_t cam_read(ag_device_t *dev, void *buf, size_t len, uint64_t off)
{
    (void)dev;
    (void)off;
    size_t got = 0;
    const uint8_t *fb = ag_port_cam_builtin_capture(&got, 2000);
    if (fb == NULL) {
        return -AG_EIO;
    }
    if (got > len) {
        got = len;
    }
    memcpy(buf, fb, got);
    return (int32_t)got;
}

static uint64_t cam_size(ag_device_t *dev)
{
    (void)dev;
    /* VGA RGB565; the transport rejects anything else it is asked for. */
    return (uint64_t)640 * 480 * 2u;
}

static const ag_dev_ops_t k_cam_ops = {
    .read = cam_read,
    .size = cam_size,
};

ag_err_t ag_camera_init(void)
{
    const ag_board_camera_t *c = ag_board_camera();

    /* No [camera] in BOARD.CFG on this board: nothing to bring up, and that is
     * not an error - a CAM-capable image can run on a board with no sensor. */
    if (c == NULL || c->xclk == AG_PIN_NONE || c->pclk == AG_PIN_NONE) {
        return AG_OK;
    }

    const ag_cam_builtin_pins_t pins = {
        .xclk = c->xclk,
        .pclk = c->pclk,
        .vsync = c->vsync,
        .href = c->href,
        .data = { c->data[0], c->data[1], c->data[2], c->data[3],
                  c->data[4], c->data[5], c->data[6], c->data[7] },
        .sccb_sda = c->sccb_sda,
        .sccb_scl = c->sccb_scl,
        .pwdn = c->pwdn,
        .reset = c->reset,
        .xclk_hz = c->xclk_hz,
    };

    ag_err_t err = ag_port_cam_builtin_start(&pins, 640, 480);
    if (err != AG_OK) {
        ag_log(AG_LOG_WARN, TAG, "camera did not start (%d)", (int)err);
        return AG_OK; /* non-fatal: the rest of the system still boots */
    }

    const ag_dev_desc_t desc = {
        .name = "cam0",
        .driver = "builtin",
        .cls = AG_DEV_SENSOR,
        .flags = 0,
        .ops = &k_cam_ops,
    };
    err = ag_dev_register(&desc, NULL);
    if (err != AG_OK) {
        ag_log(AG_LOG_ERROR, TAG, "cannot register /dev/cam0 (%d)", (int)err);
        ag_port_cam_builtin_stop();
        return AG_OK;
    }
    ag_log(AG_LOG_INFO, TAG, "/dev/cam0 up (built-in, 640x480 RGB565)");
    return AG_OK;
}

#else /* the loadable path provides the camera; nothing built in */

ag_err_t ag_camera_init(void) { return AG_OK; }

#endif /* CONFIG_ARGON_CAMERA_BUILTIN */
