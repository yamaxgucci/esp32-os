/*
 * Winamp-inspired bitmap skins (VGA + QVGA), painted into RGB565 buffers.
 * HostFS override: H:\amp\skin\{vga|qvga}\main.rgb565 etc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "amp_skin.h"

#include <string.h>

#include <argon/argon.h>

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

static void fill(uint16_t *p, uint16_t w, uint16_t h, uint16_t c)
{
    uint32_t n = (uint32_t)w * (uint32_t)h;
    uint32_t i;
    for (i = 0; i < n; i++) {
        p[i] = c;
    }
}

static void rect(uint16_t *p, uint16_t w, uint16_t h, int x, int y, int rw,
                 int rh, uint16_t c)
{
    int yy, xx;
    for (yy = y; yy < y + rh; yy++) {
        if (yy < 0 || yy >= (int)h) {
            continue;
        }
        for (xx = x; xx < x + rw; xx++) {
            if (xx < 0 || xx >= (int)w) {
                continue;
            }
            p[yy * (int)w + xx] = c;
        }
    }
}

static void hline(uint16_t *p, uint16_t w, uint16_t h, int x, int y, int len,
                  uint16_t c)
{
    rect(p, w, h, x, y, len, 1, c);
}

static void vline(uint16_t *p, uint16_t w, uint16_t h, int x, int y, int len,
                  uint16_t c)
{
    rect(p, w, h, x, y, 1, len, c);
}

static void bevel(uint16_t *p, uint16_t w, uint16_t h, int x, int y, int rw,
                  int rh, uint16_t hi, uint16_t lo)
{
    hline(p, w, h, x, y, rw, hi);
    vline(p, w, h, x, y, rh, hi);
    hline(p, w, h, x, y + rh - 1, rw, lo);
    vline(p, w, h, x + rw - 1, y, rh, lo);
}

static int sx(int v, int dw, int base)
{
    return (v * dw) / base;
}

static int sy(int v, int dh, int base)
{
    return (v * dh) / base;
}

static void paint_button(uint16_t *p, uint16_t w, uint16_t h, int x, int y,
                         int bw, int bh)
{
    uint16_t face = rgb565(0xB8, 0xB8, 0xC0);
    uint16_t hi = rgb565(0xE8, 0xE8, 0xF0);
    uint16_t lo = rgb565(0x58, 0x58, 0x68);
    rect(p, w, h, x, y, bw, bh, face);
    bevel(p, w, h, x, y, bw, bh, hi, lo);
}

/* Logical classic main 275x116 → stretched to w,h */
static void paint_main(uint16_t *p, uint16_t w, uint16_t h)
{
    const int BW = 275, BH = 116;
    uint16_t bg = rgb565(0x28, 0x34, 0x48);
    uint16_t title = rgb565(0x18, 0x20, 0x30);
    uint16_t lcd = rgb565(0x00, 0x20, 0x18);
    uint16_t accent = rgb565(0x00, 0xE0, 0x68);
    int i;

    fill(p, w, h, bg);
    rect(p, w, h, 0, 0, (int)w, sy(14, (int)h, BH), title);
    bevel(p, w, h, 0, 0, (int)w, (int)h, rgb565(0x60, 0x70, 0x88),
          rgb565(0x10, 0x14, 0x20));
    rect(p, w, h, sx(10, (int)w, BW), sy(18, (int)h, BH), sx(76, (int)w, BW),
         sy(44, (int)h, BH), lcd);
    bevel(p, w, h, sx(10, (int)w, BW), sy(18, (int)h, BH), sx(76, (int)w, BW),
          sy(44, (int)h, BH), rgb565(0x00, 0x40, 0x30), accent);
    rect(p, w, h, sx(110, (int)w, BW), sy(20, (int)h, BH), sx(150, (int)w, BW),
         sy(14, (int)h, BH), lcd);
    rect(p, w, h, sx(16, (int)w, BW), sy(70, (int)h, BH), sx(240, (int)w, BW),
         sy(8, (int)h, BH), rgb565(0x10, 0x14, 0x20));
    rect(p, w, h, sx(110, (int)w, BW), sy(40, (int)h, BH), sx(60, (int)w, BW),
         sy(8, (int)h, BH), rgb565(0x10, 0x14, 0x20));
    rect(p, w, h, sx(180, (int)w, BW), sy(40, (int)h, BH), sx(40, (int)w, BW),
         sy(8, (int)h, BH), rgb565(0x10, 0x14, 0x20));
    for (i = 0; i < 5; i++) {
        paint_button(p, w, h, sx(16 + i * 28, (int)w, BW), sy(88, (int)h, BH),
                     sx(24, (int)w, BW), sy(18, (int)h, BH));
    }
    paint_button(p, w, h, sx(160, (int)w, BW), sy(88, (int)h, BH),
                 sx(24, (int)w, BW), sy(18, (int)h, BH));
    paint_button(p, w, h, sx(200, (int)w, BW), sy(88, (int)h, BH),
                 sx(22, (int)w, BW), sy(12, (int)h, BH));
    paint_button(p, w, h, sx(226, (int)w, BW), sy(88, (int)h, BH),
                 sx(22, (int)w, BW), sy(12, (int)h, BH));
    paint_button(p, w, h, sx(200, (int)w, BW), sy(102, (int)h, BH),
                 sx(22, (int)w, BW), sy(12, (int)h, BH));
    paint_button(p, w, h, sx(226, (int)w, BW), sy(102, (int)h, BH),
                 sx(22, (int)w, BW), sy(12, (int)h, BH));
}

