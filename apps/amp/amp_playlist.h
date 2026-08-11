/*
 * Playlist for AMP.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AMP_PLAYLIST_H
#define AMP_PLAYLIST_H

#include <argon/argon.h>

enum { AMP_PL_MAX = 96 };

typedef struct amp_playlist {
    char paths[AMP_PL_MAX][AG_PATH_MAX];
    int  count;
    int  cur;      /* playing index, -1 none */
    int  sel;      /* UI selection */
    int  scroll;
} amp_playlist_t;

void amp_pl_init(amp_playlist_t *pl);
int  amp_pl_add(amp_playlist_t *pl, const char *path);
int  amp_pl_add_dir(amp_playlist_t *pl, const char *dir);
void amp_pl_remove_sel(amp_playlist_t *pl);
int  amp_pl_load_m3u(amp_playlist_t *pl, const char *path);
int  amp_pl_save_m3u(const amp_playlist_t *pl, const char *path);
const char *amp_pl_current(const amp_playlist_t *pl);
int  amp_pl_next(amp_playlist_t *pl, int wrap);
int  amp_pl_prev(amp_playlist_t *pl, int wrap);

#endif /* AMP_PLAYLIST_H */
