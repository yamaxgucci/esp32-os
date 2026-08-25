/*
 * MARAUDER - the menu tree and the navigation that walks it.
 *
 * The tree is the upstream ESP32-Marauder menu, node for node and label for
 * label (MenuFunctions.cpp), minus the entries that need hardware this board
 * does not have (GPS).  Only one leaf is live in this first slice - WiFi >
 * Sniffers > Scan AP/STA - and the rest open a placeholder with the right title,
 * so the whole tree is present and navigable and later slices fill it in.
 *
 * "Back" is not stored in the tree: every submenu gets one synthesised as its
 * first button, and the status bar's left edge is a Back target too, so touch
 * never strands the user.  The main menu has no Back (it has Reboot instead,
 * like upstream).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mrd.h"
#include <argon/keys.h>

/* ------------------------------------------------------------------------ */
/* The tree, built leaf-up so a parent can point at its children.            */
/* ------------------------------------------------------------------------ */

#define LEAF(lbl, col)      { (lbl), (col), NULL, 0, MRD_SCREEN_STUB }
#define LIVE(lbl, col, scr) { (lbl), (col), NULL, 0, (scr) }
#define SUB(lbl, col, kids) { (lbl), (col), (kids), \
                              (uint8_t)(sizeof(kids) / sizeof((kids)[0])), \
                              MRD_SCREEN_NONE }

/* WiFi > Sniffers */
static const mrd_node_t k_wifi_sniffers[] = {
    LIVE("Probe Requests", MRD_C_CYAN,    MRD_SCREEN_PROBE_REQ),
    LIVE("Beacons",        MRD_C_MAGENTA, MRD_SCREEN_SCAN_AP),
    LEAF("Deauth",         MRD_C_RED),
    LEAF("Packet Count",   MRD_C_ORANGE),
    LEAF("EAPOL",          MRD_C_VIOLET),
    LIVE("Packet Monitor", MRD_C_BLUE,    MRD_SCREEN_PKT_MON),
    LEAF("Channel Analyzer", MRD_C_CYAN),
    LEAF("Channel Summary",  MRD_C_ORANGE),
    LEAF("Raw Capture",    MRD_C_WHITE),
    LEAF("Pwnagotchi",     MRD_C_RED),
    LEAF("Pine Scan",      MRD_C_YELLOW),
    LEAF("MultiSSID",      MRD_C_ORANGE),
    LIVE("Scan AP/STA",    MRD_C_LIME, MRD_SCREEN_STA_SCAN),
    LEAF("Fox Hunt",       MRD_C_CYAN),
    LEAF("MAC Monitor",    MRD_C_MAGENTA),
    LEAF("SAE Commit",     MRD_C_LIME),
};

/* WiFi > Scanners */
static const mrd_node_t k_wifi_scanners[] = {
    LEAF("Ping Scan",      MRD_C_GREEN),
    LEAF("ARP Scan",       MRD_C_CYAN),
    LEAF("Port Scan All",  MRD_C_MAGENTA),
    LEAF("SSH Scan",       MRD_C_ORANGE),
    LEAF("Telnet Scan",    MRD_C_RED),
    LEAF("SMTP Scan",      MRD_C_WHITE),
    LEAF("DNS Scan",       MRD_C_LIME),
    LEAF("HTTP Scan",      MRD_C_SKYBLUE),
    LEAF("HTTPS Scan",     MRD_C_YELLOW),
    LEAF("RDP Scan",       MRD_C_PURPLE),
};

/* WiFi > Attacks > Evil Portal */
static const mrd_node_t k_evil_portal[] = {
    LIVE("Access Points",  MRD_C_GREEN, MRD_SCREEN_SOFTAP),
    LEAF("User SSIDs",     MRD_C_CYAN),
};

