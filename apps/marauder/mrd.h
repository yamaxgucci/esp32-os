/*
 * MARAUDER - a port of ESP32-Marauder to ArgonOS as a loadable .AXE.
 *
 * Shared declarations: the display/band renderer, the colour palette, the menu
 * node type, and the screen ids.  This is a loadable application, not a kernel
 * module - Maxim's requirement - so everything here goes through the public ABI
 * (argon/argon.h) and nothing reaches into the kernel.
 *
 * Two facts about the target board (ESP32-CYD) shape the whole design, exactly
 * as they do for apps/midipad:
 *
 *   1. There is no room for the system framebuffer with the radio up.  320x240
 *      in RGB565 is 150 KB and this board has 320 KB of SRAM total, ~15 KB free
 *      once Wi-Fi is on.  So we never take the system surface: we render the UI
 *      into a small reusable band and hand each band to the panel with
 *      gfx->present (ABI 0.31).  When a system surface does exist (QEMU -Gfx),
 *      the same band is copied in with gfx->blit instead, so the app renders and
 *      can be captured with gfxdump without a board.
 *
 *   2. Touch arrives in console cells, not pixels (the XPT2046 driver reports
 *      cells, like a terminal mouse).  So hit tests map a cell coordinate onto
 *      the logical surface by proportion.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MRD_H
#define MRD_H

#include <argon/argon.h>
#include <argon/libc.h> /* weak memcpy/memset/memmove/strlen for -nostdlib */

/* ------------------------------------------------------------------------ */
/* Geometry                                                                 */
/* ------------------------------------------------------------------------ */

/*
 * The logical UI is Marauder's native 320x240.  On the board the panel is
 * exactly that; on a larger QEMU surface we render into the top-left 320x240 and
 * leave the rest, which keeps the band buffer bounded and the layout identical
 * to the hardware.
 */
#define MRD_UI_W 320
#define MRD_UI_H 240

/* Rows pushed per present/blit.  Kept small on purpose: 2 rows * 320 * 2 bytes
 * = 1.25 KB of static bss, and on this no-PSRAM board every kilobyte the image
 * does not hold is a kilobyte the radio can (Wi-Fi bring-up needs ~36 KB
 * contiguous, and the margin is thin).  Two rows rather than four halves that
 * bss for the price of twice as many present() calls, which a menu never
 * notices.  A whole 320x240 surface would be 150 KB. */
#define MRD_BAND_H 1

/*
 * The 8x16 font, so a glyph is 16 px tall at scale 1 and 32 at scale 2.  Text is
 * vertically centred with the *scaled* height (mrd_text_h), which is the bug the
 * first cut had: labels were centred as if 16 tall while drawn at 32, so they
 * sat nine pixels low and spilled onto the next button.
 */
#define MRD_TITLE_SCALE 1
#define MRD_LABEL_SCALE 1
#define MRD_GLYPH_H     16

#define MRD_STATUS_H   20 /* top status bar height (one scale-1 line + margin) */
#define MRD_BTN_H      36 /* one menu button: a scale-1 label with room around it */
#define MRD_BTN_GAP    3
#define MRD_SCROLLBAR_W 6 /* right-edge scrollbar, shown only when it scrolls */

/* Vertical baseline that centres `scale`-high text in a box [y, y+h). */
static inline int mrd_text_y(int y, int h, int scale)
{
    return y + (h - MRD_GLYPH_H * scale) / 2;
}

/*
 * A scratch buffer (mrd_gfx.c) shared by every screen's working state - only one
 * screen is live at a time, so they take turns in it rather than each spending
 * bss the radio then cannot have.  Sized to the largest screen struct; each user
 * asserts its type fits.  See the note by the definition.
 */
#define MRD_SCRATCH_BYTES 768
void *mrd_scratch(void);

/* ------------------------------------------------------------------------ */
/* Colours - RGB565, matching the Marauder TFT palette as closely as 565     */
/* allows.  These are the accents the upstream menu assigns to each node.     */
/* ------------------------------------------------------------------------ */

static inline uint16_t mrd_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xf8u) << 8) | ((g & 0xfcu) << 3) | (b >> 3));
}

#define MRD_C_GREEN     0x07E0u
#define MRD_C_CYAN      0x07FFu
#define MRD_C_RED       0xF800u
#define MRD_C_BLUE      0x2D7Fu /* a readable blue, not the near-black 0x001F */
#define MRD_C_LGREY     0xC618u
#define MRD_C_YELLOW    0xFFE0u
#define MRD_C_ORANGE    0xFD20u
#define MRD_C_PURPLE    0xA81Fu
#define MRD_C_MAGENTA   0xF81Fu
#define MRD_C_VIOLET    0x915Cu
#define MRD_C_WHITE     0xFFFFu
#define MRD_C_LIME      0xB7E0u
#define MRD_C_SKYBLUE   0x867Du

