#ifndef SOUND_OUTPUT_H
#define SOUND_OUTPUT_H

#include <stdint.h>

/*
 * ArgonOS SMS audio sink: /dev/pcm* (audio_out) or a WAV file path.
 * Call Sound_SetPath() before Sound_Init().  "mock"/"nul" → /dev/pcmnull.
 */
void Sound_SetPath(const char *path);
void Sound_Init(void);
void Sound_Update(int16_t *sound_buffer, unsigned long len);
void Sound_Close(void);
void Sound_Pause(void);
void Sound_Unpause(void);
int  Sound_IsActive(void);

#endif
