#ifndef SOUND_OUTPUT_H
#define SOUND_OUTPUT_H

#include <stdint.h>

/*
 * ArgonOS SMS audio sink (no I2S yet): optional WAV file or discard ("mock").
 * Call Sound_SetPath() before Sound_Init().  Path NULL / "mock" / "nul" → mock.
 */
void Sound_SetPath(const char *path);
void Sound_Init(void);
void Sound_Update(int16_t *sound_buffer, unsigned long len);
void Sound_Close(void);
void Sound_Pause(void);
void Sound_Unpause(void);
int  Sound_IsActive(void);

#endif
