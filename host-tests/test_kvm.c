/*
 * ArgonOS - the remote keyboard's report builder, on the host.
 *
 * This exists because of what the board cannot check.  Plugged into a PC, the
 * only instrument available is a person looking at a text editor, and a person
 * looking at a text editor catches "nothing happens" and "the wrong letter" and
 * misses everything else: a shift that is set on the release report, a chord
 * whose fourth key silently drops, a key that stays down when the phone walks
 * out of range.  Those look like a bad cable.
 *
 * So the whole path from a line off the socket to the bytes bound for the
 * endpoint is compiled here and asked what it produces.  The one thing this
 * cannot prove is that a report shaped correctly means what we think to a real
 * host - that is what plugging it into a PC is for - but everything up to the
 * endpoint is settled without one.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"

#include "ag_kvm.h"

#include <string.h>

/* A line as it arrives from the phone, into frames. */
static int parse(ag_kvm_state_t *st, const char *line, ag_usbhid_frame_t *out,
                 int max)
{
    return ag_kvm_parse(line, strlen(line), st, out, max);
}

static void test_report_shape(void)
{
    ag_kvm_kbd_t k;
    uint8_t      rep[AG_USBHID_KBD_LEN];

    ag_kvm_kbd_reset(&k);
    ag_kvm_kbd_report(&k, rep);
    for (int i = 0; i < (int)AG_USBHID_KBD_LEN; i++) {
        AG_CHECK_INT(rep[i], 0);
    }

    /* One key down: byte 2, and byte 1 stays the reserved zero. */
    AG_CHECK(ag_kvm_kbd_key(&k, 0x04, true)); /* 'a' */
    ag_kvm_kbd_report(&k, rep);
    AG_CHECK_INT(rep[0], 0);
    AG_CHECK_INT(rep[1], 0);
    AG_CHECK_INT(rep[2], 0x04);
    AG_CHECK_INT(rep[3], 0);

    /* Modifiers are a byte of their own and do not take a slot. */
    ag_kvm_kbd_mods(&k, AG_KVM_LSHIFT | AG_KVM_LCTRL);
    ag_kvm_kbd_report(&k, rep);
    AG_CHECK_INT(rep[0], AG_KVM_LSHIFT | AG_KVM_LCTRL);
    AG_CHECK_INT(rep[2], 0x04);
    AG_CHECK_INT(rep[3], 0);

    /* Usage 0 is not a key. */
    AG_CHECK(!ag_kvm_kbd_key(&k, 0, true));
}

static void test_chords_and_release(void)
{
    ag_kvm_kbd_t k;
    uint8_t      rep[AG_USBHID_KBD_LEN];

    ag_kvm_kbd_reset(&k);
    for (uint8_t u = 0x04; u < 0x0A; u++) { /* six keys: a b c d e f */
        AG_CHECK(ag_kvm_kbd_key(&k, u, true));
    }
    ag_kvm_kbd_report(&k, rep);
    for (int i = 0; i < AG_KVM_SLOTS; i++) {
        AG_CHECK_INT(rep[2 + i], 0x04 + i);
    }

    /* Pressing one that is already down is auto-repeat, not a second slot. */
    AG_CHECK(ag_kvm_kbd_key(&k, 0x05, true));
    ag_kvm_kbd_report(&k, rep);
    AG_CHECK_INT(rep[2 + 5], 0x09);

    /* Releasing from the middle packs the slots rather than leaving a hole:
     * a host that sees a key change slot reads it as released and pressed. */
    AG_CHECK(ag_kvm_kbd_key(&k, 0x06, false)); /* 'c', the third */
    ag_kvm_kbd_report(&k, rep);
    AG_CHECK_INT(rep[2], 0x04);
    AG_CHECK_INT(rep[3], 0x05);
    AG_CHECK_INT(rep[4], 0x07);
    AG_CHECK_INT(rep[5], 0x08);
    AG_CHECK_INT(rep[6], 0x09);
    AG_CHECK_INT(rep[7], 0);

    /* Releasing something that was never down changes nothing. */
    AG_CHECK(ag_kvm_kbd_key(&k, 0x50, false));
    ag_kvm_kbd_report(&k, rep);
    AG_CHECK_INT(rep[2], 0x04);
    AG_CHECK_INT(rep[7], 0);
}

