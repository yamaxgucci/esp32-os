/*
 * ArgonOS - the part of the remote keyboard that has no board in it.
 *
 * Everything between "a finger touched the phone" and "these bytes go to
 * /dev/usbhid0" lives here, on purpose: it is the half of the system that can
 * be wrong in ways nobody would ever see.  A mouse that moves the wrong way is
 * obvious the first time somebody tries it; a shift key set on the release
 * report instead of the press, or a chord that loses its fourth key, or a
 * semicolon that comes out as a colon after a hundred characters - those look
 * like a flaky cable and are not.  So this file has no device in it, no socket
 * and no allocation, and host-tests/test_kvm.c compiles it and asks it
 * questions.
 *
 * Three things:
 *
 *   - the state of what is held down, because HID has no "key" event.  A report
 *     says which keys are down *now*, all of them, every time; whoever sends one
 *     has to remember what was already held.
 *   - the ASCII to usage-code map, which is a US layout and says so.
 *   - the phone's wire format, which is lines of text because a line of text is
 *     something a browser can send, a person can type into a socket by hand, and
 *     a test can write down.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef AG_KVM_H
#define AG_KVM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Modifier bits, byte 0 of a boot keyboard report (HID 1.11, §8.3). */
#define AG_KVM_LCTRL 0x01u
#define AG_KVM_LSHIFT 0x02u
#define AG_KVM_LALT 0x04u
#define AG_KVM_LGUI 0x08u
#define AG_KVM_RCTRL 0x10u
#define AG_KVM_RSHIFT 0x20u
#define AG_KVM_RALT 0x40u
#define AG_KVM_RGUI 0x80u

/* The six key slots a boot keyboard report carries. */
#define AG_KVM_SLOTS 6

/*
 * When a seventh key goes down a real keyboard does not drop one - it sends the
 * rollover error code in all six slots, and the host understands that to mean
 * "too many, I cannot tell you which".  This does the same, so a person leaning
 * on the phone's keyboard sees what leaning on a keyboard does.
 */
#define AG_KVM_ROLLOVER 0x01u

typedef struct {
    uint8_t mods;
    uint8_t keys[AG_KVM_SLOTS];
    uint8_t held; /* how many slots are in use; > AG_KVM_SLOTS means rollover */
} ag_kvm_kbd_t;

/*
 * Everything the far machine currently believes.
 *
 * The pointer's position is here because the far machine has exactly one
 * pointer and it is absolute: a report says where the cursor *is*, not how far
 * it moved, so somebody has to remember where that was.  This is that somebody,
 * and it is why a trackpad still works with no relative interface on the wire -
 * a step is added here and sent as a position.
 *
 * The caveat is real and worth knowing: if a hand moves a mouse on the far
 * machine, its cursor and this position disagree, and the next step from the
 * phone puts the cursor back where this thinks it was.  Every absolute pointer
 * behaves that way; it is the price of being able to point at a picture.
 */
typedef struct {
    ag_kvm_kbd_t kbd;
    uint16_t     abs_x;
    uint16_t     abs_y;
    uint8_t      buttons;
} ag_kvm_state_t;

/*
 * How far one unit of a relative step moves the pointer.  The axis is
 * 0..32767 across whatever screen the far machine has, and sixteen units per
 * step puts that at 2048 - so on a 1920-wide screen a step is about a pixel,
 * which is what a finger on a trackpad expects.  A shift rather than a divide
 * because it is on the path of every pointer move.
 */
#define AG_KVM_REL_SHIFT 4

void ag_kvm_reset(ag_kvm_state_t *s);

void ag_kvm_kbd_reset(ag_kvm_kbd_t *k);

/*
 * A key went down or came up.  Returns false for usage 0, which is not a key.
 *
 * Pressing a key that is already down is not an error and does not take a
 * second slot: that is auto-repeat, and on USB auto-repeat is the host's job.
 */
bool ag_kvm_kbd_key(ag_kvm_kbd_t *k, uint8_t usage, bool down);

/*
 * The whole modifier byte, replaced rather than adjusted - see the note on the
 * wire format below for why the browser's version of it is the one to trust.
 */
void ag_kvm_kbd_mods(ag_kvm_kbd_t *k, uint8_t mods);

/* The eight bytes as they go on the wire. */
void ag_kvm_kbd_report(const ag_kvm_kbd_t *k, uint8_t out[AG_USBHID_KBD_LEN]);

