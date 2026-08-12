/*
 * ArgonOS - command line editing.
 *
 * The buffer holds one byte per character, in the machine's code page: the same
 * bytes the screen holds and the same bytes a command receives as its arguments.
 * A typed code point is converted here, at the edge, and a character the page
 * cannot represent is not accepted at all - which is better than accepting it as
 * two bytes that would then behave as two characters everywhere else.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/lineedit.h>

#include <string.h>

#include <argon/codepage.h>

static inline bool is_word_char(char c)
{
    return c != ' ' && c != '\t';
}

/* One byte is one character, so a step is a step. */
static uint16_t step_left(const ag_lineedit_t *le, uint16_t pos)
{
    (void)le;
    return (pos == 0) ? 0 : (uint16_t)(pos - 1);
}

static uint16_t step_right(const ag_lineedit_t *le, uint16_t pos)
{
    return (pos >= le->len) ? le->len : (uint16_t)(pos + 1);
}

static uint16_t word_left(const ag_lineedit_t *le, uint16_t pos)
{
    while (pos > 0 && !is_word_char(le->buf[pos - 1])) {
        pos = step_left(le, pos);
    }
    while (pos > 0 && is_word_char(le->buf[pos - 1])) {
        pos = step_left(le, pos);
    }
    return pos;
}

static uint16_t word_right(const ag_lineedit_t *le, uint16_t pos)
{
    while (pos < le->len && is_word_char(le->buf[pos])) {
        pos = step_right(le, pos);
    }
    while (pos < le->len && !is_word_char(le->buf[pos])) {
        pos = step_right(le, pos);
    }
    return pos;
}

/* Removes [from, to) and leaves the cursor at `from`. */
static void erase_range(ag_lineedit_t *le, uint16_t from, uint16_t to)
{
    if (to <= from) {
        return;
    }
    memmove(le->buf + from, le->buf + to, (size_t)(le->len - to));
    le->len = (uint16_t)(le->len - (to - from));
    le->buf[le->len] = '\0';
    le->cursor = from;
}

/* False when the character does not fit, or the code page has no byte for it. */
static bool insert(ag_lineedit_t *le, uint32_t codepoint)
{
    const int32_t byte = ag_cp_from_unicode(ag_cp_active(), codepoint);

    if (byte < 0) {
        return false;
    }
    if ((size_t)le->len + 2 > sizeof(le->buf)) {
        return false;
    }

    memmove(le->buf + le->cursor + 1, le->buf + le->cursor,
            (size_t)(le->len - le->cursor));
    le->buf[le->cursor] = (char)byte;
    le->len = (uint16_t)(le->len + 1);
    le->cursor = (uint16_t)(le->cursor + 1);
    le->buf[le->len] = '\0';
    return true;
}

void ag_lineedit_init(ag_lineedit_t *le)
{
    if (le != NULL) {
        memset(le, 0, sizeof(*le));
        le->history_pos = -1;
    }
}

void ag_lineedit_reset(ag_lineedit_t *le)
{
    le->buf[0] = '\0';
    le->len = 0;
    le->cursor = 0;
    le->history_pos = -1;
    le->parked[0] = '\0';
}

void ag_lineedit_set(ag_lineedit_t *le, const char *text)
{
    if (text == NULL) {
        text = "";
    }
    size_t n = strlen(text);
    if (n >= sizeof(le->buf)) {
        n = sizeof(le->buf) - 1;
    }
    memcpy(le->buf, text, n);
    le->buf[n] = '\0';
    le->len = (uint16_t)n;
    le->cursor = (uint16_t)n;
}

void ag_lineedit_remember(ag_lineedit_t *le, const char *line)
{
    if (le == NULL || line == NULL || line[0] == '\0') {
        return;
    }
    /* Repeating the previous command should not fill the history with it. */
    if (le->history_count > 0 && strcmp(le->history[0], line) == 0) {
        return;
    }

    const uint8_t keep = (le->history_count < AG_HISTORY_DEPTH)
                             ? le->history_count
                             : (uint8_t)(AG_HISTORY_DEPTH - 1);
    for (uint8_t i = keep; i > 0; i--) {
        memcpy(le->history[i], le->history[i - 1], sizeof(le->history[0]));
    }

    size_t n = strlen(line);
    if (n >= sizeof(le->history[0])) {
        n = sizeof(le->history[0]) - 1;
    }
    memcpy(le->history[0], line, n);
    le->history[0][n] = '\0';

    if (le->history_count < AG_HISTORY_DEPTH) {
        le->history_count++;
    }
}