static void test_rollover(void)
{
    ag_kvm_kbd_t k;
    uint8_t      rep[AG_USBHID_KBD_LEN];

    ag_kvm_kbd_reset(&k);
    for (uint8_t u = 0x04; u < 0x0B; u++) { /* seven keys */
        AG_CHECK(ag_kvm_kbd_key(&k, u, true));
    }
    ag_kvm_kbd_report(&k, rep);
    /* Every slot says rollover - what a keyboard sends, rather than a silently
     * dropped key. */
    for (int i = 0; i < AG_KVM_SLOTS; i++) {
        AG_CHECK_INT(rep[2 + i], AG_KVM_ROLLOVER);
    }
    /* Modifiers still get through: Ctrl+Alt+Del works with a hand on the
     * keyboard. */
    ag_kvm_kbd_mods(&k, AG_KVM_LCTRL);
    ag_kvm_kbd_report(&k, rep);
    AG_CHECK_INT(rep[0], AG_KVM_LCTRL);

    /* Coming back out of rollover ends with nothing held rather than with a
     * guess: see the note in ag_kvm_kbd_key. */
    AG_CHECK(ag_kvm_kbd_key(&k, 0x0A, false));
    ag_kvm_kbd_report(&k, rep);
    for (int i = 0; i < AG_KVM_SLOTS; i++) {
        AG_CHECK_INT(rep[2 + i], 0);
    }
}

static void test_char_map(void)
{
    uint8_t u = 0, m = 0;

    AG_CHECK(ag_kvm_char_usage('a', &u, &m));
    AG_CHECK_INT(u, 0x04);
    AG_CHECK_INT(m, 0);

    AG_CHECK(ag_kvm_char_usage('z', &u, &m));
    AG_CHECK_INT(u, 0x1D);
    AG_CHECK_INT(m, 0);

    /* A capital is the same key with shift, not a different key. */
    AG_CHECK(ag_kvm_char_usage('A', &u, &m));
    AG_CHECK_INT(u, 0x04);
    AG_CHECK_INT(m, AG_KVM_LSHIFT);

    /* The digit row: 1 is 0x1E and 0 is the odd one at the end. */
    AG_CHECK(ag_kvm_char_usage('1', &u, &m));
    AG_CHECK_INT(u, 0x1E);
    AG_CHECK_INT(m, 0);
    AG_CHECK(ag_kvm_char_usage('9', &u, &m));
    AG_CHECK_INT(u, 0x26);
    AG_CHECK(ag_kvm_char_usage('0', &u, &m));
    AG_CHECK_INT(u, 0x27);
    AG_CHECK_INT(m, 0);

    /* Shifted digits land on the same keys.  These are the pairs that get
     * transposed when the table is written by hand, which it was. */
    AG_CHECK(ag_kvm_char_usage('!', &u, &m));
    AG_CHECK_INT(u, 0x1E);
    AG_CHECK_INT(m, AG_KVM_LSHIFT);
    AG_CHECK(ag_kvm_char_usage('@', &u, &m));
    AG_CHECK_INT(u, 0x1F);
    AG_CHECK(ag_kvm_char_usage('(', &u, &m));
    AG_CHECK_INT(u, 0x26);
    AG_CHECK(ag_kvm_char_usage(')', &u, &m));
    AG_CHECK_INT(u, 0x27);
    AG_CHECK_INT(m, AG_KVM_LSHIFT);

    /* Punctuation, and its shifted twin on the same key. */
    AG_CHECK(ag_kvm_char_usage(';', &u, &m));
    AG_CHECK_INT(u, 0x33);
    AG_CHECK_INT(m, 0);
    AG_CHECK(ag_kvm_char_usage(':', &u, &m));
    AG_CHECK_INT(u, 0x33);
    AG_CHECK_INT(m, AG_KVM_LSHIFT);
    AG_CHECK(ag_kvm_char_usage('/', &u, &m));
    AG_CHECK_INT(u, 0x38);
    AG_CHECK(ag_kvm_char_usage('?', &u, &m));
    AG_CHECK_INT(u, 0x38);
    AG_CHECK_INT(m, AG_KVM_LSHIFT);

    /* The keys with no glyph. */
    AG_CHECK(ag_kvm_char_usage(' ', &u, &m));
    AG_CHECK_INT(u, 0x2C);
    AG_CHECK(ag_kvm_char_usage('\n', &u, &m));
    AG_CHECK_INT(u, 0x28);
    AG_CHECK(ag_kvm_char_usage('\t', &u, &m));
    AG_CHECK_INT(u, 0x2B);
    AG_CHECK(ag_kvm_char_usage('\b', &u, &m));
    AG_CHECK_INT(u, 0x2A);

    /* Outside ASCII there is no key to ask for.  A Cyrillic letter arrives as
     * UTF-8 bytes, each of which has the high bit set and none of which is a
     * position on a keyboard - see the note in ag_kvm.h. */
    AG_CHECK(!ag_kvm_char_usage((char)0xD0, &u, &m));
    AG_CHECK(!ag_kvm_char_usage((char)0x92, &u, &m));
    AG_CHECK(!ag_kvm_char_usage((char)0x01, &u, &m));
}

