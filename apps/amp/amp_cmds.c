/*
 * Transport / playlist / EQ commands shared by mouse + keyboard.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "amp_app.h"

#include <string.h>

#include "audio_out.h"

static void set_status(amp_player_t *p, const char *s)
{
    size_t i = 0;
    if (p == NULL || s == NULL) {
        return;
    }
    while (s[i] && i + 1 < sizeof(p->status)) {
        p->status[i] = s[i];
        i++;
    }
    p->status[i] = '\0';
    p->dirty = 1;
}

static void set_title_from_path(amp_player_t *p, const char *path)
{
    const char *base = path;
    const char *s;
    for (s = path; *s; s++) {
        if (*s == '\\' || *s == '/') {
            base = s + 1;
        }
    }
    strncpy(p->title, base, sizeof(p->title) - 1);
    p->title[sizeof(p->title) - 1] = '\0';
}

int amp_open_track(amp_player_t *p, const char *path)
{
    ag_mp3_t *m = NULL;
    if (p == NULL || path == NULL) {
        return -1;
    }
    if (p->mp3) {
        ag_mp3_close(p->mp3);
        p->mp3 = NULL;
    }
    ag_printf("amp: open %s\n", path);
    ag_heartbeat();
    if (ag_mp3_open(&m, path) != 0) {
        set_status(p, "open failed");
        p->state = AMP_STOPPED;
        return -1;
    }
    /*
     * No priming decode: first HostFS reads of a real MP3 can wedge QEMU if
     * done before returning to the UI loop. Rate is discovered while pumping;
     * start at 22050 until then. Realtime pacing in amp.c prevents sink flood.
     */
    p->mp3 = m;
    p->rate = 22050;
    amp_eq_set_rate(&p->eq, p->rate);
    if (p->audio_fd < 0) {
        p->audio_fd = ag_audio_out_open_dev(p->audio_path, p->rate, 2);
        if (p->audio_fd < 0) {
            (void)ag_audio_out_resolve("pcmnull", p->audio_path,
                                       sizeof(p->audio_path));
            p->audio_fd = ag_audio_out_open_dev(p->audio_path, p->rate, 2);
        }
    }
    set_title_from_path(p, path);
    p->state = AMP_PLAYING;
    set_status(p, "playing");
    ag_printf("amp: playing (rate TBD)\n");
    p->dirty = 1;
    return 0;
}

static void request_open_current(amp_player_t *p)
{
    const char *path;
    if (p->pl.cur < 0 && p->pl.count > 0) {
        p->pl.cur = p->pl.sel;
    }
    path = amp_pl_current(&p->pl);
    if (path == NULL) {
        set_status(p, "no track");
        return;
    }
    strncpy(p->pending_path, path, sizeof(p->pending_path) - 1);
    p->pending_path[sizeof(p->pending_path) - 1] = '\0';
    p->want_open = 1;
    set_status(p, "loading...");
}

void amp_cmd_play_pause(amp_player_t *p)
{
    if (p == NULL) {
        return;
    }
    if (p->state == AMP_PLAYING) {
        p->state = AMP_PAUSED;
        set_status(p, "paused");
    } else if (p->state == AMP_PAUSED) {
        p->state = AMP_PLAYING;
        set_status(p, "playing");
        amp_pace_sync();
    } else if (p->pl.count <= 0) {
        amp_cmd_open_picker(p);
    } else {
        request_open_current(p);
    }
}

void amp_cmd_stop(amp_player_t *p)
{
    if (p == NULL) {
        return;
    }
    p->state = AMP_STOPPED;
    if (p->mp3) {
        (void)ag_mp3_seek_permille(p->mp3, 0);
    }
    set_status(p, "stopped");
}

void amp_cmd_next(amp_player_t *p)
{
    int idx;
    if (p == NULL) {
        return;
    }
    if (p->shuffle && p->pl.count > 1) {
        idx = (int)(ag_millis() % (uint32_t)p->pl.count);
        if (idx == p->pl.cur) {
            idx = (idx + 1) % p->pl.count;
        }
        p->pl.cur = idx;
        p->pl.sel = idx;
    } else if (amp_pl_next(&p->pl, p->repeat) < 0) {
        amp_cmd_stop(p);
        return;
    }
    request_open_current(p);
}

void amp_cmd_prev(amp_player_t *p)
{
    if (p == NULL) {
        return;
    }
    if (p->mp3 && ag_mp3_position_ms(p->mp3) > 3000) {
        (void)ag_mp3_seek_permille(p->mp3, 0);
        amp_pace_sync();
        p->dirty = 1;
        return;
    }
    if (amp_pl_prev(&p->pl, p->repeat) < 0) {
        return;
    }
    request_open_current(p);
}

void amp_cmd_seek_delta(amp_player_t *p, int delta_ms)
{
    uint32_t pos, dur;
    int permille;
    if (p == NULL || p->mp3 == NULL) {
        return;
    }
    pos = ag_mp3_position_ms(p->mp3);
    dur = ag_mp3_duration_ms(p->mp3);
    if (dur == 0) {
        dur = 1;
    }
    if (delta_ms < 0 && (uint32_t)(-delta_ms) > pos) {
        pos = 0;
    } else {
        pos = (uint32_t)((int)pos + delta_ms);
    }
    if (pos > dur) {
        pos = dur;
    }
    permille = (int)((pos * 1000u) / dur);
    (void)ag_mp3_seek_permille(p->mp3, permille);
    amp_pace_sync();
    p->dirty = 1;
}

void amp_cmd_seek_permille(amp_player_t *p, int permille)
{
    if (p == NULL || p->mp3 == NULL) {
        return;
    }
    (void)ag_mp3_seek_permille(p->mp3, permille);
    amp_pace_sync();
    p->dirty = 1;
}