static void paint_eq(uint16_t *p, uint16_t w, uint16_t h)
{
    const int BW = 275, BH = 116;
    uint16_t bg = rgb565(0x28, 0x34, 0x48);
    uint16_t title = rgb565(0x18, 0x20, 0x30);
    uint16_t slot = rgb565(0x10, 0x14, 0x20);
    int i;

    fill(p, w, h, bg);
    rect(p, w, h, 0, 0, (int)w, sy(14, (int)h, BH), title);
    bevel(p, w, h, 0, 0, (int)w, (int)h, rgb565(0x60, 0x70, 0x88),
          rgb565(0x10, 0x14, 0x20));
    paint_button(p, w, h, sx(12, (int)w, BW), sy(20, (int)h, BH),
                 sx(28, (int)w, BW), sy(14, (int)h, BH));
    paint_button(p, w, h, sx(44, (int)w, BW), sy(20, (int)h, BH),
                 sx(28, (int)w, BW), sy(14, (int)h, BH));
    for (i = 0; i < 11; i++) {
        int x = sx(14 + i * 22, (int)w, BW);
        rect(p, w, h, x, sy(40, (int)h, BH), sx(10, (int)w, BW),
             sy(60, (int)h, BH), slot);
        bevel(p, w, h, x, sy(40, (int)h, BH), sx(10, (int)w, BW),
              sy(60, (int)h, BH), rgb565(0x40, 0x48, 0x58),
              rgb565(0x08, 0x0c, 0x10));
    }
}

static void paint_pl(uint16_t *p, uint16_t w, uint16_t h)
{
    uint16_t bg = rgb565(0x20, 0x28, 0x38);
    uint16_t title = rgb565(0x18, 0x20, 0x30);
    uint16_t list = rgb565(0x00, 0x18, 0x10);
    fill(p, w, h, bg);
    rect(p, w, h, 0, 0, (int)w, 14, title);
    bevel(p, w, h, 0, 0, (int)w, (int)h, rgb565(0x60, 0x70, 0x88),
          rgb565(0x10, 0x14, 0x20));
    rect(p, w, h, 6, 18, (int)w - 12, (int)h - 26, list);
}

static void set_ctrl(amp_skin_t *sk, amp_ctrl_t id, amp_panel_t pan, int x,
                     int y, int w, int h)
{
    sk->ctrl[id].x = (int16_t)x;
    sk->ctrl[id].y = (int16_t)y;
    sk->ctrl[id].w = (uint16_t)w;
    sk->ctrl[id].h = (uint16_t)h;
    sk->ctrl_panel[id] = pan;
}