#define MRD_C_BG        0x0000u /* screen background: black, like Marauder */
#define MRD_C_BTN_BG    0x18E3u /* button fill: dark grey */
#define MRD_C_BTN_SEL   0x39E7u /* button fill when keyboard-selected */
#define MRD_C_BAR_BG    0x10A2u /* status bar fill */
#define MRD_C_TEXT      0xFFFFu

/* ------------------------------------------------------------------------ */
/* Display / band renderer (mrd_gfx.c)                                       */
/* ------------------------------------------------------------------------ */

typedef struct {
    ag_gfxinfo_t info;
    uint16_t     W, H;        /* logical surface, clamped to MRD_UI_W/H */
    bool         surfaceless; /* info.fb == NULL -> gfx->present path */
    uint16_t     cols, rows;  /* console cells, for touch -> surface mapping */
} mrd_disp_t;

/*
 * A band being painted: absolute y range on the surface, and the pixel buffer.
 * The screen paint code draws elements that clip themselves to [y0, y0+h).
 */
typedef struct {
    uint16_t *px;   /* MRD_UI_W * MRD_BAND_H */
    int       y0;   /* absolute y of the band's first row */
    int       h;    /* rows in this band */
    int       w;    /* logical width (disp W) */
} mrd_band_t;

bool mrd_gfx_begin(mrd_disp_t *d);
void mrd_gfx_end(mrd_disp_t *d);

/*
 * Paint the whole surface, band by band.  For each band the renderer clears it
 * to `bg`, calls paint(band, ctx), then pushes it (present or blit).  After the
 * last band a soft surface is flushed once.  paint() draws with the band_*
 * primitives below.
 */
void mrd_gfx_paint(mrd_disp_t *d, uint16_t bg,
                   void (*paint)(mrd_band_t *b, void *ctx), void *ctx);

/*
 * Render the whole surface band by band into a PPM file instead of the panel,
 * for headless verification: this captures exactly what the app draws, without
 * depending on the display snapshot (which QEMU's dedicated framebuffer does not
 * keep).  Same paint callback as mrd_gfx_paint.
 */
ag_err_t mrd_gfx_dump_ppm(mrd_disp_t *d, const char *path, uint16_t bg,
                          void (*paint)(mrd_band_t *b, void *ctx), void *ctx);

/* Primitives that draw into a band, clipping to it. */
void mrd_band_fill(mrd_band_t *b, int x, int y, int w, int h, uint16_t color);
void mrd_band_frame(mrd_band_t *b, int x, int y, int w, int h, uint16_t color);
/* 8x16 glyphs scaled by `scale`.  bg == 0xFFFFFFFF means transparent.  Returns
 * the advance in pixels (glyph_w * scale * length). */
int mrd_band_text(mrd_band_t *b, int x, int y, const char *s, uint16_t fg,
                  uint32_t bg, int scale);
int  mrd_text_w(const char *s, int scale); /* pixel width of a string */

/* Map a touch cell to a logical surface pixel (clamped). */
void mrd_touch_to_px(const mrd_disp_t *d, int16_t cx, int16_t cy, int *px,
                     int *py);

/* ------------------------------------------------------------------------ */
/* Menu tree (mrd_menu.c)                                                     */
/* ------------------------------------------------------------------------ */

/* Leaf screens.  0 means "this node is a submenu"; a positive value routes to
 * a screen.  Only MRD_SCREEN_SCAN_AP is live in this first slice; the rest are
 * faithful placeholders so the tree is whole. */
enum {
    MRD_SCREEN_NONE = 0,
    MRD_SCREEN_SCAN_AP,     /* WiFi > Sniffers > Beacons - live (wifimon)     */
    MRD_SCREEN_PROBE_REQ,   /* WiFi > Sniffers > Probe Requests - live       */
    MRD_SCREEN_PKT_MON,     /* WiFi > Sniffers > Packet Monitor - live       */
    MRD_SCREEN_BEACON_SPAM, /* WiFi > Attacks > Beacon Spam - live (inject)  */
    MRD_SCREEN_STA_SCAN,    /* WiFi > Sniffers > Scan AP/STA - live (wifi)    */
    MRD_SCREEN_SOFTAP,      /* WiFi > Attacks > Evil Portal > APs - live (AP) */
    MRD_SCREEN_BT_SNIFF,    /* Bluetooth > Sniffers > BT Sniffer - live (ble)*/
    MRD_SCREEN_BT_SKIMMER,  /* Bluetooth > Sniffers > Card Skimmers - live   */
    MRD_SCREEN_BT_SPAM,     /* Bluetooth > Attacks > BLE spam - live (adv_raw)*/
    MRD_SCREEN_DEV_INFO,    /* Device > Info - live                          */
    MRD_SCREEN_REBOOT,
    MRD_SCREEN_STUB,        /* everything not yet built                      */
};

