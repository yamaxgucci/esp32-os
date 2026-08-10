/*
 * ArgonOS - built-in mute PCM (/dev/pcmnull); virt/I2S via .SYS.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_AUDIO_H
#define ARGON_AUDIO_H

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

ag_err_t ag_audio_init(void);
int      ag_audio_opened(void); /* for process teardown */

extern const ag_audio_api_t ag_audio_api_table;

#ifdef __cplusplus
}
#endif

#endif /* ARGON_AUDIO_H */