static void map_main_ctrls(amp_skin_t *sk, int dw, int dh)
{
    const int BW = 275, BH = 116;
    set_ctrl(sk, AMP_CTRL_PREV, AMP_PANEL_MAIN, sx(16, dw, BW), sy(88, dh, BH),
             sx(24, dw, BW), sy(18, dh, BH));
    set_ctrl(sk, AMP_CTRL_PLAY, AMP_PANEL_MAIN, sx(44, dw, BW), sy(88, dh, BH),
             sx(24, dw, BW), sy(18, dh, BH));
    set_ctrl(sk, AMP_CTRL_PAUSE, AMP_PANEL_MAIN, sx(72, dw, BW), sy(88, dh, BH),
             sx(24, dw, BW), sy(18, dh, BH));
    set_ctrl(sk, AMP_CTRL_STOP, AMP_PANEL_MAIN, sx(100, dw, BW), sy(88, dh, BH),
             sx(24, dw, BW), sy(18, dh, BH));
    set_ctrl(sk, AMP_CTRL_NEXT, AMP_PANEL_MAIN, sx(128, dw, BW), sy(88, dh, BH),
             sx(24, dw, BW), sy(18, dh, BH));
    set_ctrl(sk, AMP_CTRL_EJECT, AMP_PANEL_MAIN, sx(160, dw, BW), sy(88, dh, BH),
             sx(24, dw, BW), sy(18, dh, BH));
    set_ctrl(sk, AMP_CTRL_VOL, AMP_PANEL_MAIN, sx(110, dw, BW), sy(40, dh, BH),
             sx(60, dw, BW), sy(8, dh, BH));
    set_ctrl(sk, AMP_CTRL_BAL, AMP_PANEL_MAIN, sx(180, dw, BW), sy(40, dh, BH),
             sx(40, dw, BW), sy(8, dh, BH));
    set_ctrl(sk, AMP_CTRL_SEEK, AMP_PANEL_MAIN, sx(16, dw, BW), sy(70, dh, BH),
             sx(240, dw, BW), sy(8, dh, BH));
    set_ctrl(sk, AMP_CTRL_EQ_TOGGLE, AMP_PANEL_MAIN, sx(200, dw, BW),
             sy(88, dh, BH), sx(22, dw, BW), sy(12, dh, BH));
    set_ctrl(sk, AMP_CTRL_PL_TOGGLE, AMP_PANEL_MAIN, sx(226, dw, BW),
             sy(88, dh, BH), sx(22, dw, BW), sy(12, dh, BH));
    set_ctrl(sk, AMP_CTRL_REPEAT, AMP_PANEL_MAIN, sx(200, dw, BW),
             sy(102, dh, BH), sx(22, dw, BW), sy(12, dh, BH));
    set_ctrl(sk, AMP_CTRL_SHUFFLE, AMP_PANEL_MAIN, sx(226, dw, BW),
             sy(102, dh, BH), sx(22, dw, BW), sy(12, dh, BH));
    sk->ticker.x = (int16_t)sx(110, dw, BW);
    sk->ticker.y = (int16_t)sy(20, dh, BH);
    sk->ticker.w = (uint16_t)sx(150, dw, BW);
    sk->ticker.h = (uint16_t)sy(14, dh, BH);
    sk->spectrum.x = (int16_t)sx(12, dw, BW);
    sk->spectrum.y = (int16_t)sy(22, dh, BH);
    sk->spectrum.w = (uint16_t)sx(72, dw, BW);
    sk->spectrum.h = (uint16_t)sy(36, dh, BH);
    sk->timebox.x = (int16_t)sx(12, dw, BW);
    sk->timebox.y = (int16_t)sy(48, dh, BH);
    sk->timebox.w = (uint16_t)sx(70, dw, BW);
    sk->timebox.h = (uint16_t)sy(12, dh, BH);
}

static void map_eq_ctrls(amp_skin_t *sk, int dw, int dh)
{
    const int BW = 275, BH = 116;
    int i;
    set_ctrl(sk, AMP_CTRL_EQ_ON, AMP_PANEL_EQ, sx(12, dw, BW), sy(20, dh, BH),
             sx(28, dw, BW), sy(14, dh, BH));
    set_ctrl(sk, AMP_CTRL_EQ_AUTO, AMP_PANEL_EQ, sx(44, dw, BW), sy(20, dh, BH),
             sx(28, dw, BW), sy(14, dh, BH));
    set_ctrl(sk, AMP_CTRL_EQ_PREAMP, AMP_PANEL_EQ, sx(14, dw, BW),
             sy(40, dh, BH), sx(10, dw, BW), sy(60, dh, BH));
    for (i = 0; i < 10; i++) {
        set_ctrl(sk, (amp_ctrl_t)(AMP_CTRL_EQ_BAND0 + i), AMP_PANEL_EQ,
                 sx(14 + (i + 1) * 22, dw, BW), sy(40, dh, BH), sx(10, dw, BW),
                 sy(60, dh, BH));
    }
}