static void test_typing(void)
{
    ag_usbhid_frame_t f[16];
    size_t            consumed = 0;
    ag_kvm_kbd_t      k;

    /* Two frames per character: the press, then the release. */
    ag_kvm_kbd_reset(&k);
    int n = ag_kvm_type_frames(&k, "Hi", 2, f, 16, &consumed);
    AG_CHECK_INT(n, 4);
    AG_CHECK_INT((int)consumed, 2);

    AG_CHECK_INT(f[0].kind, AG_USBHID_KBD);
    AG_CHECK_INT(f[0].len, AG_USBHID_KBD_LEN);
    AG_CHECK_INT(f[0].data[0], AG_KVM_LSHIFT); /* 'H' */
    AG_CHECK_INT(f[0].data[2], 0x0B);
    /* The release lets go of the shift as well as the letter. */
    AG_CHECK_INT(f[1].data[0], 0);
    AG_CHECK_INT(f[1].data[2], 0);
    AG_CHECK_INT(f[2].data[0], 0); /* 'i' */
    AG_CHECK_INT(f[2].data[2], 0x0C);
    AG_CHECK_INT(f[3].data[2], 0);

    /* Nothing is left held afterwards. */
    AG_CHECK_INT(k.held, 0);
    AG_CHECK_INT(k.mods, 0);

    /* A buffer that runs out stops on a whole character and says where. */
    ag_kvm_kbd_reset(&k);
    n = ag_kvm_type_frames(&k, "abc", 3, f, 4, &consumed);
    AG_CHECK_INT(n, 4);
    AG_CHECK_INT((int)consumed, 2);

    /* Characters with no key are skipped, not refused: a paste that loses a
     * symbol should still paste. */
    ag_kvm_kbd_reset(&k);
    n = ag_kvm_type_frames(&k, "a\xd0\x92" "b", 4, f, 16, &consumed);
    AG_CHECK_INT(n, 4);
    AG_CHECK_INT(f[0].data[2], 0x04);
    AG_CHECK_INT(f[2].data[2], 0x05);
}

/*
 * Typing while something is held.  This is here because the emulator caught it:
 * a version that built each report from an empty keyboard told the far machine
 * that the held key had come up, while this end went on believing it was down.
 * Neither side could see the disagreement - it showed up much later, as a key
 * that behaved strangely.
 */
