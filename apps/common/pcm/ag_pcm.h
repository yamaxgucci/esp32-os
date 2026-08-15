/*
 * ag_pcm — open / write / pace / stats for ArgonOS PCM sinks.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_PCM_H
#define AG_PCM_H

#include <argon/argon.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AG_PCM_CHUNK_DEFAULT 441u

enum {
    AG_PCM_SINK_DEV = 0,
    AG_PCM_SINK_WAV = 1
};

typedef struct ag_pcm {
    ag_handle_t fd;
    char        path[AG_PATH_MAX];
    uint32_t    rate;
    uint32_t    chunk;
    uint32_t    chunk_us;
    uint8_t     channels;
    uint8_t     sink; /* AG_PCM_SINK_* */
    ag_time_t   next_due;
    uint32_t    render_us;
    uint32_t    send_us;
    uint32_t    load_pct;
    uint32_t    late;
    uint32_t    drop;
    uint32_t    resync;
    uint32_t    wav_bytes;
    ag_audio_stats_t stats;
    int         stats_ok;
} ag_pcm_t;

/* arg: pcmvirt/pcmmix/pcmnull/path, or "*.wav" for RIFF on VFS. */
int     ag_pcm_open(ag_pcm_t *o, const char *arg, uint32_t rate, uint8_t ch);
void    ag_pcm_close(ag_pcm_t *o);
void    ag_pcm_set_chunk(ag_pcm_t *o, uint32_t frames);

int32_t ag_pcm_write(ag_pcm_t *o, const int16_t *pcm, int32_t frames);
void    ag_pcm_mark_render(ag_pcm_t *o, uint32_t us);

void    ag_pcm_pace_start(ag_pcm_t *o);
void    ag_pcm_pace_wait(ag_pcm_t *o);
int32_t ag_pcm_slack_us(const ag_pcm_t *o);
void    ag_pcm_poll_stats(ag_pcm_t *o);

#ifdef __cplusplus
}
#endif

#endif /* AG_PCM_H */
