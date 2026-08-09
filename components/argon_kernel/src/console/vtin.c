/*
 * ArgonOS - terminal input decoder.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/vtin.h>

#include <string.h>

static void ev_none(ag_event_t *ev) { memset(ev, 0, sizeof(*ev)); }

static void ev_key(ag_event_t *ev, uint16_t keycode, uint32_t unicode,
                   uint16_t mods)
{
    memset(ev, 0, sizeof(*ev));
    ev->type = AG_EV_KEY_DOWN;
    ev->key.keycode = keycode;
    ev->key.unicode = unicode;
    ev->key.mods = mods;
}

static void ev_key_typed(ag_event_t *ev, ag_event_type_t type, uint16_t keycode,
                         uint32_t unicode, uint16_t mods)
{
    memset(ev, 0, sizeof(*ev));
    ev->type = type;
    ev->key.keycode = keycode;
    ev->key.unicode = unicode;
    ev->key.mods = mods;
    if (type == AG_EV_KEY_UP) {
        ev->key.unicode = 0; /* release carries no character */
    }
}

uint16_t ag_key_from_ascii(char c, uint16_t *mods)
{
    uint16_t m = 0;
    uint16_t k = AG_KEY_NONE;

    if (c >= 'a' && c <= 'z') {
        k = (uint16_t)(AG_KEY_A + (c - 'a'));
    } else if (c >= 'A' && c <= 'Z') {
        k = (uint16_t)(AG_KEY_A + (c - 'A'));
        m = AG_MOD_SHIFT;
    } else if (c >= '1' && c <= '9') {
        k = (uint16_t)(AG_KEY_1 + (c - '1'));
    } else {
        switch (c) {
        case '0':  k = AG_KEY_0; break;
        case ' ':  k = AG_KEY_SPACE; break;

        /* Shifted digits, US layout. */
        case '!':  k = AG_KEY_1; m = AG_MOD_SHIFT; break;
        case '@':  k = AG_KEY_2; m = AG_MOD_SHIFT; break;
        case '#':  k = AG_KEY_3; m = AG_MOD_SHIFT; break;
        case '$':  k = AG_KEY_4; m = AG_MOD_SHIFT; break;
        case '%':  k = AG_KEY_5; m = AG_MOD_SHIFT; break;
        case '^':  k = AG_KEY_6; m = AG_MOD_SHIFT; break;
        case '&':  k = AG_KEY_7; m = AG_MOD_SHIFT; break;
        case '*':  k = AG_KEY_8; m = AG_MOD_SHIFT; break;
        case '(':  k = AG_KEY_9; m = AG_MOD_SHIFT; break;
        case ')':  k = AG_KEY_0; m = AG_MOD_SHIFT; break;

        case '-':  k = AG_KEY_MINUS; break;
        case '_':  k = AG_KEY_MINUS; m = AG_MOD_SHIFT; break;
        case '=':  k = AG_KEY_EQUAL; break;
        case '+':  k = AG_KEY_EQUAL; m = AG_MOD_SHIFT; break;
        case '[':  k = AG_KEY_LBRACKET; break;
        case '{':  k = AG_KEY_LBRACKET; m = AG_MOD_SHIFT; break;
        case ']':  k = AG_KEY_RBRACKET; break;
        case '}':  k = AG_KEY_RBRACKET; m = AG_MOD_SHIFT; break;
        case '\\': k = AG_KEY_BACKSLASH; break;
        case '|':  k = AG_KEY_BACKSLASH; m = AG_MOD_SHIFT; break;
        case ';':  k = AG_KEY_SEMICOLON; break;
        case ':':  k = AG_KEY_SEMICOLON; m = AG_MOD_SHIFT; break;
        case '\'': k = AG_KEY_APOSTROPHE; break;
        case '"':  k = AG_KEY_APOSTROPHE; m = AG_MOD_SHIFT; break;
        case '`':  k = AG_KEY_GRAVE; break;
        case '~':  k = AG_KEY_GRAVE; m = AG_MOD_SHIFT; break;
        case ',':  k = AG_KEY_COMMA; break;
        case '<':  k = AG_KEY_COMMA; m = AG_MOD_SHIFT; break;
        case '.':  k = AG_KEY_PERIOD; break;
        case '>':  k = AG_KEY_PERIOD; m = AG_MOD_SHIFT; break;
        case '/':  k = AG_KEY_SLASH; break;
        case '?':  k = AG_KEY_SLASH; m = AG_MOD_SHIFT; break;
        default:   break;
        }
    }

    if (mods != NULL) {
        *mods = m;
    }
    return k;
}