/* WiFi > Attacks */
static const mrd_node_t k_wifi_attacks[] = {
    LEAF("Beacon List",    MRD_C_RED),
    LIVE("Beacon Spam",    MRD_C_ORANGE, MRD_SCREEN_BEACON_SPAM),
    LEAF("Funny Beacon",   MRD_C_CYAN),
    LEAF("Rick Roll",      MRD_C_YELLOW),
    LEAF("Auth",           MRD_C_RED),
    SUB ("Evil Portal",    MRD_C_ORANGE, k_evil_portal),
    LEAF("Deauth",         MRD_C_RED),
    LEAF("AP Spam",        MRD_C_MAGENTA),
    LEAF("Deauth Targeted", MRD_C_RED),
    LEAF("Karma",          MRD_C_ORANGE),
    LEAF("Bad Msg",        MRD_C_RED),
    LEAF("Bad Msg Targeted", MRD_C_YELLOW),
    LEAF("Assoc Sleep",    MRD_C_RED),
    LEAF("Assoc Sleep Targ", MRD_C_MAGENTA),
    LEAF("SAE Commit Flood", MRD_C_LIME),
    LEAF("Channel Switch", MRD_C_ORANGE),
    LEAF("Quiet Time",     MRD_C_RED),
};

/* WiFi > General > Save/Load Files */
static const mrd_node_t k_save_load[] = {
    LEAF("Save SSIDs",     MRD_C_GREEN),
    LEAF("Load SSIDs",     MRD_C_CYAN),
    LEAF("Save APs",       MRD_C_RED),
    LEAF("Load APs",       MRD_C_YELLOW),
    LEAF("Save Airtags",   MRD_C_MAGENTA),
    LEAF("Load Airtags",   MRD_C_WHITE),
};

/* WiFi > General */
static const mrd_node_t k_wifi_general[] = {
    LEAF("Generate SSIDs",   MRD_C_SKYBLUE),
    LEAF("Select Probe SSID", MRD_C_CYAN),
    SUB ("Save/Load Files",  MRD_C_PURPLE, k_save_load),
};

/* WiFi */
static const mrd_node_t k_wifi[] = {
    SUB("Sniffers",  MRD_C_YELLOW, k_wifi_sniffers),
    SUB("Scanners",  MRD_C_ORANGE, k_wifi_scanners),
    SUB("Attacks",   MRD_C_RED,    k_wifi_attacks),
    SUB("General",   MRD_C_PURPLE, k_wifi_general),
};

/* Bluetooth > Sniffers */
static const mrd_node_t k_bt_sniffers[] = {
    LIVE("Bluetooth Sniffer",    MRD_C_CYAN, MRD_SCREEN_BT_SNIFF),
    LIVE("Detect Card Skimmers", MRD_C_RED,  MRD_SCREEN_BT_SKIMMER),
    LEAF("Wardrive",             MRD_C_GREEN),  /* needs GPS this board lacks */
    LEAF("Analyzer",             MRD_C_YELLOW),
};

/*
 * Bluetooth > Attacks.  The BLE-spam family (Sour Apple, SwiftPair, Samsung,
 * Google) forges raw advertising packets - manufacturer data that mimics those
 * vendors' pairing pop-ups - through api->ble->adv_raw (ABI 0.40), the BLE
 * analog of wifimon's raw 802.11 injection.
 */
static const mrd_node_t k_bt_attacks[] = {
    LIVE("Sour Apple",     MRD_C_RED,     MRD_SCREEN_BT_SPAM),
    LIVE("SwiftPair Spam", MRD_C_ORANGE,  MRD_SCREEN_BT_SPAM),
    LIVE("Samsung Spam",   MRD_C_CYAN,    MRD_SCREEN_BT_SPAM),
    LIVE("Google Spam",    MRD_C_GREEN,   MRD_SCREEN_BT_SPAM),
    LIVE("Spam All",       MRD_C_MAGENTA, MRD_SCREEN_BT_SPAM),
};

/* Bluetooth */
static const mrd_node_t k_bt[] = {
    SUB("Sniffers", MRD_C_CYAN, k_bt_sniffers),
    SUB("Attacks",  MRD_C_RED,  k_bt_attacks),
};

/* Device */
static const mrd_node_t k_device[] = {
    LEAF("Update",   MRD_C_GREEN),
    LIVE("Info",     MRD_C_BLUE, MRD_SCREEN_DEV_INFO),
    LEAF("Settings", MRD_C_CYAN),
};

/* Main menu */
static const mrd_node_t k_main[] = {
    SUB ("WiFi",      MRD_C_GREEN, k_wifi),
    SUB ("Bluetooth", MRD_C_CYAN,  k_bt),
    SUB ("Device",    MRD_C_BLUE,  k_device),
    LIVE("Reboot",    MRD_C_LGREY, MRD_SCREEN_REBOOT),
};