static void test_typing_over_held_keys(void)
{
    ag_usbhid_frame_t f[8];
    size_t            consumed = 0;
    ag_kvm_kbd_t      k;

    ag_kvm_kbd_reset(&k);
    AG_CHECK(ag_kvm_kbd_key(&k, 0x29, true)); /* Escape, held down */
    ag_kvm_kbd_mods(&k, AG_KVM_LCTRL);

    const int n = ag_kvm_type_frames(&k, "a", 1, f, 8, &consumed);
    AG_CHECK_INT(n, 2);

    /* The press carries Escape as well as the letter, and keeps Ctrl. */
    AG_CHECK_INT(f[0].data[0], AG_KVM_LCTRL);
    AG_CHECK_INT(f[0].data[2], 0x29);
    AG_CHECK_INT(f[0].data[3], 0x04);
    /* The release gives up the letter and nothing else. */
    AG_CHECK_INT(f[1].data[0], AG_KVM_LCTRL);
    AG_CHECK_INT(f[1].data[2], 0x29);
    AG_CHECK_INT(f[1].data[3], 0);

    /* And the state agrees with the last report that was sent. */
    AG_CHECK_INT(k.held, 1);
    AG_CHECK_INT(k.keys[0], 0x29);
    AG_CHECK_INT(k.mods, AG_KVM_LCTRL);

    /* A capital in the middle of a chord: shift is added for the letter and
     * taken away again, and Ctrl survives both. */
    const int n2 = ag_kvm_type_frames(&k, "A", 1, f, 8, &consumed);
    AG_CHECK_INT(n2, 2);
    AG_CHECK_INT(f[0].data[0], AG_KVM_LCTRL | AG_KVM_LSHIFT);
    AG_CHECK_INT(f[1].data[0], AG_KVM_LCTRL);
}

static void test_parse_keys(void)
{
    ag_kvm_state_t    st;
    ag_usbhid_frame_t f[4];

    ag_kvm_reset(&st);

    AG_CHECK_INT(parse(&st, "K d 4 0", f, 4), 1);
    AG_CHECK_INT(f[0].kind, AG_USBHID_KBD);
    AG_CHECK_INT(f[0].data[2], 4);

    /* The modifier byte is told to us, not counted up here: the browser knows
     * what is held and a count gets stuck the first time a keyup is lost. */
    AG_CHECK_INT(parse(&st, "K d 5 2", f, 4), 1);
    AG_CHECK_INT(f[0].data[0], AG_KVM_LSHIFT);
    AG_CHECK_INT(f[0].data[2], 4);
    AG_CHECK_INT(f[0].data[3], 5);

    AG_CHECK_INT(parse(&st, "K u 4 0", f, 4), 1);
    AG_CHECK_INT(f[0].data[0], 0);
    AG_CHECK_INT(f[0].data[2], 5);
    AG_CHECK_INT(f[0].data[3], 0);

    /* Shift going down on its own: no usage, only the modifier. */
    AG_CHECK_INT(parse(&st, "K d 0 2", f, 4), 1);
    AG_CHECK_INT(f[0].data[0], AG_KVM_LSHIFT);

    /* A line ending is not part of the line. */
    AG_CHECK_INT(parse(&st, "K d 6 0\r\n", f, 4), 1);
    AG_CHECK_INT(f[0].data[3], 6);
}

/*
 * A step, on a wire that carries no relative interface.
 *
 * The far machine has one pointer and it is absolute, so `M` is added to a
 * remembered position here rather than sent as a step.  What must not happen -
 * and is the whole reason this is tested rather than assumed - is a step that
 * silently does nothing because nobody remembered where the cursor was.
 */
