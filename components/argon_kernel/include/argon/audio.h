/*
 * ArgonOS - built-in mute PCM (/dev/pcmnull); virt/I2S via .SYS.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_AUDIO_H
#define ARGON_AUDIO_H

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

ag_err_t ag_audio_init(void);
ag_err_t ag_pcmmix_init(void); /* multi-open summing mixer → pcmvirt/pcm0/pcmnull */
ag_err_t ag_pcmhw_init(void);  /* /dev/pcm0, when the port has an output */
/* Times /dev/pcm0 ran dry in the middle of a sound - each one a click - and
 * the longest of them in microseconds. */
void ag_pcmhw_dry(uint32_t *times, uint32_t *longest_us);
/* What this board's own converter was given and what it took, in frames,
 * since the start, and its rate - the proof a sound was played here. */
void ag_pcmhw_counts(uint64_t *frames_in, uint64_t *frames_out, uint32_t *rate);
/* The loudest sample (of 32767) given to this board's converter since the
 * last call, and how many channels it is given. */
uint32_t ag_pcmhw_peak(uint32_t *channels);
/* When the first of those were, and the pauses of 300 ms or more: ms since
 * this board's first sound, the top bit set for a pause; how many kept. */
uint32_t ag_pcmhw_dry_at(uint32_t *at_ms, uint32_t cap, uint32_t *pauses);
int      ag_audio_opened(void); /* for process teardown */

extern const ag_audio_api_t ag_audio_api_table;

#ifdef __cplusplus
}
#endif

#endif /* ARGON_AUDIO_H */
