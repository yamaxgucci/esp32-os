#ifndef SMSPLUS_H
#define SMSPLUS_H

#include <stdint.h>

#define VIDEO_WIDTH_SMS  256
#define VIDEO_HEIGHT_SMS 192
#define VIDEO_WIDTH_GG   160
#define VIDEO_HEIGHT_GG  144

/* PSG → WAV / mock (no I2S). 22.05 kHz keeps CPU and file size modest. */
#define SOUND_FREQUENCY 22050

extern uint16_t *sms_bitmap;

#define LOCK_VIDEO   ((void)0);
#define UNLOCK_VIDEO ((void)0);

typedef struct {
    char gamename[64];
} gamedata_t;

void smsp_state(uint8_t slot_number, uint8_t mode);

#endif