static void layout_vga(amp_skin_t *sk)
{
    /* Fits 640x400: main + eq + playlist stacked */
    sk->qvga = 0;
    sk->panel[AMP_PANEL_MAIN].w = 550;
    sk->panel[AMP_PANEL_MAIN].h = 148;
    sk->panel[AMP_PANEL_MAIN].pos.x = 45;
    sk->panel[AMP_PANEL_MAIN].pos.y = 8;
    sk->panel[AMP_PANEL_MAIN].pos.w = 550;
    sk->panel[AMP_PANEL_MAIN].pos.h = 148;

    sk->panel[AMP_PANEL_EQ].w = 550;
    sk->panel[AMP_PANEL_EQ].h = 100;
    sk->panel[AMP_PANEL_EQ].pos.x = 45;
    sk->panel[AMP_PANEL_EQ].pos.y = 164;
    sk->panel[AMP_PANEL_EQ].pos.w = 550;
    sk->panel[AMP_PANEL_EQ].pos.h = 100;

    sk->panel[AMP_PANEL_PL].w = 550;
    sk->panel[AMP_PANEL_PL].h = 120;
    sk->panel[AMP_PANEL_PL].pos.x = 45;
    sk->panel[AMP_PANEL_PL].pos.y = 272;
    sk->panel[AMP_PANEL_PL].pos.w = 550;
    sk->panel[AMP_PANEL_PL].pos.h = 120;

    map_main_ctrls(sk, 550, 148);
    map_eq_ctrls(sk, 550, 100);
    set_ctrl(sk, AMP_CTRL_PL_LIST, AMP_PANEL_PL, 6, 18, 550 - 12, 120 - 26);
    sk->pl_row_h = 14;
}

static void layout_qvga(amp_skin_t *sk, uint16_t fb_w, uint16_t fb_h)
{
    uint16_t mw = 275, mh = 116;
    sk->qvga = 1;
    sk->panel[AMP_PANEL_MAIN].w = mw;
    sk->panel[AMP_PANEL_MAIN].h = mh;
    sk->panel[AMP_PANEL_MAIN].pos.x = (int16_t)((fb_w - mw) / 2);
    sk->panel[AMP_PANEL_MAIN].pos.y = (int16_t)((fb_h - mh) / 2);
    sk->panel[AMP_PANEL_MAIN].pos.w = mw;
    sk->panel[AMP_PANEL_MAIN].pos.h = mh;

    sk->panel[AMP_PANEL_EQ].w = mw;
    sk->panel[AMP_PANEL_EQ].h = mh;
    sk->panel[AMP_PANEL_EQ].pos = sk->panel[AMP_PANEL_MAIN].pos;

    sk->panel[AMP_PANEL_PL].w = (uint16_t)(fb_w >= 8 ? fb_w - 8 : fb_w);
    sk->panel[AMP_PANEL_PL].h = (uint16_t)(fb_h >= 8 ? fb_h - 8 : fb_h);
    sk->panel[AMP_PANEL_PL].pos.x = 4;
    sk->panel[AMP_PANEL_PL].pos.y = 4;
    sk->panel[AMP_PANEL_PL].pos.w = sk->panel[AMP_PANEL_PL].w;
    sk->panel[AMP_PANEL_PL].pos.h = sk->panel[AMP_PANEL_PL].h;

    map_main_ctrls(sk, (int)mw, (int)mh);
    map_eq_ctrls(sk, (int)mw, (int)mh);
    set_ctrl(sk, AMP_CTRL_PL_LIST, AMP_PANEL_PL, 6, 18,
             (int)sk->panel[AMP_PANEL_PL].w - 12,
             (int)sk->panel[AMP_PANEL_PL].h - 26);
    sk->pl_row_h = 12;
}

static int try_load_rgb565(const char *path, uint16_t *dst, uint32_t bytes)
{
    ag_handle_t h;
    int32_t n;
    h = ag_open(path, AG_O_RDONLY);
    if (h < 0) {
        return -1;
    }
    n = ag_read(h, dst, bytes);
    (void)ag_close(h);
    return (n == (int32_t)bytes) ? 0 : -1;
}

