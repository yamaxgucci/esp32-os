/*
 * MARAUDER - Bluetooth LE, through api->ble (ABI 0.38).
 *
 * The board's radio as a BLE observer and GATT client: watch every device in
 * range and say what it is, flag the ones that look like card skimmers, and -
 * on a tap - connect to one and discover its services.  It is the same NimBLE
 * host and port the shell's `ble` command drives, reached through the public
 * ABI so the application stays a loadable .AXE.
 *
 * The observing/connecting half is here; the BLE-spam attacks (Sour Apple and
 * the rest) live in mrd_spam.c and forge raw advertisements through
 * api->ble->adv_raw (ABI 0.40).
 *
 * Memory: bringing the BLE stack up costs ~53 KB, leaving ~17-30 KB free -
 * enough for this (slimmed) app once it is loaded.  The app does NOT raise the
 * radio: scan/connect answer -AG_ENODEV until it is up (an app-context bring-up
 * is not safe on this chip), so the reliable path is `bt on` before `run`,
 * exactly as Wi-Fi wants `wifi on` first.  The screens say so.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mrd.h"
#include <argon/keys.h>

#define MRD_BTMAX 10 /* devices kept from one scan (shares the screen scratch) */
#define MRD_BTROW 34 /* two 8x16 lines: name line + address line */
#define MRD_BT_SECS 3 /* scan window, seconds */

/* ---- tiny string builders (file-scoped copies; the SDK has no libc) ------ */

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