typedef struct mrd_node {
    const char             *label;
    uint16_t                color;
    const struct mrd_node  *children; /* submenu, or NULL for a leaf */
    uint8_t                 nchildren;
    uint8_t                 screen;    /* MRD_SCREEN_* for a leaf */
} mrd_node_t;

const mrd_node_t *mrd_root(void);

/* Run the menu UI.  Returns when the user asks to leave the app (QUIT) or the
 * root Back is not possible.  Handles the whole navigation stack and dispatch to
 * the leaf screens. */
void mrd_menu_run(mrd_disp_t *d);

/* Paint one screen once and return, for a screenshot / self-test (no input
 * loop).  `path` selects a node by dotted path like "wifi.sniffers"; NULL or ""
 * is the main menu.  Used by `marauder shot` so the UI can be captured with
 * gfxdump without a board. */
void mrd_menu_shot(mrd_disp_t *d, const char *path);

/* Render one menu screen to a PPM file (headless verification). */
void mrd_menu_dump(mrd_disp_t *d, const char *ppm_path, const char *menu_path);

/* ------------------------------------------------------------------------ */
/* Screens (mrd_scan.c)                                                       */
/* ------------------------------------------------------------------------ */

/* WiFi > Sniffers > Scan AP/STA: sniff beacons through api->wifimon and list
 * the access points.  Returns when the user leaves the screen. */
void mrd_screen_scan_ap(mrd_disp_t *d);

/* WiFi > Sniffers > Probe Requests: list the SSIDs client devices are asking
 * for, with the client's MAC.  Capture through api->wifimon. */
void mrd_screen_probe_req(mrd_disp_t *d);

/* WiFi > Sniffers > Packet Monitor: live frame counts by type, per channel,
 * while hopping.  Capture through api->wifimon. */
void mrd_screen_pkt_mon(mrd_disp_t *d);

/* WiFi > Attacks > Beacon Spam: inject beacons for a rotating list of SSIDs so
 * they appear in nearby Wi-Fi scans.  Raw injection through api->wifimon. */
void mrd_screen_beacon_spam(mrd_disp_t *d);

/* Device > Info: what this machine is - chip, profile, ABI, memory. */
void mrd_screen_dev_info(mrd_disp_t *d);

/* ---- station / access point / ESP-NOW screens (mrd_wifi.c, ABI 0.39) ---- */

/* WiFi > Sniffers > Scan AP/STA: a real station scan through api->wifi - the
 * networks in reach with their security, strongest first.  Tapping an open one
 * joins it; a footer shows the station's standing. */
void mrd_screen_sta_scan(mrd_disp_t *d);

/* WiFi > Attacks > Evil Portal > Access Points: bring up an open access point of
 * the board's own through api->wifi, and show its name, address and clients so a
 * phone can be joined to it.  Back stops it. */
void mrd_screen_softap(mrd_disp_t *d);

/* Headless self-test (`marauder wifitest`): exercise every api->wifi call once -
 * status, scan, access point, ESP-NOW - and print each result as a "[mrd] wifi"
 * line, so the whole surface can be verified over the serial cable without
 * touch.  Returns when done. */
void mrd_wifi_selftest(void);

/* ---- Bluetooth LE screens (mrd_bt.c, api->ble ABI 0.38) ---------------- */

/* Bluetooth > Sniffers > Bluetooth Sniffer: observe every BLE device in range
 * through api->ble - name, address, signal, whether it is connectable - the
 * strongest first.  Tapping one connects and discovers its GATT services. */
void mrd_screen_bt_sniff(mrd_disp_t *d);

/* Bluetooth > Sniffers > Detect Card Skimmers: the same scan, flagging devices
 * whose name matches the cheap serial modules skimmers are built from
 * (HC-05/HC-06 and the like). */
void mrd_screen_bt_skimmer(mrd_disp_t *d);

/* Headless self-test (`marauder bttest`): scan for BLE devices and, if one is
 * connectable, connect and discover it - printing each result as a "[mrd] bt"
 * line for verification over the serial cable. */
void mrd_bt_selftest(void);

/* Bluetooth > Attacks > (Sour Apple / SwiftPair / Samsung / Google / Spam All):
 * forge the raw advertising packets those vendors' pairing pop-ups key on, under
 * rotating spoofed addresses, through api->ble->adv_raw.  `label` picks the
 * vendor (the menu leaf's own text). */
void mrd_screen_bt_spam(mrd_disp_t *d, const char *label);

/* A placeholder screen with the right title and a Back button. */
void mrd_screen_stub(mrd_disp_t *d, const char *title);

#endif /* MRD_H */
