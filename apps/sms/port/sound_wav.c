/*
 * ArgonOS SMS — PCM sink: write stereo s16 LE WAV, or discard (mock).
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sound_output.h"

#include <argon/argon.h>

#include "smsplus.h"
#include "sound.h"

#define WAV_HDR 44u

static char        s_path[AG_PATH_MAX];
static int         s_mock = 1;
static int         s_active;
static int         s_write_err_reported;
static ag_handle_t s_fd = -1;
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

    if (s_fd < 0) {
        return;
    }
    (void)ag_seek(s_fd, 0, AG_SEEK_SET);
    (void)ag_write(s_fd, h, WAV_HDR);
    (void)ag_seek(s_fd, (int64_t)(WAV_HDR + data_bytes), AG_SEEK_SET);
}

void Sound_SetPath(const char *path)
{
    s_path[0] = '\0';
    s_mock = 1;
    if (path == NULL || path[0] == '\0') {
        return;
    }
    if (path[0] == 'm' || path[0] == 'M') {
        if ((path[1] == 'o' || path[1] == 'O') &&
            (path[2] == 'c' || path[2] == 'C') &&
            (path[3] == 'k' || path[3] == 'K') && path[4] == '\0') {
            return;
        }
    }
    if ((path[0] == 'n' || path[0] == 'N') &&
        (path[1] == 'u' || path[1] == 'U') &&
        (path[2] == 'l' || path[2] == 'L') && path[3] == '\0') {
        return;
    }
    size_t n = 0;
    while (path[n] && n + 1u < sizeof(s_path)) {
        s_path[n] = path[n];
        n++;
    }
    s_path[n] = '\0';
    s_mock = 0;
}

void Sound_Init(void)
{
    s_active = 0;
    s_data_bytes = 0;
    s_write_err_reported = 0;
    s_rate = SOUND_FREQUENCY;
    argon_sms_audio_sink = NULL;
    if (s_fd >= 0) {
        ag_close(s_fd);
        s_fd = -1;
    }
    if (s_mock) {
        s_active = 1;
        argon_sms_audio_sink = sound_sink_frame;
        ag_printf("sms: sound = mock (PSG, discard)\n");
        return;
    }
    if (s_path[0] == '\0') {
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
    if (!s_active || sound_buffer == NULL || len == 0) {
        return;
    }
    if (s_mock || s_fd < 0) {
        return;
    }
    const size_t bytes = (size_t)len * 4u;
    const int32_t n = ag_write(s_fd, sound_buffer, bytes);
    if (n > 0) {
        s_data_bytes += (uint32_t)n;
        if ((size_t)n < bytes && !s_write_err_reported) {
            s_write_err_reported = 1;
            ag_printf("sms: %s: %s\n", s_path, ag_strerror(-AG_ENOSPC));
        }
        return;
    }
    if (n < 0 && !s_write_err_reported) {
        s_write_err_reported = 1;
        ag_printf("sms: %s: %s\n", s_path, ag_strerror((ag_err_t)n));
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
        write_header(s_data_bytes);
        ag_close(s_fd);
        s_fd = -1;
        ag_printf("sms: wav closed, %u bytes PCM\n", (unsigned)s_data_bytes);
    }
    s_active = 0;
    s_data_bytes = 0;
}

void Sound_Pause(void) {}
void Sound_Unpause(void) {}
int  Sound_IsActive(void) { return s_active; }