void amp_cmd_volume(amp_player_t *p, int vol)
{
    if (p == NULL) {
        return;
    }
    if (vol < 0) {
        vol = 0;
    }
    if (vol > 100) {
        vol = 100;
    }
    p->volume = vol;
    p->dirty = 1;
}

void amp_cmd_balance(amp_player_t *p, int bal)
{
    if (p == NULL) {
        return;
    }
    if (bal < -100) {
        bal = -100;
    }
    if (bal > 100) {
        bal = 100;
    }
    p->balance = bal;
    p->dirty = 1;
}

void amp_cmd_open_sel(amp_player_t *p)
{
    if (p == NULL || p->pl.count <= 0) {
        return;
    }
    p->pl.cur = p->pl.sel;
    request_open_current(p);
}

void amp_cmd_add_dirs(amp_player_t *p)
{
    int n = 0;
    int r;
    char msg[64];
    int i;
    if (p == NULL) {
        return;
    }
    /* Shallow dirs only. Failed opendir counts as 0 added, not -1. */
    r = amp_pl_add_dir(&p->pl, "h:\\amp");
    if (r > 0) {
        n += r;
    }
    r = amp_pl_add_dir(&p->pl, "h:\\music");
    if (r > 0) {
        n += r;
    }
    r = amp_pl_add_dir(&p->pl, "h:\\");
    if (r > 0) {
        n += r;
    }
    i = 0;
    if (p->pl.count >= 100) {
        msg[i++] = (char)('0' + (p->pl.count / 100) % 10);
    }
    if (p->pl.count >= 10) {
        msg[i++] = (char)('0' + (p->pl.count / 10) % 10);
    }
    msg[i++] = (char)('0' + p->pl.count % 10);
    msg[i++] = ' ';
    msg[i++] = 't';
    msg[i++] = 'r';
    msg[i++] = 'a';
    msg[i++] = 'c';
    msg[i++] = 'k';
    msg[i++] = 's';
    msg[i] = '\0';
    if (p->pl.count == 0) {
        set_status(p, "no mp3 (Eject/A)");
    } else {
        set_status(p, msg);
    }
    ag_printf("amp: playlist %d (+%d). Eject/A=rescan, Enter=play\n",
              p->pl.count, n);
}

void amp_btn_press(amp_player_t *p, amp_ctrl_t c)
{
    if (p == NULL || c == AMP_CTRL_NONE) {
        return;
    }
    p->pressed = c;
    p->press_ms = ag_millis();
    p->dirty = 1;
}

void amp_cmd_open_picker(amp_player_t *p)
{
    int i;
    if (p == NULL) {
        return;
    }
    /* Refresh playlist from disk, then show picker of current list. */
    amp_cmd_add_dirs(p);
    p->pick_n = 0;
    p->pick_i = 0;
    for (i = 0; i < p->pl.count && p->pick_n < 32; i++) {
        strncpy(p->pick[p->pick_n], p->pl.paths[i], AG_PATH_MAX - 1);
        p->pick[p->pick_n][AG_PATH_MAX - 1] = '\0';
        p->pick_n++;
    }
    if (p->pick_n == 0) {
        set_status(p, "no mp3 in H:\\amp|music|\\");
        p->picker = 0;
        p->dirty = 1;
        return;
    }
    p->picker = 1;
    p->focus = AMP_PANEL_PL;
    set_status(p, "pick file, Enter");
    p->dirty = 1;
}

void amp_cmd_picker_choose(amp_player_t *p)
{
    int i;
    if (p == NULL || !p->picker || p->pick_n <= 0) {
        return;
    }
    if (p->pick_i < 0) {
        p->pick_i = 0;
    }
    if (p->pick_i >= p->pick_n) {
        p->pick_i = p->pick_n - 1;
    }
    /* Select matching playlist row and open. */
    for (i = 0; i < p->pl.count; i++) {
        if (strcmp(p->pl.paths[i], p->pick[p->pick_i]) == 0) {
            p->pl.sel = i;
            p->pl.cur = i;
            break;
        }
    }
    p->picker = 0;
    request_open_current(p);
}

void amp_cmd_load_m3u(amp_player_t *p)
{
    if (p == NULL) {
        return;
    }
    if (amp_pl_load_m3u(&p->pl, "h:\\amp\\playlist.m3u") >= 0) {
        set_status(p, "m3u loaded");
    } else {
        set_status(p, "m3u missing");
    }
}

void amp_cmd_save_m3u(amp_player_t *p)
{
    if (p == NULL) {
        return;
    }
    if (amp_pl_save_m3u(&p->pl, "h:\\amp\\playlist.m3u") == 0) {
        set_status(p, "m3u saved");
    } else {
        set_status(p, "m3u save fail");
    }
}

void amp_cmd_focus_next(amp_player_t *p, int dir)
{
    int f;
    if (p == NULL) {
        return;
    }
    f = (int)p->focus + (dir >= 0 ? 1 : -1);
    if (f < 0) {
        f = AMP_PANEL_N - 1;
    }
    if (f >= AMP_PANEL_N) {
        f = 0;
    }
    p->focus = (amp_panel_t)f;
    p->dirty = 1;
}

void amp_cmd_eq_adjust(amp_player_t *p, int delta)
{
    int8_t *g;
    if (p == NULL) {
        return;
    }
    if (p->eq.band_sel <= 0) {
        g = &p->eq.preamp;
    } else {
        g = &p->eq.gain[p->eq.band_sel - 1];
    }
    {
        int v = (int)*g + delta;
        if (v < -12) {
            v = -12;
        }
        if (v > 12) {
            v = 12;
        }
        *g = (int8_t)v;
    }
    amp_eq_recalc(&p->eq);
    p->dirty = 1;
}