static const mrd_node_t k_root = {
    "Main Menu", MRD_C_WHITE, k_main,
    (uint8_t)(sizeof(k_main) / sizeof(k_main[0])), MRD_SCREEN_NONE
};

const mrd_node_t *mrd_root(void) { return &k_root; }

/* ------------------------------------------------------------------------ */
/* A view of one level, shared with the paint callback.                      */
/* ------------------------------------------------------------------------ */

typedef struct {
    const char            *title;
    uint16_t               title_color;
    const mrd_node_t      *items;
    int                    count;
    bool                   has_back;   /* synthesised Back as button 0 */
    int                    scroll;     /* first visible button */
    int                    sel;        /* keyboard highlight, -1 none */
    int                    nvis;       /* buttons that fit */
    int                    total;      /* count + (has_back ? 1 : 0) */
    int                    W, H;
} mrd_view_t;

/* Buttons visible = however many fit under the status bar. */
static int visible_count(int H)
{
    int n = (H - MRD_STATUS_H) / (MRD_BTN_H + MRD_BTN_GAP);
    return n < 1 ? 1 : n;
}

/* Width available to the buttons: the surface, less the scrollbar when shown. */
static int list_width(int W, bool scrollbar)
{
    return W - (scrollbar ? (MRD_SCROLLBAR_W + 2) : 0);
}

/* The i-th visible button's rectangle (i is a slot 0..nvis-1). */
static void slot_rect(int i, int listw, int *x, int *y, int *w, int *h)
{
    *x = 3;
    *w = listw - 6;
    *y = MRD_STATUS_H + MRD_BTN_GAP + i * (MRD_BTN_H + MRD_BTN_GAP);
    *h = MRD_BTN_H;
}

/* The scrollbar thumb rectangle for the current view (only meaningful when
 * total > nvis). */
static void thumb_rect(const mrd_view_t *v, int *y, int *h)
{
    const int track = v->H - MRD_STATUS_H;
    int th = track * v->nvis / v->total;
    if (th < 14) {
        th = 14;
    }
    const int span = v->total - v->nvis;
    int ty = MRD_STATUS_H;
    if (span > 0) {
        ty += (track - th) * v->scroll / span;
    }
    *y = ty;
    *h = th;
}

/* Button index (0 = Back when present) for a logical button number `b`. */
static const char *button_label(const mrd_view_t *v, int b, uint16_t *color,
                                bool *is_back, bool *is_submenu)
{
    *is_back = false;
    *is_submenu = false;
    if (v->has_back) {
        if (b == 0) {
            *is_back = true;
            *color = MRD_C_LGREY;
            return "< Back";
        }
        b -= 1;
    }
    const mrd_node_t *n = &v->items[b];
    *color = n->color;
    *is_submenu = (n->children != NULL);
    return n->label;
}

