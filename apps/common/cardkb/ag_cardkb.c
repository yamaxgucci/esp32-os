/*
 * ArgonOS - an I2C keyboard that answers like M5Stack's CardKB (ag_cardkb.h).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "ag_cardkb.h"

#include <stddef.h>

#include <argon/abi.h>
#include <argon/keys.h>

AG_CARDKB_FN int ag_cardkb_judge(uint8_t key, uint8_t check, int *mask)
{
    if (mask != NULL) {
        *mask = -1;
    }
    if (key == 0u) {
        return AG_CARDKB_NONE;
    }
    if (check == (uint8_t)(key ^ AG_CARDKB_CHECK) ||
        check == (uint8_t)(key ^ AG_CARDKB_CHECK1)) {
        if (mask != NULL) {
            *mask = (check == (uint8_t)(key ^ AG_CARDKB_CHECK)) ? 0 : 1;
        }
        return AG_CARDKB_KEY;
    }
    /* A real CardKB sends one byte and the line idles high after it.  'Z'
     * (0x5A) and 0xA5 check as 0xFF too, and are keys either way. */
    if (check == 0xFFu) {
        return AG_CARDKB_PLAIN;
    }
    return AG_CARDKB_GARBLED;
}

/* The punctuation of a US keyboard: the character, its key, shifted or not. */
static const struct {
    char     c;
    uint8_t  key;
    uint8_t  shift;
} k_punct[] = {
    {' ', AG_KEY_SPACE, 0},       {'!', AG_KEY_1, 1},          {'@', AG_KEY_2, 1},
    {'#', AG_KEY_3, 1},           {'$', AG_KEY_4, 1},          {'%', AG_KEY_5, 1},
    {'^', AG_KEY_6, 1},           {'&', AG_KEY_7, 1},          {'*', AG_KEY_8, 1},
    {'(', AG_KEY_9, 1},           {')', AG_KEY_0, 1},          {'-', AG_KEY_MINUS, 0},
    {'_', AG_KEY_MINUS, 1},       {'=', AG_KEY_EQUAL, 0},      {'+', AG_KEY_EQUAL, 1},
    {'[', AG_KEY_LBRACKET, 0},    {'{', AG_KEY_LBRACKET, 1},   {']', AG_KEY_RBRACKET, 0},
    {'}', AG_KEY_RBRACKET, 1},    {'\\', AG_KEY_BACKSLASH, 0}, {'|', AG_KEY_BACKSLASH, 1},
    {';', AG_KEY_SEMICOLON, 0},   {':', AG_KEY_SEMICOLON, 1},  {'\'', AG_KEY_APOSTROPHE, 0},
    {'"', AG_KEY_APOSTROPHE, 1},  {'`', AG_KEY_GRAVE, 0},      {'~', AG_KEY_GRAVE, 1},
    {',', AG_KEY_COMMA, 0},       {'<', AG_KEY_COMMA, 1},      {'.', AG_KEY_PERIOD, 0},
    {'>', AG_KEY_PERIOD, 1},      {'/', AG_KEY_SLASH, 0},      {'?', AG_KEY_SLASH, 1},
};

AG_CARDKB_FN uint16_t ag_cardkb_key(uint8_t code, uint32_t *unicode,
                                    uint16_t *mods)
{
    uint32_t ch = 0;
    uint16_t m = 0;
    uint16_t k = 0;

    if (code >= 'a' && code <= 'z') {
        k = (uint16_t)(AG_KEY_A + (code - 'a'));
        ch = code;
    } else if (code >= 'A' && code <= 'Z') {
        k = (uint16_t)(AG_KEY_A + (code - 'A'));
        m = AG_MOD_SHIFT;
        ch = code;
    } else if (code >= '1' && code <= '9') {
        k = (uint16_t)(AG_KEY_1 + (code - '1'));
        ch = code;
    } else if (code == '0') {
        k = AG_KEY_0;
        ch = code;
    } else {
        switch (code) {
        case 0x08: k = AG_KEY_BACKSPACE; ch = 0x08; break;
        case 0x09: k = AG_KEY_TAB;       ch = '\t'; break;
        case 0x0D: k = AG_KEY_ENTER;     ch = '\r'; break;
        case 0x1B: k = AG_KEY_ESC;       ch = 0x1B; break;
        case 0x7F: k = AG_KEY_DELETE;    break;
        case AG_CARDKB_LEFT:  k = AG_KEY_LEFT;  break;
        case AG_CARDKB_UP:    k = AG_KEY_UP;    break;
        case AG_CARDKB_DOWN:  k = AG_KEY_DOWN;  break;
        case AG_CARDKB_RIGHT: k = AG_KEY_RIGHT; break;
        default:
            for (unsigned i = 0; i < sizeof(k_punct) / sizeof(k_punct[0]); i++) {
                if ((uint8_t)k_punct[i].c == code) {
                    k = k_punct[i].key;
                    m = k_punct[i].shift ? AG_MOD_SHIFT : 0u;
                    ch = code;
                    break;
                }
            }
            break;
        }
    }
    if (unicode != NULL) {
        *unicode = (k != 0u) ? ch : 0u;
    }
    if (mods != NULL) {
        *mods = (k != 0u) ? m : 0u;
    }
    return k;
}
