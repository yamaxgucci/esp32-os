/*
 * Streaming MP3 decoder wrapper (minimp3) for ArgonOS apps.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_MP3_H
#define AG_MP3_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ag_mp3 ag_mp3_t;

/*
 * Open path, load the whole file into RAM (max 512 KiB), then decode from
 * memory. Returns 0 on success. Larger files fail — keep tracks small or
 * split them for the current heap budget.
 */
int ag_mp3_open(ag_mp3_t **out, const char *path);

void ag_mp3_close(ag_mp3_t *m);

/* Sample rate / channels after first successful decode (0 until then). */
uint32_t ag_mp3_rate(const ag_mp3_t *m);
uint8_t  ag_mp3_channels(const ag_mp3_t *m);

/* Estimated duration in milliseconds (0 if unknown). */
uint32_t ag_mp3_duration_ms(const ag_mp3_t *m);

/* Current playback position estimate in milliseconds. */
uint32_t ag_mp3_position_ms(const ag_mp3_t *m);

/*
 * Decode up to max_frames stereo (or mono duplicated) s16 interleaved samples
 * into out[]. Returns frames written (>=0), 0 at EOF, <0 on error.
 */
int ag_mp3_read(ag_mp3_t *m, int16_t *out, int max_frames);

/* Approximate seek by file fraction 0..1000; returns 0 on success. */
int ag_mp3_seek_permille(ag_mp3_t *m, int permille);

/* Title from ID3v1/v2 if found (truncated); empty string otherwise. */
const char *ag_mp3_title(const ag_mp3_t *m);

#ifdef __cplusplus
}
#endif

#endif /* AG_MP3_H */
