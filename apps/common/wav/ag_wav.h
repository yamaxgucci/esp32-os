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

void ag_wav_free(ag_wav_pcm_t *w);

#endif /* AG_WAV_H */