static void paint_menu(mrd_band_t *b, void *ctx)
{
    const mrd_view_t *v = (const mrd_view_t *)ctx;
    const bool sb = v->total > v->nvis;
    const int  listw = list_width(v->W, sb);
    const int  ty0 = mrd_text_y(0, MRD_STATUS_H, MRD_TITLE_SCALE);

    /* Status bar: a "<" when there is somewhere to go back to, then the title. */
    mrd_band_fill(b, 0, 0, v->W, MRD_STATUS_H, MRD_C_BAR_BG);
    mrd_band_fill(b, 0, MRD_STATUS_H - 1, v->W, 1, MRD_C_LGREY);
    int tx = 6;
    if (v->has_back) {
        tx += mrd_band_text(b, 6, ty0, "<", MRD_C_LGREY, 0xFFFFFFFFu,
                            MRD_TITLE_SCALE) + 4;
    }
    mrd_band_text(b, tx, ty0, v->title, v->title_color, 0xFFFFFFFFu,
                  MRD_TITLE_SCALE);

    /* Buttons. */
    for (int i = 0; i < v->nvis; i++) {
        const int bi = v->scroll + i;
        if (bi >= v->total) {
            break;
        }
        int x, y, w, h;
        slot_rect(i, listw, &x, &y, &w, &h);

        uint16_t color;
        bool     is_back, is_sub;
        const char *lbl = button_label(v, bi, &color, &is_back, &is_sub);

        const uint16_t fill = (bi == v->sel) ? MRD_C_BTN_SEL : MRD_C_BTN_BG;
        mrd_band_fill(b, x, y, w, h, fill);
        mrd_band_fill(b, x, y, 4, h, color);         /* colour stripe */
        mrd_band_frame(b, x, y, w, h, color);

        const int ly = mrd_text_y(y, h, MRD_LABEL_SCALE);
        mrd_band_text(b, x + 12, ly, lbl, color, 0xFFFFFFFFu, MRD_LABEL_SCALE);
        if (is_sub) {
            mrd_band_text(b, x + w - 8 * MRD_LABEL_SCALE - 6, ly, ">",
                          MRD_C_LGREY, 0xFFFFFFFFu, MRD_LABEL_SCALE);
        }
    }

    /* Scrollbar on the right edge, with a thumb sized and placed by position. */
    if (sb) {
        const int sx = v->W - MRD_SCROLLBAR_W;
        mrd_band_fill(b, sx, MRD_STATUS_H, MRD_SCROLLBAR_W,
                      v->H - MRD_STATUS_H, MRD_C_BTN_BG);
        int thy, thh;
        thumb_rect(v, &thy, &thh);
        mrd_band_fill(b, sx, thy, MRD_SCROLLBAR_W, thh, MRD_C_LGREY);
    }
}

/* ------------------------------------------------------------------------ */
/* Navigation                                                                */
/* ------------------------------------------------------------------------ */

#define MRD_STACK_MAX 8

typedef struct {
    const mrd_node_t *node; /* NULL at the root */
    int               scroll;
    int               sel;
} mrd_level_t;

static void make_view(const mrd_level_t *lvl, mrd_disp_t *d, mrd_view_t *v)
{
    const mrd_node_t *node = lvl->node;
    v->W = d->W;
    v->H = d->H;
    if (node == NULL) {
        v->title = "Main Menu";
        v->title_color = MRD_C_WHITE;
        v->items = mrd_root()->children;
        v->count = mrd_root()->nchildren;
        v->has_back = false;
    } else {
        v->title = node->label;
        v->title_color = node->color;
        v->items = node->children;
        v->count = node->nchildren;
        v->has_back = true;
    }
    v->scroll = lvl->scroll;
    v->sel = lvl->sel;
    v->nvis = visible_count(d->H);
    v->total = v->count + (v->has_back ? 1 : 0);
}

/* Keep the selected button on screen. */
static void clamp_scroll(mrd_level_t *lvl, const mrd_view_t *v)
{
    if (lvl->sel < 0) {
        lvl->sel = 0;
    }
    if (lvl->sel >= v->total) {
        lvl->sel = v->total - 1;
    }
    if (lvl->sel < lvl->scroll) {
        lvl->scroll = lvl->sel;
    }
    if (lvl->sel >= lvl->scroll + v->nvis) {
        lvl->scroll = lvl->sel - v->nvis + 1;
    }
    if (lvl->scroll < 0) {
        lvl->scroll = 0;
    }
    int maxscroll = v->total - v->nvis;
    if (maxscroll < 0) {
        maxscroll = 0;
    }
    if (lvl->scroll > maxscroll) {
        lvl->scroll = maxscroll;
    }
}

/*
 * Which logical button a touch landed on, or -1.  The whole status bar is Back
 * (when there is one); a tap in the scrollbar column sets *seek to the item at
 * that fraction of the list, and the list scrolls to reveal it.
 */
static int hit_button(const mrd_view_t *v, int px, int py, bool *back, int *seek)
{
    *back = false;
    *seek = -1;
    const bool sb = v->total > v->nvis;

    if (py < MRD_STATUS_H) {
        if (v->has_back) {
            *back = true;
        }
        return -1;
    }

    if (sb && px >= v->W - MRD_SCROLLBAR_W - 4) {
        const int track = v->H - MRD_STATUS_H;
        *seek = (py - MRD_STATUS_H) * v->total / (track > 0 ? track : 1);
        return -1;
    }

    const int listw = list_width(v->W, sb);
    for (int i = 0; i < v->nvis; i++) {
        const int bi = v->scroll + i;
        if (bi >= v->total) {
            break;
        }
        int x, y, w, h;
        slot_rect(i, listw, &x, &y, &w, &h);
        if (px >= x && px < x + w && py >= y && py < y + h) {
            return bi;
        }
    }
    return -1;
}