void ag_vtin_init(ag_vtin_t *in)
{
    if (in != NULL) {
        memset(in, 0, sizeof(*in));
    }
}

bool ag_vtin_busy(const ag_vtin_t *in)
{
    return in != NULL && in->state != AG_VTIN_GROUND;
}

/* ---------------------------------------------------------------------- */
/* CSI parameter handling                                                 */
/* ---------------------------------------------------------------------- */

#define AG_VTIN_MAX_PARAMS 6

typedef struct {
    int32_t value[AG_VTIN_MAX_PARAMS];
    int32_t event; /* kitty/win32 event type after ':', -1 if absent */
    uint8_t count;
    char    prefix; /* '?', '<', '>' or 0 */
} csi_params_t;

static void parse_params(const char *seq, uint8_t len, csi_params_t *out)
{
    memset(out, 0, sizeof(*out));
    for (uint8_t i = 0; i < AG_VTIN_MAX_PARAMS; i++) {
        out->value[i] = -1;
    }
    out->event = -1;

    uint8_t i = 0;
    if (len > 0 && (seq[0] == '?' || seq[0] == '<' || seq[0] == '>')) {
        out->prefix = seq[0];
        i = 1;
    }

    bool in_event = false;
    for (; i < len; i++) {
        const char c = seq[i];
        if (c >= '0' && c <= '9') {
            if (in_event) {
                const int32_t base = (out->event < 0) ? 0 : out->event;
                if (base < 1000000) {
                    out->event = base * 10 + (c - '0');
                }
                continue;
            }
            if (out->count == 0) {
                out->count = 1;
            }
            int32_t *p = &out->value[out->count - 1];
            const int32_t base = (*p < 0) ? 0 : *p;
            if (base < 1000000) {
                *p = base * 10 + (c - '0');
            }
        } else if (c == ':') {
            /*
             * Kitty keyboard protocol: modifiers:event-type as a sub-parameter
             * of the second field.  Digits after ':' are the event type
             * (1=press, 2=repeat, 3=release), not more of the modifier value.
             */
            in_event = true;
            out->event = 0;
        } else if (c == ';') {
            in_event = false;
            if (out->count == 0) {
                out->count = 1;
            }
            if (out->count < AG_VTIN_MAX_PARAMS) {
                out->count++;
            }
        }
    }
}

static int32_t param(const csi_params_t *p, uint8_t index, int32_t fallback)
{
    if (index >= p->count || p->value[index] < 0) {
        return fallback;
    }
    return p->value[index];
}

/*
 * xterm encodes modifiers as 1 + a bitmask, so plain arrows carry 1 and
 * Ctrl+Shift+Up carries 6.
 */
static uint16_t decode_mods(int32_t encoded)
{
    if (encoded <= 1) {
        return 0;
    }
    const int32_t bits = encoded - 1;
    uint16_t      m = 0;

    if (bits & 1) { m |= AG_MOD_SHIFT; }
    if (bits & 2) { m |= AG_MOD_ALT; }
    if (bits & 4) { m |= AG_MOD_CTRL; }
    if (bits & 8) { m |= AG_MOD_GUI; }
    return m;
}

