/*
 * ArgonOS - normalised pad / button input layer.
 *
 * Sources (HostFS PADPUSH today; USB HID and GPIO later) push level-state
 * snapshots here.  Applications read them through inp->pad / btn / btnp, or
 * as the /dev/joy0 character device.  H:\sms.pad remains a compatibility
 * view of the same cache.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_INPUT_H
#define ARGON_INPUT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wire / device snapshot size.  Bytes 0..2 are the legacy 3-byte PADPUSH. */
#define AG_PAD_BYTES 6u
#define AG_PAD_VER   1u

/* High-byte button bits (pad0hi / pad1hi). */
#define AG_PAD_HI_C     0x01u
#define AG_PAD_HI_START 0x02u
#define AG_PAD_HI_X     0x04u
#define AG_PAD_HI_Y     0x08u
#define AG_PAD_HI_Z     0x10u
#define AG_PAD_HI_MODE  0x20u

/* Sys byte (unchanged). */
#define AG_PAD_SYS_PAUSE 0x01u
#define AG_PAD_SYS_QUIT  0x02u

/* Registers joy0 and clears state.  Called from ag_devices_init. */
ag_err_t ag_input_init(void);

/*
 * Accept a PADPUSH-shaped snapshot.  len may be 3 (legacy) or 6 (current);
 * missing high bytes are zeroed.  Safe to call from the HostFS RX path.
 */
void ag_input_push_pad(const uint8_t *blob, size_t len);

/*
 * Copy the last snapshot when it is no older than max_age_ms (0 = any).
 * out must hold AG_PAD_BYTES bytes.
 */
bool ag_input_pad_peek(uint8_t out[AG_PAD_BYTES], uint32_t max_age_ms);

/* which: 0=pad0, 1=pad1, 2=sys, 3=pad0hi, 4=pad1hi.  0 when stale/missing. */
uint32_t ag_input_pad_byte(int which);

/*
 * Level button on pad 0 or 1.  id is ag_btn.  Prefers a live snapshot; when
 * none is available, pad 0 falls back to sticky console keys.
 */
int32_t ag_input_btnp(int pad, int id);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_INPUT_H */