static void test_parse_step(void)
{
    ag_kvm_state_t    st;
    ag_usbhid_frame_t f[4];

    ag_kvm_reset(&st);
    const int mid = AG_USBHID_ABS_MAX / 2;

    AG_CHECK_INT(parse(&st, "M -3 7 0 1", f, 4), 1);
    AG_CHECK_INT(f[0].kind, AG_USBHID_ABS);
    AG_CHECK_INT(f[0].len, AG_USBHID_ABS_LEN);
    AG_CHECK_INT(f[0].data[0], 1);
    {
        const int x = f[0].data[1] | (f[0].data[2] << 8);
        const int y = f[0].data[3] | (f[0].data[4] << 8);
        AG_CHECK_INT(x, mid + (-3 << AG_KVM_REL_SHIFT));
        AG_CHECK_INT(y, mid + (7 << AG_KVM_REL_SHIFT));
    }

    /* Steps accumulate; that is what makes a trackpad a trackpad. */
    AG_CHECK_INT(parse(&st, "M 1 1 0 0", f, 4), 1);
    {
        const int x = f[0].data[1] | (f[0].data[2] << 8);
        AG_CHECK_INT(x, mid + (-3 << AG_KVM_REL_SHIFT) +
                            (1 << AG_KVM_REL_SHIFT));
    }

    /* A swipe off the edge stops at the edge rather than wrapping round to
     * the other side, which is what an unclamped sum looks like. */
    AG_CHECK_INT(parse(&st, "M 99999 99999 0 0", f, 4), 1);
    AG_CHECK_INT(f[0].data[1], 0xFF);
    AG_CHECK_INT(f[0].data[2], 0x7F);
    AG_CHECK_INT(f[0].data[3], 0xFF);
    AG_CHECK_INT(f[0].data[4], 0x7F);
    AG_CHECK_INT(parse(&st, "M -99999 -99999 0 0", f, 4), 1);
    AG_CHECK_INT(f[0].data[1], 0);
    AG_CHECK_INT(f[0].data[2], 0);
    AG_CHECK_INT(f[0].data[3], 0);
    AG_CHECK_INT(f[0].data[4], 0);
}

static void test_parse_pointer(void)
{
    ag_kvm_state_t    st;
    ag_usbhid_frame_t f[4];

    ag_kvm_reset(&st);

    /* Absolute: little-endian, and clamped to the axis. */
    AG_CHECK_INT(parse(&st, "A 300 32767 0 0", f, 4), 1);
    AG_CHECK_INT(f[0].kind, AG_USBHID_ABS);
    AG_CHECK_INT(f[0].len, AG_USBHID_ABS_LEN);
    AG_CHECK_INT(f[0].data[1], 300 & 0xFF);
    AG_CHECK_INT(f[0].data[2], 300 >> 8);
    AG_CHECK_INT(f[0].data[3], 0xFF);
    AG_CHECK_INT(f[0].data[4], 0x7F);

    AG_CHECK_INT(parse(&st, "A 99999 -5 0 0", f, 4), 1);
    AG_CHECK_INT(f[0].data[1], 0xFF);
    AG_CHECK_INT(f[0].data[2], 0x7F);
    AG_CHECK_INT(f[0].data[3], 0);
    AG_CHECK_INT(f[0].data[4], 0);

    /* The wheel is signed and rides on the same report, whichever line
     * produced it. */
    AG_CHECK_INT(parse(&st, "M 0 0 -1 0", f, 4), 1);
    AG_CHECK_INT((int8_t)f[0].data[5], -1);
    AG_CHECK_INT(parse(&st, "A 10 10 3 0", f, 4), 1);
    AG_CHECK_INT((int8_t)f[0].data[5], 3);
}

static void test_parse_release(void)
{
    ag_kvm_state_t    st;
    ag_usbhid_frame_t f[4];

    ag_kvm_reset(&st);
    AG_CHECK_INT(parse(&st, "K d 4 2", f, 4), 1);
    AG_CHECK_INT(parse(&st, "A 1000 2000 0 1", f, 4), 1);

    /* Release everything: one report per interface - the keyboard and the one
     * pointer the far machine has. */
    const int n = parse(&st, "R", f, 4);
    AG_CHECK_INT(n, 2);
    AG_CHECK_INT(f[0].kind, AG_USBHID_KBD);
    AG_CHECK_INT(f[0].data[0], 0);
    AG_CHECK_INT(f[0].data[2], 0);
    AG_CHECK_INT(f[1].kind, AG_USBHID_ABS);
    AG_CHECK_INT(f[1].data[0], 0);
    /* Released where it stands: the position is not disturbed by letting go. */
    AG_CHECK_INT(f[1].data[1], 1000 & 0xFF);
    AG_CHECK_INT(f[1].data[2], 1000 >> 8);
    AG_CHECK_INT(f[1].data[3], 2000 & 0xFF);
    AG_CHECK_INT(f[1].data[4], 2000 >> 8);

    /* Both frames or neither: a caller with room for one would otherwise be
     * told a release happened that did not. */
    AG_CHECK_INT(parse(&st, "R", f, 1), -AG_ENOSPC);
}

