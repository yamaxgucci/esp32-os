/*
 * Minimal RIFF PCM16 WAV loader for HostFS / guest FS.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_WAV_H
#define AG_WAV_H

#include <stdint.h>

typedef struct ag_wav_pcm {
    int16_t *data;     /* mono s16, ag_malloc */
    uint32_t frames;
    uint32_t rate;
} ag_wav_pcm_t;

/*
 * Load path (e.g. "h:\\grain\\demo.wav") into mono s16.
 * Stereo is mixed; non-16-bit / non-PCM rejected.
 * On success fills out and returns 0; caller frees out->data with ag_free.
 */
int ag_wav_load(const char *path, ag_wav_pcm_t *out);

/*
 * The same, stopping after `max_frames` (0 for the built-in 30-second cap).
 *
 * For a caller that would rather have the beginning of a file than nothing at
 * all.  A board's arena is not a PC's: a seven-second take at 22.05 kHz is 308
 * KB in one block, and the S3-Zero's largest free block is 192 - so the choice
 * is between four seconds of the take and a failure that reads as "cannot read
 * the file".  Ask for what fits and say so; a take that is played in a loop
 * loses nothing by being shorter.
 */
int ag_wav_load_max(const char *path, ag_wav_pcm_t *out, uint32_t max_frames);

void ag_wav_free(ag_wav_pcm_t *w);

#endif /* AG_WAV_H */
