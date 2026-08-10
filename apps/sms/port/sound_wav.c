/*
 * ArgonOS SMS — PCM sink: /dev/* via audio_out, or stereo s16 WAV file.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sound_output.h"

#include <argon/argon.h>

#include "audio_out.h"
#include "smsplus.h"
#include "sound.h"

#define WAV_HDR 44u

enum {
    SINK_DEV = 0,
    SINK_WAV = 1,
};

static char        s_path[AG_PATH_MAX];
static int         s_sink = SINK_DEV;
static int         s_active;
static int         s_write_err_reported;
static ag_handle_t s_fd = -1; /* WAV file or device handle */
static uint32_t    s_data_bytes;
static uint32_t    s_rate = SOUND_FREQUENCY;

static void sound_sink_frame(int16_t *sound_buffer, int32_t len);

static void mem_copy(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) {
        *d++ = *s++;
    }
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void write_header(uint32_t data_bytes)
{
    uint8_t h[WAV_HDR];
    const uint32_t byte_rate = s_rate * 4u;
    const uint16_t block_align = 4;

    mem_copy(h, "RIFF", 4);
    put_u32(h + 4, 36u + data_bytes);
    mem_copy(h + 8, "WAVE", 4);
    mem_copy(h + 12, "fmt ", 4);
    put_u32(h + 16, 16u);
    put_u16(h + 20, 1u);
    put_u16(h + 22, 2u);
    put_u32(h + 24, s_rate);
    put_u32(h + 28, byte_rate);
    put_u16(h + 32, block_align);
    put_u16(h + 34, 16u);
    mem_copy(h + 36, "data", 4);
    put_u32(h + 40, data_bytes);

    if (s_fd < 0 || s_sink != SINK_WAV) {
        return;
    }
    (void)ag_seek(s_fd, 0, AG_SEEK_SET);
    (void)ag_write(s_fd, h, WAV_HDR);
    (void)ag_seek(s_fd, (int64_t)(WAV_HDR + data_bytes), AG_SEEK_SET);
}

void Sound_SetPath(const char *path)
{
    if (ag_audio_out_resolve(path, s_path, sizeof(s_path))) {
        s_sink = SINK_DEV;
    } else {
        s_sink = SINK_WAV;
    }
}

void Sound_Init(void)
{
    s_active = 0;
    s_data_bytes = 0;
    s_write_err_reported = 0;
    s_rate = SOUND_FREQUENCY;
    argon_sms_audio_sink = NULL;
    if (s_fd >= 0) {
        if (s_sink == SINK_DEV) {
            (void)ag_dev_close(s_fd);
        } else {
            ag_close(s_fd);
        }
        s_fd = -1;
    }
    if (s_path[0] == '\0') {
        Sound_SetPath("mock");
    }

    if (s_sink == SINK_DEV) {
        s_fd = ag_audio_out_open_dev(s_path, s_rate, 2);
        if (s_fd < 0) {
            ag_printf("sms: %s: %s\n", s_path, ag_strerror((ag_err_t)s_fd));
            Sound_SetPath("mock");
            s_fd = ag_audio_out_open_dev(s_path, s_rate, 2);
            if (s_fd < 0) {
                return;
            }
        }
        s_active = 1;
        argon_sms_audio_sink = sound_sink_frame;
        ag_printf("sms: sound = %s @ %u Hz\n", s_path, (unsigned)s_rate);
        return;
    }

    s_fd = ag_open(s_path, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (s_fd < 0) {
        ag_printf("sms: %s: %s\n", s_path, ag_strerror((ag_err_t)s_fd));
        return;
    }
    write_header(0);
    s_active = 1;
    argon_sms_audio_sink = sound_sink_frame;
    ag_printf("sms: sound = wav %s @ %u Hz\n", s_path, (unsigned)s_rate);
}

void Sound_Update(int16_t *sound_buffer, unsigned long len)
{
    if (!s_active || sound_buffer == NULL || len == 0 || s_fd < 0) {
        return;
    }
    if (s_sink == SINK_DEV) {
        const size_t  bytes = (size_t)len * 4u;
        const int32_t n = ag_dev_write(s_fd, sound_buffer, bytes);
        if (n < 0 && !s_write_err_reported) {
            s_write_err_reported = 1;
            ag_printf("sms: %s: %s\n", s_path, ag_strerror((ag_err_t)n));
        }
        return;
    }
    {
        const size_t  bytes = (size_t)len * 4u;
        const int32_t n = ag_write(s_fd, sound_buffer, bytes);
        if (n > 0) {
            s_data_bytes += (uint32_t)n;
            return;
        }
        if (n < 0 && !s_write_err_reported) {
            s_write_err_reported = 1;
            ag_printf("sms: %s: %s\n", s_path, ag_strerror((ag_err_t)n));
        }
    }
}

static void sound_sink_frame(int16_t *sound_buffer, int32_t len)
{
    Sound_Update(sound_buffer, (unsigned long)len);
}

void Sound_Close(void)
{
    argon_sms_audio_sink = NULL;
    if (s_fd >= 0) {
        if (s_sink == SINK_WAV) {
            write_header(s_data_bytes);
            ag_close(s_fd);
            ag_printf("sms: wav closed, %u bytes PCM\n", (unsigned)s_data_bytes);
        } else {
            (void)ag_dev_close(s_fd);
        }
        s_fd = -1;
    }
    s_active = 0;
    s_data_bytes = 0;
}

void Sound_Pause(void) {}
void Sound_Unpause(void) {}
int  Sound_IsActive(void) { return s_active; }
