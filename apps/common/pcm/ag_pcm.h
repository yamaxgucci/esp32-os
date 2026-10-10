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
    /*
     * Set the first time a frame is read back from this stream.
     *
     * A converter that records as well as plays hands the loop its own clock:
     * the read cannot answer until the block has arrived, which is exactly the
     * pacing the sleep was imitating.  Running both means two clocks - the
     * codec's crystal and ag_micros() - and they drift, so the deadline slips
     * a little every block and the stream spends its life resynchronising
     * against a schedule nothing is actually keeping.
     */
    uint8_t     clocked_by_in;
    /* The last time the pacing was asked, for measuring a turn of the loop. */
    ag_time_t   paced_at;
    ag_audio_stats_t stats;
    int         stats_ok;
} ag_pcm_t;

/* arg: pcmvirt/pcmmix/pcmnull/path, or "*.wav" for RIFF on VFS. */
int     ag_pcm_open(ag_pcm_t *o, const char *arg, uint32_t rate, uint8_t ch);
void    ag_pcm_close(ag_pcm_t *o);
void    ag_pcm_set_chunk(ag_pcm_t *o, uint32_t frames);

int32_t ag_pcm_write(ag_pcm_t *o, const int16_t *pcm, int32_t frames);

/*
 * Frames from the same device, for a machine whose converter records as well
 * as plays.  The same handle as the write, on purpose: one open, one clock,
 * both directions - a second open would be a second stream that drifts.
 *
 * Returns frames, 0 when there are none yet, or a negative -AG_Exxx; -AG_ENOSYS
 * on a sink that has no input at all, which a file sink never does.
 */
int32_t ag_pcm_read(ag_pcm_t *o, int16_t *pcm, int32_t frames);
void    ag_pcm_mark_render(ag_pcm_t *o, uint32_t us);

/*
 * Hold the loop to the stream's rate.
 *
 * ag_pcm_pace_wait does nothing once the stream has been read from: there the
 * converter is the clock, and a second one would only fight it.
 */
void    ag_pcm_pace_start(ag_pcm_t *o);
void    ag_pcm_pace_wait(ag_pcm_t *o);
int32_t ag_pcm_slack_us(const ag_pcm_t *o);
void    ag_pcm_poll_stats(ag_pcm_t *o);

/*
 * The largest block this sink can actually serve, in frames; 0 when it has no
 * such limit - a file, a null sink, anything that is not a converter.
 *
 * Ask it before choosing a block.  A block bigger than this is not slower by
 * a little: the read cannot be answered inside the block it belongs to, so
 * every block is late and the time shows up as processing that is not there.
 */
int32_t ag_pcm_period(const ag_pcm_t *o);

#ifdef __cplusplus
}
#endif

#endif /* AG_PCM_H */
