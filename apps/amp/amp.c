/*
 * AMP — Winamp-style MP3 player for ArgonOS.
 *
 * Soft RGB565 UI (VGA 640x400 or QVGA 320x240 bitmap skins), streaming
 * minimp3 decode, 10-band EQ, playlist, mouse + keyboard.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include <argon/argon.h>
#include <argon/keys.h>

#include "amp_app.h"
#include "audio_out.h"

/* 512 KB arena is enough for VGA skin paint buffers; 1 MB blocked FM+AMP. */
AG_APP_SIZED("AMP", "0.1", "argon", AG_AXE_NEEDS_GFX, 16 * 1024, 512 * 1024);

#define CHUNK_FRAMES 512
#define UI_PERIOD_MS 100u
#define UI_PERIOD_PLAY_MS 200u
/* Wall budget per main-loop audio burst (keep UI/events alive). */
#define PUMP_BUDGET_MS 12u
/* How far ahead of wall-clock we may fill the PCM sink. */
#define PACE_LEAD_MS 100u

static amp_player_t s_p;
static int16_t      s_pcm[CHUNK_FRAMES * 2];
static uint64_t     s_pcm_sent;       /* frames written to audio sink */
static uint32_t     s_pace_origin_ms; /* wall clock when s_pcm_sent matched pos */

void amp_pace_sync(void)
{
    uint32_t rate = s_p.rate ? s_p.rate : 22050u;
    uint32_t pos_ms = (s_p.mp3 != NULL) ? ag_mp3_position_ms(s_p.mp3) : 0u;
    s_pcm_sent = ((uint64_t)pos_ms * (uint64_t)rate) / 1000ull;
    s_pace_origin_ms = ag_millis();
}

static int ends_mp3(const char *s)
{
    size_t n;
    if (s == NULL) {
        return 0;
    }
    n = strlen(s);
    return n >= 4 && s[n - 4] == '.' &&
           (s[n - 3] == 'm' || s[n - 3] == 'M') &&
           (s[n - 2] == 'p' || s[n - 2] == 'P') && s[n - 1] == '3';
}

static void apply_volume(int16_t *stereo, int frames, int vol, int bal)
{
    int i;
    int vl = vol;
    int vr = vol;
    if (bal < 0) {
        vr = (vr * (100 + bal)) / 100;
    } else if (bal > 0) {
        vl = (vl * (100 - bal)) / 100;
    }
    for (i = 0; i < frames; i++) {
        int l = ((int)stereo[i * 2] * vl) / 100;
        int r = ((int)stereo[i * 2 + 1] * vr) / 100;
        if (l > 32767) {
            l = 32767;
        }
        if (l < -32768) {
            l = -32768;
        }
        if (r > 32767) {
            r = 32767;
        }
        if (r < -32768) {
            r = -32768;
        }
        stereo[i * 2] = (int16_t)l;
        stereo[i * 2 + 1] = (int16_t)r;
    }
}

static int s_sync_empty; /* consecutive empty reads before first frame */

