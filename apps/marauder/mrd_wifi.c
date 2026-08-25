/*
 * MARAUDER - the radio as a network: station scan/connect and the access point.
 *
 * Where mrd_scan.c drives api->wifimon (the receiver on one channel and the
 * frame forge), this drives api->wifi (ABI 0.39): a real station scan with the
 * security of each network, joining one, and offering an access point of the
 * board's own.  It is the same port the shell's `wifi` command uses, reached
 * through the public ABI so the application stays a loadable .AXE.
 *
 * api->wifi is NULL on a board with no radio (QEMU); the screens say so and
 * offer Back.  Raising the radio costs a large contiguous slice of internal RAM
 * on this no-PSRAM board, so a scan started from inside the resident app can
 * fail -AG_ENOMEM - the screens catch that and point at the `wifi on`-first
 * workflow, exactly as the wifimon screens do.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mrd.h"
#include <argon/keys.h>

#define MRD_WMAX  12 /* networks kept from one scan (shares screen scratch) */
#define MRD_WROW  34 /* two 8x16 lines: SSID line + BSSID line */

/* ---- tiny string builders (the SDK has no libc; file-scoped copies) ------ */

static char *put(char *p, char *end, char c)
{
    if (p < end - 1) {
        *p++ = c;
    }
    return p;
}
static char *put_str(char *p, char *end, const char *s)
{
    while (s && *s) {
        p = put(p, end, *s++);
    }
    return p;
}
static char *put_uint(char *p, char *end, uint32_t v)
{
    char tmp[10];
    int  n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v && n < (int)sizeof(tmp));
    while (n > 0) {
        p = put(p, end, tmp[--n]);
    }
    return p;
}
static char *put_hex2(char *p, char *end, uint8_t v)
{
    static const char h[] = "0123456789abcdef";
    p = put(p, end, h[(v >> 4) & 0xf]);
    p = put(p, end, h[v & 0xf]);
    return p;
}

static bool is_back_key(uint16_t kc)
{
    return kc == AG_KEY_ESC || kc == AG_KEY_LEFT || kc == AG_KEY_BACKSPACE;
}

/* Short label for a security type, for the badge on each row. */
static const char *sec_name(uint8_t auth)
{
    switch (auth) {
    case AG_WIFI_SEC_OPEN:       return "open";
    case AG_WIFI_SEC_WEP:        return "wep";
    case AG_WIFI_SEC_WPA:        return "wpa";
    case AG_WIFI_SEC_WPA2:       return "wpa2";
    case AG_WIFI_SEC_WPA3:       return "wpa3";
    case AG_WIFI_SEC_ENTERPRISE: return "ent";
    default:                     return "?";
    }
}

static const char *state_name(uint8_t st)
{
    switch (st) {
    case AG_WIFI_ST_OFF:     return "off";
    case AG_WIFI_ST_IDLE:    return "idle";
    case AG_WIFI_ST_JOINING: return "joining";
    case AG_WIFI_ST_JOINED:  return "joined";
    default:                 return "?";
    }
}

/* ---- a one-line message screen with Back (radio absent / ENOMEM) --------- */

static void paint_msg(mrd_band_t *b, void *ctx)
{
    const char **m = (const char **)ctx; /* [0]=title, [1]=message */
    const int    ty0 = mrd_text_y(0, MRD_STATUS_H, MRD_TITLE_SCALE);
    mrd_band_fill(b, 0, 0, b->w, MRD_STATUS_H, MRD_C_BAR_BG);
    mrd_band_fill(b, 0, MRD_STATUS_H - 1, b->w, 1, MRD_C_LGREY);
    int tx = 6 + mrd_band_text(b, 6, ty0, "<", MRD_C_LGREY, 0xFFFFFFFFu,
                               MRD_TITLE_SCALE) + 4;
    mrd_band_text(b, tx, ty0, m[0], MRD_C_LIME, 0xFFFFFFFFu, MRD_TITLE_SCALE);
    mrd_band_text(b, 8, MRD_STATUS_H + 20, m[1], MRD_C_YELLOW, 0xFFFFFFFFu, 1);
}