/*
 * One ASCII character as a key on a US layout: the usage code, and whether
 * shift is held for it.  False for anything this cannot type, which is
 * everything outside printable ASCII plus tab, newline and backspace.
 *
 * That limit is the protocol's, not this code's.  A HID keyboard sends
 * positions on a keyboard, and which letter a position produces is decided by
 * the layout the *far* machine is set to.  There is no key on a US keyboard
 * that produces a Cyrillic letter, so there is no report that asks for one: a
 * host set to a Russian layout will produce Cyrillic from these same codes and
 * a host set to English will not, and neither is something this end can see or
 * fix.  Text that must arrive as itself whatever the far machine is set to goes
 * through the clipboard, which needs an agent over there - M2's problem.
 */
bool ag_kvm_char_usage(char c, uint8_t *usage, uint8_t *mods);

/* ------------------------------------------------------------------------ */
/* Frames                                                                   */
/* ------------------------------------------------------------------------ */

/* Fills one frame for /dev/usbhid0. */
void ag_kvm_frame(ag_usbhid_frame_t *f, ag_usbhid_kind_t kind,
                  const uint8_t *data, uint8_t len);

/* The two reports, built into a frame. */
void ag_kvm_frame_kbd(ag_usbhid_frame_t *f, const ag_kvm_kbd_t *k);
void ag_kvm_frame_abs(ag_usbhid_frame_t *f, uint8_t buttons, int x, int y,
                      int wheel);

/*
 * Type a run of ASCII as press/release pairs, stopping when `out` is full.
 * Returns how many frames were written and sets *consumed to how many input
 * bytes they cover, so a caller with a fixed buffer can come back for the rest.
 *
 * Two frames per character, and never fewer: the press and the release have to
 * be separate reports, because a report is a statement about what is held down
 * and "pressed and released" is not a state.  A character this cannot type is
 * skipped rather than refused - a paste that loses one symbol should still
 * paste.
 *
 * It types *on the keyboard as it is*, which is why it takes the state rather
 * than building reports from nothing.  A report says what is held down, all of
 * it, so one built from an empty keyboard while a key is actually held tells
 * the far machine that key came up - and this end would go on believing it was
 * down.  The two would then disagree until something released everything, and
 * that disagreement is invisible from either side.  Each character leaves the
 * state exactly as it found it.
 */
int ag_kvm_type_frames(ag_kvm_kbd_t *k, const char *text, size_t len,
                       ag_usbhid_frame_t *out, int max, size_t *consumed);

/* ------------------------------------------------------------------------ */
/* The phone's wire format                                                  */
/* ------------------------------------------------------------------------ */

/*
 * One line, one event.  Fields are separated by single spaces; numbers are
 * decimal and may be negative.
 *
 *   K d <usage> <mods>              a key went down, with the modifiers held
 *   K u <usage> <mods>              and came up
 *   M <dx> <dy> <wheel> <buttons>   a step: added to the remembered position
 *   A <x> <y> <wheel> <buttons>     a position outright, 0..32767 each way
 *   R                               release everything: every key, every button
 *
 * Both pointer lines produce the same report, because the far machine has one
 * pointer and it is absolute.  `M` is kept because a trackpad produces steps
 * and a browser has no idea how big the far screen is; `A` is what a finger on
 * a picture of that screen produces, where it does.
 *
 * `mods` is the whole modifier byte rather than a change to it, because that is
 * what the browser gives us - a keydown event carries the state of shift, ctrl,
 * alt and meta as they are at that instant - and because a modifier tracked by
 * counting presses gets stuck the first time a keyup is lost to the network.
 * The state here is told, not inferred.
 *
 * R exists for the same reason.  A phone that walks out of Wi-Fi range mid-word
 * leaves a key down on the far machine forever; the application sends R when
 * the socket closes, and it is a line rather than a convention so that a person
 * debugging with a raw socket can send it too.
 *
 * Returns the number of frames written to `out`, or a negative ag_err_t:
 * -AG_EINVAL for a line this does not understand, -AG_ENOSPC when `max` is too
 * small for the frames the line produces.
 */
int ag_kvm_parse(const char *line, size_t len, ag_kvm_state_t *st,
                 ag_usbhid_frame_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* AG_KVM_H */