static void dispatch_leaf(mrd_disp_t *d, const mrd_node_t *node)
{
    switch (node->screen) {
    case MRD_SCREEN_SCAN_AP:
        mrd_screen_scan_ap(d);
        break;
    case MRD_SCREEN_PROBE_REQ:
        mrd_screen_probe_req(d);
        break;
    case MRD_SCREEN_PKT_MON:
        mrd_screen_pkt_mon(d);
        break;
    case MRD_SCREEN_BEACON_SPAM:
        mrd_screen_beacon_spam(d);
        break;
    case MRD_SCREEN_STA_SCAN:
        mrd_screen_sta_scan(d);
        break;
    case MRD_SCREEN_SOFTAP:
        mrd_screen_softap(d);
        break;
    case MRD_SCREEN_BT_SNIFF:
        mrd_screen_bt_sniff(d);
        break;
    case MRD_SCREEN_BT_SKIMMER:
        mrd_screen_bt_skimmer(d);
        break;
    case MRD_SCREEN_BT_SPAM:
        mrd_screen_bt_spam(d, node->label);
        break;
    case MRD_SCREEN_DEV_INFO:
        mrd_screen_dev_info(d);
        break;
    case MRD_SCREEN_REBOOT:
        mrd_screen_stub(d, "Reboot");
        break;
    default:
        mrd_screen_stub(d, node->label);
        break;
    }
}

