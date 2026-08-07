#ifndef SOUND_OUTPUT_H
#define SOUND_OUTPUT_H

#include <stdint.h>

static inline void Sound_Init(void) {}
static inline void Sound_Update(int16_t *sound_buffer, unsigned long len)
{
    (void)sound_buffer;
    (void)len;
}
static inline void Sound_Close(void) {}
static inline void Sound_Pause(void) {}
static inline void Sound_Unpause(void) {}

#endif
