/*
 * ArgonOS - key codes.
 *
 * Key codes are USB HID keyboard/keypad usage IDs.  Using the HID numbering
 * rather than inventing our own means the USB host driver passes values
 * through untouched, and every other input source (PS/2, a VT100 escape
 * sequence over telnet, a matrix keypad driver) converts into the same space.
 *
 * A key code identifies the physical key.  The character it produced, if any,
 * arrives separately in ag_event_t.key.unicode, because that depends on the
 * layout and on the modifiers.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_KEYS_H
#define ARGON_KEYS_H

#ifdef __cplusplus
extern "C" {
#endif

enum ag_key {
    AG_KEY_NONE = 0x00,

    AG_KEY_A = 0x04, AG_KEY_B, AG_KEY_C, AG_KEY_D, AG_KEY_E, AG_KEY_F,
    AG_KEY_G, AG_KEY_H, AG_KEY_I, AG_KEY_J, AG_KEY_K, AG_KEY_L, AG_KEY_M,
    AG_KEY_N, AG_KEY_O, AG_KEY_P, AG_KEY_Q, AG_KEY_R, AG_KEY_S, AG_KEY_T,
    AG_KEY_U, AG_KEY_V, AG_KEY_W, AG_KEY_X, AG_KEY_Y, AG_KEY_Z,

    AG_KEY_1 = 0x1e, AG_KEY_2, AG_KEY_3, AG_KEY_4, AG_KEY_5,
    AG_KEY_6, AG_KEY_7, AG_KEY_8, AG_KEY_9, AG_KEY_0,

    AG_KEY_ENTER = 0x28,
    AG_KEY_ESC = 0x29,
    AG_KEY_BACKSPACE = 0x2a,
    AG_KEY_TAB = 0x2b,
    AG_KEY_SPACE = 0x2c,
    AG_KEY_MINUS = 0x2d,      /* - _ */
    AG_KEY_EQUAL = 0x2e,      /* = + */
    AG_KEY_LBRACKET = 0x2f,   /* [ { */
    AG_KEY_RBRACKET = 0x30,   /* ] } */
    AG_KEY_BACKSLASH = 0x31,  /* \ | */
    AG_KEY_NONUS_HASH = 0x32,
    AG_KEY_SEMICOLON = 0x33,  /* ; : */
    AG_KEY_APOSTROPHE = 0x34, /* ' " */
    AG_KEY_GRAVE = 0x35,      /* ` ~ */
    AG_KEY_COMMA = 0x36,      /* , < */
    AG_KEY_PERIOD = 0x37,     /* . > */
    AG_KEY_SLASH = 0x38,      /* / ? */
    AG_KEY_CAPSLOCK = 0x39,

    AG_KEY_F1 = 0x3a, AG_KEY_F2, AG_KEY_F3, AG_KEY_F4, AG_KEY_F5, AG_KEY_F6,
    AG_KEY_F7, AG_KEY_F8, AG_KEY_F9, AG_KEY_F10, AG_KEY_F11, AG_KEY_F12,

    AG_KEY_PRINTSCREEN = 0x46,
    AG_KEY_SCROLLLOCK = 0x47,
    AG_KEY_PAUSE = 0x48,
    AG_KEY_INSERT = 0x49,
    AG_KEY_HOME = 0x4a,
    AG_KEY_PAGEUP = 0x4b,
    AG_KEY_DELETE = 0x4c,
    AG_KEY_END = 0x4d,
    AG_KEY_PAGEDOWN = 0x4e,
    AG_KEY_RIGHT = 0x4f,
    AG_KEY_LEFT = 0x50,
    AG_KEY_DOWN = 0x51,
    AG_KEY_UP = 0x52,
    AG_KEY_NUMLOCK = 0x53,

    AG_KEY_KP_DIVIDE = 0x54,
    AG_KEY_KP_MULTIPLY = 0x55,
    AG_KEY_KP_MINUS = 0x56,
    AG_KEY_KP_PLUS = 0x57,
    AG_KEY_KP_ENTER = 0x58,
    AG_KEY_KP_1 = 0x59, AG_KEY_KP_2, AG_KEY_KP_3, AG_KEY_KP_4, AG_KEY_KP_5,
    AG_KEY_KP_6, AG_KEY_KP_7, AG_KEY_KP_8, AG_KEY_KP_9, AG_KEY_KP_0,
    AG_KEY_KP_PERIOD = 0x63,

    AG_KEY_APPLICATION = 0x65,

    AG_KEY_F13 = 0x68, AG_KEY_F14, AG_KEY_F15, AG_KEY_F16, AG_KEY_F17,
    AG_KEY_F18, AG_KEY_F19, AG_KEY_F20, AG_KEY_F21, AG_KEY_F22, AG_KEY_F23,
    AG_KEY_F24,

    AG_KEY_LCTRL = 0xe0,
    AG_KEY_LSHIFT = 0xe1,
    AG_KEY_LALT = 0xe2,
    AG_KEY_LGUI = 0xe3,
    AG_KEY_RCTRL = 0xe4,
    AG_KEY_RSHIFT = 0xe5,
    AG_KEY_RALT = 0xe6,
    AG_KEY_RGUI = 0xe7,
};

#ifdef __cplusplus
}
#endif

#endif /* ARGON_KEYS_H */