void mrd_menu_run(mrd_disp_t *d)
{
    mrd_level_t stack[MRD_STACK_MAX];
    int depth = 0;
    stack[0].node = NULL;
    stack[0].scroll = 0;
    stack[0].sel = 0;

    bool dirty = true;

    /*
     * Touch gesture state, persistent across iterations because a press and its
     * release can fall in different event bursts.  A press that stays put is a
     * tap (acted on release); one that moves past a threshold is a swipe, and
     * then the list scrolls with the finger and the release does nothing.
     */
    int  g_down_y = -1;  /* press y in pixels, or -1 for no press in progress */
    int  g_scroll0 = 0;  /* lvl->scroll at the press, the swipe's anchor      */
    bool g_swipe = false;
    bool g_down_back = false;
    int  g_down_btn = -2;
    /* Three cells: a resistive panel jitters a cell or two under a still
     * finger, and at one cell that jitter turned taps into swipes - "works
     * every other time".  Below this, a drag is still a tap. */
    const int SWIPE_THRESH = 24;

    for (;;) {
        mrd_level_t *lvl = &stack[depth];
        mrd_view_t   v;
        make_view(lvl, d, &v);
        clamp_scroll(lvl, &v);
        make_view(lvl, d, &v); /* refresh scroll/sel after clamp */

        /* Mirror the highlighted item to the serial console when it changes,
         * so the app can be driven and watched over the cable without touch. */
        static int last_sel = -1000, last_depth = -1;
        if (lvl->sel != last_sel || depth != last_depth) {
            uint16_t    c;
            bool        bk, sub;
            const char *l = button_label(&v, lvl->sel, &c, &bk, &sub);
            ag_printf("[mrd] \"%s\" [%d/%d] \"%s\"%s\n", v.title, lvl->sel + 1,
                      v.total, l, sub ? " >" : "");
            last_sel = lvl->sel;
            last_depth = depth;
        }

        if (dirty) {
            mrd_gfx_paint(d, MRD_C_BG, paint_menu, &v);
            dirty = false;
        }

        ag_event_t ev;
        if (!ag_poll_event(&ev, 200)) {
            if (!ag_focused()) {
                ag_delay(50);
            }
            continue;
        }

        int  activate = -2; /* -2 none, -1 back, >=0 button */
        bool quit = false;
        const int pitch = MRD_BTN_H + MRD_BTN_GAP;
        int maxs = v.total - v.nvis;
        if (maxs < 0) {
            maxs = 0;
        }

        /* Process this event, then drain the burst behind it, so a swipe's
         * stream of moves becomes one scroll and one repaint. */
        do {
            if (ev.type == AG_EV_QUIT) {
                quit = true;
                break;
            }
            if (ev.type == AG_EV_FOCUS_GAINED) {
                dirty = true;
                continue;
            }
            if (ev.type == AG_EV_KEY_DOWN) {
                switch (ev.key.keycode) {
                case AG_KEY_UP:
                    lvl->sel--;
                    dirty = true;
                    break;
                case AG_KEY_DOWN:
                    lvl->sel++;
                    dirty = true;
                    break;
                case AG_KEY_PAGEUP:
                    lvl->sel -= v.nvis;
                    dirty = true;
                    break;
                case AG_KEY_PAGEDOWN:
                    lvl->sel += v.nvis;
                    dirty = true;
                    break;
                case AG_KEY_ENTER:
                case AG_KEY_RIGHT:
                case AG_KEY_SPACE:
                    activate = lvl->sel;
                    break;
                case AG_KEY_ESC:
                case AG_KEY_LEFT:
                case AG_KEY_BACKSPACE:
                    activate = -1;
                    break;
                default:
                    break;
                }
            } else if (ev.type == AG_EV_POINTER_DOWN) {
                int px, py;
                mrd_touch_to_px(d, ev.ptr.x, ev.ptr.y, &px, &py);
                bool back;
                int  seek;
                const int bi = hit_button(&v, px, py, &back, &seek);
                if (seek >= 0) {
                    lvl->sel = seek; /* scrollbar tap: jump, no gesture */
                    dirty = true;
                    g_down_y = -1;
                } else {
                    g_down_y = py; /* arm a tap-or-swipe */
                    g_scroll0 = lvl->scroll;
                    g_swipe = false;
                    g_down_back = back;
                    g_down_btn = bi;
                }
            } else if (ev.type == AG_EV_POINTER_MOVE) {
                if (g_down_y >= 0) {
                    int px, py;
                    mrd_touch_to_px(d, ev.ptr.x, ev.ptr.y, &px, &py);
                    const int dy = py - g_down_y;
                    if (!g_swipe && (dy > SWIPE_THRESH || dy < -SWIPE_THRESH)) {
                        g_swipe = true;
                    }
                    if (g_swipe) {
                        int ns = g_scroll0 - dy / pitch; /* content follows */
                        if (ns < 0) {
                            ns = 0;
                        }
                        if (ns > maxs) {
                            ns = maxs;
                        }
                        if (ns != lvl->scroll) {
                            lvl->scroll = ns;
                            lvl->sel = ns; /* keep clamp_scroll from fighting */
                            dirty = true;
                        }
                    }
                }
            } else if (ev.type == AG_EV_POINTER_UP) {
                if (g_down_y >= 0 && !g_swipe) {
                    if (g_down_back) {
                        activate = -1;
                    } else if (g_down_btn >= 0) {
                        activate = g_down_btn;
                    }
                }
                g_down_y = -1;
                g_swipe = false;
            }
        } while (activate == -2 && !quit && ag_poll_event(&ev, 0));

        if (quit) {
            return;
        }
        if (activate == -2) {
            continue;
        }

        /* A synthesised Back button (button 0 when has_back). */
        bool want_back = (activate == -1) ||
                         (v.has_back && activate == 0);
        if (want_back) {
            if (depth > 0) {
                depth--;
            }
            dirty = true;
            continue;
        }

        /* Real item index (drop the Back slot). */
        int idx = activate - (v.has_back ? 1 : 0);
        if (idx < 0 || idx >= v.count) {
            continue;
        }
        const mrd_node_t *node = &v.items[idx];
        if (node->children != NULL) {
            if (depth + 1 < MRD_STACK_MAX) {
                depth++;
                stack[depth].node = node;
                stack[depth].scroll = 0;
                stack[depth].sel = 0;
            }
        } else {
            dispatch_leaf(d, node);
        }
        dirty = true;
    }
}

/* ------------------------------------------------------------------------ */
/* One-shot paint, for `marauder shot` (screenshot / self-test)              */
/* ------------------------------------------------------------------------ */

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* True when `label` starts with the token `seg` (length seglen), ignoring case
 * and spaces/slashes in the label. */
static bool label_matches(const char *label, const char *seg, int seglen)
{
    int i = 0;
    for (const char *l = label; *l && i < seglen; l++) {
        if (*l == ' ' || *l == '/') {
            continue;
        }
        if (lower(*l) != lower(seg[i])) {
            return false;
        }
        i++;
    }
    return i == seglen;
}