static void wait_back(mrd_disp_t *d, const char *title, const char *msg)
{
    const char *m[2] = { title, msg };
    bool        dirty = true;
    for (;;) {
        if (dirty) {
            mrd_gfx_paint(d, MRD_C_BG, paint_msg, m);
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
            return;
        } else if (ev.type == AG_EV_POINTER_DOWN) {
            int px, py;
            mrd_touch_to_px(d, ev.ptr.x, ev.ptr.y, &px, &py);
            if (py < MRD_STATUS_H) {
                return;
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* WiFi > Sniffers > Scan AP/STA - a real station scan                       */
/* ------------------------------------------------------------------------ */

typedef struct {
    int          W, H;
    ag_wifi_ap_t ap[MRD_WMAX];
    int          count;
    int          scroll;
    int          nvis;
    bool         scanning;
    ag_wifi_status_t st; /* the station's own standing, for the footer */
} mrd_sta_t;

static int sta_visible(int H)
{
    /* leave a row at the bottom for the status footer */
    int n = (H - MRD_STATUS_H - 18) / MRD_WROW;
    return n < 1 ? 1 : n;
}

/* Insertion sort by RSSI, strongest first (the list is short). */
static void sort_sta(mrd_sta_t *s)
{
    for (int i = 1; i < s->count; i++) {
        ag_wifi_ap_t key = s->ap[i];
        int          j = i - 1;
        while (j >= 0 && s->ap[j].rssi < key.rssi) {
            s->ap[j + 1] = s->ap[j];
            j--;
        }
        s->ap[j + 1] = key;
    }
}

static void paint_sta(mrd_band_t *b, void *ctx)
{
    mrd_sta_t *s = (mrd_sta_t *)ctx;
    char       buf[64];
    char      *end = buf + sizeof(buf);

    const bool sb = s->count > s->nvis;
    const int  rext = sb ? (MRD_SCROLLBAR_W + 2) : 0;
    const int  ty0 = mrd_text_y(0, MRD_STATUS_H, MRD_TITLE_SCALE);

    mrd_band_fill(b, 0, 0, s->W, MRD_STATUS_H, MRD_C_BAR_BG);
    mrd_band_fill(b, 0, MRD_STATUS_H - 1, s->W, 1, MRD_C_LGREY);
    int tx = 6 + mrd_band_text(b, 6, ty0, "<", MRD_C_LGREY, 0xFFFFFFFFu,
                               MRD_TITLE_SCALE) + 4;
    mrd_band_text(b, tx, ty0, "Scan AP/STA", MRD_C_LIME, 0xFFFFFFFFu,
                  MRD_TITLE_SCALE);

    char *p = buf;
    p = put_str(p, end, "n:");
    p = put_uint(p, end, (uint32_t)s->count);
    *p = '\0';
    mrd_band_text(b, s->W - mrd_text_w(buf, 1) - rext - 4, ty0, buf,
                  MRD_C_LGREY, 0xFFFFFFFFu, 1);

    if (s->scanning && s->count == 0) {
        mrd_band_text(b, 8, MRD_STATUS_H + 10, "scanning...", MRD_C_LGREY,
                      0xFFFFFFFFu, 1);
    } else if (s->count == 0) {
        mrd_band_text(b, 8, MRD_STATUS_H + 10, "no networks found - tap to rescan",
                      MRD_C_LGREY, 0xFFFFFFFFu, 1);
    }

    for (int i = 0; i < s->nvis; i++) {
        const int oi = s->scroll + i;
        if (oi >= s->count) {
            break;
        }
        const ag_wifi_ap_t *a = &s->ap[oi];
        const int           y = MRD_STATUS_H + i * MRD_WROW + 2;

        uint16_t col = MRD_C_GREEN;
        if (a->rssi < -80) {
            col = MRD_C_RED;
        } else if (a->rssi < -67) {
            col = MRD_C_YELLOW;
        }

        const char *name = (a->ssid[0] != '\0') ? a->ssid : "<hidden>";
        /* an open network in cyan, so what a tap will join stands out */
        const uint16_t nc =
            (a->auth == AG_WIFI_SEC_OPEN) ? MRD_C_CYAN : MRD_C_TEXT;
        mrd_band_text(b, 6, y, name, nc, 0xFFFFFFFFu, 1);

        p = buf;
        p = put_str(p, end, "c");
        p = put_uint(p, end, a->channel);
        p = put_str(p, end, " -");
        p = put_uint(p, end, (uint32_t)(-(int)a->rssi));
        *p = '\0';
        mrd_band_text(b, s->W - mrd_text_w(buf, 1) - rext - 6, y, buf, col,
                      0xFFFFFFFFu, 1);

        /* Line 2: BSSID on the left, security badge on the right. */
        p = buf;
        for (int k = 0; k < 6; k++) {
            if (k) {
                p = put(p, end, ':');
            }
            p = put_hex2(p, end, a->bssid[k]);
        }
        *p = '\0';
        mrd_band_text(b, 6, y + 16, buf, MRD_C_LGREY, 0xFFFFFFFFu, 1);
        const char *sec = sec_name(a->auth);
        mrd_band_text(b, s->W - mrd_text_w(sec, 1) - rext - 6, y + 16, sec,
                      MRD_C_LGREY, 0xFFFFFFFFu, 1);

        mrd_band_fill(b, 0, y + MRD_WROW - 3, s->W - rext, 1, 0x2104);
    }

    /* Footer: the station's own standing. */
    {
        const int fy = s->H - 16;
        mrd_band_fill(b, 0, fy - 2, s->W, s->H - (fy - 2), MRD_C_BAR_BG);
        p = buf;
        p = put_str(p, end, "sta: ");
        p = put_str(p, end, state_name(s->st.state));
        if (s->st.state == AG_WIFI_ST_JOINED ||
            s->st.state == AG_WIFI_ST_JOINING) {
            p = put_str(p, end, " ");
            p = put_str(p, end, s->st.ssid);
        }
        *p = '\0';
        const uint16_t fc = (s->st.state == AG_WIFI_ST_JOINED)
                                ? MRD_C_GREEN
                                : MRD_C_LGREY;
        mrd_band_text(b, 6, fy, buf, fc, 0xFFFFFFFFu, 1);
    }

    if (sb) {
        const int sx = s->W - MRD_SCROLLBAR_W;
        const int track = s->H - MRD_STATUS_H - 18;
        mrd_band_fill(b, sx, MRD_STATUS_H, MRD_SCROLLBAR_W, track,
                      MRD_C_BTN_BG);
        int th = track * s->nvis / s->count;
        if (th < 14) {
            th = 14;
        }
        const int span = s->count - s->nvis;
        int thy =
            MRD_STATUS_H + (span > 0 ? (track - th) * s->scroll / span : 0);
        mrd_band_fill(b, sx, thy, MRD_SCROLLBAR_W, th, MRD_C_LGREY);
    }
}

/* Run one blocking scan into the list; paints "scanning..." first. */
static void do_scan(mrd_disp_t *d, mrd_sta_t *s)
{
    s->scanning = true;
    mrd_gfx_paint(d, MRD_C_BG, paint_sta, s);

    uint32_t       found = 0;
    const ag_err_t err = ag_wifi_scan(s->ap, MRD_WMAX, &found);
    s->scanning = false;
    if (err == AG_OK) {
        s->count = (found < MRD_WMAX) ? (int)found : MRD_WMAX;
        sort_sta(s);
        if (s->scroll > s->count - s->nvis) {
            s->scroll = s->count - s->nvis;
        }
        if (s->scroll < 0) {
            s->scroll = 0;
        }
    }
    (void)ag_wifi_status(&s->st);
    ag_printf("[mrd] sta scan: err=%d found=%u shown=%d state=%s\n", (int)err,
              found, s->count, state_name(s->st.state));
}

void mrd_screen_sta_scan(mrd_disp_t *d)
{
    if (!ag_wifi_available()) {
        wait_back(d, "Scan AP/STA", "no radio in this build (QEMU?)");
        return;
    }

    _Static_assert(sizeof(mrd_sta_t) <= MRD_SCRATCH_BYTES, "sta scratch");
    mrd_sta_t *const sp = (mrd_sta_t *)mrd_scratch();
#define s (*sp)
    memset(&s, 0, sizeof(s));
    s.W = d->W;
    s.H = d->H;
    s.nvis = sta_visible(d->H);

    /* The first scan raises the radio if it is down; from a resident app on
     * this board that can fail for want of a contiguous block - say so and
     * point at the workflow that fixes it. */
    do_scan(d, &s);
    if (s.count == 0 && s.st.state == AG_WIFI_ST_OFF) {
        wait_back(d, "Scan AP/STA", "no radio: run 'wifi on' first, then reopen");
        return;
    }

    uint32_t last_status = ag_millis();
    bool     running = true;
    bool     dirty = true;

    /* Swipe-to-scroll (same pattern as the wifimon list). */
    int  g_down_y = -1;
    int  g_scroll0 = 0;
    bool g_swipe = false;
    int  g_down_row = -1;

    while (running) {
        if (dirty) {
            mrd_gfx_paint(d, MRD_C_BG, paint_sta, &s);
            dirty = false;
        }

        ag_event_t ev;
        if (!ag_poll_event(&ev, 200)) {
            /* Refresh the station footer periodically (join progresses). */
            const uint32_t now = ag_millis();
            if (now - last_status >= 700u) {
                ag_wifi_status(&s.st);
                last_status = now;
                dirty = true;
            }
            ag_heartbeat();
            continue;
        }

        if (ev.type == AG_EV_QUIT) {
            running = false;
        } else if (ev.type == AG_EV_FOCUS_GAINED) {
            dirty = true;
        } else if (ev.type == AG_EV_KEY_DOWN) {
            if (is_back_key(ev.key.keycode)) {
                running = false;
            } else if (ev.key.keycode == AG_KEY_ENTER ||
                       ev.key.keycode == AG_KEY_SPACE) {
                do_scan(d, &s); /* rescan */
                dirty = true;
            } else if (ev.key.keycode == AG_KEY_UP) {
                if (s.scroll > 0) {
                    s.scroll--;
                }
                dirty = true;
            } else if (ev.key.keycode == AG_KEY_DOWN) {
                if (s.scroll + s.nvis < s.count) {
                    s.scroll++;
                }
                dirty = true;
            }
        } else if (ev.type == AG_EV_POINTER_DOWN) {
            int px, py;
            mrd_touch_to_px(d, ev.ptr.x, ev.ptr.y, &px, &py);
            const bool sb = s.count > s.nvis;
            if (py < MRD_STATUS_H) {
                running = false;
            } else if (sb && px >= s.W - MRD_SCROLLBAR_W - 4) {
                const int track = s.H - MRD_STATUS_H - 18;
                s.scroll = (py - MRD_STATUS_H) * s.count /
                           (track > 0 ? track : 1);
                dirty = true;
                g_down_y = -1;
            } else {
                g_down_y = py;
                g_scroll0 = s.scroll;
                g_swipe = false;
                /* which row, for a tap-to-connect on release */
                g_down_row = -1;
                if (py < s.H - 18) {
                    const int row = s.scroll + (py - MRD_STATUS_H) / MRD_WROW;
                    if (row >= 0 && row < s.count) {
                        g_down_row = row;
                    }
                }
            }
        } else if (ev.type == AG_EV_POINTER_MOVE) {
            if (g_down_y >= 0) {
                int px, py;
                mrd_touch_to_px(d, ev.ptr.x, ev.ptr.y, &px, &py);
                const int dy = py - g_down_y;
                if (!g_swipe && (dy > 24 || dy < -24)) {
                    g_swipe = true;
                }
                if (g_swipe) {
                    int maxs = s.count - s.nvis;
                    if (maxs < 0) {
                        maxs = 0;
                    }
                    int ns = g_scroll0 - dy / MRD_WROW;
                    if (ns < 0) {
                        ns = 0;
                    }
                    if (ns > maxs) {
                        ns = maxs;
                    }
                    if (ns != s.scroll) {
                        s.scroll = ns;
                        dirty = true;
                    }
                }
            }
        } else if (ev.type == AG_EV_POINTER_UP) {
            if (g_down_y >= 0 && !g_swipe && g_down_row >= 0 &&
                g_down_row < s.count) {
                const ag_wifi_ap_t *a = &s.ap[g_down_row];
                if (a->auth == AG_WIFI_SEC_OPEN && a->ssid[0] != '\0') {
                    /* Join an open network - the one a tap can do without a
                     * key.  Returns at once; the footer shows it progress. */
                    const ag_err_t cerr = ag_wifi_connect(a->ssid, "", NULL);
                    ag_printf("[mrd] sta connect \"%s\": err=%d\n", a->ssid,
                              (int)cerr);
                    ag_wifi_status(&s.st);
                    dirty = true;
                } else if (a->auth != AG_WIFI_SEC_OPEN) {
                    ag_printf("[mrd] \"%s\" needs a key - connect from the "
                              "shell: wifi connect\n",
                              a->ssid[0] ? a->ssid : "<hidden>");
                }
            }
            g_down_y = -1;
            g_swipe = false;
            g_down_row = -1;
        }
    }
    /* Radio left up between screens; torn down once at app exit (see ag_main). */
#undef s
}

/* ------------------------------------------------------------------------ */
/* WiFi > Attacks > Evil Portal > Access Points - the board's own AP         */
/* ------------------------------------------------------------------------ */

typedef struct {
    int                 W, H;
    ag_wifi_ap_status_t st;
    bool                up;
    char                ssid[AG_WIFI_SSID_MAX + 1];
} mrd_ap_t2;

static void paint_ap(mrd_band_t *b, void *ctx)
{
    mrd_ap_t2 *a = (mrd_ap_t2 *)ctx;
    char       buf[40];
    char      *e = buf + sizeof(buf);
    const int  ty0 = mrd_text_y(0, MRD_STATUS_H, MRD_TITLE_SCALE);

    mrd_band_fill(b, 0, 0, a->W, MRD_STATUS_H, MRD_C_BAR_BG);
    mrd_band_fill(b, 0, MRD_STATUS_H - 1, a->W, 1, MRD_C_LGREY);
    int tx = 6 + mrd_band_text(b, 6, ty0, "<", MRD_C_LGREY, 0xFFFFFFFFu,
                               MRD_TITLE_SCALE) + 4;
    mrd_band_text(b, tx, ty0, "Access Point", MRD_C_GREEN, 0xFFFFFFFFu,
                  MRD_TITLE_SCALE);

    int y = MRD_STATUS_H + 10;
    if (!a->up) {
        mrd_band_text(b, 10, y, "starting access point...", MRD_C_LGREY,
                      0xFFFFFFFFu, 1);
        return;
    }

    mrd_band_text(b, 10, y, "SSID", MRD_C_LGREY, 0xFFFFFFFFu, 1);
    mrd_band_text(b, 90, y, a->st.ssid[0] ? a->st.ssid : a->ssid, MRD_C_WHITE,
                  0xFFFFFFFFu, 1);
    y += 22;

    mrd_band_text(b, 10, y, "Security", MRD_C_LGREY, 0xFFFFFFFFu, 1);
    mrd_band_text(b, 90, y, a->st.secured ? "WPA2" : "open",
                  a->st.secured ? MRD_C_YELLOW : MRD_C_CYAN, 0xFFFFFFFFu, 1);
    y += 22;

    mrd_band_text(b, 10, y, "Channel", MRD_C_LGREY, 0xFFFFFFFFu, 1);
    char *p = put_uint(buf, e, a->st.channel);
    *p = '\0';
    mrd_band_text(b, 90, y, buf, MRD_C_WHITE, 0xFFFFFFFFu, 1);
    y += 22;

    mrd_band_text(b, 10, y, "Address", MRD_C_LGREY, 0xFFFFFFFFu, 1);
    p = buf;
    p = put_uint(p, e, (a->st.ip >> 24) & 0xffu);
    p = put(p, e, '.');
    p = put_uint(p, e, (a->st.ip >> 16) & 0xffu);
    p = put(p, e, '.');
    p = put_uint(p, e, (a->st.ip >> 8) & 0xffu);
    p = put(p, e, '.');
    p = put_uint(p, e, a->st.ip & 0xffu);
    *p = '\0';
    mrd_band_text(b, 90, y, buf, MRD_C_WHITE, 0xFFFFFFFFu, 1);
    y += 22;

    mrd_band_text(b, 10, y, "Clients", MRD_C_LGREY, 0xFFFFFFFFu, 1);
    p = put_uint(buf, e, a->st.clients);
    *p = '\0';
    mrd_band_text(b, 90, y, buf, MRD_C_GREEN, 0xFFFFFFFFu, 1);
    y += 30;

    mrd_band_text(b, 10, y, "join it, then open the address", MRD_C_LGREY,
                  0xFFFFFFFFu, 1);
    y += 16;
    mrd_band_text(b, 10, y, "in a browser.  Back stops it.", MRD_C_LGREY,
                  0xFFFFFFFFu, 1);
}

void mrd_screen_softap(mrd_disp_t *d)
{
    if (!AG_HAS(ag_api()->wifi, ap_start)) {
        wait_back(d, "Access Point",
                  "no access point in this build (ARGON_NET_WIFI_AP)");
        return;
    }

    static mrd_ap_t2 a;
    memset(&a, 0, sizeof(a));
    a.W = d->W;
    a.H = d->H;

    /* A fixed, obviously-ours name.  Open, so a phone joins it with one tap and
     * the address is reachable in a browser - the Evil Portal shape. */
    {
        char *p = a.ssid;
        char *e = a.ssid + sizeof(a.ssid);
        p = put_str(p, e, "ArgonOS-AP");
        *p = '\0';
    }

    a.up = false;
    mrd_gfx_paint(d, MRD_C_BG, paint_ap, &a);

    const ag_err_t err = ag_wifi_ap_start(a.ssid, "", 0, false);
    if (err != AG_OK) {
        ag_printf("[mrd] ap start \"%s\": err=%d\n", a.ssid, (int)err);
        wait_back(d, "Access Point",
                  err == -AG_ENOMEM
                      ? "no radio: run 'wifi on' first, then reopen"
                      : "could not start the access point");
        return;
    }
    a.up = true;
    (void)ag_wifi_ap_status(&a.st);
    ag_printf("[mrd] ap up \"%s\" ch=%u ip=%u.%u.%u.%u\n",
              a.st.ssid[0] ? a.st.ssid : a.ssid, a.st.channel,
              (a.st.ip >> 24) & 0xffu, (a.st.ip >> 16) & 0xffu,
              (a.st.ip >> 8) & 0xffu, a.st.ip & 0xffu);

    uint32_t last = 0;
    bool     running = true;
    bool     dirty = true;
    while (running) {
        if (dirty) {
            mrd_gfx_paint(d, MRD_C_BG, paint_ap, &a);
            dirty = false;
        }
        ag_event_t ev;
        if (ag_poll_event(&ev, 200)) {
            if (ev.type == AG_EV_QUIT) {
                running = false;
            } else if (ev.type == AG_EV_FOCUS_GAINED) {
                dirty = true;
            } else if (ev.type == AG_EV_KEY_DOWN) {
                if (is_back_key(ev.key.keycode)) {
                    running = false;
                }
            } else if (ev.type == AG_EV_POINTER_DOWN) {
                int px, py;
                mrd_touch_to_px(d, ev.ptr.x, ev.ptr.y, &px, &py);
                if (py < MRD_STATUS_H) {
                    running = false;
                }
            }
        }
        const uint32_t now = ag_millis();
        if (now - last >= 1000u) { /* poll for clients joining */
            ag_wifi_ap_status(&a.st);
            last = now;
            dirty = true;
        }
        ag_heartbeat();
    }

    (void)ag_wifi_ap_stop();
    ag_printf("[mrd] ap stopped\n");
    /* The station radio is left up (other screens may use it); only the point
     * is torn down here, which is what Back on this screen means. */
}

/* ------------------------------------------------------------------------ */
/* Headless self-test: `marauder wifitest`                                   */
/* ------------------------------------------------------------------------ */

void mrd_wifi_selftest(void)
{
    ag_printf("[mrd] wifi selftest start\n");
    if (!ag_wifi_available()) {
        ag_printf("[mrd] wifi: api->wifi is NULL (no radio in this build)\n");
        return;
    }

    ag_printf("[mrd] wifi caps: ap=%d espnow=%d\n",
              AG_HAS(ag_api()->wifi, ap_start) ? 1 : 0,
              AG_HAS(ag_api()->wifi, espnow_start) ? 1 : 0);

    /* start (raise the radio) */
    ag_err_t err = ag_wifi_start();
    ag_printf("[mrd] wifi start: err=%d\n", (int)err);
    if (err == -AG_ENOMEM) {
        ag_printf("[mrd] wifi: run 'wifi on' before 'run marauder wifitest'\n");
        return;
    }

    /* status */
    ag_wifi_status_t st;
    if (ag_wifi_status(&st) == AG_OK) {
        ag_printf("[mrd] wifi status: state=%s ssid=\"%s\" ch=%u\n",
                  state_name(st.state), st.ssid, st.channel);
    }

    /* scan (into the shared screen scratch, not its own bss) */
    _Static_assert(sizeof(ag_wifi_ap_t) * MRD_WMAX <= MRD_SCRATCH_BYTES,
                   "selftest scratch");
    ag_wifi_ap_t *aps = (ag_wifi_ap_t *)mrd_scratch();
    uint32_t      found = 0;
    err = ag_wifi_scan(aps, MRD_WMAX, &found);
    ag_printf("[mrd] wifi scan: err=%d found=%u\n", (int)err, found);
    const uint32_t show = (found < 8u) ? found : 8u;
    for (uint32_t i = 0; i < show; i++) {
        ag_printf("[mrd]   ssid=\"%s\" ch=%u rssi=%d sec=%s\n",
                  aps[i].ssid[0] ? aps[i].ssid : "<hidden>",
                  (uint32_t)aps[i].channel, (int)aps[i].rssi,
                  sec_name(aps[i].auth));
    }

    /* access point */
    if (AG_HAS(ag_api()->wifi, ap_start)) {
        err = ag_wifi_ap_start("ArgonOS-Test", "", 0, false);
        ag_printf("[mrd] wifi ap_start: err=%d\n", (int)err);
        if (err == AG_OK) {
            ag_wifi_ap_status_t ap;
            if (ag_wifi_ap_status(&ap) == AG_OK) {
                ag_printf("[mrd] wifi ap: on=%d ssid=\"%s\" ch=%u "
                          "ip=%u.%u.%u.%u clients=%u\n",
                          ap.on ? 1 : 0, ap.ssid, (uint32_t)ap.channel,
                          (ap.ip >> 24) & 0xffu, (ap.ip >> 16) & 0xffu,
                          (ap.ip >> 8) & 0xffu, ap.ip & 0xffu, ap.clients);
            }
            ag_delay(300);
            err = ag_wifi_ap_stop();
            ag_printf("[mrd] wifi ap_stop: err=%d\n", (int)err);
        }
    }

    /* ESP-NOW: self address, start, stop (a second board is needed to send) */
    if (AG_HAS(ag_api()->wifi, espnow_start)) {
        err = ag_wifi_espnow_start();
        ag_printf("[mrd] wifi espnow_start: err=%d\n", (int)err);
        if (err == AG_OK) {
            uint8_t mac[6] = {0};
            if (ag_wifi_espnow_self(mac) == AG_OK) {
                char  mb[18];
                char *mp = mb;
                char *me = mb + sizeof(mb);
                for (int k = 0; k < 6; k++) {
                    if (k) {
                        mp = put(mp, me, ':');
                    }
                    mp = put_hex2(mp, me, mac[k]);
                }
                *mp = '\0';
                ag_printf("[mrd] wifi espnow self: %s\n", mb);
            }
            err = ag_wifi_espnow_stop();
            ag_printf("[mrd] wifi espnow_stop: err=%d\n", (int)err);
        }
    }

    ag_printf("[mrd] wifi selftest done\n");
}
