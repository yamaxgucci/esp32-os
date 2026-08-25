/*
 * MARAUDER - WiFi > Sniffers > Scan AP/STA.
 *
 * The one screen taken all the way through in this first slice.  It does what
 * the upstream sniffer does: it turns the radio into a receiver on one channel,
 * hops the channel, and reads the beacons and probe responses in the air,
 * building a list of the access points around it - SSID, channel, RSSI and
 * BSSID - the authentic Marauder way, through api->wifimon (ABI 0.38).
 *
 * api->wifimon is NULL unless the firmware was built with
 * CONFIG_ARGON_NET_WIFI_MON (off by default).  When it is, the screen says so
 * and offers Back rather than failing silently.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mrd.h"
#include <argon/keys.h>

#define MRD_MAX_AP     16 /* bounded: shares the screen scratch, and RAM is scarce */
#define MRD_ROW_H      34 /* two 8x16 lines: SSID line + MAC line */
#define MRD_HOP_MS     300
#define MRD_REPAINT_MS 400

/* What a capture-list screen is collecting. */
enum { CAP_APS = 0, CAP_PROBES = 1 };

typedef struct {
    uint8_t mac[6]; /* BSSID (APs) or client address (probe requests) */
    char    ssid[33];
    int8_t  rssi;
    uint8_t channel;
} mrd_ap_t;

typedef struct {
    int      kind;              /* CAP_APS | CAP_PROBES */
    mrd_ap_t ap[MRD_MAX_AP];
    int      count;
    int      order[MRD_MAX_AP]; /* indices sorted by RSSI, strongest first */
    uint8_t  channel;           /* channel currently listened on */
    int      scroll;
    int      W, H;
    int      nvis;
    uint32_t free_kb;
} mrd_scan_t;

/* ---- tiny string builders (the SDK has no libc) ------------------------- */

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

/* ---- 802.11 beacon / probe-response parsing ----------------------------- */

/*
 * The frame handed up starts at the 802.11 MAC header (frame control first).
 * Management beacon = type 0, subtype 8; probe response = subtype 5.  BSSID is
 * addr3 (offset 16).  The fixed body is 12 bytes; tagged parameters start at
 * offset 36 - SSID is tag 0, the DS channel is tag 3.
 */
