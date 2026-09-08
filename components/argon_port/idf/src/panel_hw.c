/*
 * ArgonOS port: ESP-IDF - the virtual RGB panel Espressif's QEMU provides.
 *
 * On real hardware there is no such device and open() says so; the driver for a
 * real panel (ST7789, ILI9341, an RGB LCD) goes here beside it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/panel.h>

/*
 * Needed for CONFIG_ARGON_PANEL_QEMU below, and its absence is why the
 * emulator's window was black for as long as it was.
 *
 * An undefined macro in #if is zero, not an error, so without this the whole
 * file compiled down to the "there is no panel here" half and open() answered
 * false without a word.  Everything downstream then behaved correctly and
 * uselessly: the display driver allocated a soft surface, applications drew
 * into it, and QEMU showed its own default 800x600 window with nothing in it.
 *
 * Every other file in this port includes this header.  This one was the
 * exception, and the only visible trace was a window that was the wrong size.
 */
#include "sdkconfig.h"

#if CONFIG_ARGON_PANEL_QEMU

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_qemu_rgb.h"
#include "esp_log.h"

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
    const esp_err_t       rc = esp_lcd_new_rgb_qemu(&cfg, &panel);
    if (rc != ESP_OK || panel == NULL) {
        /*
         * Said out loud, because the silent version of this cost an afternoon.
         * The display driver falls back to a soft surface nobody shows, and
         * from the outside that looks exactly like an application that draws
         * nothing: an SDL window at QEMU's own default size, staying black.
         *
         * ESP_ERR_NOT_SUPPORTED here means the emulator did not identify
         * itself - either this is real hardware, or QEMU was started without
         * the machine's `graphics=on` option, which is off by default and is
         * what instantiates the panel at all.
         */
        ESP_LOGW("panel",
                 "no QEMU RGB panel (%s); is the machine started with "
                 "graphics=on?",
                 esp_err_to_name(rc));
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
            /*
             * Still in flight after the wait gave up.  Writing the registers
             * now replaces the request QEMU has not finished with, and the
             * rows it had not reached are simply never copied - which is not
             * a dropped frame but a *torn* one, and it stays torn until
             * something happens to redraw those rows.
             *
             * Seen as the shell's first full paint arriving with the console
             * text still on the lower half of the screen: the whole frame was
             * kicked, a sixteen-pixel pointer square was kicked a moment
             * later, and the pointer's request truncated the frame's.  A
             * forced repaint cleared it, which is what says the pixels were in
             * the framebuffer all along and only the sending was cut short.
             *
             * So absorb what was pending instead of discarding it.  The cost
             * is a wider copy for one frame; the alternative is a picture that
             * is wrong with nothing on either side reporting it.
             */
            if ((rgb[RGB_MMIO_UPDATE_STATUS] & 1u) != 0) {
                if (s_y0 < y) {
                    h += y - s_y0;
                    y = s_y0;
                }
                if (s_y1 > y + h) {
                    h = s_y1 - y;
                }
                if (y + h > (int32_t)s_h) {
                    h = (int32_t)s_h - y;
                }
            }
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

#else /* !CONFIG_ARGON_PANEL_QEMU - a machine with no emulator behind it */

/*
 * No panel, and the contract has a word for that: open() answers false and the
 * system carries on with a serial console and a soft framebuffer nobody shows.
 * On a board the glass is reached the other way round - a loadable .SYS driving
 * the panel over the SPI primitive, the way c:\st7789.sys does on the
 * ESP32-C6-LCD-1.47 - and that path does not come through here at all.
 */
bool ag_port_panel_open(uint16_t w, uint16_t h, void **fb)
{
    (void)w;
    (void)h;
    (void)fb;
    return false;
}

void ag_port_panel_present(int32_t y, int32_t h)
{
    (void)y;
    (void)h;
}

#endif /* CONFIG_ARGON_PANEL_QEMU */