static char up1(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/*
 * The cheap serial BLE modules a skimmer is usually built from advertise under
 * these default names.  A match is a reason to look, not proof - said so on the
 * screen.  Prefix, case-insensitive.
 */
static const char *k_skimmer[] = {
    "HC-05", "HC-06", "HC-08", "HC-03", "HC-04",
    "JDY-",  "AT-09", "BT05",  "MLT-BT05", "SPP",
};
#define SKIMMER_N ((int)(sizeof(k_skimmer) / sizeof(k_skimmer[0])))

static bool name_matches(const char *name, const char *pat)
{
    for (int i = 0; pat[i]; i++) {
        if (up1(name[i]) != up1(pat[i])) {
            return false;
        }
    }
    return true;
}
static bool looks_like_skimmer(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    for (int i = 0; i < SKIMMER_N; i++) {
        if (name_matches(name, k_skimmer[i])) {
            return true;
        }
    }
    return false;
}

/* ---- shared scan state (on the screen scratch, not its own bss) ---------- */

typedef struct {
    int          W, H;
    ag_ble_dev_t dev[MRD_BTMAX];
    int          count;
    int          scroll;
    int          nvis;
    bool         scanning;
    bool         skimmer; /* skimmer screen: flag suspicious names            */
} mrd_bt_t;

static int bt_visible(int H) { int n = (H - MRD_STATUS_H) / MRD_BTROW; return n < 1 ? 1 : n; }

static void sort_bt(mrd_bt_t *b)
{
    for (int i = 1; i < b->count; i++) {
        ag_ble_dev_t key = b->dev[i];
        int          j = i - 1;
        while (j >= 0 && b->dev[j].rssi < key.rssi) {
            b->dev[j + 1] = b->dev[j];
            j--;
        }
        b->dev[j + 1] = key;
    }
}

/* ---- a one-line message screen with Back --------------------------------- */

static void paint_msg(mrd_band_t *bd, void *ctx)
{
    const char **m = (const char **)ctx;
    const int    ty0 = mrd_text_y(0, MRD_STATUS_H, MRD_TITLE_SCALE);
    mrd_band_fill(bd, 0, 0, bd->w, MRD_STATUS_H, MRD_C_BAR_BG);
    mrd_band_fill(bd, 0, MRD_STATUS_H - 1, bd->w, 1, MRD_C_LGREY);
    int tx = 6 + mrd_band_text(bd, 6, ty0, "<", MRD_C_LGREY, 0xFFFFFFFFu,
                               MRD_TITLE_SCALE) + 4;
    mrd_band_text(bd, tx, ty0, m[0], MRD_C_CYAN, 0xFFFFFFFFu, MRD_TITLE_SCALE);
    mrd_band_text(bd, 8, MRD_STATUS_H + 20, m[1], MRD_C_YELLOW, 0xFFFFFFFFu, 1);
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

/* ---- list rendering ------------------------------------------------------ */

static void paint_bt(mrd_band_t *bd, void *ctx)
{
    mrd_bt_t *b = (mrd_bt_t *)ctx;
    char      buf[64];
    char     *end = buf + sizeof(buf);

    const bool sb = b->count > b->nvis;
    const int  rext = sb ? (MRD_SCROLLBAR_W + 2) : 0;
    const int  ty0 = mrd_text_y(0, MRD_STATUS_H, MRD_TITLE_SCALE);
    const char *title = b->skimmer ? "Card Skimmers" : "BT Sniffer";

    mrd_band_fill(bd, 0, 0, b->W, MRD_STATUS_H, MRD_C_BAR_BG);
    mrd_band_fill(bd, 0, MRD_STATUS_H - 1, b->W, 1, MRD_C_LGREY);
    int tx = 6 + mrd_band_text(bd, 6, ty0, "<", MRD_C_LGREY, 0xFFFFFFFFu,
                               MRD_TITLE_SCALE) + 4;
    mrd_band_text(bd, tx, ty0, title, MRD_C_CYAN, 0xFFFFFFFFu, MRD_TITLE_SCALE);

    char *p = buf;
    p = put_str(p, end, "n:");
    p = put_uint(p, end, (uint32_t)b->count);
    *p = '\0';
    mrd_band_text(bd, b->W - mrd_text_w(buf, 1) - rext - 4, ty0, buf,
                  MRD_C_LGREY, 0xFFFFFFFFu, 1);

    if (b->scanning && b->count == 0) {
        mrd_band_text(bd, 8, MRD_STATUS_H + 10, "scanning...", MRD_C_LGREY,
                      0xFFFFFFFFu, 1);
        return;
    }
    if (b->count == 0) {
        mrd_band_text(bd, 8, MRD_STATUS_H + 10, "nothing found - tap to rescan",
                      MRD_C_LGREY, 0xFFFFFFFFu, 1);
        return;
    }

    for (int i = 0; i < b->nvis; i++) {
        const int oi = b->scroll + i;
        if (oi >= b->count) {
            break;
        }
        const ag_ble_dev_t *dv = &b->dev[oi];
        const int           y = MRD_STATUS_H + i * MRD_BTROW + 2;

        uint16_t col = MRD_C_GREEN;
        if (dv->rssi < -85) {
            col = MRD_C_RED;
        } else if (dv->rssi < -70) {
            col = MRD_C_YELLOW;
        }

        const bool skim = looks_like_skimmer(dv->name);
        const char *name = (dv->name[0] != '\0') ? dv->name : "<no name>";
        /* On the skimmer screen a match is red and shouts; otherwise white. */
        uint16_t nc = MRD_C_TEXT;
        if (b->skimmer) {
            nc = skim ? MRD_C_RED : 0x8410 /* dim */;
        }
        mrd_band_text(bd, 6, y, name, nc, 0xFFFFFFFFu, 1);

        p = buf;
        p = put(p, end, '-');
        p = put_uint(p, end, (uint32_t)(-(int)dv->rssi));
        *p = '\0';
        mrd_band_text(bd, b->W - mrd_text_w(buf, 1) - rext - 6, y, buf, col,
                      0xFFFFFFFFu, 1);

        /* Line 2: address, then a tag - [conn] connectable, or SKIMMER? */
        p = buf;
        for (int k = 0; k < 6; k++) {
            if (k) {
                p = put(p, end, ':');
            }
            p = put_hex2(p, end, dv->addr[k]);
        }
        *p = '\0';
        mrd_band_text(bd, 6, y + 16, buf, MRD_C_LGREY, 0xFFFFFFFFu, 1);

        const char *tag = NULL;
        uint16_t    tagc = MRD_C_LGREY;
        if (b->skimmer && skim) {
            tag = "SKIMMER?";
            tagc = MRD_C_RED;
        } else if (!b->skimmer && dv->connectable) {
            tag = "[conn]";
            tagc = MRD_C_LIME;
        }
        if (tag != NULL) {
            mrd_band_text(bd, b->W - mrd_text_w(tag, 1) - rext - 6, y + 16, tag,
                          tagc, 0xFFFFFFFFu, 1);
        }

        mrd_band_fill(bd, 0, y + MRD_BTROW - 3, b->W - rext, 1, 0x2104);
    }

    if (sb) {
        const int sx = b->W - MRD_SCROLLBAR_W;
        const int track = b->H - MRD_STATUS_H;
        mrd_band_fill(bd, sx, MRD_STATUS_H, MRD_SCROLLBAR_W, track, MRD_C_BTN_BG);
        int th = track * b->nvis / b->count;
        if (th < 14) {
            th = 14;
        }
        const int span = b->count - b->nvis;
        int thy =
            MRD_STATUS_H + (span > 0 ? (track - th) * b->scroll / span : 0);
        mrd_band_fill(bd, sx, thy, MRD_SCROLLBAR_W, th, MRD_C_LGREY);
    }
}

static void do_bt_scan(mrd_disp_t *d, mrd_bt_t *b)
{
    b->scanning = true;
    mrd_gfx_paint(d, MRD_C_BG, paint_bt, b);

    uint32_t       found = 0;
    const ag_err_t err = ag_ble_scan(b->dev, MRD_BTMAX, &found, MRD_BT_SECS);
    b->scanning = false;
    if (err == AG_OK) {
        b->count = (found < MRD_BTMAX) ? (int)found : MRD_BTMAX;
        sort_bt(b);
        if (b->scroll > b->count - b->nvis) {
            b->scroll = b->count - b->nvis;
        }
        if (b->scroll < 0) {
            b->scroll = 0;
        }
    } else {
        b->count = -1; /* signals "scan error" to the caller */
    }
    int skim = 0;
    if (b->count > 0) {
        for (int i = 0; i < b->count; i++) {
            if (looks_like_skimmer(b->dev[i].name)) {
                skim++;
            }
        }
    }
    ag_printf("[mrd] bt scan: err=%d found=%u shown=%d skimmer-like=%d\n",
              (int)err, found, b->count > 0 ? b->count : 0, skim);
}

/* Connect to a device on a tap, discover its GATT, show the counts (overlay).
 * Exercises connect/discover/services/chars.  Returns when the user backs out. */
static void bt_detail(mrd_disp_t *d, const ag_ble_dev_t *dv)
{
    char        title[24];
    char       *tp = title, *te = title + sizeof(title);
    tp = put_str(tp, te, dv->name[0] ? dv->name : "device");
    *tp = '\0';

    const char *m[2] = { title, "connecting..." };
    mrd_gfx_paint(d, MRD_C_BG, paint_msg, m);
    ag_printf("[mrd] bt connect \"%s\"...\n", dv->name[0] ? dv->name : "?");

    const ag_err_t cerr = ag_ble_connect(dv->addr, dv->addr_type, 6000);
    if (cerr != AG_OK) {
        ag_printf("[mrd] bt connect: err=%d\n", (int)cerr);
        wait_back(d, title, "connect failed - tap Back");
        return;
    }
    m[1] = "discovering...";
    mrd_gfx_paint(d, MRD_C_BG, paint_msg, m);
    (void)ag_ble_discover(6000);

    ag_ble_svc_t svc[6];
    ag_ble_chr_t chr[14];
    const uint32_t nsvc = ag_ble_services(svc, 6);
    const uint32_t nchr = ag_ble_chars(chr, 14);
    ag_printf("[mrd] bt discover: svc=%u chr=%u\n", nsvc, nchr);

    char line[40];
    char *p = line, *e = line + sizeof(line);
    p = put_str(p, e, "svc=");
    p = put_uint(p, e, nsvc);
    p = put_str(p, e, " chr=");
    p = put_uint(p, e, nchr);
    *p = '\0';
    wait_back(d, title, line);

    (void)ag_ble_disconnect();
    ag_printf("[mrd] bt disconnect\n");
}

static void bt_screen(mrd_disp_t *d, bool skimmer)
{
    const char *title = skimmer ? "Card Skimmers" : "BT Sniffer";
    if (!ag_ble_available()) {
        wait_back(d, title, "no BLE in this build (QEMU?)");
        return;
    }

    _Static_assert(sizeof(mrd_bt_t) <= MRD_SCRATCH_BYTES, "bt scratch");
    mrd_bt_t *const bp = (mrd_bt_t *)mrd_scratch();
#define b (*bp)
    memset(&b, 0, sizeof(b));
    b.W = d->W;
    b.H = d->H;
    b.nvis = bt_visible(d->H);
    b.skimmer = skimmer;

    do_bt_scan(d, &b);
    if (b.count < 0) {
        wait_back(d, title, "no radio: run 'bt on' first, then reopen");
        return;
    }

    bool running = true;
    bool dirty = true;
    int  g_down_y = -1, g_scroll0 = 0, g_down_row = -1;
    bool g_swipe = false;

    while (running) {
        if (dirty) {
            mrd_gfx_paint(d, MRD_C_BG, paint_bt, &b);
            dirty = false;
        }
        ag_event_t ev;
        if (!ag_poll_event(&ev, 200)) {
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
                do_bt_scan(d, &b);
                dirty = true;
            } else if (ev.key.keycode == AG_KEY_UP) {
                if (b.scroll > 0) {
                    b.scroll--;
                }
                dirty = true;
            } else if (ev.key.keycode == AG_KEY_DOWN) {
                if (b.scroll + b.nvis < b.count) {
                    b.scroll++;
                }
                dirty = true;
            }
        } else if (ev.type == AG_EV_POINTER_DOWN) {
            int px, py;
            mrd_touch_to_px(d, ev.ptr.x, ev.ptr.y, &px, &py);
            const bool sb = b.count > b.nvis;
            if (py < MRD_STATUS_H) {
                running = false;
            } else if (sb && px >= b.W - MRD_SCROLLBAR_W - 4) {
                const int track = b.H - MRD_STATUS_H;
                b.scroll = (py - MRD_STATUS_H) * b.count /
                           (track > 0 ? track : 1);
                dirty = true;
                g_down_y = -1;
            } else {
                g_down_y = py;
                g_scroll0 = b.scroll;
                g_swipe = false;
                g_down_row = -1;
                const int row = b.scroll + (py - MRD_STATUS_H) / MRD_BTROW;
                if (row >= 0 && row < b.count) {
                    g_down_row = row;
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
                    int maxs = b.count - b.nvis;
                    if (maxs < 0) {
                        maxs = 0;
                    }
                    int ns = g_scroll0 - dy / MRD_BTROW;
                    if (ns < 0) {
                        ns = 0;
                    }
                    if (ns > maxs) {
                        ns = maxs;
                    }
                    if (ns != b.scroll) {
                        b.scroll = ns;
                        dirty = true;
                    }
                }
            }
        } else if (ev.type == AG_EV_POINTER_UP) {
            if (g_down_y >= 0 && !g_swipe && g_down_row >= 0 &&
                g_down_row < b.count && !skimmer &&
                b.dev[g_down_row].connectable) {
                bt_detail(d, &b.dev[g_down_row]);
                dirty = true;
            }
            g_down_y = -1;
            g_swipe = false;
            g_down_row = -1;
        }
    }
#undef b
    /* Radio left up between screens; torn down once at app exit. */
}

void mrd_screen_bt_sniff(mrd_disp_t *d) { bt_screen(d, false); }
void mrd_screen_bt_skimmer(mrd_disp_t *d) { bt_screen(d, true); }

/* ------------------------------------------------------------------------ */
/* Headless self-test: `marauder bttest`                                     */
/* ------------------------------------------------------------------------ */

void mrd_bt_selftest(void)
{
    ag_printf("[mrd] bt selftest start\n");
    if (!ag_ble_available()) {
        ag_printf("[mrd] bt: api->ble is NULL (no BLE in this build)\n");
        return;
    }
    ag_printf("[mrd] bt caps: scan=%d connect=%d adv=%d\n",
              AG_HAS(ag_api()->ble, scan) ? 1 : 0,
              AG_HAS(ag_api()->ble, connect) ? 1 : 0,
              AG_HAS(ag_api()->ble, adv_start) ? 1 : 0);

    mrd_bt_t *const bp = (mrd_bt_t *)mrd_scratch();
    uint32_t        found = 0;
    const ag_err_t  err =
        ag_ble_scan(bp->dev, MRD_BTMAX, &found, MRD_BT_SECS);
    ag_printf("[mrd] bt scan: err=%d found=%u\n", (int)err, found);
    if (err != AG_OK) {
        if (err == -AG_ENOMEM) {
            ag_printf("[mrd] bt: run 'bt on' before 'run marauder bttest'\n");
        }
        return;
    }
    const uint32_t shown = (found < MRD_BTMAX) ? found : MRD_BTMAX;
    int            conn_idx = -1;
    for (uint32_t i = 0; i < shown; i++) {
        const ag_ble_dev_t *dv = &bp->dev[i];
        char  mb[18];
        char *mp = mb, *me = mb + sizeof(mb);
        for (int k = 0; k < 6; k++) {
            if (k) {
                mp = put(mp, me, ':');
            }
            mp = put_hex2(mp, me, dv->addr[k]);
        }
        *mp = '\0';
        ag_printf("[mrd]   %s  %s  rssi=%d conn=%d%s\n", mb,
                  dv->name[0] ? dv->name : "<no name>", (int)dv->rssi,
                  dv->connectable ? 1 : 0,
                  looks_like_skimmer(dv->name) ? " SKIMMER?" : "");
        if (conn_idx < 0 && dv->connectable) {
            conn_idx = (int)i;
        }
    }

    /* If something is connectable, exercise connect + discover once. */
    if (conn_idx >= 0 && AG_HAS(ag_api()->ble, connect)) {
        const ag_ble_dev_t *dv = &bp->dev[conn_idx];
        ag_printf("[mrd] bt connect \"%s\"...\n",
                  dv->name[0] ? dv->name : "?");
        const ag_err_t cerr = ag_ble_connect(dv->addr, dv->addr_type, 6000);
        ag_printf("[mrd] bt connect: err=%d\n", (int)cerr);
        if (cerr == AG_OK) {
            (void)ag_ble_discover(6000);
            ag_ble_svc_t svc[6];
            ag_ble_chr_t chr[14];
            const uint32_t nsvc = ag_ble_services(svc, 6);
            const uint32_t nchr = ag_ble_chars(chr, 14);
            ag_printf("[mrd] bt discover: svc=%u chr=%u\n", nsvc, nchr);
            (void)ag_ble_disconnect();
            ag_printf("[mrd] bt disconnect\n");
        }
    }
    /* raw advertising injection (adv_raw, ABI 0.40): one SwiftPair packet. */
    if (AG_HAS(ag_api()->ble, adv_raw)) {
        static const uint8_t pkt[] = { 0x02, 0x01, 0x06, 0x08, 0xff, 0x06,
                                       0x00, 0x03, 0x00, 0x80, 0x41, 0x58 };
        static const uint8_t rmac[6] = { 0xc0, 0x11, 0x22, 0x33, 0x44, 0x55 };
        const ag_err_t rerr = ag_ble_adv_raw(rmac, pkt, sizeof(pkt));
        ag_printf("[mrd] bt adv_raw: err=%d\n", (int)rerr);
        if (rerr == AG_OK) {
            ag_delay(200);
            (void)ag_ble_adv_stop();
        }
    }
    ag_printf("[mrd] bt selftest done\n");
}
