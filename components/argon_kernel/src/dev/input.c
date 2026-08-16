/*
 * ArgonOS - pad / button input layer and /dev/joy0.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/input.h>

#include <string.h>

#include <argon/console.h>
#include <argon/device.h>
#include <argon/keys.h>
#include <argon/log.h>

#include <argon/port/time.h>

#define AG_INPUT_PADS 2

static uint8_t     s_snap[AG_PAD_BYTES];
static uint32_t    s_snap_ms; /* 0 = never pushed */
static ag_device_t *s_joy;

static uint32_t now_ms(void)
{
    return (uint32_t)(ag_port_us() / 1000);
}

void ag_input_push_pad(const uint8_t *blob, size_t len)
{
    if (blob == NULL || len < 2u) {
        return;
    }
    memset(s_snap, 0, sizeof(s_snap));
    const size_t n = (len > AG_PAD_BYTES) ? AG_PAD_BYTES : len;
    memcpy(s_snap, blob, n);
    /* Legacy 3-byte hosts leave hi/ver at zero; that is fine. */
    if (len >= AG_PAD_BYTES && s_snap[5] == 0u) {
        s_snap[5] = AG_PAD_VER;
    }
    s_snap_ms = now_ms();
}

bool ag_input_pad_peek(uint8_t out[AG_PAD_BYTES], uint32_t max_age_ms)
{
    if (out == NULL || s_snap_ms == 0u) {
        return false;
    }
    if (max_age_ms != 0u) {
        const uint32_t age = now_ms() - s_snap_ms;
        if (age > max_age_ms) {
            return false;
        }
    }
    memcpy(out, s_snap, AG_PAD_BYTES);
    return true;
}

uint32_t ag_input_pad_byte(int which)
{
    uint8_t snap[AG_PAD_BYTES];
    if (which < 0 || which > 4) {
        return 0;
    }
    if (!ag_input_pad_peek(snap, 250u)) {
        return 0;
    }
    return snap[which];
}

/*
 * Map ag_btn id onto the low or high pad byte.  PAUSE/QUIT live in sys and
 * are pad-independent; START also accepts the legacy sys pause bit so a
 * 3-byte host still starts a Mega Drive game.
 */
static int32_t btn_from_snap(const uint8_t snap[AG_PAD_BYTES], int pad, int id)
{
    if (pad < 0 || pad >= AG_INPUT_PADS) {
        return 0;
    }
    if (id == AG_BTN_PAUSE) {
        return (snap[2] & AG_PAD_SYS_PAUSE) != 0 ? 1 : 0;
    }
    if (id == AG_BTN_QUIT) {
        return (snap[2] & AG_PAD_SYS_QUIT) != 0 ? 1 : 0;
    }

    static const uint8_t k_lo[6] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20};
    if (id >= AG_BTN_UP && id <= AG_BTN_B2) {
        return (snap[pad] & k_lo[id]) != 0 ? 1 : 0;
    }

    const uint8_t hi = snap[3u + (uint32_t)pad];
    switch (id) {
    case AG_BTN_C:
        return (hi & AG_PAD_HI_C) != 0 ? 1 : 0;
    case AG_BTN_START:
        /* Dedicated Start bit, or the sys pause bit for old hosts. */
        if ((hi & AG_PAD_HI_START) != 0) {
            return 1;
        }
        return (snap[2] & AG_PAD_SYS_PAUSE) != 0 ? 1 : 0;
    case AG_BTN_X:
        return (hi & AG_PAD_HI_X) != 0 ? 1 : 0;
    case AG_BTN_Y:
        return (hi & AG_PAD_HI_Y) != 0 ? 1 : 0;
    case AG_BTN_Z:
        return (hi & AG_PAD_HI_Z) != 0 ? 1 : 0;
    case AG_BTN_MODE:
        return (hi & AG_PAD_HI_MODE) != 0 ? 1 : 0;
    default:
        return 0;
    }
}

static int32_t btn_sticky_fallback(int pad, int id)
{
    /* Defaults match tools/sms_pad.py / apps/sms/sms.cfg. */
    static const uint16_t k_pad0[14] = {
        AG_KEY_UP, AG_KEY_DOWN, AG_KEY_LEFT, AG_KEY_RIGHT,
        AG_KEY_Z, AG_KEY_X, AG_KEY_ENTER, AG_KEY_ESC,
        AG_KEY_C, AG_KEY_ENTER, AG_KEY_A, AG_KEY_S, AG_KEY_D, AG_KEY_M,
    };
    static const uint16_t k_pad1[14] = {
        AG_KEY_W, AG_KEY_S, AG_KEY_A, AG_KEY_D,
        AG_KEY_J, AG_KEY_K, AG_KEY_P, AG_KEY_Q,
        AG_KEY_L, AG_KEY_P, AG_KEY_U, AG_KEY_I, AG_KEY_O, AG_KEY_N,
    };
    if (id < 0 || id > AG_BTN_MODE) {
        return 0;
    }
    const uint16_t *tab = (pad == 0) ? k_pad0 : k_pad1;
    if (pad != 0 && pad != 1) {
        return 0;
    }
    return ag_console_key_pressed(tab[id]) ? 1 : 0;
}

int32_t ag_input_btnp(int pad, int id)
{
    uint8_t snap[AG_PAD_BYTES];
    if (ag_input_pad_peek(snap, 250u)) {
        return btn_from_snap(snap, pad, id);
    }
    return btn_sticky_fallback(pad, id);
}

/* ---- /dev/joy0 --------------------------------------------------------- */

static int32_t joy_read(ag_device_t *dev, void *buf, size_t len, uint64_t off)
{
    (void)dev;
    if (buf == NULL) {
        return -AG_EINVAL;
    }
    uint8_t snap[AG_PAD_BYTES];
    if (!ag_input_pad_peek(snap, 250u)) {
        memset(snap, 0, sizeof(snap));
    }
    if (off >= AG_PAD_BYTES) {
        return 0;
    }
    const size_t avail = AG_PAD_BYTES - (size_t)off;
    const size_t n = (len < avail) ? len : avail;
    memcpy(buf, snap + (size_t)off, n);
    return (int32_t)n;
}

static uint64_t joy_size(ag_device_t *dev)
{
    (void)dev;
    return AG_PAD_BYTES;
}

static const ag_dev_ops_t k_joy_ops = {
    .read = joy_read,
    .size = joy_size,
};

ag_err_t ag_input_init(void)
{
    memset(s_snap, 0, sizeof(s_snap));
    s_snap_ms = 0;
    s_joy = NULL;

    const ag_dev_desc_t desc = {
        .name = "joy0",
        .driver = "pad",
        .cls = AG_DEV_INPUT,
        .flags = 0,
        .ops = &k_joy_ops,
        .priv = NULL,
    };
    const ag_err_t err = ag_dev_register(&desc, &s_joy);
    if (err != AG_OK) {
        ag_log(AG_LOG_ERROR, "input", "cannot register joy0: %d", (int)err);
        return err;
    }
    ag_log(AG_LOG_INFO, "input", "joy0 ready (pad layer)");
    return AG_OK;
}
