/*
 * ArgonOS port: ESP-IDF - the DVP camera transport, on esp_cam_ctlr_dvp.
 *
 * Contract and reasoning in argon/port/camera.h and abi.h (0.41).  This is only
 * the LCD_CAM + DMA path: it clocks the sensor (XCLK), captures a frame over the
 * parallel bus into a PSRAM buffer, and hands it up.  It does not touch the
 * sensor's registers - that is the loadable driver's job over io->i2c (SCCB).
 *
 * esp_cam_ctlr_dvp is ESP-IDF's own driver (not the esp32-camera component with
 * its sensor zoo), so what the image carries is small and generic.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/camera.h>

#include "sdkconfig.h"

#if defined(CONFIG_ARGON_ENABLE_CAMERA) && CONFIG_ARGON_ENABLE_CAMERA

#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_dvp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "soc/gpio_sig_map.h"

#define TAG "camera_hw"

static esp_cam_ctlr_handle_t s_ctlr;
static uint8_t              *s_fb;      /* the one frame buffer, in PSRAM */
static size_t               s_fb_len;
static SemaphoreHandle_t    s_done;    /* given per completed frame, from ISR */

/*
 * The driver asks for a buffer to fill; hand it the one buffer, every time.
 * One in flight is enough for a still: capture() waits for it and reads it
 * before asking for the next.
 */
static volatile uint32_t s_get, s_fin, s_last_size;
static bool on_get_new_trans(esp_cam_ctlr_handle_t h, esp_cam_ctlr_trans_t *trans,
                             void *user_data)
{
    (void)h;
    (void)user_data;
    s_get++;
    trans->buffer = s_fb;
    trans->buflen = s_fb_len;
    return false;
}

/*
 * A frame has finished into s_fb.  The DVP driver delivers frames this way, not
 * through esp_cam_ctlr_receive (which it does not implement), so this is where a
 * capture completes: signal the waiter.  ISR context, so give from ISR and
 * report whether a higher-priority task should run.
 */
static bool on_trans_finished(esp_cam_ctlr_handle_t h, esp_cam_ctlr_trans_t *trans,
                              void *user_data)
{
    (void)h;
    (void)trans;
    (void)user_data;
    s_fin++;
    s_last_size = (uint32_t)trans->received_size;
    BaseType_t woken = pdFALSE;
    if (s_done != NULL) {
        xSemaphoreGiveFromISR(s_done, &woken);
    }
    return woken == pdTRUE;
}

bool ag_port_cam_present(void) { return true; }