static int pump_audio_once(void)
{
    int n;
    uint32_t rate;
    if (s_p.state != AMP_PLAYING || s_p.mp3 == NULL || s_p.audio_fd < 0) {
        return 0;
    }
    n = ag_mp3_read(s_p.mp3, s_pcm, CHUNK_FRAMES);
    if (n < 0) {
        s_p.state = AMP_STOPPED;
        strncpy(s_p.status, "decode fail", sizeof(s_p.status) - 1);
        s_p.dirty = 1;
        s_sync_empty = 0;
        return 0;
    }
    if (n == 0) {
        if (ag_mp3_rate(s_p.mp3) == 0) {
            if (++s_sync_empty > 256) {
                s_p.state = AMP_STOPPED;
                strncpy(s_p.status, "no mp3 frames", sizeof(s_p.status) - 1);
                s_p.dirty = 1;
                s_sync_empty = 0;
                ag_printf("amp: no frames after sync hunt\n");
            }
            return 0;
        }
        s_sync_empty = 0;
        {
            int prev = s_p.pl.cur;
            amp_cmd_next(&s_p);
            if (s_p.pl.cur == prev && s_p.state == AMP_PLAYING) {
                s_p.state = AMP_STOPPED;
                strncpy(s_p.status, "end of track", sizeof(s_p.status) - 1);
                s_p.dirty = 1;
            }
        }
        return 0;
    }
    s_sync_empty = 0;
    rate = ag_mp3_rate(s_p.mp3);
    if (rate != 0 && rate != s_p.rate && rate <= 48000u) {
        ag_audio_fmt_t fmt;
        s_p.rate = rate;
        amp_eq_set_rate(&s_p.eq, rate);
        fmt.rate = rate;
        fmt.channels = 2;
        fmt.bits = 16;
        (void)ag_dev_ioctl(s_p.audio_fd, AG_IOC_AUDIO_SETFMT, &fmt, sizeof(fmt));
        ag_printf("amp: rate=%u\n", (unsigned)rate);
    }
    amp_eq_process(&s_p.eq, s_pcm, n);
    if (s_p.volume != 100 || s_p.balance != 0) {
        apply_volume(s_pcm, n, s_p.volume, s_p.balance);
    }
    (void)ag_dev_write(s_p.audio_fd, s_pcm, (size_t)n * 4u);
    return n;
}

static int sink_almost_full(void)
{
    ag_audio_stats_t st;
    if (s_p.audio_fd < 0) {
        return 0;
    }
    if (ag_dev_ioctl(s_p.audio_fd, AG_IOC_AUDIO_GETSTATS, &st, sizeof(st)) !=
        0) {
        return 0;
    }
    if (st.ring_cap == 0u) {
        return 0;
    }
    /* Leave ~25% free so pcmvirt never discards oldest (heard as end-jump). */
    return st.ring_used >= (st.ring_cap - (st.ring_cap / 4u));
}

static void pump_audio(void)
{
    uint32_t t0;
    uint32_t rate;
    uint32_t elapsed;
    uint64_t limit;
    if (s_p.state != AMP_PLAYING || s_p.mp3 == NULL || s_p.audio_fd < 0) {
        return;
    }
    rate = s_p.rate ? s_p.rate : 22050u;
    elapsed = ag_millis() - s_pace_origin_ms;
    limit = ((uint64_t)(elapsed + PACE_LEAD_MS) * (uint64_t)rate) / 1000ull;
    /*
     * In-RAM decode is far faster than realtime. Without pacing we dump the
     * whole track into pcmvirt's 32KB ring; it drops the head and the UI
     * clock jumps to the end after a few hundred ms of audio.
     */
    if (s_pcm_sent >= limit || sink_almost_full()) {
        ag_delay(1);
        return;
    }
    t0 = ag_millis();
    while (s_pcm_sent < limit && !sink_almost_full()) {
        int n = pump_audio_once();
        if (n <= 0) {
            break;
        }
        s_pcm_sent += (uint64_t)n;
        if ((ag_millis() - t0) >= PUMP_BUDGET_MS) {
            break;
        }
    }
}

static void open_mouse(void)
{
    s_p.mouse_fd = ag_dev_open("/dev/mouse0");
}

static void pump_mouse(void)
{
    uint8_t buf[64];
    if (s_p.mouse_fd < 0) {
        return;
    }
    (void)ag_dev_read(s_p.mouse_fd, buf, sizeof(buf));
}

