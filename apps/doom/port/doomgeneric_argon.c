/*
 * ArgonOS doomgeneric platform: gfx, time, kbd/mouse → Doom keys/ev_mouse.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/keys.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_video.h"

#define KEYQUEUE_SIZE 32
#define SCREEN_W 320
#define SCREEN_H 200

#define MOUSE_CLAMP 320
#define MWHEEL_PREV (1 << 3)
#define MWHEEL_NEXT (1 << 4)

static ag_gfxinfo_t s_gi;
static int          s_quit;
static int          s_use_live_pad;
static ag_handle_t  s_kbd = -1;
static ag_handle_t  s_mouse = -1;
static int          s_mx;
static int          s_my;
static int          s_mhave;
static int          s_mbtn;
static int          s_mdx;
static int          s_mdy;
static int          s_mwheel;
static int          s_mpending;

static unsigned short s_keyq[KEYQUEUE_SIZE];
static unsigned       s_keyq_w;
static unsigned       s_keyq_r;

static uint16_t s_lut[256];

static const struct {
    int          btn;
    unsigned char doom;
} k_pad_map[] = {
    {AG_BTN_UP, KEY_UPARROW},
    {AG_BTN_DOWN, KEY_DOWNARROW},
    {AG_BTN_LEFT, KEY_LEFTARROW},
    {AG_BTN_RIGHT, KEY_RIGHTARROW},
    {AG_BTN_B1, KEY_FIRE},
    {AG_BTN_B2, KEY_USE},
    {AG_BTN_C, KEY_RSHIFT},
    {AG_BTN_START, KEY_ENTER},
    {AG_BTN_PAUSE, KEY_ESCAPE},
    {AG_BTN_Y, 'y'},
    {AG_BTN_X, 'n'},
};

static unsigned char hid_to_doom(uint16_t hid)
{
    switch (hid) {
    case AG_KEY_ENTER:
        return KEY_ENTER;
    case AG_KEY_ESC:
        return KEY_ESCAPE;
    case AG_KEY_LEFT:
        return KEY_LEFTARROW;
    case AG_KEY_RIGHT:
        return KEY_RIGHTARROW;
    case AG_KEY_UP:
        return KEY_UPARROW;
    case AG_KEY_DOWN:
        return KEY_DOWNARROW;
    case AG_KEY_SPACE:
        return KEY_USE;
    case AG_KEY_TAB:
        return KEY_TAB;
    case AG_KEY_BACKSPACE:
        return KEY_BACKSPACE;
    case AG_KEY_LCTRL:
    case AG_KEY_RCTRL:
        return KEY_FIRE;
    case AG_KEY_LSHIFT:
    case AG_KEY_RSHIFT:
        return KEY_RSHIFT;
    case AG_KEY_LALT:
    case AG_KEY_RALT:
        return KEY_LALT;
    default:
        if (hid >= AG_KEY_A && hid <= AG_KEY_Z) {
            return (unsigned char)('a' + (hid - AG_KEY_A));
        }
        if (hid >= AG_KEY_1 && hid <= AG_KEY_9) {
            return (unsigned char)('1' + (hid - AG_KEY_1));
        }
        if (hid == AG_KEY_0) {
            return '0';
        }
        return 0;
    }
}

static void keyq_push(int pressed, unsigned char key)
{
    unsigned short data = (unsigned short)((pressed ? 0x100u : 0u) | key);
    s_keyq[s_keyq_w] = data;
    s_keyq_w = (s_keyq_w + 1u) % KEYQUEUE_SIZE;
}

static int keyq_pop(int *pressed, unsigned char *key)
{
    if (s_keyq_r == s_keyq_w) {
        return 0;
    }
    unsigned short data = s_keyq[s_keyq_r];
    s_keyq_r = (s_keyq_r + 1u) % KEYQUEUE_SIZE;
    *pressed = (data >> 8) & 1;
    *key = (unsigned char)(data & 0xffu);
    return 1;
}

static void poll_pad_edges(void)
{
    static uint32_t prev;
    uint32_t now = 0;
    unsigned i;

    if (!s_use_live_pad) {
        return;
    }
    for (i = 0; i < (unsigned)(sizeof k_pad_map / sizeof k_pad_map[0]); i++) {
        if (ag_btn(k_pad_map[i].btn)) {
            now |= 1u << i;
        }
    }
    for (i = 0; i < (unsigned)(sizeof k_pad_map / sizeof k_pad_map[0]); i++) {
        uint32_t bit = 1u << i;
        if ((now & bit) && !(prev & bit)) {
            keyq_push(1, k_pad_map[i].doom);
        } else if (!(now & bit) && (prev & bit)) {
            keyq_push(0, k_pad_map[i].doom);
        }
    }
    prev = now;
}

int doom_argon_quit(void) { return s_quit; }

static uint32_t     s_svc_ms;

void doom_argon_service(void)
{
    uint32_t now = ag_millis();
    if (s_svc_ms != 0u && (now - s_svc_ms) < 15u) {
        return;
    }
    s_svc_ms = now;
    (void)doom_argon_poll_sys();
    doom_argon_audio_pump();
}

void doom_argon_set_live_pad(int on) { s_use_live_pad = on ? 1 : 0; }

static int clamp_delta(int v)
{
    if (v > MOUSE_CLAMP) {
        return MOUSE_CLAMP;
    }
    if (v < -MOUSE_CLAMP) {
        return -MOUSE_CLAMP;
    }
    return v;
}

static void on_pointer(const ag_event_t *ev)
{
    int x = ev->ptr.x;
    int y = ev->ptr.y;

    s_mbtn = ev->ptr.buttons & 7;
    if (s_mhave) {
        s_mdx += x - s_mx;
        s_mdy += y - s_my;
    }
    s_mx = x;
    s_my = y;
    s_mhave = 1;
    s_mpending = 1;
}

int doom_argon_get_mouse(int *buttons, int *dx, int *dy)
{
    int b;

    if (!s_mpending) {
        return 0;
    }
    b = s_mbtn | s_mwheel;
    if (buttons != NULL) {
        *buttons = b;
    }
    if (dx != NULL) {
        *dx = clamp_delta(s_mdx);
    }
    if (dy != NULL) {
        *dy = clamp_delta(s_mdy);
    }
    s_mdx = 0;
    s_mdy = 0;
    s_mwheel = 0;
    s_mpending = 0;
    return 1;
}

int doom_argon_poll_sys(void)
{
    ag_event_t ev;
    if (s_kbd >= 0) {
        uint8_t buf[64];
        /* read() runs KBDVIRT pump_rx → inject KEY_*; poll_event consumes them. */
        (void)ag_dev_read(s_kbd, buf, sizeof(buf));
    }
    if (s_mouse >= 0) {
        uint8_t buf[64];
        (void)ag_dev_read(s_mouse, buf, sizeof(buf));
    }
    while (ag_poll_event(&ev, 0)) {
        if (ev.type == AG_EV_QUIT) {
            ag_printf("doom: quit (F12 / supervisor)\n");
            s_quit = 1;
            return 1;
        }
        if (ev.type == AG_EV_FOCUS_GAINED) {
            if (ag_gfx_acquire(&s_gi) == AG_OK) {
                ag_gfx_clear(0);
                ag_gfx_flush(0, 0, s_gi.width, s_gi.height);
            }
        }
        if (ev.type == AG_EV_KEY_DOWN || ev.type == AG_EV_KEY_UP) {
            unsigned char k = hid_to_doom(ev.key.keycode);
            if (k != 0) {
                keyq_push(ev.type == AG_EV_KEY_DOWN, k);
            }
        }
        if (ev.type == AG_EV_POINTER_DOWN || ev.type == AG_EV_POINTER_UP ||
            ev.type == AG_EV_POINTER_MOVE) {
            on_pointer(&ev);
        }
        if (ev.type == AG_EV_WHEEL) {
            if (ev.ptr.dy > 0) {
                s_mwheel |= MWHEEL_NEXT;
            } else if (ev.ptr.dy < 0) {
                s_mwheel |= MWHEEL_PREV;
            }
            s_mpending = 1;
        }
    }
    /* Do not use ag_btn(AG_BTN_QUIT): without a live pad that is sticky Esc,
     * which Doom needs for the menu.  Process stop is AG_EV_QUIT (F12). */
    poll_pad_edges();
    return s_quit;
}