ag_err_t ag_port_cam_configure(const ag_cam_pins_t *pins, ag_cam_fmt_t fmt,
                               uint32_t width, uint32_t height)
{
    if (pins == NULL || fmt != AG_CAM_FMT_RGB565 || width == 0 || height == 0) {
        return -AG_EINVAL;
    }
    if (s_ctlr != NULL) {
        return AG_OK; /* already up */
    }

    s_fb_len = (size_t)width * height * 2u; /* RGB565: 2 bytes/pixel */

    /* Cache-line aligned so the DMA and the CPU see the same bytes; 64 is the
     * S3's line, and over-aligning is harmless. */
    s_fb = heap_caps_aligned_calloc(64, 1, s_fb_len,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_fb == NULL) {
        ESP_LOGW(TAG, "no %u KB PSRAM for the frame buffer",
                 (unsigned)(s_fb_len / 1024u));
        return -AG_ENOMEM;
    }

    if (s_done == NULL) {
        s_done = xSemaphoreCreateBinary();
        if (s_done == NULL) {
            heap_caps_free(s_fb);
            s_fb = NULL;
            return -AG_ENOMEM;
        }
    }

    esp_cam_ctlr_dvp_pin_config_t pin_cfg = {
        .data_width = CAM_CTLR_DATA_WIDTH_8,
        .vsync_io = pins->vsync,
        .de_io = pins->href,
        .pclk_io = pins->pclk,
        .xclk_io = pins->xclk,
    };
    for (int i = 0; i < 8; i++) {
        pin_cfg.data_io[i] = pins->data[i];
    }

    esp_cam_ctlr_dvp_config_t cfg = {
        .ctlr_id = 0,
        .clk_src = CAM_CLK_SRC_DEFAULT,
        .h_res = width,
        .v_res = height,
        .input_data_color_type = CAM_CTLR_COLOR_RGB565,
        .cam_data_width = 8,
        .dma_burst_size = 64,
        .bk_buffer_dis = true,       /* we own the buffer, handed via callback */
        .xclk_freq = pins->xclk_hz ? pins->xclk_hz : 20000000u,
        .pin = &pin_cfg,
    };

    esp_err_t err = esp_cam_new_dvp_ctlr(&cfg, &s_ctlr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dvp ctlr: %s", esp_err_to_name(err));
        heap_caps_free(s_fb);
        s_fb = NULL;
        return -AG_EIO;
    }

    /*
     * Undo the generic driver's hard VSYNC inversion.  esp_cam_ctlr_dvp binds
     * the VSYNC pin with inv=true; this GC2145 (like esp32-camera's own ll_cam)
     * wants it uninverted, and with the inversion the controller reads frame
     * boundaries in the wrong phase and never completes a frame.  Re-bind the
     * input signal with inv=false after the driver has set the pin up.
     */
#if defined(CAM_V_SYNC_IDX)
    esp_rom_gpio_connect_in_signal((uint32_t)pins->vsync, CAM_V_SYNC_IDX, false);
#endif

    const esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = on_get_new_trans,
        .on_trans_finished = on_trans_finished,
    };
    (void)esp_cam_ctlr_register_event_callbacks(s_ctlr, &cbs, NULL);

    /*
     * Left created but not enabled.  The controller drives XCLK from creation,
     * which the sensor needs for SCCB, but the DMA only cleanly delivers its
     * first frame after an enable; a second frame never completes on this part.
     * So capture() does a full enable/start/stop/disable cycle each time - the
     * reliable first frame, every time.
     */

    ESP_LOGI(TAG, "DVP up: %ux%u RGB565, XCLK %u Hz on pin %d",
             (unsigned)width, (unsigned)height,
             (unsigned)cfg.xclk_freq, (int)pins->xclk);
    return AG_OK;
}

const uint8_t *ag_port_cam_capture(size_t *len, uint32_t timeout_ms)
{
    if (s_ctlr == NULL || len == NULL) {
        return NULL;
    }
    /* Full cycle for one clean frame: enable, start, wait, stop, disable. */
    (void)xSemaphoreTake(s_done, 0);
    if (esp_cam_ctlr_enable(s_ctlr) != ESP_OK) {
        ESP_LOGW(TAG, "dvp enable failed");
        return NULL;
    }
    if (esp_cam_ctlr_start(s_ctlr) != ESP_OK) {
        ESP_LOGW(TAG, "dvp start failed");
        (void)esp_cam_ctlr_disable(s_ctlr);
        return NULL;
    }
    const BaseType_t ok = xSemaphoreTake(s_done, pdMS_TO_TICKS(timeout_ms));
    (void)esp_cam_ctlr_stop(s_ctlr);
    (void)esp_cam_ctlr_disable(s_ctlr);
    if (ok != pdTRUE) {
        ESP_LOGW(TAG, "no frame in %u ms (get=%u fin=%u last=%u)",
                 (unsigned)timeout_ms, (unsigned)s_get, (unsigned)s_fin,
                 (unsigned)s_last_size);
        return NULL;
    }
    *len = s_fb_len;
    return s_fb;
}

void ag_port_cam_stop(void)
{
    if (s_ctlr != NULL) {
        (void)esp_cam_ctlr_stop(s_ctlr);
        (void)esp_cam_ctlr_disable(s_ctlr);
        (void)esp_cam_ctlr_del(s_ctlr);
        s_ctlr = NULL;
    }
    if (s_fb != NULL) {
        heap_caps_free(s_fb);
        s_fb = NULL;
    }
    if (s_done != NULL) {
        vSemaphoreDelete(s_done);
        s_done = NULL;
    }
}

#else /* no camera in this build */

bool           ag_port_cam_present(void) { return false; }
ag_err_t       ag_port_cam_configure(const ag_cam_pins_t *pins, ag_cam_fmt_t fmt,
                                     uint32_t width, uint32_t height)
{
    (void)pins; (void)fmt; (void)width; (void)height;
    return -AG_ENOSYS;
}
const uint8_t *ag_port_cam_capture(size_t *len, uint32_t timeout_ms)
{
    (void)len; (void)timeout_ms;
    return NULL;
}
void           ag_port_cam_stop(void) {}

#endif /* CONFIG_ARGON_ENABLE_CAMERA */