static void resolve_level(const char *path, mrd_level_t *lvl)
{
    lvl->node = NULL;
    lvl->scroll = 0;
    lvl->sel = 0;

    const char *p = path;
    while (p && *p) {
        while (*p == '.' || *p == ' ') {
            p++;
        }
        const char *seg = p;
        while (*p && *p != '.') {
            p++;
        }
        const int seglen = (int)(p - seg);
        if (seglen == 0) {
            break;
        }
        const mrd_node_t *items =
            lvl->node ? lvl->node->children : mrd_root()->children;
        const int count =
            lvl->node ? lvl->node->nchildren : mrd_root()->nchildren;
        for (int i = 0; i < count; i++) {
            if (items[i].children != NULL &&
                label_matches(items[i].label, seg, seglen)) {
                lvl->node = &items[i];
                break;
            }
        }
    }
}

void mrd_menu_shot(mrd_disp_t *d, const char *path)
{
    mrd_level_t lvl;
    resolve_level(path, &lvl);
    mrd_view_t v;
    make_view(&lvl, d, &v);
    mrd_gfx_paint(d, MRD_C_BG, paint_menu, &v);
}

void mrd_menu_dump(mrd_disp_t *d, const char *ppm_path, const char *menu_path)
{
    mrd_level_t lvl;
    resolve_level(menu_path, &lvl);
    mrd_view_t v;
    make_view(&lvl, d, &v);
    (void)mrd_gfx_dump_ppm(d, ppm_path, MRD_C_BG, paint_menu, &v);
}

/* ------------------------------------------------------------------------ */
/* Placeholder screen                                                        */
/* ------------------------------------------------------------------------ */

typedef struct {
    const char *title;
    int         W, H;
} mrd_stub_ctx_t;

static void paint_stub(mrd_band_t *b, void *ctx)
{
    const mrd_stub_ctx_t *s = (const mrd_stub_ctx_t *)ctx;

    const int ty0 = mrd_text_y(0, MRD_STATUS_H, MRD_TITLE_SCALE);
    mrd_band_fill(b, 0, 0, s->W, MRD_STATUS_H, MRD_C_BAR_BG);
    mrd_band_fill(b, 0, MRD_STATUS_H - 1, s->W, 1, MRD_C_LGREY);
    int tx = 6 + mrd_band_text(b, 6, ty0, "<", MRD_C_LGREY, 0xFFFFFFFFu,
                               MRD_TITLE_SCALE) + 4;
    mrd_band_text(b, tx, ty0, s->title, MRD_C_WHITE, 0xFFFFFFFFu,
                  MRD_TITLE_SCALE);

    const char *msg = "Not implemented yet";
    const int   tw = mrd_text_w(msg, 1);
    mrd_band_text(b, (s->W - tw) / 2, s->H / 2 - 8, msg, MRD_C_LGREY,
                  0xFFFFFFFFu, 1);
}

void mrd_screen_stub(mrd_disp_t *d, const char *title)
{
    mrd_stub_ctx_t s = { title, d->W, d->H };
    bool dirty = true;

    for (;;) {
        if (dirty) {
            mrd_gfx_paint(d, MRD_C_BG, paint_stub, &s);
            dirty = false;
        }
        ag_event_t ev;
        if (!ag_poll_event(&ev, 200)) {
            continue;
        }
        if (ev.type == AG_EV_QUIT) {
            return;
        }
        if (ev.type == AG_EV_FOCUS_GAINED) {
            dirty = true;
        } else if (ev.type == AG_EV_KEY_DOWN) {
            if (ev.key.keycode == AG_KEY_ESC ||
                ev.key.keycode == AG_KEY_LEFT ||
                ev.key.keycode == AG_KEY_BACKSPACE ||
                ev.key.keycode == AG_KEY_ENTER) {
                return;
            }
        } else if (ev.type == AG_EV_POINTER_DOWN) {
            int px, py;
            mrd_touch_to_px(d, ev.ptr.x, ev.ptr.y, &px, &py);
            if (py < MRD_STATUS_H) { /* the whole bar is Back here */
                return;
            }
        }
    }
}
