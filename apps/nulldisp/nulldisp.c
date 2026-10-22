/*
 * ArgonOS - NULLDISP.SYS: a screen with no glass, for measuring what reaches
 * one.
 *
 *   drv load c:\drv\nulldisp.sys     (publishes /dev/lcd0)
 *
 * It says it is a 320x240 RGB565 panel with a text mode, exactly as a small
 * SPI panel driver does (see apps/st7789), and draws nothing: every rectangle
 * and every text row it is handed is counted and thrown away.  Once a second,
 * while anything is arriving, it says in the journal what came:
 *
 *   nulldisp: 12.4 frames/s (3 810 KB/s, 1 902 rects), 0 text rows, 0 carets
 *
 * "Frames" are pixels received over the pixels of the whole surface, so a
 * program that redraws only part of its picture gets the rate of whole
 * pictures that many pixels would be - the number that says what the wire
 * can carry, not what the program chose to send.
 *
 * WHY IT EXISTS: a board with a panel is the only thing that serves a
 * neighbour's display over the cluster link (net/noderdev.c), and the bench
 * has none for now.  Loaded on the board without one, it lets the other board
 * attach it (`link devs`) and draw to it, so the link's picture rate can be
 * measured with nothing in the way but the wire - no panel's own SPI, no
 * flash, no scaling.
 *
 *   [nulldisp]
 *   width  = 320    ; the surface it claims
 *   height = 240
 *   cols   = 53     ; its text mode, in cells
 *   rows   = 30
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_DRV("NULLDISP", "0.1", "argon");

static uint16_t s_w = 320, s_h = 240, s_cols = 53, s_rows = 30;

/*
 * Counted where they happen and read by the reporting task.  Single words
 * written by one side and read by the other; a torn read costs one line of a
 * report, which is not worth a lock inside a call that holds the registry.
 */
static volatile uint32_t s_px, s_bytes, s_rects, s_rows_in, s_carets;
static volatile uint32_t s_surf; /* the surface the last rectangle was in */

static ag_svc_t s_svc;

static ag_err_t nd_info(ag_handle_t h, ag_gfxinfo_t *out)
{
    (void)h;
    if (out == NULL) {
        return -AG_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    out->width = s_w;
    out->height = s_h;
    out->fmt = AG_PIX_RGB565;
    out->stride = (uint32_t)s_w * 2u;
    out->fb = NULL;
    return AG_OK;
}

static ag_err_t nd_text_info(ag_handle_t h, uint16_t *cols, uint16_t *rows)
{
    (void)h;
    if (cols != NULL) {
        *cols = s_cols;
    }
    if (rows != NULL) {
        *rows = s_rows;
    }
    return AG_OK;
}

/* These three hold the registry: no printing here (see argon/abi.h). */
static void nd_text_row(ag_handle_t h, uint16_t row, const ag_textcell_t *cells,
                        uint16_t count)
{
    (void)h;
    (void)row;
    (void)cells;
    (void)count;
    s_rows_in++;
}

static void nd_text_cursor(ag_handle_t h, uint16_t col, uint16_t row,
                           ag_textcell_t under, bool visible)
{
    (void)h;
    (void)col;
    (void)row;
    (void)under;
    (void)visible;
    s_carets++;
}

static void nd_blit_rect(ag_handle_t h, const ag_blit_t *b)
{
    (void)h;
    if (b == NULL || b->w == 0 || b->h == 0) {
        return;
    }
    const uint32_t px = (uint32_t)b->w * b->h;
    s_px += px;
    s_bytes += px * 2u;
    s_rects++;
    s_surf = (uint32_t)b->surf_w * b->surf_h;
}

static const ag_display_ops_t k_display_ops = {
    .size = sizeof(ag_display_ops_t),
    .info = nd_info,
    .text_info = nd_text_info,
    .text_row = nd_text_row,
    .text_cursor = nd_text_cursor,
    .blit_rect = nd_blit_rect,
};

static const ag_dev_ops_t k_dev_ops = {0};

/* Once a second, and only when something came: a quiet screen says nothing. */
static void report_task(void *arg)
{
    (void)arg;
    while (s_svc == NULL) {
        ag_api()->task->sleep_ms(1);
    }
    uint32_t px0 = s_px, b0 = s_bytes, r0 = s_rects, t0 = s_rows_in,
             c0 = s_carets;
    uint64_t at = ag_api()->time->us();
    while (!ag_api()->task->svc_stopping(s_svc)) {
        ag_api()->task->sleep_ms(1000);
        const uint64_t now = ag_api()->time->us();
        const uint32_t px = s_px, b = s_bytes, r = s_rects, t = s_rows_in,
                       c = s_carets;
        const uint32_t us = (uint32_t)(now - at);
        if (us > 0u && (px != px0 || t != t0 || c != c0)) {
            const uint32_t surf =
                (s_surf != 0u) ? s_surf : (uint32_t)s_w * s_h;
            /* tenths of a frame a second, and KB a second, over the time
             * that actually passed */
            const uint32_t f10 = (uint32_t)((uint64_t)(px - px0) * 10000000u /
                                            ((uint64_t)surf * us));
            const uint32_t kbs =
                (uint32_t)((uint64_t)(b - b0) * 1000000u / 1024u / us);
            ag_log(AG_LOG_INFO, "nulldisp",
                   "%u.%u frames/s (%u KB/s, %u rects), %u text rows, %u "
                   "carets",
                   (unsigned)(f10 / 10u), (unsigned)(f10 % 10u),
                   (unsigned)kbs, (unsigned)(r - r0), (unsigned)(t - t0),
                   (unsigned)(c - c0));
        }
        px0 = px;
        b0 = b;
        r0 = r;
        t0 = t;
        c0 = c;
        at = now;
    }
}

static uint16_t cfg16(const char *key, uint16_t dflt)
{
    if (ag_api()->cfg != NULL && AG_HAS(ag_api()->cfg, get_int)) {
        const int32_t v = ag_api()->cfg->get_int(key, dflt);
        if (v > 0 && v <= 4096) {
            return (uint16_t)v;
        }
    }
    return dflt;
}

ag_err_t ag_driver_init(void)
{
    if (!AG_HAS(ag_api()->dev, add)) {
        return -AG_ENOSYS;
    }
    s_w = cfg16("nulldisp.width", s_w);
    s_h = cfg16("nulldisp.height", s_h);
    s_cols = cfg16("nulldisp.cols", s_cols);
    s_rows = cfg16("nulldisp.rows", s_rows);

    const ag_dev_add_t desc = {
        .name = "lcd0",
        .driver = "NULLDISP",
        .cls = AG_DEV_DISPLAY,
        .ops = &k_dev_ops,
        .class_ops = &k_display_ops,
        .priv = NULL,
    };
    const ag_err_t err = ag_dev_add(&desc);
    if (err != AG_OK) {
        return err;
    }
    if (AG_HAS(ag_api()->task, svc_start)) {
        s_svc = ag_api()->task->svc_start(report_task, NULL, "nulldisp", 3072,
                                          0);
    }
    ag_printf("NULLDISP: %ux%u, %ux%u cells, draws nothing and counts\n",
              (unsigned)s_w, (unsigned)s_h, (unsigned)s_cols,
              (unsigned)s_rows);
    return AG_OK;
}
