/*
 * ArgonOS - GC2145 camera sensor driver, as a loadable .SYS.
 *
 * The firmware carries only the DVP transport (api->cam); this module is the
 * sensor half: it brings the GC2145 up over SCCB (an I2C bus), points the
 * transport at it, and publishes /dev/cam0.  A different sensor is a different
 * .SYS - no rebuild of the image.
 *
 *   drv load a:\gc2145.sys      (or A:\VIRT-style autoload)
 *   run a:\cam.axe a:\shot.ppm
 *
 * The register table in gc2145_init.h is Espressif's (Apache-2.0); the rest is
 * ArgonOS.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/argon.h>
#include <argon/libc.h>

#include "gc2145_init.h"

AG_DRV("GC2145", "1.0", "argon");

/* This board's wiring (Freenove ESP32-S3-WROOM CAM, silkscreen 127294G). */
#define CAM_SCCB_BUS  0
#define CAM_ADDR      0x3c
#define CAM_W         640
#define CAM_H         480

#define P0_OUTPUT_FORMAT 0x84
#define P0_CROP_ENABLE   0x90
#define RESET_RELATED    0xfe
#define UXGA_W           1600
#define UXGA_H           1200

#define H8(v) (uint8_t)(((v) >> 8) & 0xff)
#define L8(v) (uint8_t)((v) & 0xff)

static int wr(uint8_t reg, uint8_t val)
{
    const uint8_t b[2] = { reg, val };
    return ag_i2c_write(CAM_SCCB_BUS, CAM_ADDR, b, 2, 100) == AG_OK ? 0 : -1;
}

static int rd(uint8_t reg)
{
    uint8_t v = 0;
    if (ag_i2c_wrrd(CAM_SCCB_BUS, CAM_ADDR, &reg, 1, &v, 1, 100) != AG_OK) {
        return -1;
    }
    return v;
}

/* Read-modify-write the low `mask` bits of a register. */
static int set_bits(uint8_t reg, uint8_t mask, uint8_t val)
{
    int cur = rd(reg);
    if (cur < 0) {
        return -1;
    }
    return wr(reg, (uint8_t)((cur & ~mask) | (val & mask)));
}

static int write_table(void)
{
    for (int i = 0; gc2145_default_init_regs[i][0] != REGLIST_TAIL; i++) {
        const uint16_t reg = gc2145_default_init_regs[i][0];
        const uint16_t val = gc2145_default_init_regs[i][1];
        if (reg == REG_DLY) {
            ag_delay(val);
        } else if (wr((uint8_t)reg, (uint8_t)val) != 0) {
            return -1;
        }
    }
    return 0;
}

/* Center-crop a CAM_W x CAM_H window out of the sensor's UXGA field. */
static void set_window(void)
{
    const uint16_t row_s = (UXGA_H - CAM_H) / 2;
    const uint16_t col_s = (UXGA_W - CAM_W) / 2;
    wr(RESET_RELATED, 0x00); /* page 0 */
    wr(P0_CROP_ENABLE, 0x01);
    wr(0x09, H8(row_s)); wr(0x0a, L8(row_s));
    wr(0x0b, H8(col_s)); wr(0x0c, L8(col_s));
    wr(0x0d, H8(CAM_H + 8)); wr(0x0e, L8(CAM_H + 8));
    wr(0x0f, H8(CAM_W + 8)); wr(0x10, L8(CAM_W + 8));
    wr(0x95, H8(CAM_H)); wr(0x96, L8(CAM_H));
    wr(0x97, H8(CAM_W)); wr(0x98, L8(CAM_W));

    /*
     * OPEN: DVP frame sync.  The sensor answers on SCCB and /dev/cam0 comes up,
     * but esp_cam_ctlr_dvp does not complete frames from this GC2145 - one frame
     * lands ~3 s after start, then none, which is the signature of a VSYNC the
     * controller reads in the wrong phase.  The generic driver hard-inverts
     * VSYNC (config_input_gpio inv=true) and exposes no polarity knob, and the
     * sensor's sync-mode register 0x86 (default 0x03; tried 0x01/0x0b) has not
     * squared it.  esp32-camera drove this sensor through its own ll_cam, not
     * esp_cam_ctlr_dvp, so this pairing needs more work - or a thin ll_cam-style
     * path in the port.  Left at the table default until then.
     */
}

static int sensor_init(void)
{
    /* The chip id proves XCLK is running and SCCB is wired. */
    const int hi = rd(0xf0), lo = rd(0xf1);
    if (hi < 0 || lo < 0) {
        return -AG_ENODEV;
    }
    if (((hi << 8) | lo) != 0x2145) {
        ag_printf("gc2145: unexpected id 0x%02x%02x\n", hi, lo);
        return -AG_ENODEV;
    }
    if (wr(RESET_RELATED, 0xe0) != 0) { /* soft reset */
        return -AG_EIO;
    }
    ag_delay(30);
    if (write_table() != 0) {
        return -AG_EIO;
    }
    ag_delay(20);
    wr(RESET_RELATED, 0x00);                 /* page 0 */
    set_bits(P0_OUTPUT_FORMAT, 0x1f, 6);     /* RGB565 */
    set_window();
    return 0;
}

/* /dev/cam0: a read returns one captured frame (raw RGB565). */
static int32_t cam_read(ag_device_t *dev, void *buf, size_t len, uint64_t off)
{
    (void)dev; (void)off;
    size_t got = 0;
    const uint8_t *fb = ag_api()->cam->capture(&got, 2000);
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
    return (uint64_t)CAM_W * CAM_H * 2u;
}

static const ag_dev_ops_t k_cam_ops = {
    .read = cam_read,
    .size = cam_size,
};

ag_err_t ag_driver_init(void)
{
    if (ag_api()->cam == NULL) {
        ag_printf("gc2145: this firmware has no camera transport\n");
        return -AG_ENOSYS;
    }
    if (!AG_HAS(ag_api()->dev, add)) {
        return -AG_ENOSYS;
    }

    /* The transport first: it drives XCLK, which the sensor needs before it
     * will answer on SCCB, and it sizes the frame buffer for our window. */
    const ag_cam_pins_t pins = {
        .xclk = 15, .pclk = 13, .vsync = 6, .href = 7,
        .data = { 11, 9, 8, 10, 12, 18, 17, 16 }, /* D0..D7 */
        .xclk_hz = 20000000,
    };
    ag_err_t err = ag_api()->cam->configure(&pins, AG_CAM_FMT_RGB565, CAM_W, CAM_H);
    if (err != AG_OK) {
        ag_printf("gc2145: transport did not start (%d)\n", (int)err);
        return err;
    }

    err = sensor_init();
    if (err != 0) {
        ag_printf("gc2145: sensor init failed (%d)\n", (int)err);
        ag_api()->cam->stop();
        return err;
    }

    const ag_dev_add_t desc = {
        .name = "cam0",
        .driver = "GC2145",
        .cls = AG_DEV_SENSOR,
        .flags = 0,
        .ops = &k_cam_ops,
        .priv = NULL,
    };
    err = ag_dev_add(&desc);
    if (err != AG_OK) {
        ag_api()->cam->stop();
        return err;
    }
    ag_printf("gc2145: /dev/cam0 up, %dx%d RGB565\n", CAM_W, CAM_H);
    return AG_OK;
}
