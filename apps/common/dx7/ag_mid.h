/*
 * Minimal Standard MIDI File player (format 0/1) for ag_dx7 demos.
 * Note on/off + tempo + program change. Loops. No libm.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_MID_H
#define AG_MID_H

#include <stdint.h>

enum {
    AG_MID_EV_ON = 1,
    AG_MID_EV_OFF = 2,
    AG_MID_EV_PROG = 3,
};

typedef struct ag_mid_ev {
    uint32_t t_us; /* absolute song time, microseconds */
    uint8_t  type;
    uint8_t  note;
    uint8_t  vel;
    uint8_t  ch;
} ag_mid_ev_t;

typedef struct ag_mid_player {
    ag_mid_ev_t *ev;
    int          nev;
    int          iev;
    uint64_t     pos_us;
    uint64_t     len_us;
    int          loop;
    int          playing;
    char         name[32];
} ag_mid_player_t;

typedef void (*ag_mid_note_fn)(void *ctx, int on, uint8_t note, uint8_t vel,
                               uint8_t ch);
typedef void (*ag_mid_prog_fn)(void *ctx, uint8_t prog, uint8_t ch);

void ag_mid_init(ag_mid_player_t *p);
void ag_mid_unload(ag_mid_player_t *p);
/* Parse SMF bytes; returns 0 ok. Uses ag_malloc for event list. */
int  ag_mid_load(ag_mid_player_t *p, const uint8_t *data, int len,
                 const char *name);
void ag_mid_start(ag_mid_player_t *p);
void ag_mid_stop(ag_mid_player_t *p);
void ag_mid_set_loop(ag_mid_player_t *p, int on);
/* Advance playback by `frames` at `sample_rate`; fire callbacks. */
void ag_mid_advance(ag_mid_player_t *p, uint32_t frames, uint32_t sample_rate,
                    ag_mid_note_fn on_note, ag_mid_prog_fn on_prog, void *ctx);

#endif