static int alloc_paint(amp_skin_t *sk)
{
    int i;

    for (i = 0; i < AMP_PANEL_N; i++) {
        uint32_t bytes =
            (uint32_t)sk->panel[i].w * (uint32_t)sk->panel[i].h * 2u;
        sk->panel[i].pixels = (uint16_t *)ag_malloc(bytes);
        if (sk->panel[i].pixels == NULL) {
            return -1;
        }
    }
    paint_main(sk->panel[AMP_PANEL_MAIN].pixels, sk->panel[AMP_PANEL_MAIN].w,
               sk->panel[AMP_PANEL_MAIN].h);
    paint_eq(sk->panel[AMP_PANEL_EQ].pixels, sk->panel[AMP_PANEL_EQ].w,
             sk->panel[AMP_PANEL_EQ].h);
    paint_pl(sk->panel[AMP_PANEL_PL].pixels, sk->panel[AMP_PANEL_PL].w,
             sk->panel[AMP_PANEL_PL].h);

    if (sk->qvga) {
        (void)try_load_rgb565("h:\\amp\\skin\\qvga\\main.rgb565",
                              sk->panel[AMP_PANEL_MAIN].pixels,
                              (uint32_t)sk->panel[AMP_PANEL_MAIN].w *
                                  sk->panel[AMP_PANEL_MAIN].h * 2u);
        (void)try_load_rgb565("h:\\amp\\skin\\qvga\\eq.rgb565",
                              sk->panel[AMP_PANEL_EQ].pixels,
                              (uint32_t)sk->panel[AMP_PANEL_EQ].w *
                                  sk->panel[AMP_PANEL_EQ].h * 2u);
        (void)try_load_rgb565("h:\\amp\\skin\\qvga\\pl.rgb565",
                              sk->panel[AMP_PANEL_PL].pixels,
                              (uint32_t)sk->panel[AMP_PANEL_PL].w *
                                  sk->panel[AMP_PANEL_PL].h * 2u);
    } else {
        (void)try_load_rgb565("h:\\amp\\skin\\vga\\main.rgb565",
                              sk->panel[AMP_PANEL_MAIN].pixels,
                              (uint32_t)sk->panel[AMP_PANEL_MAIN].w *
                                  sk->panel[AMP_PANEL_MAIN].h * 2u);
        (void)try_load_rgb565("h:\\amp\\skin\\vga\\eq.rgb565",
                              sk->panel[AMP_PANEL_EQ].pixels,
                              (uint32_t)sk->panel[AMP_PANEL_EQ].w *
                                  sk->panel[AMP_PANEL_EQ].h * 2u);
        (void)try_load_rgb565("h:\\amp\\skin\\vga\\pl.rgb565",
                              sk->panel[AMP_PANEL_PL].pixels,
                              (uint32_t)sk->panel[AMP_PANEL_PL].w *
                                  sk->panel[AMP_PANEL_PL].h * 2u);
    }
    return 0;
}

int amp_skin_load(amp_skin_t *sk, uint16_t fb_w, uint16_t fb_h)
{
    if (sk == NULL) {
        return -1;
    }
    memset(sk, 0, sizeof(*sk));
    if (fb_w < 400) {
        layout_qvga(sk, fb_w, fb_h);
    } else {
        layout_vga(sk);
    }
    return alloc_paint(sk);
}

void amp_skin_free(amp_skin_t *sk)
{
    int i;
    if (sk == NULL) {
        return;
    }
    for (i = 0; i < AMP_PANEL_N; i++) {
        if (sk->panel[i].pixels) {
            ag_free(sk->panel[i].pixels);
            sk->panel[i].pixels = NULL;
        }
    }
}

amp_ctrl_t amp_skin_hit(const amp_skin_t *sk, amp_panel_t focus, int x, int y,
                        amp_panel_t *out_panel)
{
    int c;
    if (sk == NULL) {
        return AMP_CTRL_NONE;
    }
    for (c = 1; c < AMP_CTRL_N; c++) {
        amp_panel_t pan = sk->ctrl_panel[c];
        const amp_skin_panel_t *pp;
        int lx, ly;
        if (sk->ctrl[c].w == 0) {
            continue;
        }
        if (sk->qvga && pan != focus) {
            continue;
        }
        pp = &sk->panel[pan];
        if (x < pp->pos.x || y < pp->pos.y ||
            x >= pp->pos.x + (int)pp->pos.w ||
            y >= pp->pos.y + (int)pp->pos.h) {
            continue;
        }
        lx = x - pp->pos.x;
        ly = y - pp->pos.y;
        if (lx >= sk->ctrl[c].x && ly >= sk->ctrl[c].y &&
            lx < sk->ctrl[c].x + (int)sk->ctrl[c].w &&
            ly < sk->ctrl[c].y + (int)sk->ctrl[c].h) {
            if (out_panel) {
                *out_panel = pan;
            }
            return (amp_ctrl_t)c;
        }
    }
    return AMP_CTRL_NONE;
}