static void merge_ap(mrd_scan_t *s, const uint8_t *f, int n, int8_t rssi,
                     uint8_t meta_ch)
{
    if (n < 24) {
        return;
    }
    const uint8_t fc0 = f[0];
    const int type = (fc0 >> 2) & 0x3;
    const int subtype = (fc0 >> 4) & 0xf;

    const uint8_t *mac;
    int            tag0;
    if (s->kind == CAP_PROBES) {
        if (type != 0 || subtype != 4) {
            return; /* not a probe request */
        }
        mac = &f[10];  /* addr2 = the client asking */
        tag0 = 24;     /* probe request body is tagged params from here */
    } else {
        if (type != 0 || (subtype != 8 && subtype != 5)) {
            return; /* not a beacon or probe response */
        }
        mac = &f[16];  /* addr3 = BSSID */
        tag0 = 36;     /* after the 12-byte fixed beacon body */
    }

    char    ssid[33];
    int     ssid_len = 0;
    uint8_t channel = meta_ch;

    int p = tag0;
    while (p + 2 <= n) {
        const int tag = f[p];
        const int len = f[p + 1];
        const int val = p + 2;
        if (val + len > n) {
            break;
        }
        if (tag == 0) { /* SSID */
            ssid_len = len > 32 ? 32 : len;
            for (int i = 0; i < ssid_len; i++) {
                const uint8_t c = f[val + i];
                ssid[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
            }
        } else if (tag == 3 && len >= 1) { /* DS parameter set: channel */
            channel = f[val];
        }
        p += 2 + len;
    }
    ssid[ssid_len] = '\0';

    /*
     * Dedup: access points by their address alone (one BSSID is one AP, and a
     * hidden one that later names itself should update in place); probe requests
     * by address *and* SSID, since one client asks for several networks.
     */
    const bool by_ssid = (s->kind == CAP_PROBES);
    for (int i = 0; i < s->count; i++) {
        mrd_ap_t *a = &s->ap[i];
        bool same = true;
        for (int k = 0; k < 6; k++) {
            if (a->mac[k] != mac[k]) {
                same = false;
                break;
            }
        }
        if (same && by_ssid) {
            for (int j = 0; j <= ssid_len; j++) {
                if (a->ssid[j] != ssid[j]) {
                    same = false;
                    break;
                }
            }
        }
        if (!same) {
            continue;
        }
        a->rssi = rssi;
        if (channel) {
            a->channel = channel;
        }
        if (ssid_len > 0 && a->ssid[0] == '\0') {
            for (int j = 0; j <= ssid_len; j++) {
                a->ssid[j] = ssid[j];
            }
        }
        return;
    }

    if (s->count >= MRD_MAX_AP) {
        return;
    }
    mrd_ap_t *a = &s->ap[s->count++];
    for (int k = 0; k < 6; k++) {
        a->mac[k] = mac[k];
    }
    for (int j = 0; j <= ssid_len; j++) {
        a->ssid[j] = ssid[j];
    }
    a->rssi = rssi;
    a->channel = channel;
}

/* Order indices by RSSI, strongest first (insertion sort; the list is short). */
static void sort_aps(mrd_scan_t *s)
{
    for (int i = 0; i < s->count; i++) {
        s->order[i] = i;
    }
    for (int i = 1; i < s->count; i++) {
        const int key = s->order[i];
        int j = i - 1;
        while (j >= 0 && s->ap[s->order[j]].rssi < s->ap[key].rssi) {
            s->order[j + 1] = s->order[j];
            j--;
        }
        s->order[j + 1] = key;
    }
}

/* ---- rendering ---------------------------------------------------------- */

static int scan_visible(int H) { return (H - MRD_STATUS_H) / MRD_ROW_H; }

static void paint_scan(mrd_band_t *b, void *ctx)
{
    mrd_scan_t *s = (mrd_scan_t *)ctx;
    char buf[64];
    char *end = buf + sizeof(buf);

    const bool sb = s->count > s->nvis;
    const int  rext = sb ? (MRD_SCROLLBAR_W + 2) : 0; /* right margin for bar */
    const int  ty0 = mrd_text_y(0, MRD_STATUS_H, MRD_TITLE_SCALE);

    const char *title = (s->kind == CAP_PROBES) ? "Probe Reqs" : "Scan APs";

    /* Status bar: "< <title>" on the left, "n: ch: free" on the right. */
    mrd_band_fill(b, 0, 0, s->W, MRD_STATUS_H, MRD_C_BAR_BG);
    mrd_band_fill(b, 0, MRD_STATUS_H - 1, s->W, 1, MRD_C_LGREY);
    int tx = 6 + mrd_band_text(b, 6, ty0, "<", MRD_C_LGREY, 0xFFFFFFFFu,
                               MRD_TITLE_SCALE) + 4;
    mrd_band_text(b, tx, ty0, title, MRD_C_LIME, 0xFFFFFFFFu, MRD_TITLE_SCALE);

    char *p = buf;
    p = put_str(p, end, "n:");
    p = put_uint(p, end, (uint32_t)s->count);
    p = put_str(p, end, " ch:");
    p = put_uint(p, end, s->channel);
    p = put_str(p, end, " ");
    p = put_uint(p, end, s->free_kb);
    p = put_str(p, end, "k");
    *p = '\0';
    mrd_band_text(b, s->W - mrd_text_w(buf, 1) - rext - 4, ty0, buf,
                  MRD_C_LGREY, 0xFFFFFFFFu, 1);

    if (s->count == 0) {
        const char *waiting = (s->kind == CAP_PROBES)
                                  ? "listening for probe requests..."
                                  : "listening for beacons...";
        mrd_band_text(b, 8, MRD_STATUS_H + 8, waiting, MRD_C_LGREY,
                      0xFFFFFFFFu, 1);
        return;
    }

    for (int i = 0; i < s->nvis; i++) {
        const int oi = s->scroll + i;
        if (oi >= s->count) {
            break;
        }
        const mrd_ap_t *a = &s->ap[s->order[oi]];
        const int y = MRD_STATUS_H + i * MRD_ROW_H + 2;

        /* Colour by signal, like Marauder: strong green, weak red. */
        uint16_t col = MRD_C_GREEN;
        if (a->rssi < -80) {
            col = MRD_C_RED;
        } else if (a->rssi < -67) {
            col = MRD_C_YELLOW;
        }

        /* Line 1: SSID (or hidden/any), then channel and RSSI on the right. */
        const char *empty = (s->kind == CAP_PROBES) ? "<any>" : "<hidden>";
        const char *name = (a->ssid[0] != '\0') ? a->ssid : empty;
        mrd_band_text(b, 6, y, name, MRD_C_TEXT, 0xFFFFFFFFu, 1);

        p = buf;
        p = put_str(p, end, "c");
        p = put_uint(p, end, a->channel);
        p = put_str(p, end, "  ");
        p = put(p, end, '-');
        p = put_uint(p, end, (uint32_t)(-(int)a->rssi));
        *p = '\0';
        mrd_band_text(b, s->W - mrd_text_w(buf, 1) - rext - 6, y, buf, col,
                      0xFFFFFFFFu, 1);

        /* Line 2: the address (BSSID or client MAC), dimmed. */
        p = buf;
        for (int k = 0; k < 6; k++) {
            if (k) {
                p = put(p, end, ':');
            }
            p = put_hex2(p, end, a->mac[k]);
        }
        *p = '\0';
        mrd_band_text(b, 6, y + 16, buf, MRD_C_LGREY, 0xFFFFFFFFu, 1);

        mrd_band_fill(b, 0, y + MRD_ROW_H - 3, s->W - rext, 1, 0x2104);
    }

    /* Scrollbar on the right, thumb sized/placed by scroll position. */
    if (sb) {
        const int sx = s->W - MRD_SCROLLBAR_W;
        const int track = s->H - MRD_STATUS_H;
        mrd_band_fill(b, sx, MRD_STATUS_H, MRD_SCROLLBAR_W, track,
                      MRD_C_BTN_BG);
        int th = track * s->nvis / s->count;
        if (th < 14) {
            th = 14;
        }
        const int span = s->count - s->nvis;
        int thy = MRD_STATUS_H + (span > 0 ? (track - th) * s->scroll / span : 0);
        mrd_band_fill(b, sx, thy, MRD_SCROLLBAR_W, th, MRD_C_LGREY);
    }
}

/* A one-line message screen (wifimon absent, etc.) with Back. */
static void paint_msg(mrd_band_t *b, void *ctx)
{
    const char **m = (const char **)ctx;
    /* m[0] = title, m[1] = message, m[2] = (int)W as string is awkward; pass via
     * a small struct instead is cleaner, but two static-life strings suffice. */
    const int ty0 = mrd_text_y(0, MRD_STATUS_H, MRD_TITLE_SCALE);
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
    bool dirty = true;
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

static uint32_t free_kb(void)
{
    ag_meminfo_t mi;
    ag_meminfo(&mi);
    return (uint32_t)(mi.system_free / 1024u);
}

static void capture_list(mrd_disp_t *d, int kind)
{
    const char *title = (kind == CAP_PROBES) ? "Probe Reqs" : "Scan APs";

    if (!ag_wifimon_available()) {
        wait_back(d, title,
                  "wifimon off: build with CONFIG_ARGON_NET_WIFI_MON");
        return;
    }

    const ag_err_t serr = ag_wifimon_start();
    if (serr != AG_OK) {
        ag_printf("[mrd] %s: wifimon start failed, err=%d\n", title, (int)serr);
        wait_back(d, title, "no radio: run 'wifi on' first, then reopen");
        return;
    }
    (void)ag_wifimon_filter(AG_WIFIMON_MGMT);

    /* One shared scratch, not a per-screen static: only one screen is live. */
    _Static_assert(sizeof(mrd_scan_t) <= MRD_SCRATCH_BYTES, "scan scratch");
    mrd_scan_t *const sp = (mrd_scan_t *)mrd_scratch();
#define s (*sp)
    s.kind = kind;
    s.count = 0;
    s.scroll = 0;
    s.W = d->W;
    s.H = d->H;
    s.nvis = scan_visible(d->H);
    s.channel = 1;
    s.free_kb = free_kb();
    (void)ag_wifimon_channel(s.channel);

    uint8_t  buf[AG_WIFIMON_SNAP];
    uint32_t last_hop = ag_millis();
    uint32_t last_paint = 0;
    bool     running = true;

    /* Swipe-to-scroll state (see the menu for the same pattern). */
    int  g_down_y = -1;
    int  g_scroll0 = 0;
    bool g_swipe = false;

    while (running) {
        /* Drain a few frames from the ring. */
        for (int i = 0; i < 16; i++) {
            ag_wifimon_frame_t meta;
            const int32_t n = ag_wifimon_recv(buf, sizeof(buf), &meta, 20);
            if (n <= 0) {
                break;
            }
            merge_ap(&s, buf, (int)n, meta.rssi,
                     meta.channel ? meta.channel : s.channel);
        }

        const uint32_t now = ag_millis();

        if (now - last_hop >= MRD_HOP_MS) {
            s.channel = (uint8_t)(s.channel % 13 + 1);
            (void)ag_wifimon_channel(s.channel);
            last_hop = now;
        }

        /* Input, non-blocking so the capture keeps running. */
        ag_event_t ev;
        while (ag_poll_event(&ev, 0)) {
            if (ev.type == AG_EV_QUIT) {
                running = false;
                break;
            }
            if (ev.type == AG_EV_FOCUS_GAINED) {
                last_paint = 0; /* force repaint */
            } else if (ev.type == AG_EV_KEY_DOWN) {
                switch (ev.key.keycode) {
                case AG_KEY_ESC:
                case AG_KEY_LEFT:
                case AG_KEY_BACKSPACE:
                    running = false;
                    break;
                case AG_KEY_UP:
                    if (s.scroll > 0) {
                        s.scroll--;
                    }
                    last_paint = 0;
                    break;
                case AG_KEY_DOWN:
                    if (s.scroll + s.nvis < s.count) {
                        s.scroll++;
                    }
                    last_paint = 0;
                    break;
                default:
                    break;
                }
            } else if (ev.type == AG_EV_POINTER_DOWN) {
                int px, py;
                mrd_touch_to_px(d, ev.ptr.x, ev.ptr.y, &px, &py);
                const bool sb = s.count > s.nvis;
                if (py < MRD_STATUS_H) {
                    running = false; /* the whole status bar is Back */
                } else if (sb && px >= s.W - MRD_SCROLLBAR_W - 4) {
                    /* Scrollbar: jump so the tapped fraction is at the top. */
                    const int track = s.H - MRD_STATUS_H;
                    s.scroll = (py - MRD_STATUS_H) * s.count /
                               (track > 0 ? track : 1);
                    last_paint = 0;
                    g_down_y = -1;
                } else {
                    g_down_y = py; /* arm a swipe over the list */
                    g_scroll0 = s.scroll;
                    g_swipe = false;
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
                        int ns = g_scroll0 - dy / MRD_ROW_H;
                        if (ns < 0) {
                            ns = 0;
                        }
                        if (ns > maxs) {
                            ns = maxs;
                        }
                        if (ns != s.scroll) {
                            s.scroll = ns;
                            last_paint = 0;
                        }
                    }
                }
            } else if (ev.type == AG_EV_POINTER_UP) {
                g_down_y = -1;
                g_swipe = false;
            }
        }

        if (running && (now - last_paint >= MRD_REPAINT_MS)) {
            s.free_kb = free_kb();
            sort_aps(&s);
            if (s.scroll > s.count - s.nvis) {
                s.scroll = s.count - s.nvis;
            }
            if (s.scroll < 0) {
                s.scroll = 0;
            }
            mrd_gfx_paint(d, MRD_C_BG, paint_scan, &s);
            ag_printf("[mrd] %s n=%d ch=%d free=%uk\n", title, s.count,
                      s.channel, s.free_kb);
            last_paint = now;
        }

        ag_heartbeat();
    }

    /*
     * The radio is deliberately left running between screens.  Starting and
     * stopping it per screen re-ran the interface bring-up each time, which
     * leaked netif handlers ("handler already registered", "duplicate key")
     * until the interface failed with ENOMEM and the process faulted.  It is
     * torn down once, when the application exits (see ag_main).
     */
#undef s
}

void mrd_screen_scan_ap(mrd_disp_t *d) { capture_list(d, CAP_APS); }
void mrd_screen_probe_req(mrd_disp_t *d) { capture_list(d, CAP_PROBES); }

/* True for the keys that mean "leave this screen". */
static bool is_back_key(uint16_t kc)
{
    return kc == AG_KEY_ESC || kc == AG_KEY_LEFT || kc == AG_KEY_BACKSPACE;
}

/* ------------------------------------------------------------------------ */
/* WiFi > Sniffers > Packet Monitor                                          */
/* ------------------------------------------------------------------------ */

typedef struct {
    int      W, H;
    uint32_t c[AG_WIFIMON_C_N];
    uint32_t dropped;
    uint32_t rate; /* total frames in the last one-second window */
    uint8_t  channel;
    uint32_t free_kb;
} mrd_mon_t;

static void row_kv(mrd_band_t *b, int y, const char *k, uint32_t v,
                   uint16_t col, int W)
{
    char  buf[16];
    char *e = buf + sizeof(buf);
    char *p = put_uint(buf, e, v);
    *p = '\0';
    mrd_band_text(b, 12, y, k, MRD_C_LGREY, 0xFFFFFFFFu, 1);
    mrd_band_text(b, W - mrd_text_w(buf, 1) - 14, y, buf, col, 0xFFFFFFFFu, 1);
}

static void paint_mon(mrd_band_t *b, void *ctx)
{
    mrd_mon_t *m = (mrd_mon_t *)ctx;
    char       buf[24];
    char      *e = buf + sizeof(buf);
    const int  ty0 = mrd_text_y(0, MRD_STATUS_H, MRD_TITLE_SCALE);

    mrd_band_fill(b, 0, 0, m->W, MRD_STATUS_H, MRD_C_BAR_BG);
    mrd_band_fill(b, 0, MRD_STATUS_H - 1, m->W, 1, MRD_C_LGREY);
    int tx = 6 + mrd_band_text(b, 6, ty0, "<", MRD_C_LGREY, 0xFFFFFFFFu,
                               MRD_TITLE_SCALE) + 4;
    mrd_band_text(b, tx, ty0, "Packet Monitor", MRD_C_BLUE, 0xFFFFFFFFu,
                  MRD_TITLE_SCALE);

    char *p = buf;
    p = put_str(p, e, "ch:");
    p = put_uint(p, e, m->channel);
    p = put_str(p, e, " ");
    p = put_uint(p, e, m->free_kb);
    p = put_str(p, e, "k");
    *p = '\0';
    mrd_band_text(b, m->W - mrd_text_w(buf, 1) - 4, ty0, buf, MRD_C_LGREY,
                  0xFFFFFFFFu, 1);

    int y = MRD_STATUS_H + 8;

    /* Total, big. */
    p = buf;
    p = put_uint(p, e, m->c[AG_WIFIMON_C_TOTAL]);
    *p = '\0';
    mrd_band_text(b, 12, y + 6, "frames", MRD_C_LGREY, 0xFFFFFFFFu, 1);
    mrd_band_text(b, m->W - mrd_text_w(buf, 2) - 14, y, buf, MRD_C_WHITE,
                  0xFFFFFFFFu, 2);
    y += 40;

    /* Rate and a bar (capped at 200/s for the scale). */
    p = buf;
    p = put_uint(p, e, m->rate);
    p = put_str(p, e, " /s");
    *p = '\0';
    mrd_band_text(b, 12, y, buf, MRD_C_GREEN, 0xFFFFFFFFu, 1);
    {
        const int bw = m->W - 24;
        uint32_t  r = m->rate > 200 ? 200 : m->rate;
        mrd_band_frame(b, 12, y + 18, bw, 10, MRD_C_LGREY);
        mrd_band_fill(b, 12, y + 18, (int)(bw * r / 200), 10, MRD_C_GREEN);
    }
    y += 40;

    row_kv(b, y, "Mgmt", m->c[AG_WIFIMON_C_MGMT], MRD_C_CYAN, m->W);
    y += 20;
    row_kv(b, y, "Ctrl", m->c[AG_WIFIMON_C_CTRL], MRD_C_YELLOW, m->W);
    y += 20;
    row_kv(b, y, "Data", m->c[AG_WIFIMON_C_DATA], MRD_C_GREEN, m->W);
    y += 20;
    row_kv(b, y, "Misc", m->c[AG_WIFIMON_C_MISC], MRD_C_LGREY, m->W);
    y += 20;
    row_kv(b, y, "Dropped", m->dropped, MRD_C_RED, m->W);
}

void mrd_screen_pkt_mon(mrd_disp_t *d)
{
    if (!ag_wifimon_available()) {
        wait_back(d, "Packet Monitor",
                  "wifimon off: build with CONFIG_ARGON_NET_WIFI_MON");
        return;
    }
    const ag_err_t serr = ag_wifimon_start();
    if (serr != AG_OK) {
        ag_printf("[mrd] Packet Monitor: wifimon start failed, err=%d\n",
                  (int)serr);
        wait_back(d, "Packet Monitor", "no radio: run 'wifi on' first, then reopen");
        return;
    }
    (void)ag_wifimon_filter(AG_WIFIMON_ALL);

    static mrd_mon_t m;
    memset(&m, 0, sizeof(m));
    m.W = d->W;
    m.H = d->H;
    m.channel = 1;
    (void)ag_wifimon_channel(m.channel);

    uint8_t  buf[AG_WIFIMON_SNAP];
    uint32_t last_hop = ag_millis();
    uint32_t last_paint = 0;
    uint32_t last_rate = ag_millis();
    uint32_t prev_total = 0;
    bool     running = true;

    while (running) {
        /* Drain frames so `dropped` reflects real loss, not our own laziness. */
        for (int i = 0; i < 32; i++) {
            if (ag_wifimon_recv(buf, sizeof(buf), NULL, 10) <= 0) {
                break;
            }
        }
        const uint32_t now = ag_millis();
        if (now - last_hop >= MRD_HOP_MS) {
            m.channel = (uint8_t)(m.channel % 13 + 1);
            (void)ag_wifimon_channel(m.channel);
            last_hop = now;
        }

        ag_event_t ev;
        while (ag_poll_event(&ev, 0)) {
            if (ev.type == AG_EV_QUIT) {
                running = false;
                break;
            }
            if (ev.type == AG_EV_FOCUS_GAINED) {
                last_paint = 0;
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

        if (now - last_rate >= 1000) {
            ag_wifimon_counters(m.c);
            const uint32_t t = m.c[AG_WIFIMON_C_TOTAL];
            m.rate = t - prev_total;
            prev_total = t;
            last_rate = now;
        }
        if (running && (now - last_paint >= MRD_REPAINT_MS)) {
            ag_wifimon_counters(m.c);
            m.dropped = ag_wifimon_dropped();
            m.free_kb = free_kb();
            mrd_gfx_paint(d, MRD_C_BG, paint_mon, &m);
            ag_printf("[mrd] mon total=%u rate=%u/s mgmt=%u data=%u free=%uk\n",
                      m.c[AG_WIFIMON_C_TOTAL], m.rate, m.c[AG_WIFIMON_C_MGMT],
                      m.c[AG_WIFIMON_C_DATA], m.free_kb);
            last_paint = now;
        }
        ag_heartbeat();
    }
    /*
     * The radio is deliberately left running between screens.  Starting and
     * stopping it per screen re-ran the interface bring-up each time, which
     * leaked netif handlers ("handler already registered", "duplicate key")
     * until the interface failed with ENOMEM and the process faulted.  It is
     * torn down once, when the application exits (see ag_main).
     */
}

/* ------------------------------------------------------------------------ */
/* WiFi > Attacks > Beacon Spam                                              */
/* ------------------------------------------------------------------------ */

/*
 * Obviously-fake names, and a locally-administered BSSID derived from each (bit
 * 1 of the first octet set), so nothing here impersonates real hardware - the
 * same rule the shell's `mon beacon` follows.
 */
static const char *k_spam_ssid[] = {
    "Free WiFi",       "Pretty Fly for WiFi", "FBI Surveillance Van",
    "Loading...",      "Mom Click Here",      "Tell My WiFi Love Her",
    "Drop It Hotspot", "Abraham Linksys",
};
#define SPAM_N ((int)(sizeof(k_spam_ssid) / sizeof(k_spam_ssid[0])))

static uint32_t build_beacon(uint8_t *b, const char *ssid, uint8_t channel)
{
    uint32_t n = 0;
    size_t   sl = 0;
    while (ssid[sl]) {
        sl++;
    }
    const uint8_t slen = (sl > 32u) ? 32u : (uint8_t)sl;

    b[n++] = 0x80; /* mgmt / beacon */
    b[n++] = 0x00;
    b[n++] = 0x00;
    b[n++] = 0x00;
    for (int i = 0; i < 6; i++) {
        b[n++] = 0xff; /* addr1 broadcast */
    }
    uint8_t bssid[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
    for (size_t i = 0; i < sl; i++) {
        bssid[3 + (i % 3u)] ^= (uint8_t)ssid[i];
    }
    for (int i = 0; i < 6; i++) {
        b[n++] = bssid[i]; /* addr2 */
    }
    for (int i = 0; i < 6; i++) {
        b[n++] = bssid[i]; /* addr3 */
    }
    b[n++] = 0x00; /* sequence */
    b[n++] = 0x00;
    for (int i = 0; i < 8; i++) {
        b[n++] = 0x00; /* timestamp */
    }
    b[n++] = 0x64; /* beacon interval 100 TU */
    b[n++] = 0x00;
    b[n++] = 0x01; /* capabilities: ESS, short slot */
    b[n++] = 0x04;
    b[n++] = 0x00; /* tag 0: SSID */
    b[n++] = slen;
    for (int i = 0; i < slen; i++) {
        b[n++] = (uint8_t)ssid[i];
    }
    b[n++] = 0x01; /* tag 1: supported rates */
    b[n++] = 0x04;
    b[n++] = 0x82;
    b[n++] = 0x84;
    b[n++] = 0x8b;
    b[n++] = 0x96;
    b[n++] = 0x03; /* tag 3: DS (channel) */
    b[n++] = 0x01;
    b[n++] = channel;
    return n;
}

typedef struct {
    int      W, H;
    uint32_t sent;
    uint8_t  channel;
} mrd_spam_t;

static void paint_spam(mrd_band_t *b, void *ctx)
{
    mrd_spam_t *s = (mrd_spam_t *)ctx;
    char        buf[16];
    char       *e = buf + sizeof(buf);
    const int   ty0 = mrd_text_y(0, MRD_STATUS_H, MRD_TITLE_SCALE);

    mrd_band_fill(b, 0, 0, s->W, MRD_STATUS_H, MRD_C_BAR_BG);
    mrd_band_fill(b, 0, MRD_STATUS_H - 1, s->W, 1, MRD_C_LGREY);
    int tx = 6 + mrd_band_text(b, 6, ty0, "<", MRD_C_LGREY, 0xFFFFFFFFu,
                               MRD_TITLE_SCALE) + 4;
    mrd_band_text(b, tx, ty0, "Beacon Spam", MRD_C_ORANGE, 0xFFFFFFFFu,
                  MRD_TITLE_SCALE);

    char *p = buf;
    p = put_str(p, e, "ch:");
    p = put_uint(p, e, s->channel);
    *p = '\0';
    mrd_band_text(b, s->W - mrd_text_w(buf, 1) - 4, ty0, buf, MRD_C_LGREY,
                  0xFFFFFFFFu, 1);

    int y = MRD_STATUS_H + 8;
    p = buf;
    p = put_uint(p, e, s->sent);
    *p = '\0';
    mrd_band_text(b, 12, y + 6, "sent", MRD_C_LGREY, 0xFFFFFFFFu, 1);
    mrd_band_text(b, s->W - mrd_text_w(buf, 2) - 14, y, buf, MRD_C_ORANGE,
                  0xFFFFFFFFu, 2);
    y += 42;

    mrd_band_text(b, 12, y, "broadcasting:", MRD_C_LGREY, 0xFFFFFFFFu, 1);
    y += 20;
    for (int i = 0; i < SPAM_N; i++) {
        if (y + 16 > s->H) {
            break;
        }
        mrd_band_text(b, 18, y, k_spam_ssid[i], MRD_C_TEXT, 0xFFFFFFFFu, 1);
        y += 18;
    }
}

void mrd_screen_beacon_spam(mrd_disp_t *d)
{
    if (!ag_wifimon_available()) {
        wait_back(d, "Beacon Spam",
                  "wifimon off: build with CONFIG_ARGON_NET_WIFI_MON");
        return;
    }
    const ag_err_t serr = ag_wifimon_start();
    if (serr != AG_OK) {
        ag_printf("[mrd] Beacon Spam: wifimon start failed, err=%d\n",
                  (int)serr);
        wait_back(d, "Beacon Spam", "no radio: run 'wifi on' first, then reopen");
        return;
    }

    static const uint8_t k_ch[3] = {1, 6, 11};
    static mrd_spam_t     s;
    memset(&s, 0, sizeof(s));
    s.W = d->W;
    s.H = d->H;
    s.channel = 1;

    uint8_t  frame[128];
    uint32_t last_paint = 0;
    int      chi = 0;
    bool     running = true;

    while (running) {
        /* One sweep: every fake SSID on the current channel, a couple of
         * frames each, then move to the next channel. */
        s.channel = k_ch[chi];
        (void)ag_wifimon_channel(s.channel);
        for (int i = 0; i < SPAM_N; i++) {
            const uint32_t len = build_beacon(frame, k_spam_ssid[i], s.channel);
            for (int r = 0; r < 2; r++) {
                if (ag_wifimon_tx(frame, len) == AG_OK) {
                    s.sent++;
                }
            }
        }
        chi = (chi + 1) % 3;

        ag_event_t ev;
        while (ag_poll_event(&ev, 0)) {
            if (ev.type == AG_EV_QUIT) {
                running = false;
                break;
            }
            if (ev.type == AG_EV_FOCUS_GAINED) {
                last_paint = 0;
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
        if (running && (now - last_paint >= MRD_REPAINT_MS)) {
            mrd_gfx_paint(d, MRD_C_BG, paint_spam, &s);
            ag_printf("[mrd] spam sent=%u ch=%d\n", s.sent, s.channel);
            last_paint = now;
        }
        ag_heartbeat();
        ag_delay(30); /* a beat between sweeps, so this is spam not a firehose */
    }
    /*
     * The radio is deliberately left running between screens.  Starting and
     * stopping it per screen re-ran the interface bring-up each time, which
     * leaked netif handlers ("handler already registered", "duplicate key")
     * until the interface failed with ENOMEM and the process faulted.  It is
     * torn down once, when the application exits (see ag_main).
     */
}

/* ------------------------------------------------------------------------ */
/* Device > Info                                                             */
/* ------------------------------------------------------------------------ */

typedef struct {
    int          W, H;
    ag_sysinfo_t si;
    ag_meminfo_t mi;
} mrd_info_t;

static void info_line(mrd_band_t *b, int y, const char *k, const char *v)
{
    int x = 12 + mrd_band_text(b, 12, y, k, MRD_C_LGREY, 0xFFFFFFFFu, 1);
    mrd_band_text(b, x + 6, y, v, MRD_C_TEXT, 0xFFFFFFFFu, 1);
}

static void info_line_u(mrd_band_t *b, int y, const char *k, uint32_t v,
                        const char *suffix)
{
    char  buf[24];
    char *e = buf + sizeof(buf);
    char *p = put_uint(buf, e, v);
    p = put_str(p, e, suffix);
    *p = '\0';
    info_line(b, y, k, buf);
}

static void paint_info(mrd_band_t *b, void *ctx)
{
    mrd_info_t *f = (mrd_info_t *)ctx;
    const int   ty0 = mrd_text_y(0, MRD_STATUS_H, MRD_TITLE_SCALE);

    mrd_band_fill(b, 0, 0, f->W, MRD_STATUS_H, MRD_C_BAR_BG);
    mrd_band_fill(b, 0, MRD_STATUS_H - 1, f->W, 1, MRD_C_LGREY);
    int tx = 6 + mrd_band_text(b, 6, ty0, "<", MRD_C_LGREY, 0xFFFFFFFFu,
                               MRD_TITLE_SCALE) + 4;
    mrd_band_text(b, tx, ty0, "Device Info", MRD_C_BLUE, 0xFFFFFFFFu,
                  MRD_TITLE_SCALE);

    int y = MRD_STATUS_H + 8;
    info_line(b, y, "OS ", f->si.os_name);
    y += 18;
    info_line(b, y, "Version ", f->si.os_version);
    y += 18;
    info_line(b, y, "Build ", f->si.build);
    y += 18;
    info_line(b, y, "Chip ", f->si.chip);
    y += 18;
    info_line(b, y, "Board ", f->si.board);
    y += 18;
    info_line(b, y, "Profile ", f->si.profile);
    y += 18;
    info_line_u(b, y, "CPU ", f->si.cpu_hz / 1000000u, " MHz");
    y += 18;

    char  buf[24];
    char *e = buf + sizeof(buf);
    char *p = put_uint(buf, e, f->si.abi_major);
    p = put_str(p, e, ".");
    p = put_uint(p, e, f->si.abi_minor);
    *p = '\0';
    info_line(b, y, "ABI ", buf);
    y += 18;
    info_line_u(b, y, "Heap free ", (uint32_t)(f->mi.system_free / 1024u),
                " KB");
    y += 18;
    info_line_u(b, y, "Arena free ", (uint32_t)(f->mi.arena_free / 1024u),
                " KB");
}

void mrd_screen_dev_info(mrd_disp_t *d)
{
    static mrd_info_t f;
    memset(&f, 0, sizeof(f));
    f.W = d->W;
    f.H = d->H;
    ag_sysinfo_get(&f.si);
    ag_meminfo(&f.mi);

    bool dirty = true;
    for (;;) {
        if (dirty) {
            ag_meminfo(&f.mi); /* the free numbers move; refresh on redraw */
            mrd_gfx_paint(d, MRD_C_BG, paint_info, &f);
            dirty = false;
        }
        ag_event_t ev;
        if (!ag_poll_event(&ev, 500)) {
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
