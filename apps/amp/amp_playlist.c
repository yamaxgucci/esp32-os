/*
 * Playlist + m3u + directory scan.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "amp_playlist.h"

#include <string.h>

static int ends_with_mp3(const char *name)
{
    size_t n;
    if (name == NULL) {
        return 0;
    }
    n = strlen(name);
    if (n < 4) {
        return 0;
    }
    /* Skip generator stub — it is not a real bitstream and wedges decode/HostFS. */
    if (n == 8 &&
        (name[0] == 'd' || name[0] == 'D') &&
        (name[1] == 'e' || name[1] == 'E') &&
        (name[2] == 'm' || name[2] == 'M') &&
        (name[3] == 'o' || name[3] == 'O') &&
        name[4] == '.' ) {
        return 0;
    }
    return (name[n - 4] == '.' &&
            (name[n - 3] == 'm' || name[n - 3] == 'M') &&
            (name[n - 2] == 'p' || name[n - 2] == 'P') &&
            (name[n - 1] == '3'));
}

void amp_pl_init(amp_playlist_t *pl)
{
    if (pl == NULL) {
        return;
    }
    memset(pl, 0, sizeof(*pl));
    pl->cur = -1;
    pl->sel = 0;
    pl->scroll = 0;
}

int amp_pl_add(amp_playlist_t *pl, const char *path)
{
    size_t i;
    if (pl == NULL || path == NULL || path[0] == '\0' || pl->count >= AMP_PL_MAX) {
        return -1;
    }
    for (i = 0; i < (size_t)pl->count; i++) {
        if (strcmp(pl->paths[i], path) == 0) {
            return 0;
        }
    }
    strncpy(pl->paths[pl->count], path, AG_PATH_MAX - 1);
    pl->paths[pl->count][AG_PATH_MAX - 1] = '\0';
    pl->count++;
    return 0;
}

int amp_pl_add_dir(amp_playlist_t *pl, const char *dir)
{
    ag_handle_t h;
    ag_dirent_t e;
    char        path[AG_PATH_MAX];
    int         added = 0;

    if (pl == NULL || dir == NULL) {
        return -1;
    }
    h = ag_opendir(dir);
    if (h < 0) {
        return -1;
    }
    while (ag_readdir(h, &e) == AG_OK) {
        size_t dlen;
        size_t nlen;
        if (e.name[0] == '.') {
            continue;
        }
        if (!ends_with_mp3(e.name)) {
            continue;
        }
        dlen = strlen(dir);
        nlen = strlen(e.name);
        if (dlen + 1 + nlen >= AG_PATH_MAX) {
            continue;
        }
        memcpy(path, dir, dlen);
        if (dlen > 0 && dir[dlen - 1] != '\\' && dir[dlen - 1] != '/') {
            path[dlen++] = '\\';
        }
        memcpy(path + dlen, e.name, nlen + 1);
        if (amp_pl_add(pl, path) == 0) {
            added++;
        }
    }
    (void)ag_closedir(h);
    return added;
}

void amp_pl_remove_sel(amp_playlist_t *pl)
{
    int i;
    if (pl == NULL || pl->sel < 0 || pl->sel >= pl->count) {
        return;
    }
    for (i = pl->sel; i + 1 < pl->count; i++) {
        memcpy(pl->paths[i], pl->paths[i + 1], AG_PATH_MAX);
    }
    pl->count--;
    if (pl->cur == pl->sel) {
        pl->cur = -1;
    } else if (pl->cur > pl->sel) {
        pl->cur--;
    }
    if (pl->sel >= pl->count) {
        pl->sel = pl->count > 0 ? pl->count - 1 : 0;
    }
}

int amp_pl_load_m3u(amp_playlist_t *pl, const char *path)
{
    ag_handle_t h;
    char        line[AG_PATH_MAX];
    int         n = 0;
    int         li = 0;
    uint8_t     ch;

    if (pl == NULL || path == NULL) {
        return -1;
    }
    h = ag_open(path, AG_O_RDONLY);
    if (h < 0) {
        return -1;
    }
    amp_pl_init(pl);
    while (ag_read(h, &ch, 1) == 1) {
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n' || li + 1 >= (int)sizeof(line)) {
            line[li] = '\0';
            if (li > 0 && line[0] != '#') {
                if (amp_pl_add(pl, line) == 0) {
                    n++;
                }
            }
            li = 0;
            continue;
        }
        line[li++] = (char)ch;
    }
    if (li > 0) {
        line[li] = '\0';
        if (line[0] != '#') {
            if (amp_pl_add(pl, line) == 0) {
                n++;
            }
        }
    }
    (void)ag_close(h);
    return n;
}

int amp_pl_save_m3u(const amp_playlist_t *pl, const char *path)
{
    ag_handle_t h;
    int         i;
    if (pl == NULL || path == NULL) {
        return -1;
    }
    h = ag_open(path, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (h < 0) {
        return -1;
    }
    for (i = 0; i < pl->count; i++) {
        size_t n = strlen(pl->paths[i]);
        if (ag_write(h, pl->paths[i], n) != (int32_t)n ||
            ag_write(h, "\n", 1) != 1) {
            (void)ag_close(h);
            return -1;
        }
    }
    (void)ag_close(h);
    return 0;
}

const char *amp_pl_current(const amp_playlist_t *pl)
{
    if (pl == NULL || pl->cur < 0 || pl->cur >= pl->count) {
        return NULL;
    }
    return pl->paths[pl->cur];
}

int amp_pl_next(amp_playlist_t *pl, int wrap)
{
    if (pl == NULL || pl->count <= 0) {
        return -1;
    }
    if (pl->cur < 0) {
        pl->cur = 0;
    } else if (pl->cur + 1 < pl->count) {
        pl->cur++;
    } else if (wrap) {
        pl->cur = 0;
    } else {
        return -1;
    }
    pl->sel = pl->cur;
    return pl->cur;
}

int amp_pl_prev(amp_playlist_t *pl, int wrap)
{
    if (pl == NULL || pl->count <= 0) {
        return -1;
    }
    if (pl->cur < 0) {
        pl->cur = 0;
    } else if (pl->cur > 0) {
        pl->cur--;
    } else if (wrap) {
        pl->cur = pl->count - 1;
    } else {
        return -1;
    }
    pl->sel = pl->cur;
    return pl->cur;
}