void DG_Init(void)
{
    if (ag_gfx_acquire(&s_gi) != AG_OK) {
        ag_printf("doom: gfx acquire failed\n");
        ag_exit(1);
    }
    ag_gfx_clear(0);
    ag_gfx_flush(0, 0, s_gi.width, s_gi.height);
    s_kbd = ag_dev_open("/dev/kbd0");
    if (s_kbd >= 0) {
        ag_printf("doom: kbd = /dev/kbd0 (kbdvirt.py :5561)\n");
    } else {
        ag_printf("doom: no /dev/kbd0 (drv install a:\\kbdvirt.sys)\n");
    }
    s_mouse = ag_dev_open("/dev/mouse0");
    if (s_mouse >= 0) {
        ag_printf("doom: mouse = /dev/mouse0 (mousevirt.py :5560)\n");
    } else {
        ag_printf("doom: no /dev/mouse0 (drv install a:\\mousevirt.sys)\n");
    }
}

void DG_DrawFrame(void)
{
    int scale, dw, dh, ox, oy, x, y, r, k;
    const unsigned char *src;
    uint16_t           *dst;
    uint32_t            stride;
    static int          s_presents;

    (void)doom_argon_poll_sys();
    doom_argon_service();
    ag_heartbeat();

    if (s_gi.fb == NULL || I_VideoBuffer == NULL) {
        return;
    }

    scale = (int)s_gi.width / SCREEN_W;
    if ((int)s_gi.height / SCREEN_H < scale) {
        scale = (int)s_gi.height / SCREEN_H;
    }
    if (scale < 1) {
        scale = 1;
    }
    dw = SCREEN_W * scale;
    dh = SCREEN_H * scale;
    ox = ((int)s_gi.width - dw) / 2;
    oy = ((int)s_gi.height - dh) / 2;
    stride = s_gi.stride;

    for (k = 0; k < 256; k++) {
        unsigned r8 = colors[k].r;
        unsigned g8 = colors[k].g;
        unsigned b8 = colors[k].b;
        s_lut[k] = (uint16_t)(((r8 & 0xF8u) << 8) | ((g8 & 0xFCu) << 3) |
                              (b8 >> 3));
    }

    for (y = 0; y < SCREEN_H; y++) {
        if ((y & 7) == 0) {
            doom_argon_service();
        }
        src = I_VideoBuffer + (size_t)y * SCREEN_W;
        for (r = 0; r < scale; r++) {
            dst = (uint16_t *)((uint8_t *)s_gi.fb +
                               (size_t)(oy + y * scale + r) * stride) +
                  ox;
            for (x = 0; x < SCREEN_W; x++) {
                uint16_t p = s_lut[src[x]];
                for (k = 0; k < scale; k++) {
                    *dst++ = p;
                }
            }
        }
    }
    ag_gfx_flush(0, 0, s_gi.width, s_gi.height);
    doom_argon_service();
    if (s_presents < 3) {
        s_presents++;
        ag_printf("doom: present %d\n", s_presents);
    }
}

void DG_SleepMs(uint32_t ms)
{
    doom_argon_service();
    ag_heartbeat();
    if (ms == 0) {
        ag_yield();
        return;
    }
    ag_delay(ms < 2 ? 2 : ms);
}

uint32_t DG_GetTicksMs(void) { return ag_millis(); }

int DG_GetKey(int *pressed, unsigned char *key)
{
    (void)doom_argon_poll_sys();
    return keyq_pop(pressed, key);
}

void DG_SetWindowTitle(const char *title)
{
    if (title != NULL) {
        ag_printf("doom: %s\n", title);
    }
}