/* CSI <n>~ : the numbering is historical and not worth rationalising. */
static uint16_t tilde_key(int32_t n)
{
    switch (n) {
    case 1:  return AG_KEY_HOME;
    case 2:  return AG_KEY_INSERT;
    case 3:  return AG_KEY_DELETE;
    case 4:  return AG_KEY_END;
    case 5:  return AG_KEY_PAGEUP;
    case 6:  return AG_KEY_PAGEDOWN;
    case 7:  return AG_KEY_HOME;
    case 8:  return AG_KEY_END;
    case 11: return AG_KEY_F1;
    case 12: return AG_KEY_F2;
    case 13: return AG_KEY_F3;
    case 14: return AG_KEY_F4;
    case 15: return AG_KEY_F5;
    case 17: return AG_KEY_F6;
    case 18: return AG_KEY_F7;
    case 19: return AG_KEY_F8;
    case 20: return AG_KEY_F9;
    case 21: return AG_KEY_F10;
    case 23: return AG_KEY_F11;
    case 24: return AG_KEY_F12;
    default: return AG_KEY_NONE;
    }
}

/* SGR mouse reporting: CSI < button ; column ; row (M|m) */
static bool handle_mouse(ag_vtin_t *in, const csi_params_t *p, char final,
                         ag_event_t *ev)
{
    const int32_t b = param(p, 0, 0);
    const int16_t x = (int16_t)(param(p, 1, 1) - 1);
    const int16_t y = (int16_t)(param(p, 2, 1) - 1);

    ev_none(ev);
    ev->ptr.x = x;
    ev->ptr.y = y;

    if (b & 64) {
        ev->type = AG_EV_WHEEL;
        ev->ptr.dy = (b & 1) ? 1 : -1;
    } else if (b & 32) {
        ev->type = AG_EV_POINTER_MOVE;
    } else if (final == 'M') {
        ev->type = AG_EV_POINTER_DOWN;
        in->buttons |= (uint8_t)(1u << (b & 3));
    } else {
        ev->type = AG_EV_POINTER_UP;
        in->buttons &= (uint8_t)~(1u << (b & 3));
    }

    ev->ptr.buttons = in->buttons;
    return true;
}

