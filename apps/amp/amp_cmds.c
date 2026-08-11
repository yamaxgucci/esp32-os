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

int amp_open_track(amp_player_t *p, const char *path)
{
    ag_mp3_t *m = NULL;
    uint32_t rate;
    if (p == NULL || path == NULL) {
        return -1;
    }
    if (p->mp3) {
        ag_mp3_close(p->mp3);
        p->mp3 = NULL;
    }
    if (ag_mp3_open(&m, path) != 0) {
        set_status(p, "open failed");
        p->state = AMP_STOPPED;
        return -1;
    }
    p->mp3 = m;
    /* Bounded prime: junk/HostFS MP3 must not block the UI thread. */
    {
        int16_t tmp[2304];
        int     tries;
        for (tries = 0; tries < 8 && ag_mp3_rate(m) == 0; tries++) {
            int n = ag_mp3_read(m, tmp, 1);
            if (n < 0) {
                break;
            }
            if (n == 0) {
                break;
            }
        }
        (void)ag_mp3_seek_permille(m, 0);
    }
    rate = ag_mp3_rate(m);
    if (rate == 0) {
        rate = 22050;
    }
    if (rate > 48000) {
        rate = 48000;
    }
    p->rate = rate;
    amp_eq_set_rate(&p->eq, rate);
    if (p->audio_fd < 0) {
        p->audio_fd = ag_audio_out_open_dev(p->audio_path, rate, 2);
        if (p->audio_fd < 0) {
            (void)ag_audio_out_resolve("pcmnull", p->audio_path,
                                       sizeof(p->audio_path));
            p->audio_fd = ag_audio_out_open_dev(p->audio_path, rate, 2);
        }
    } else if (rate != 0) {
        ag_audio_fmt_t fmt;
        fmt.rate = rate;
        fmt.channels = 2;
        fmt.bits = 16;
        (void)ag_dev_ioctl(p->audio_fd, AG_IOC_AUDIO_SETFMT, &fmt, sizeof(fmt));
    }
    {
        const char *t = ag_mp3_title(m);
        if (t && t[0]) {
            strncpy(p->title, t, sizeof(p->title) - 1);
        } else {
            const char *base = path;
            const char *s;
            for (s = path; *s; s++) {
                if (*s == '\\' || *s == '/') {
                    base = s + 1;
                }
            }
            strncpy(p->title, base, sizeof(p->title) - 1);
        }
        p->title[sizeof(p->title) - 1] = '\0';
    }
    p->state = AMP_PLAYING;
    set_status(p, "playing");
    p->dirty = 1;
    return 0;
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
    } else {
        if (p->pl.cur < 0 && p->pl.count > 0) {
            p->pl.cur = p->pl.sel;
        }
        if (amp_pl_current(&p->pl)) {
            (void)amp_open_track(p, amp_pl_current(&p->pl));
        } else {
            set_status(p, "no track");
        }
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
    if (amp_pl_current(&p->pl)) {
        (void)amp_open_track(p, amp_pl_current(&p->pl));
    }
}

void amp_cmd_prev(amp_player_t *p)
{
    if (p == NULL) {
        return;
    }
    if (p->mp3 && ag_mp3_position_ms(p->mp3) > 3000) {
        (void)ag_mp3_seek_permille(p->mp3, 0);
        p->dirty = 1;
        return;
    }
    if (amp_pl_prev(&p->pl, p->repeat) < 0) {
        return;
    }
    if (amp_pl_current(&p->pl)) {
        (void)amp_open_track(p, amp_pl_current(&p->pl));
    }
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
    p->dirty = 1;
}

void amp_cmd_seek_permille(amp_player_t *p, int permille)
{
    if (p == NULL || p->mp3 == NULL) {
        return;
    }
    (void)ag_mp3_seek_permille(p->mp3, permille);
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
    if (amp_pl_current(&p->pl)) {
        (void)amp_open_track(p, amp_pl_current(&p->pl));
    }
}

void amp_cmd_add_dirs(amp_player_t *p)
{
    int n = 0;
    if (p == NULL) {
        return;
    }
    /* Only shallow music folders — scanning all of H: is brutal on HostFS. */
    n += amp_pl_add_dir(&p->pl, "h:\\amp");
    n += amp_pl_add_dir(&p->pl, "h:\\music");
    if (n > 0) {
        set_status(p, "added tracks");
    } else {
        set_status(p, "no mp3 found");
    }
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
