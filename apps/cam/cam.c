/*
 * ArgonOS - CAM.AXE: take a picture through /dev/cam0 and save it as a PPM.
 *
 * The sensor and the transport are elsewhere (GC2145.SYS + the firmware's
 * api->cam); this only reads a frame and writes a file.  PPM, not JPEG, on
 * purpose: no encoder belongs in the firmware, and a P6 is a three-line header
 * over raw pixels, which any viewer opens and the host converts trivially.
 *
 *   drv load a:\gc2145.sys
 *   run a:\cam.axe a:\shot.ppm
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_APP("CAM", "1.0", "argon", 0);

#define CAM_W 640
#define CAM_H 480

int ag_main(int argc, char **argv)
{
    const char *path = (argc >= 2) ? argv[1] : "a:\shot.ppm";

    const size_t npix = (size_t)CAM_W * CAM_H;
    uint8_t *rgb565 = (uint8_t *)ag_malloc(npix * 2u);
    if (rgb565 == NULL) {
        ag_printf("cam: no memory for the frame\n");
        return 1;
    }

    const ag_handle_t d = ag_dev_open("cam0");
    if (d < 0) {
        ag_printf("cam: no /dev/cam0 (load gc2145.sys first)\n");
        ag_free(rgb565);
        return 1;
    }

    /* First frames after power-up are the sensor settling exposure; drop a few. */
    int32_t got = 0;
    for (int i = 0; i < 4; i++) {
        got = ag_dev_read(d, rgb565, npix * 2u);
    }
    ag_dev_close(d);
    if (got < (int32_t)(npix * 2u)) {
        ag_printf("cam: short frame (%d)\n", (int)got);
        ag_free(rgb565);
        return 1;
    }

    const ag_handle_t f =
        ag_open(path, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (f < 0) {
        ag_printf("cam: cannot write %s (%d)\n", path, (int)f);
        ag_free(rgb565);
        return 1;
    }

    /* Fixed dimensions, so the P6 header is a constant - no formatting needed. */
    static const char hdr[] = "P6\n640 480\n255\n";
    ag_write(f, hdr, sizeof(hdr) - 1);

    /*
     * RGB565 (little-endian, two bytes per pixel) to PPM's RGB888.  A row
     * buffer at a time so the whole 900 KB output is not held twice.
     */
    uint8_t row[CAM_W * 3];
    for (int y = 0; y < CAM_H; y++) {
        const uint8_t *src = rgb565 + (size_t)y * CAM_W * 2u;
        for (int x = 0; x < CAM_W; x++) {
            const uint16_t p = (uint16_t)(src[x * 2] | (src[x * 2 + 1] << 8));
            const uint8_t r5 = (p >> 11) & 0x1f;
            const uint8_t g6 = (p >> 5) & 0x3f;
            const uint8_t b5 = p & 0x1f;
            row[x * 3 + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
            row[x * 3 + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
            row[x * 3 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
        }
        ag_write(f, row, sizeof(row));
    }
    ag_close(f);
    ag_free(rgb565);
    ag_printf("cam: %s, %dx%d PPM\n", path, CAM_W, CAM_H);
    return 0;
}