static bool handle_csi(ag_vtin_t *in, char final, ag_event_t *ev)
{
    csi_params_t p;
    parse_params(in->seq, in->seqlen, &p);

    if (p.prefix == '<') {
        return handle_mouse(in, &p, final, ev);
    }

    switch (final) {
    case 'A': ev_key(ev, AG_KEY_UP, 0, decode_mods(param(&p, 1, 1))); return true;
    case 'B': ev_key(ev, AG_KEY_DOWN, 0, decode_mods(param(&p, 1, 1))); return true;
    case 'C': ev_key(ev, AG_KEY_RIGHT, 0, decode_mods(param(&p, 1, 1))); return true;
    case 'D': ev_key(ev, AG_KEY_LEFT, 0, decode_mods(param(&p, 1, 1))); return true;
    case 'H': ev_key(ev, AG_KEY_HOME, 0, decode_mods(param(&p, 1, 1))); return true;
    case 'F': ev_key(ev, AG_KEY_END, 0, decode_mods(param(&p, 1, 1))); return true;

    /* Some terminals send the function keys this way when modified. */
    case 'P': ev_key(ev, AG_KEY_F1, 0, decode_mods(param(&p, 1, 1))); return true;
    case 'Q': ev_key(ev, AG_KEY_F2, 0, decode_mods(param(&p, 1, 1))); return true;
    case 'S': ev_key(ev, AG_KEY_F4, 0, decode_mods(param(&p, 1, 1))); return true;

    case 'Z': ev_key(ev, AG_KEY_TAB, '\t', AG_MOD_SHIFT); return true;

    case '~': {
        const uint16_t k = tilde_key(param(&p, 0, 0));
        if (k == AG_KEY_NONE) {
            return false;
        }
        ev_key(ev, k, 0, decode_mods(param(&p, 1, 1)));
        return true;
    }

    case 'u': {
        /*
         * CSI u: xterm modifyOtherKeys, and kitty's keyboard protocol.
         * Kitty adds an event type after a colon on the modifiers field
         * (1=press, 2=repeat, 3=release).  Without a colon this is a press.
         */
        const int32_t cp = param(&p, 0, -1);
        if (cp < 0) {
            return false;
        }
        uint16_t base_mods = 0;
        uint16_t k = AG_KEY_NONE;
        switch (cp) {
        /* The control codes name keys, not characters. */
        case 8: case 127: k = AG_KEY_BACKSPACE; break;
        case 9:           k = AG_KEY_TAB; break;
        case 13:          k = AG_KEY_ENTER; break;
        case 27:          k = AG_KEY_ESC; break;
        default:
            if (cp >= 0x20 && cp < 0x7f) {
                k = ag_key_from_ascii((char)cp, &base_mods);
            }
            break;
        }
        const uint16_t m = decode_mods(param(&p, 1, 1));
        /* Ctrl and Alt suppress the character, exactly as in the raw stream. */
        const uint32_t unicode = (m & (AG_MOD_CTRL | AG_MOD_ALT))
                                     ? 0
                                     : (uint32_t)cp;
        const uint16_t mods = (uint16_t)(m | (base_mods & AG_MOD_SHIFT));
        if (p.event == 3) {
            ev_key_typed(ev, AG_EV_KEY_UP, k, 0, mods);
        } else {
            /* press (1), repeat (2), or legacy with no event field */
            ev_key_typed(ev, AG_EV_KEY_DOWN, k, unicode, mods);
            if (p.event == 2) {
                ev->key.repeat = true;
            }
        }
        return true;
    }

    case '_': {
        /*
         * Windows Terminal win32-input-mode:
         * CSI Vk ; Sc ; Uc ; Kd ; Cs ; Rc _
         * Kd is 1 for key-down and 0 for key-up.
         */
        const int32_t vk = param(&p, 0, -1);
        const int32_t uc = param(&p, 2, 0);
        const int32_t kd = param(&p, 3, 1);
        if (vk < 0) {
            return false;
        }
        uint16_t k = AG_KEY_NONE;
        uint16_t base_mods = 0;
        if (vk >= 0x41 && vk <= 0x5A) {
            k = (uint16_t)(AG_KEY_A + (vk - 0x41));
        } else if (vk >= 0x30 && vk <= 0x39) {
            k = (vk == 0x30) ? AG_KEY_0
                             : (uint16_t)(AG_KEY_1 + (vk - 0x31));
        } else {
            switch (vk) {
            case 0x08: k = AG_KEY_BACKSPACE; break;
            case 0x09: k = AG_KEY_TAB; break;
            case 0x0D: k = AG_KEY_ENTER; break;
            case 0x1B: k = AG_KEY_ESC; break;
            case 0x20: k = AG_KEY_SPACE; break;
            case 0x25: k = AG_KEY_LEFT; break;
            case 0x26: k = AG_KEY_UP; break;
            case 0x27: k = AG_KEY_RIGHT; break;
            case 0x28: k = AG_KEY_DOWN; break;
            case 0x2D: k = AG_KEY_INSERT; break;
            case 0x2E: k = AG_KEY_DELETE; break;
            case 0x24: k = AG_KEY_HOME; break;
            case 0x23: k = AG_KEY_END; break;
            case 0x21: k = AG_KEY_PAGEUP; break;
            case 0x22: k = AG_KEY_PAGEDOWN; break;
            default:
                if (uc >= 0x20 && uc < 0x7f) {
                    k = ag_key_from_ascii((char)uc, &base_mods);
                }
                break;
            }
        }
        if (k == AG_KEY_NONE) {
            return false;
        }
        const uint32_t unicode =
            (kd != 0 && uc >= 0x20 && uc < 0x7f) ? (uint32_t)uc : 0u;
        ev_key_typed(ev, (kd != 0) ? AG_EV_KEY_DOWN : AG_EV_KEY_UP, k, unicode,
                     base_mods);
        return true;
    }

    default:
        return false;
    }
}

