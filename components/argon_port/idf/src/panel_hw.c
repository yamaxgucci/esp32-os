/*
 * ArgonOS port: ESP-IDF - the virtual RGB panel Espressif's QEMU provides.
 *
 * On real hardware there is no such device and open() says so; the driver for a
 * real panel (ST7789, ILI9341, an RGB LCD) goes here beside it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/panel.h>

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_qemu_rgb.h"

#include <argon/port/task.h>
#include <argon/port/time.h>

/*
 * QEMU RGB MMIO (see espressif/qemu hw/display/esp_rgb.c).  Do NOT call
 * esp_lcd_rgb_qemu_refresh: it busy-waits on UPDATE_STATUS.ENA, and that bit
 * only clears from QEMU's display thread - a guest spin with no yield deadlocks
 * the emulator and the guest together.  Do not wait on every kick either: that
 * capped gfx at ~32 fps.  Wait only when the new region does not cover the one
 * still in flight.
 */
enum {
    RGB_MMIO_UPDATE_FROM = 0x08u / 4u,
    RGB_MMIO_UPDATE_TO = 0x0cu / 4u,
    RGB_MMIO_UPDATE_CONTENT = 0x10u / 4u,
    RGB_MMIO_UPDATE_STATUS = 0x14u / 4u,
};

#define RGB_MMIO_BASE 0x21000000u

static esp_lcd_panel_handle_t s_panel;
static uint8_t               *s_fb;
static uint16_t               s_w;
static uint16_t               s_h;
static size_t                 s_stride;
static int32_t                s_y0; /* last kicked row range, exclusive y1 */
static int32_t                s_y1;

bool ag_port_panel_open(uint16_t w, uint16_t h, void **fb)
{
    if (fb == NULL || w == 0 || h == 0) {
        return false;
    }
    if (s_panel != NULL) {
        *fb = s_fb;
        return true;
    }

    const esp_lcd_rgb_qemu_config_t cfg = {
        .width = w,
        .height = h,
        .bpp = RGB_QEMU_BPP_16,
    };

    esp_lcd_panel_handle_t panel = NULL;
    if (esp_lcd_new_rgb_qemu(&cfg, &panel) != ESP_OK || panel == NULL) {
        return false;
    }
    (void)esp_lcd_panel_reset(panel);
    (void)esp_lcd_panel_init(panel);

    void *qfb = NULL;
    if (esp_lcd_rgb_qemu_get_frame_buffer(panel, &qfb) != ESP_OK ||
        qfb == NULL) {
        (void)esp_lcd_panel_del(panel);
        return false;
    }

    s_panel = panel;
    s_fb = (uint8_t *)qfb;
    s_w = w;
    s_h = h;
    s_stride = (size_t)w * 2u;
    *fb = qfb;
    return true;
}

static void wait_idle(void)
{
    volatile uint32_t *const rgb = (volatile uint32_t *)RGB_MMIO_BASE;
    const int64_t            deadline = ag_port_us() + 50000; /* 50 ms */
    int                      yields = 0;

    while ((rgb[RGB_MMIO_UPDATE_STATUS] & 1u) != 0) {
        if (ag_port_us() >= deadline) {
            break;
        }
        if (yields < 8) {
            ag_port_task_yield();
            yields++;
        } else {
            ag_port_task_delay(1);
        }
    }
}

void ag_port_panel_present(int32_t y, int32_t h)
{
    if (s_panel == NULL || s_fb == NULL) {
        return;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (y + h > (int32_t)s_h) {
        h = (int32_t)s_h - y;
    }
    if (h <= 0) {
        return;
    }

    volatile uint32_t *const rgb = (volatile uint32_t *)RGB_MMIO_BASE;
    /*
     * Replacing an in-flight *larger* blit with a smaller one leaves stale rows
     * in the window (a dirty benchmark on top of the loading banner).  A kick
     * that is the same size or larger may overwrite: dropping a full frame is
     * how ~130 fps full redraw stays possible.
     */
    if ((rgb[RGB_MMIO_UPDATE_STATUS] & 1u) != 0) {
        const bool covers = (y <= s_y0) && ((y + h) >= s_y1);
        if (!covers) {
            wait_idle();
        }
    }

    /* X in the high half, Y in the low one, per rgb_qemu_dev_t.  Ends are
     * exclusive. */
    rgb[RGB_MMIO_UPDATE_FROM] = (uint32_t)y;
    rgb[RGB_MMIO_UPDATE_TO] = ((uint32_t)s_w << 16) | (uint32_t)(y + h);
    rgb[RGB_MMIO_UPDATE_CONTENT] =
        (uint32_t)(uintptr_t)(s_fb + (size_t)y * s_stride);
    rgb[RGB_MMIO_UPDATE_STATUS] = 1u; /* ENA; QEMU clears asynchronously */
    s_y0 = y;
    s_y1 = y + h;
    ag_port_task_yield();
}