/* Up and down walk the history, parking whatever was being typed. */
static ag_line_result_t browse_history(ag_lineedit_t *le, int direction)
{
    if (le->history_count == 0) {
        return AG_LINE_IDLE;
    }

    int16_t pos = (int16_t)(le->history_pos + direction);
    if (pos < -1) {
        pos = -1;
    }
    if (pos >= (int16_t)le->history_count) {
        return AG_LINE_IDLE; /* already at the oldest entry */
    }
    if (pos == le->history_pos) {
        return AG_LINE_IDLE;
    }

    if (le->history_pos == -1) {
        memcpy(le->parked, le->buf, (size_t)le->len + 1);
    }

    le->history_pos = pos;
    ag_lineedit_set(le, (pos == -1) ? le->parked : le->history[pos]);
    return AG_LINE_CHANGED;
}

ag_line_result_t ag_lineedit_key(ag_lineedit_t *le, const ag_event_t *ev)
{
    if (le == NULL || ev == NULL || ev->type != AG_EV_KEY_DOWN) {
        return AG_LINE_IDLE;
    }

    const uint16_t mods = ev->key.mods;
    const bool     ctrl = (mods & AG_MOD_CTRL) != 0;
    const bool     alt = (mods & AG_MOD_ALT) != 0;

    if (ctrl) {
        switch (ev->key.keycode) {
        case AG_KEY_C:
            return AG_LINE_CANCEL;

        case AG_KEY_D:
            if (le->len == 0) {
                return AG_LINE_EOF;
            }
            if (le->cursor < le->len) {
                erase_range(le, le->cursor, step_right(le, le->cursor));
                return AG_LINE_CHANGED;
            }
            return AG_LINE_IDLE;

        case AG_KEY_U: /* discard the whole line */
            if (le->len == 0) {
                return AG_LINE_IDLE;
            }
            le->len = 0;
            le->cursor = 0;
            le->buf[0] = '\0';
            return AG_LINE_CHANGED;

        case AG_KEY_K: /* discard to end of line */
            if (le->cursor >= le->len) {
                return AG_LINE_IDLE;
            }
            erase_range(le, le->cursor, le->len);
            return AG_LINE_CHANGED;

        case AG_KEY_W: { /* discard the word before the cursor */
            const uint16_t from = word_left(le, le->cursor);
            if (from == le->cursor) {
                return AG_LINE_IDLE;
            }
            erase_range(le, from, le->cursor);
            return AG_LINE_CHANGED;
        }

        case AG_KEY_A:
            if (le->cursor == 0) {
                return AG_LINE_IDLE;
            }
            le->cursor = 0;
            return AG_LINE_CHANGED;

        case AG_KEY_E:
            if (le->cursor == le->len) {
                return AG_LINE_IDLE;
            }
            le->cursor = le->len;
            return AG_LINE_CHANGED;

        case AG_KEY_LEFT: {
            const uint16_t to = word_left(le, le->cursor);
            if (to == le->cursor) {
                return AG_LINE_IDLE;
            }
            le->cursor = to;
            return AG_LINE_CHANGED;
        }

        case AG_KEY_RIGHT: {
            const uint16_t to = word_right(le, le->cursor);
            if (to == le->cursor) {
                return AG_LINE_IDLE;
            }
            le->cursor = to;
            return AG_LINE_CHANGED;
        }

        default:
            return AG_LINE_IDLE;
        }
    }

    switch (ev->key.keycode) {
    case AG_KEY_ENTER:
        return AG_LINE_DONE;

    case AG_KEY_TAB:
        return AG_LINE_COMPLETE;

    case AG_KEY_BACKSPACE: {
        if (le->cursor == 0) {
            return AG_LINE_IDLE;
        }
        erase_range(le, step_left(le, le->cursor), le->cursor);
        return AG_LINE_CHANGED;
    }

    case AG_KEY_DELETE:
        if (le->cursor >= le->len) {
            return AG_LINE_IDLE;
        }
        erase_range(le, le->cursor, step_right(le, le->cursor));
        return AG_LINE_CHANGED;

    case AG_KEY_LEFT:
        if (le->cursor == 0) {
            return AG_LINE_IDLE;
        }
        le->cursor = step_left(le, le->cursor);
        return AG_LINE_CHANGED;

    case AG_KEY_RIGHT:
        if (le->cursor >= le->len) {
            return AG_LINE_IDLE;
        }
        le->cursor = step_right(le, le->cursor);
        return AG_LINE_CHANGED;

    case AG_KEY_HOME:
        if (le->cursor == 0) {
            return AG_LINE_IDLE;
        }
        le->cursor = 0;
        return AG_LINE_CHANGED;

    case AG_KEY_END:
        if (le->cursor == le->len) {
            return AG_LINE_IDLE;
        }
        le->cursor = le->len;
        return AG_LINE_CHANGED;

    case AG_KEY_UP:
        return browse_history(le, +1);

    case AG_KEY_DOWN:
        return browse_history(le, -1);

    default:
        break;
    }

    /* Alt combinations are commands, not text. */
    if (alt) {
        return AG_LINE_IDLE;
    }

    if (ev->key.unicode >= 0x20 && ev->key.unicode != 0x7f) {
        return insert(le, ev->key.unicode) ? AG_LINE_CHANGED : AG_LINE_IDLE;
    }
    return AG_LINE_IDLE;
}