static bool handle_ss3(char final, ag_event_t *ev)
{
    switch (final) {
    case 'A': ev_key(ev, AG_KEY_UP, 0, 0); return true;
    case 'B': ev_key(ev, AG_KEY_DOWN, 0, 0); return true;
    case 'C': ev_key(ev, AG_KEY_RIGHT, 0, 0); return true;
    case 'D': ev_key(ev, AG_KEY_LEFT, 0, 0); return true;
    case 'H': ev_key(ev, AG_KEY_HOME, 0, 0); return true;
    case 'F': ev_key(ev, AG_KEY_END, 0, 0); return true;
    case 'P': ev_key(ev, AG_KEY_F1, 0, 0); return true;
    case 'Q': ev_key(ev, AG_KEY_F2, 0, 0); return true;
    case 'R': ev_key(ev, AG_KEY_F3, 0, 0); return true;
    case 'S': ev_key(ev, AG_KEY_F4, 0, 0); return true;
    case 'M': ev_key(ev, AG_KEY_KP_ENTER, '\r', 0); return true;
    default:  return false;
    }
}

/* ---------------------------------------------------------------------- */

static bool ground_byte(ag_vtin_t *in, uint8_t b, ag_event_t *ev)
{
    /*
     * Enter, Tab and Backspace arrive as control codes that also happen to be
     * Ctrl+M, Ctrl+I and Ctrl+H.  Naming the key is more useful than naming
     * the combination, so they are matched first.
     */
    switch (b) {
    case 0x00:
        ev_key(ev, AG_KEY_SPACE, 0, AG_MOD_CTRL);
        return true;
    case '\t':
        ev_key(ev, AG_KEY_TAB, '\t', 0);
        return true;
    case '\n':
    case '\r':
        /* Enter reports '\r', the way DOS getch() did. */
        ev_key(ev, AG_KEY_ENTER, '\r', 0);
        return true;
    case 0x08:
    case 0x7f:
        /* Most terminals send DEL for the backspace key. */
        ev_key(ev, AG_KEY_BACKSPACE, 0x08, 0);
        return true;
    case 0x1b:
        in->state = AG_VTIN_ESC;
        in->seqlen = 0;
        return false;
    default:
        break;
    }

    if (b >= 0x01 && b <= 0x1a) {
        /* Ctrl + letter, and only letter: the formula stops at Ctrl+Z. */
        ev_key(ev, (uint16_t)(AG_KEY_A + (b - 1)), 0, AG_MOD_CTRL);
        return true;
    }

    /*
     * The four control codes above Ctrl+Z are Ctrl with a punctuation key, and
     * they are worth naming correctly rather than reporting as a letter that was
     * never pressed.  Ctrl+\ in particular is the only thing a plain terminal can
     * send that the supervisor treats as "stop this now" - a terminal has no way
     * to send Ctrl-Alt-Del.
     */
    switch (b) {
    case 0x1c:
        ev_key(ev, AG_KEY_BACKSLASH, 0, AG_MOD_CTRL);
        return true;
    case 0x1d:
        ev_key(ev, AG_KEY_RBRACKET, 0, AG_MOD_CTRL);
        return true;
    case 0x1e:
        ev_key(ev, AG_KEY_6, 0, AG_MOD_CTRL | AG_MOD_SHIFT);
        return true;
    case 0x1f:
        ev_key(ev, AG_KEY_MINUS, 0, AG_MOD_CTRL | AG_MOD_SHIFT);
        return true;
    default:
        break;
    }

    if (b < 0x7f) {
        uint16_t mods = 0;
        const uint16_t k = ag_key_from_ascii((char)b, &mods);
        ev_key(ev, k, b, mods);
        return true;
    }

    /* UTF-8 lead byte. */
    if ((b & 0xe0) == 0xc0) {
        in->cp = (uint32_t)(b & 0x1f);
        in->cp_remaining = 1;
        in->state = AG_VTIN_UTF8;
    } else if ((b & 0xf0) == 0xe0) {
        in->cp = (uint32_t)(b & 0x0f);
        in->cp_remaining = 2;
        in->state = AG_VTIN_UTF8;
    } else if ((b & 0xf8) == 0xf0) {
        in->cp = (uint32_t)(b & 0x07);
        in->cp_remaining = 3;
        in->state = AG_VTIN_UTF8;
    }
    /* A stray continuation byte is dropped. */
    return false;
}

