/*
 * ArgonOS port: ESP-IDF - the camera brought into the image whole.
 *
 * The sibling of camera_hw.c, and its opposite trade.  Where that file is a
 * thin, sensor-blind DVP transport that a loadable .SYS drives through the ABI,
 * this one is Espressif's esp32-camera component compiled in: transport and the
 * whole zoo of sensor drivers, one esp_camera_init() and a frame comes out.  A
 * fixed product that always has the same sensor and wants it the moment it
 * powers up pays the image size once and never loads a module.
 *
 * The two share the LCD_CAM peripheral and so cannot both exist; Kconfig makes
 * CONFIG_ARGON_CAMERA_BUILTIN and CONFIG_ARGON_ENABLE_CAMERA mutually
 * exclusive, and the espressif/esp32-camera dependency is fetched only when
 * this one is set (main/idf_component.yml).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/camera.h>

#include "sdkconfig.h"

#if defined(CONFIG_ARGON_CAMERA_BUILTIN) && CONFIG_ARGON_CAMERA_BUILTIN

#include "esp_camera.h"
#include "esp_log.h"

#define TAG "cam_builtin"

static bool          s_up;
static camera_fb_t  *s_held;   /* the frame handed out last, returned on the next */
static size_t        s_len;    /* bytes in a frame, RGB565: w*h*2 */

ag_err_t ag_port_cam_builtin_start(const ag_cam_builtin_pins_t *pins,
                                   uint32_t width, uint32_t height)
{
    if (pins == NULL || width == 0 || height == 0) {
        return -AG_EINVAL;
    }
    if (s_up) {
        return AG_OK; /* already brought up */
    }

    /*
     * VGA is what this path is sized for; esp32-camera picks the frame size
     * from an enum, not a width/height, so map the one resolution the rest of
     * the system asks for and reject anything else rather than guess.
     */
    framesize_t fs;
    if (width == 640 && height == 480) {
        fs = FRAMESIZE_VGA;
    } else if (width == 320 && height == 240) {
        fs = FRAMESIZE_QVGA;
    } else {
        ESP_LOGW(TAG, "unsupported size %ux%u", (unsigned)width, (unsigned)height);
        return -AG_EINVAL;
    }

    camera_config_t cfg = {
        .pin_pwdn = pins->pwdn,
        .pin_reset = pins->reset,
        .pin_xclk = pins->xclk,
        .pin_sccb_sda = pins->sccb_sda,
        .pin_sccb_scl = pins->sccb_scl,
        .pin_d0 = pins->data[0],
        .pin_d1 = pins->data[1],
        .pin_d2 = pins->data[2],
        .pin_d3 = pins->data[3],
        .pin_d4 = pins->data[4],
        .pin_d5 = pins->data[5],
        .pin_d6 = pins->data[6],
        .pin_d7 = pins->data[7],
        .pin_vsync = pins->vsync,
        .pin_href = pins->href,
        .pin_pclk = pins->pclk,
        .xclk_freq_hz = (int)(pins->xclk_hz ? pins->xclk_hz : 20000000u),
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size = fs,
        .jpeg_quality = 12,             /* unused for RGB565, but a valid value */
        .fb_count = 2,                  /* two buffers so GRAB_LATEST has a spare */
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
        .sccb_i2c_port = -1,            /* let it install its own I2C on those pins */
    };

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_camera_init: %s", esp_err_to_name(err));
        return -AG_EIO;
    }

    s_len = (size_t)width * height * 2u;
    s_up = true;
    ESP_LOGI(TAG, "built-in camera up: %ux%u RGB565, XCLK %u Hz",
             (unsigned)width, (unsigned)height, (unsigned)cfg.xclk_freq_hz);
    return AG_OK;
}

const uint8_t *ag_port_cam_builtin_capture(size_t *len, uint32_t timeout_ms)
{
    (void)timeout_ms; /* esp_camera_fb_get blocks for the next frame */
    if (!s_up || len == NULL) {
        return NULL;
    }
    /* Return the previous frame before asking for the next: the contract is
     * that the pointer is valid until the following capture. */
    if (s_held != NULL) {
        esp_camera_fb_return(s_held);
        s_held = NULL;
    }
    s_held = esp_camera_fb_get();
    if (s_held == NULL) {
        ESP_LOGW(TAG, "no frame");
        return NULL;
    }
    *len = s_held->len;
    return s_held->buf;
}

void ag_port_cam_builtin_stop(void)
{
    if (s_held != NULL) {
        esp_camera_fb_return(s_held);
        s_held = NULL;
    }
    if (s_up) {
        (void)esp_camera_deinit();
        s_up = false;
    }
}

#endif /* CONFIG_ARGON_CAMERA_BUILTIN */