static int parse_args(int argc, char **argv)
{
    int i;
    const char *audio = "pcmmix";
    (void)ag_audio_out_resolve(audio, s_p.audio_path, sizeof(s_p.audio_path));
    for (i = 1; i < argc; i++) {
        if (ag_audio_out_ieq(argv[i], "pcmvirt") ||
            ag_audio_out_ieq(argv[i], "pcmmix") ||
            ag_audio_out_ieq(argv[i], "mix") ||
            ag_audio_out_ieq(argv[i], "pcmnull") ||
            ag_audio_out_ieq(argv[i], "audio") ||
            ag_audio_out_ieq(argv[i], "mock") ||
            ag_audio_out_ieq(argv[i], "net")) {
            (void)ag_audio_out_resolve(argv[i], s_p.audio_path,
                                       sizeof(s_p.audio_path));
        } else if (ends_mp3(argv[i])) {
            (void)amp_pl_add(&s_p.pl, argv[i]);
            s_p.pl.cur = 0;
            s_p.pl.sel = 0;
        } else {
            (void)amp_pl_add_dir(&s_p.pl, argv[i]);
        }
    }
    return 0;
}

int ag_main(int argc, char **argv)
{
    /*
     * The system cruises at a lower clock while nothing says otherwise, and
     * this does: the arithmetic below is paced by a wall clock and does not fit
     * in two thirds of the speed.  Said in the first line rather than answered
     * later, because the first buffer is as real as the thousandth.
     */
    (void)ag_power_declare(AG_POWER_FIT_FULL_ONLY,
                           "22 kHz tube tract");

    ag_gfxinfo_t info;
    uint32_t     ui_ms = 0;

    memset(&s_p, 0, sizeof(s_p));
    s_p.audio_fd = -1;
    s_p.mouse_fd = -1;
    s_p.volume = 80;
    s_p.balance = 0;
    s_p.focus = AMP_PANEL_MAIN;
    s_p.dirty = 1;
    s_p.rate = 22050;
    amp_pl_init(&s_p.pl);
    amp_eq_init(&s_p.eq, s_p.rate);
    parse_args(argc, argv);
    if (s_p.pl.count == 0) {
        amp_cmd_add_dirs(&s_p);
    }

    if (ag_gfx_acquire(&info) != AG_OK) {
        ag_printf("amp: gfx acquire failed\n");
        return 1;
    }
    s_p.fb_w = info.width;
    s_p.fb_h = info.height;
    if (amp_skin_load(&s_p.skin, s_p.fb_w, s_p.fb_h) != 0) {
        ag_printf("amp: skin load failed\n");
        ag_gfx_release();
        return 1;
    }

    open_mouse();
    if (s_p.mouse_fd < 0) {
        ag_printf("amp: no /dev/mouse0 (drv install h:\\mousevirt.sys)\n");
    } else {
        ag_printf("amp: mouse = /dev/mouse0\n");
    }
    /* Open PCM early (non-blocking sink); do not wait on a track first. */
    s_p.audio_fd = ag_audio_out_open_dev(s_p.audio_path, s_p.rate, 2);
    if (s_p.audio_fd < 0) {
        (void)ag_audio_out_resolve("pcmnull", s_p.audio_path,
                                   sizeof(s_p.audio_path));
        s_p.audio_fd = ag_audio_out_open_dev(s_p.audio_path, s_p.rate, 2);
    }
    ag_printf("amp: %ux%u skin=%s audio=%s tracks=%d\n", (unsigned)s_p.fb_w,
              (unsigned)s_p.fb_h, s_p.skin.qvga ? "qvga" : "vga",
              s_p.audio_path, s_p.pl.count);
    if (s_p.pl.count > 0 && s_p.pl.cur < 0) {
        s_p.pl.cur = 0;
        s_p.pl.sel = 0;
    }
    /* Cursor visible before first host event (center). */
    s_p.mx = (int)s_p.fb_w / 2;
    s_p.my = (int)s_p.fb_h / 2;
    s_p.mouse_live = 1;
    /* First paint before any HostFS MP3 I/O — otherwise QEMU looks hung. */
    amp_ui_draw(&s_p);
    s_p.dirty = 0;
    ui_ms = ag_millis();
    ag_printf("amp: ui ready (Space = play)\n");

    while (!s_p.quit && !ag_interrupted()) {
        ag_event_t ev;
        pump_mouse();
        while (ag_poll_event(&ev, 0)) {
            if (ev.type == AG_EV_QUIT) {
                s_p.quit = 1;
                break;
            }
            if (ev.type == AG_EV_FOCUS_LOST) {
                /* Keep decode/playback; only UI pauses. */
                continue;
            }
            if (ev.type == AG_EV_FOCUS_GAINED) {
                ag_gfxinfo_t gi;
                if (ag_gfx_acquire(&gi) == AG_OK) {
                    s_p.fb_w = gi.width;
                    s_p.fb_h = gi.height;
                }
                s_p.dirty = 1;
                continue;
            }
            if (!ag_focused()) {
                continue;
            }
            if (ev.type == AG_EV_KEY_DOWN || ev.type == AG_EV_KEY_UP) {
                amp_handle_key(&s_p, (int)ev.key.keycode,
                               ev.type == AG_EV_KEY_DOWN, ev.key.mods);
            } else if (ev.type == AG_EV_POINTER_DOWN ||
                       ev.type == AG_EV_POINTER_UP ||
                       ev.type == AG_EV_POINTER_MOVE ||
                       ev.type == AG_EV_WHEEL) {
                amp_ui_pointer(&s_p, &ev);
            }
        }
        /* Open / decode / audio continue without focus; drawing does not. */
        if (s_p.want_open && s_p.pending_path[0]) {
            char path[AG_PATH_MAX];
            strncpy(path, s_p.pending_path, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
            s_p.want_open = 0;
            s_p.pending_path[0] = '\0';
            if (ag_focused()) {
                amp_ui_draw(&s_p);
                s_p.dirty = 0;
                ui_ms = ag_millis();
            }
            ag_printf("amp: opening %s\n", path);
            s_sync_empty = 0;
            (void)amp_open_track(&s_p, path);
            amp_pace_sync();
        }
        pump_audio();
        if (!ag_focused()) {
            ag_heartbeat();
            if (s_p.state != AMP_PLAYING) {
                ag_delay(10);
            }
            continue;
        }
        {
            /* Kernel may have force-released gfx while we were unfocused. */
            ag_gfxinfo_t gi;
            if (ag_gfx_acquire(&gi) == AG_OK) {
                s_p.fb_w = gi.width;
                s_p.fb_h = gi.height;
                s_p.dirty = 1;
            }
        }
        {
            uint32_t now = ag_millis();
            uint32_t ui_period =
                (s_p.state == AMP_PLAYING) ? UI_PERIOD_PLAY_MS : UI_PERIOD_MS;
            if (s_p.pressed != AMP_CTRL_NONE &&
                (now - s_p.press_ms) > 180u) {
                s_p.pressed = AMP_CTRL_NONE;
                s_p.dirty = 1;
            }
            if (s_p.dirty || (now - ui_ms) >= ui_period) {
                amp_ui_draw(&s_p);
                s_p.dirty = 0;
                ui_ms = now;
            }
        }
        ag_heartbeat();
        /*
         * Do not sleep while playing: a 2ms delay after each tiny decode
         * guaranteed underruns (slow clock + crackle). Idle only when stopped.
         */
        if (s_p.state != AMP_PLAYING) {
            ag_delay(10);
        }
    }

    if (s_p.mp3) {
        ag_mp3_close(s_p.mp3);
        s_p.mp3 = NULL;
    }
    if (s_p.mouse_fd >= 0) {
        (void)ag_dev_close(s_p.mouse_fd);
    }
    if (s_p.audio_fd >= 0) {
        (void)ag_dev_close(s_p.audio_fd);
    }
    amp_skin_free(&s_p.skin);
    ag_gfx_release();
    return 0;
}