static void test_parse_junk(void)
{
    ag_kvm_state_t    st;
    ag_usbhid_frame_t f[4];

    ag_kvm_reset(&st);

    /* A blank line is a blank line, not an error - a socket produces them. */
    AG_CHECK_INT(parse(&st, "", f, 4), 0);
    AG_CHECK_INT(parse(&st, "   ", f, 4), 0);
    AG_CHECK_INT(parse(&st, "\n", f, 4), 0);

    AG_CHECK_INT(parse(&st, "X 1 2", f, 4), -AG_EINVAL);
    AG_CHECK_INT(parse(&st, "K", f, 4), -AG_EINVAL);
    AG_CHECK_INT(parse(&st, "K x 4 0", f, 4), -AG_EINVAL);
    AG_CHECK_INT(parse(&st, "K d", f, 4), -AG_EINVAL);
    AG_CHECK_INT(parse(&st, "K d 4", f, 4), -AG_EINVAL);
    AG_CHECK_INT(parse(&st, "K d abc 0", f, 4), -AG_EINVAL);
    AG_CHECK_INT(parse(&st, "K d 999 0", f, 4), -AG_EINVAL);
    AG_CHECK_INT(parse(&st, "M 1 2 3", f, 4), -AG_EINVAL);
    AG_CHECK_INT(parse(&st, "A 1 2 3", f, 4), -AG_EINVAL);

    /* Not NUL-terminated and not ours to write to: a line that is all digits
     * with no command must not walk off the end. */
    const char raw[] = {'M', ' ', '1'};
    AG_CHECK_INT(ag_kvm_parse(raw, sizeof(raw), &st, f, 4), -AG_EINVAL);
}

static void test_frame_layout(void)
{
    /* The frame is what the kernel's usbhid_write reads back with memcpy, so
     * its size is part of the ABI rather than an implementation detail. */
    AG_CHECK_INT(sizeof(ag_usbhid_frame_t), AG_USBHID_FRAME);
    AG_CHECK_INT(AG_USBHID_DATA_MAX + 2, AG_USBHID_FRAME);
    AG_CHECK(AG_USBHID_KBD_LEN <= AG_USBHID_DATA_MAX);
    AG_CHECK(AG_USBHID_ABS_LEN <= AG_USBHID_DATA_MAX);
    /* Two is a hole where the relative mouse was, not a spare number: a frame
     * from something built earlier must not become a pointer report. */
    AG_CHECK_INT(AG_USBHID_KBD, 1);
    AG_CHECK_INT(AG_USBHID_ABS, 3);

    ag_usbhid_frame_t f;
    const uint8_t     data[3] = {1, 2, 3};
    ag_kvm_frame(&f, AG_USBHID_ABS, data, 3);
    AG_CHECK_INT(f.kind, AG_USBHID_ABS);
    AG_CHECK_INT(f.len, 3);
    AG_CHECK_INT(f.data[0], 1);
    AG_CHECK_INT(f.data[3], 0); /* the rest of the frame is zeroed */
}

void run_kvm_tests(void)
{
    test_report_shape();
    test_chords_and_release();
    test_rollover();
    test_char_map();
    test_typing();
    test_typing_over_held_keys();
    test_parse_keys();
    test_parse_step();
    test_parse_pointer();
    test_parse_release();
    test_parse_junk();
    test_frame_layout();
}
