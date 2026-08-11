/*
 * Shared AMP player state + commands (mouse and keyboard).
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AMP_APP_H
#define AMP_APP_H

#include <argon/argon.h>

#include "ag_mp3.h"
#include "amp_eq.h"
#include "amp_playlist.h"
#include "amp_skin.h"

typedef enum amp_state {
    AMP_STOPPED = 0,
    AMP_PLAYING,
    AMP_PAUSED
} amp_state_t;

typedef struct amp_player {
    amp_skin_t     skin;
    amp_playlist_t pl;
    amp_eq_t       eq;
    ag_mp3_t      *mp3;
    amp_state_t    state;
    amp_panel_t    focus;
    int            volume;   /* 0..100 */
    int            balance;  /* -100..100 */
    int            repeat;
    int            shuffle;
    int            dirty;
    int            quit;
    uint16_t       fb_w, fb_h;
    char           title[80];
    char           status[80];
    ag_handle_t    audio_fd;
    ag_handle_t    mouse_fd;
    char           audio_path[AG_PATH_MAX];
    uint32_t       rate;
    /* drag */
    amp_ctrl_t     drag;
    int            mx, my, mbtn;
} amp_player_t;

void amp_cmd_play_pause(amp_player_t *p);
void amp_cmd_stop(amp_player_t *p);
void amp_cmd_next(amp_player_t *p);
void amp_cmd_prev(amp_player_t *p);
void amp_cmd_seek_delta(amp_player_t *p, int delta_ms);
void amp_cmd_seek_permille(amp_player_t *p, int permille);
void amp_cmd_volume(amp_player_t *p, int vol);
void amp_cmd_balance(amp_player_t *p, int bal);
void amp_cmd_open_sel(amp_player_t *p);
void amp_cmd_add_dirs(amp_player_t *p);
void amp_cmd_load_m3u(amp_player_t *p);
void amp_cmd_save_m3u(amp_player_t *p);
void amp_cmd_focus_next(amp_player_t *p, int dir);
void amp_cmd_eq_adjust(amp_player_t *p, int delta);
int  amp_open_track(amp_player_t *p, const char *path);

void amp_ui_draw(amp_player_t *p);
void amp_ui_pointer(amp_player_t *p, const ag_event_t *ev);
void amp_handle_key(amp_player_t *p, int key, int down, uint32_t mods);

#endif /* AMP_APP_H */
