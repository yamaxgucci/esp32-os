/*
 * ArgonOS - keystrokes and pointer moves as HID reports.  See ag_kvm.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ag_kvm.h"

#include <string.h>

/* ------------------------------------------------------------------------ */
/* The keyboard's state                                                     */
/* ------------------------------------------------------------------------ */

void ag_kvm_kbd_reset(ag_kvm_kbd_t *k)
{
    if (k != NULL) {
        memset(k, 0, sizeof(*k));
    }
}

void ag_kvm_reset(ag_kvm_state_t *s)
{
    if (s != NULL) {
        memset(s, 0, sizeof(*s));
        /* Centre, so that the first absolute report after a reset does not
         * fling the cursor into a corner before the phone has said where it
         * wants it. */
        s->abs_x = (uint16_t)(AG_USBHID_ABS_MAX / 2);
        s->abs_y = (uint16_t)(AG_USBHID_ABS_MAX / 2);
    }
}

void ag_kvm_kbd_mods(ag_kvm_kbd_t *k, uint8_t mods)
{
    if (k != NULL) {
        k->mods = mods;
    }
}

bool ag_kvm_kbd_key(ag_kvm_kbd_t *k, uint8_t usage, bool down)
{
    if (k == NULL || usage == 0) {
        return false;
    }

    uint32_t i;
    for (i = 0; i < AG_KVM_SLOTS; i++) {
        if (i < k->held && k->keys[i] == usage) {
            break;
        }
    }

    if (down) {
        if (i < k->held && i < AG_KVM_SLOTS) {
            return true; /* already down: auto-repeat is the host's business */
        }
        if (k->held >= AG_KVM_SLOTS) {
            /* A seventh key.  Count it so that letting go of one brings the
             * report back out of rollover, but there is no slot to put it in. */
            if (k->held < 255u) {
                k->held++;
            }
            return true;
        }
        k->keys[k->held++] = usage;
        return true;
    }

    /* Up.  Close the gap so the slots stay packed: a host that sees a key move
     * between slots treats it as a release and a fresh press. */
    if (k->held > AG_KVM_SLOTS) {
        /*
         * We are in rollover and do not know which keys the extra presses were.
         * The only honest thing is to drop the count; the slots themselves are
         * only trustworthy again once everything is up, which is the state a
         * person ends in anyway.
         */
        k->held--;
        if (k->held == AG_KVM_SLOTS) {
            /* Back inside what a report can express, but the six we are
             * holding may no longer be the six that are down.  Start clean. */
            memset(k->keys, 0, sizeof(k->keys));
            k->held = 0;
        }
        return true;
    }
    if (i >= k->held) {
        return true; /* was not down; nothing to do */
    }
    for (uint32_t j = i; j + 1 < k->held; j++) {
        k->keys[j] = k->keys[j + 1];
    }
    k->keys[--k->held] = 0;
    return true;
}

void ag_kvm_kbd_report(const ag_kvm_kbd_t *k, uint8_t out[AG_USBHID_KBD_LEN])
{
    memset(out, 0, AG_USBHID_KBD_LEN);
    if (k == NULL) {
        return;
    }
    out[0] = k->mods;
    /* out[1] stays zero: the reserved byte. */
    if (k->held > AG_KVM_SLOTS) {
        for (uint32_t i = 0; i < AG_KVM_SLOTS; i++) {
            out[2 + i] = AG_KVM_ROLLOVER;
        }
        return;
    }
    for (uint32_t i = 0; i < k->held; i++) {
        out[2 + i] = k->keys[i];
    }
}

/* ------------------------------------------------------------------------ */
/* ASCII on a US keyboard                                                   */
/* ------------------------------------------------------------------------ */

/*
 * The punctuation keys, in usage order from 0x2D, and what each produces
 * unshifted and shifted.  A table rather than a switch because it is data, and
 * because the pairing is the thing a reader wants to check.
 */
static const struct {
    char    plain;
    char    shifted;
    uint8_t usage;
} k_punct[] = {
    {'-', '_', 0x2D}, {'=', '+', 0x2E},  {'[', '{', 0x2F}, {']', '}', 0x30},
    {'\\', '|', 0x31}, {';', ':', 0x33}, {'\'', '"', 0x34}, {'`', '~', 0x35},
    {',', '<', 0x36}, {'.', '>', 0x37},  {'/', '?', 0x38},
};

/* The digit row shifted: 1 gives !, 2 gives @, and so on round to 0 giving ). */
static const char k_digit_shifted[10] = {')', '!', '@', '#', '$',
                                         '%', '^', '&', '*', '('};