static bool esc_byte(ag_vtin_t *in, uint8_t b, ag_event_t *ev)
{
    switch (b) {
    case '[':
        in->state = AG_VTIN_CSI;
        in->seqlen = 0;
        return false;
    case 'O':
        in->state = AG_VTIN_SS3;
        return false;
    case 0x1b:
        /* ESC ESC: report the first and keep waiting on the second. */
        ev_key(ev, AG_KEY_ESC, 0x1b, 0);
        in->state = AG_VTIN_ESC;
        return true;
    default:
        break;
    }

    in->state = AG_VTIN_GROUND;

    /* Alt+key is sent as ESC followed by the key. */
    if (b >= 0x20 && b < 0x7f) {
        uint16_t mods = 0;
        const uint16_t k = ag_key_from_ascii((char)b, &mods);
        ev_key(ev, k, b, (uint16_t)(mods | AG_MOD_ALT));
        return true;
    }
    if (b == 0x08 || b == 0x7f) {
        ev_key(ev, AG_KEY_BACKSPACE, 0, AG_MOD_ALT);
        return true;
    }
    if (b == '\r' || b == '\n') {
        ev_key(ev, AG_KEY_ENTER, 0, AG_MOD_ALT);
        return true;
    }
    return false;
}

bool ag_vtin_feed(ag_vtin_t *in, uint8_t b, ag_event_t *ev)
{
    if (in == NULL || ev == NULL) {
        return false;
    }

    switch (in->state) {
    case AG_VTIN_GROUND:
        return ground_byte(in, b, ev);

    case AG_VTIN_ESC:
        return esc_byte(in, b, ev);

    case AG_VTIN_CSI:
        if (b >= 0x40 && b <= 0x7e) {
            in->state = AG_VTIN_GROUND;
            return handle_csi(in, (char)b, ev);
        }
        if (in->seqlen < AG_VTIN_SEQ_MAX) {
            in->seq[in->seqlen++] = (char)b;
        } else {
            in->state = AG_VTIN_GROUND; /* runaway sequence, drop it */
        }
        return false;

    case AG_VTIN_SS3:
        in->state = AG_VTIN_GROUND;
        return handle_ss3((char)b, ev);

    case AG_VTIN_UTF8:
        if ((b & 0xc0) != 0x80) {
            /* Malformed: abandon the code point and reinterpret the byte. */
            in->state = AG_VTIN_GROUND;
            in->cp_remaining = 0;
            return ground_byte(in, b, ev);
        }
        in->cp = (in->cp << 6) | (uint32_t)(b & 0x3f);
        if (--in->cp_remaining == 0) {
            in->state = AG_VTIN_GROUND;
            /* No physical key is known for a decoded code point. */
            ev_key(ev, AG_KEY_NONE, in->cp, 0);
            return true;
        }
        return false;
    }

    return false;
}

bool ag_vtin_idle(ag_vtin_t *in, ag_event_t *ev)
{
    if (in == NULL || ev == NULL) {
        return false;
    }

    const ag_vtin_state_t was = in->state;

    in->state = AG_VTIN_GROUND;
    in->seqlen = 0;
    in->cp_remaining = 0;

    if (was == AG_VTIN_ESC) {
        ev_key(ev, AG_KEY_ESC, 0x1b, 0);
        return true;
    }
    return false;
}