bool ag_kvm_char_usage(char c, uint8_t *usage, uint8_t *mods)
{
    uint8_t u = 0;
    uint8_t m = 0;

    if (c >= 'a' && c <= 'z') {
        u = (uint8_t)(0x04 + (c - 'a'));
    } else if (c >= 'A' && c <= 'Z') {
        u = (uint8_t)(0x04 + (c - 'A'));
        m = AG_KVM_LSHIFT;
    } else if (c >= '1' && c <= '9') {
        u = (uint8_t)(0x1E + (c - '1'));
    } else if (c == '0') {
        u = 0x27;
    } else if (c == '\n' || c == '\r') {
        u = 0x28; /* Enter */
    } else if (c == 0x1B) {
        u = 0x29; /* Escape */
    } else if (c == '\b' || c == 0x7F) {
        u = 0x2A; /* Backspace */
    } else if (c == '\t') {
        u = 0x2B;
    } else if (c == ' ') {
        u = 0x2C;
    } else {
        for (size_t i = 0; i < sizeof(k_punct) / sizeof(k_punct[0]); i++) {
            if (c == k_punct[i].plain) {
                u = k_punct[i].usage;
                break;
            }
            if (c == k_punct[i].shifted) {
                u = k_punct[i].usage;
                m = AG_KVM_LSHIFT;
                break;
            }
        }
        if (u == 0) {
            for (uint8_t d = 0; d < 10; d++) {
                if (c == k_digit_shifted[d]) {
                    u = (d == 0) ? 0x27 : (uint8_t)(0x1E + d - 1);
                    m = AG_KVM_LSHIFT;
                    break;
                }
            }
        }
    }

    if (u == 0) {
        return false;
    }
    if (usage != NULL) {
        *usage = u;
    }
    if (mods != NULL) {
        *mods = m;
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* Frames                                                                   */
/* ------------------------------------------------------------------------ */

void ag_kvm_frame(ag_usbhid_frame_t *f, ag_usbhid_kind_t kind,
                  const uint8_t *data, uint8_t len)
{
    if (f == NULL) {
        return;
    }
    memset(f, 0, sizeof(*f));
    f->kind = (uint8_t)kind;
    if (len > AG_USBHID_DATA_MAX) {
        len = (uint8_t)AG_USBHID_DATA_MAX;
    }
    f->len = len;
    if (data != NULL && len > 0) {
        memcpy(f->data, data, len);
    }
}

void ag_kvm_frame_kbd(ag_usbhid_frame_t *f, const ag_kvm_kbd_t *k)
{
    uint8_t rep[AG_USBHID_KBD_LEN];
    ag_kvm_kbd_report(k, rep);
    ag_kvm_frame(f, AG_USBHID_KBD, rep, (uint8_t)AG_USBHID_KBD_LEN);
}

/* The wheel is one signed byte, so a long flick arrives as several. */
static int8_t clamp8(int v)
{
    if (v > 127) {
        return 127;
    }
    if (v < -127) {
        return -127;
    }
    return (int8_t)v;
}

static uint16_t clamp_abs(int v)
{
    if (v < 0) {
        return 0;
    }
    if (v > AG_USBHID_ABS_MAX) {
        return (uint16_t)AG_USBHID_ABS_MAX;
    }
    return (uint16_t)v;
}

void ag_kvm_frame_abs(ag_usbhid_frame_t *f, uint8_t buttons, int x, int y,
                      int wheel)
{
    const uint16_t ax = clamp_abs(x);
    const uint16_t ay = clamp_abs(y);
    uint8_t        rep[AG_USBHID_ABS_LEN];

    rep[0] = (uint8_t)(buttons & 0x07u);
    rep[1] = (uint8_t)(ax & 0xFFu);
    rep[2] = (uint8_t)(ax >> 8);
    rep[3] = (uint8_t)(ay & 0xFFu);
    rep[4] = (uint8_t)(ay >> 8);
    rep[5] = (uint8_t)clamp8(wheel);
    ag_kvm_frame(f, AG_USBHID_ABS, rep, (uint8_t)AG_USBHID_ABS_LEN);
}

int ag_kvm_type_frames(ag_kvm_kbd_t *k, const char *text, size_t len,
                       ag_usbhid_frame_t *out, int max, size_t *consumed)
{
    int    n = 0;
    size_t i = 0;

    if (consumed != NULL) {
        *consumed = 0;
    }
    if (k == NULL || text == NULL || out == NULL || max < 2) {
        return 0;
    }

    for (; i < len; i++) {
        uint8_t usage = 0;
        uint8_t mods = 0;

        if (!ag_kvm_char_usage(text[i], &usage, &mods)) {
            continue; /* nothing on a US keyboard makes this character */
        }
        if (n + 2 > max) {
            break;
        }

        const uint8_t held = k->mods;

        k->mods = (uint8_t)(held | mods);
        (void)ag_kvm_kbd_key(k, usage, true);
        ag_kvm_frame_kbd(&out[n++], k);

        /*
         * The release drops this character's shift but keeps whatever the
         * caller was already holding.  Holding shift across two characters
         * would be fewer reports and is a trap: the far machine's auto-repeat
         * starts on the *first* report that shows a key held, so a shift left
         * down between "A" and "B" is a shift the host may decide is being
         * leant on.
         */
        (void)ag_kvm_kbd_key(k, usage, false);
        k->mods = held;
        ag_kvm_frame_kbd(&out[n++], k);

        if (consumed != NULL) {
            *consumed = i + 1;
        }
    }
    if (consumed != NULL && i >= len) {
        *consumed = len;
    }
    return n;
}

/* ------------------------------------------------------------------------ */
/* The wire format                                                          */
/* ------------------------------------------------------------------------ */

/*
 * Fields, taken one at a time from a line that is not NUL-terminated.  Written
 * out rather than reached for with strtok because the input arrives from a
 * socket: it is a length and a pointer, it is not ours to write a NUL into, and
 * it is the one place in this file an attacker chooses the bytes.
 */
typedef struct {
    const char *p;
    const char *end;
} scan_t;

static void scan_init(scan_t *s, const char *line, size_t len)
{
    s->p = line;
    s->end = line + len;
}

static void scan_spaces(scan_t *s)
{
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\t')) {
        s->p++;
    }
}

/* Reads a decimal integer.  False when there is no number where one is due. */
static bool scan_int(scan_t *s, int *out)
{
    scan_spaces(s);

    bool neg = false;
    if (s->p < s->end && (*s->p == '-' || *s->p == '+')) {
        neg = (*s->p == '-');
        s->p++;
    }
    if (s->p >= s->end || *s->p < '0' || *s->p > '9') {
        return false;
    }

    long v = 0;
    while (s->p < s->end && *s->p >= '0' && *s->p <= '9') {
        v = v * 10 + (*s->p - '0');
        if (v > 1000000L) {
            v = 1000000L; /* saturate rather than wrap; the callers clamp */
        }
        s->p++;
    }
    *out = (int)(neg ? -v : v);
    return true;
}

int ag_kvm_parse(const char *line, size_t len, ag_kvm_state_t *st,
                 ag_usbhid_frame_t *out, int max)
{
    if (line == NULL || st == NULL || out == NULL || max < 1) {
        return -AG_EINVAL;
    }

    /* A line from a socket may end in CR, LF, both, or neither. */
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        len--;
    }

    scan_t s;
    scan_init(&s, line, len);
    scan_spaces(&s);
    if (s.p >= s.end) {
        return 0; /* a blank line is not an error, it is a blank line */
    }

    const char cmd = *s.p++;

    switch (cmd) {
    case 'K': {
        scan_spaces(&s);
        if (s.p >= s.end) {
            return -AG_EINVAL;
        }
        const char dir = *s.p++;
        if (dir != 'd' && dir != 'u') {
            return -AG_EINVAL;
        }
        int usage = 0;
        int mods = 0;
        if (!scan_int(&s, &usage) || !scan_int(&s, &mods)) {
            return -AG_EINVAL;
        }
        if (usage < 0 || usage > 255 || mods < 0 || mods > 255) {
            return -AG_EINVAL;
        }
        ag_kvm_kbd_mods(&st->kbd, (uint8_t)mods);
        /*
         * Usage 0 with a modifier byte is how a browser reports Shift itself
         * going down: there is no key code, only the modifier.  ag_kvm_kbd_key
         * refuses it and is right to, and the report is still worth sending -
         * so the refusal is not an error here.
         */
        (void)ag_kvm_kbd_key(&st->kbd, (uint8_t)usage, dir == 'd');
        ag_kvm_frame_kbd(&out[0], &st->kbd);
        return 1;
    }

    case 'M': {
        /*
         * A step, on a wire that carries no relative interface.  It is added
         * to the position this end remembers and sent as a position, which is
         * the only kind of pointer report the far machine has - see the note
         * on ag_kvm_state_t.
         */
        int dx = 0, dy = 0, wheel = 0, buttons = 0;
        if (!scan_int(&s, &dx) || !scan_int(&s, &dy) ||
            !scan_int(&s, &wheel) || !scan_int(&s, &buttons)) {
            return -AG_EINVAL;
        }
        st->abs_x = clamp_abs((int)st->abs_x + (dx << AG_KVM_REL_SHIFT));
        st->abs_y = clamp_abs((int)st->abs_y + (dy << AG_KVM_REL_SHIFT));
        st->buttons = (uint8_t)(buttons & 0x07);
        ag_kvm_frame_abs(&out[0], st->buttons, st->abs_x, st->abs_y, wheel);
        return 1;
    }

    case 'A': {
        int x = 0, y = 0, wheel = 0, buttons = 0;
        if (!scan_int(&s, &x) || !scan_int(&s, &y) || !scan_int(&s, &wheel) ||
            !scan_int(&s, &buttons)) {
            return -AG_EINVAL;
        }
        st->abs_x = clamp_abs(x);
        st->abs_y = clamp_abs(y);
        st->buttons = (uint8_t)(buttons & 0x07);
        ag_kvm_frame_abs(&out[0], st->buttons, st->abs_x, st->abs_y, wheel);
        return 1;
    }

    case 'R': {
        if (max < 2) {
            return -AG_ENOSPC;
        }
        ag_kvm_kbd_reset(&st->kbd);
        ag_kvm_frame_kbd(&out[0], &st->kbd);
        /*
         * The pointer is released where it stands.  Sending it to a corner
         * would be a visible jump for a release nobody asked to see, and the
         * position is state the far machine keeps whether we mention it or not.
         */
        st->buttons = 0;
        ag_kvm_frame_abs(&out[1], 0, st->abs_x, st->abs_y, 0);
        return 2;
    }

    default:
        return -AG_EINVAL;
    }
}
